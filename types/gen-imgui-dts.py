#!/usr/bin/env python3
"""Generates types/imgui.d.ts from imgui-quickjs/imgui-quickjs.cpp.

The bindings are hand-written wrappers with their own JS shapes (widgets return
{changed, value} rather than writing through a pointer), so neither the ImGui
headers nor dcimgui.json describe them - the C++ is the only source of truth.
This reads it three ways:

  * the export list, for the set of names and their grouping comments;
  * each wrapper body, for parameter and return types, inferred from which
    JS_To*/JS_New* the binding actually calls;
  * the doc comment above each wrapper, for parameter *names* only.

Anything not confidently inferred becomes `any`, and the run prints how many of
those there were. Regenerate after touching imgui-quickjs.cpp:

    python3 types/gen-imgui-dts.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "imgui-quickjs", "imgui-quickjs.cpp")
OUTPUT = os.path.join(HERE, "imgui.d.ts")

# --- reading the source ------------------------------------------------------


def read_source():
    with open(SOURCE, encoding="utf-8") as f:
        return f.read()


def export_list(src):
    """The js_imgui_funcs[] entries, in order, with their section comments."""
    start = src.index("const JSCFunctionListEntry js_imgui_funcs[]")
    end = src.index("\n};", start)
    body = src[start:end]

    entries = []
    section = None
    for line in body.splitlines():
        line = line.strip()
        comment = re.match(r"^// (.+)$", line)
        if comment:
            section = comment.group(1)
            continue
        func = re.match(
            r"^JS_CFUNC_DEF\(\"([A-Za-z0-9_]+)\", *(\d+), *(js_ImGui_[A-Za-z0-9_]+)\)", line
        )
        if func:
            entries.append(("func", func.group(1), func.group(3), section))
            continue
        enum = re.match(r"^JS_ENUM_DEF\(([A-Za-z0-9_]+)\)", line)
        if enum:
            entries.append(("enum", enum.group(1), None, section))
    return entries


def enum_members(src, name):
    """The JS_ENUM_ELEM members of one enum table, in order."""
    match = re.search(
        r"const JSCFunctionListEntry " + re.escape(name) + r"\[\] = \{(.*?)\n\};",
        src,
        re.S,
    )
    if not match:
        return []
    return re.findall(r"JS_ENUM_ELEM\([A-Za-z0-9_]+, *([A-Za-z0-9_]+)\)", match.group(1))


def function_bodies(src):
    """impl name -> (doc comment, body text)."""
    bodies = {}
    pattern = re.compile(
        r"^static JSValue (js_ImGui_[A-Za-z0-9_]+)\(JSContext", re.M)
    for match in pattern.finditer(src):
        name = match.group(1)
        # The file puts a function's closing brace at column 0.
        end = src.find("\n}\n", match.end())
        body = src[match.start(): end if end != -1 else len(src)]
        # The comment block immediately above, up to the previous blank line.
        head = src[:match.start()].rstrip("\n")
        comment = []
        for line in reversed(head.splitlines()):
            if not line.startswith("//"):
                break
            comment.append(line[2:].strip())
        bodies[name] = ("\n".join(reversed(comment)), body)
    return bodies


# --- type inference ----------------------------------------------------------

# How a binding reads argv[N] -> the TypeScript type it accepts. `\s*` rather
# than ` *` throughout: clang-format wraps these calls freely.
ARG_PATTERNS = [
    (r"JS_ToCString\(ctx,\s*argv\[{i}\]\)", "string"),
    (r"JS_ToFloat64\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*argv\[{i}\]\)", "number"),
    (r"JS_ToInt32\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*argv\[{i}\]\)", "number"),
    (r"JS_ToUint32\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*argv\[{i}\]\)", "number"),
    (r"JS_ToInt64\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*argv\[{i}\]\)", "number"),
    (r"js_to_ImVec2\(ctx,\s*argv\[{i}\]", "ImGui.Vec2"),
    (r"js_to_ImVec4\(ctx,\s*argv\[{i}\]", "ImGui.Vec4"),
    (r"js_get_float_array\(ctx,\s*argv\[{i}\]", "number[]"),
    (r"js_get_int_array\(ctx,\s*argv\[{i}\]", "number[]"),
    (r"js_get_color_array\(ctx,\s*argv\[{i}\]", "number[]"),
    (r"JS_ToBool\(ctx,\s*argv\[{i}\]\)", "boolean"),
    (r"JS_IsArray\([^)]*argv\[{i}\]\)", "any[]"),
    (r"JS_IsFunction\(ctx,\s*argv\[{i}\]\)", "Function"),
    (r"JS_IsObject\(argv\[{i}\]\)", "object"),
    (r"JS_IsString\(argv\[{i}\]\)", "string"),
    (r"JS_IsNumber\(argv\[{i}\]\)", "number"),
    (r"JS_GetPropertyStr\(ctx,\s*argv\[{i}\]", "object"),
    (r"JS_GetPropertyUint32\(ctx,\s*argv\[{i}\]", "any[]"),
    (r"JS_GetLength\(ctx,\s*argv\[{i}\]", "any[]"),
]

NEW_TO_TS = {
    "JS_NewBool": "boolean",
    "JS_NewInt32": "number",
    "JS_NewUint32": "number",
    "JS_NewInt64": "number",
    "JS_NewFloat64": "number",
    "JS_NewString": "string",
    "JS_NewArray": "any[]",
    "ImVec2_to_js": "ImGui.Vec2",
    "ImVec4_to_js": "ImGui.Vec4",
    "js_new_float_array": "number[]",
    "js_new_int_array": "number[]",
    "js_new_color_array": "number[]",
}


def arg_type(body, index):
    """What argv[index] is converted to, or None when nothing matched."""
    found = []
    for pattern, ts in ARG_PATTERNS:
        if re.search(pattern.format(i=index), body):
            found.append(ts)
    if not found:
        return None
    # A widget that accepts "string or number" tests both; keep the union.
    unique = []
    for ts in found:
        if ts not in unique:
            unique.append(ts)
    # An options object is read with both JS_IsObject and JS_GetPropertyStr.
    if "object" in unique and len(unique) > 1:
        unique = [t for t in unique if t != "object"]
    if "any[]" in unique:
        # An array read element-wise matches the generic array pattern too;
        # whatever the elements turned out to be is the better answer.
        specific = [t for t in unique if t.endswith("[]") and t != "any[]"]
        if specific:
            unique = [t for t in unique if t != "any[]"]
        elif elements_are_strings(body, index):
            unique = ["string[]" if t == "any[]" else t for t in unique]
    return " | ".join(unique[:2])


def elements_are_strings(body, index):
    """Whether the binding JS_ToCStrings each element it pulls out of argv."""
    for var in re.findall(
        r"JSValue\s+([A-Za-z0-9_]+)\s*=\s*JS_GetPropertyUint32\(ctx,\s*argv\[%d\]"
            % index, body):
        if re.search(r"JS_ToCString\(ctx,\s*%s\)" % re.escape(var), body):
            return True
    return False


def options_shape(body, index):
    """The `{format, flags, size, ...}` options object a binding reads out of
    argv[index], built from the JS_GetPropertyStr keys and how each value is
    then converted. None when argv[index] is not read that way."""
    reads = re.findall(
        r"JSValue\s+([A-Za-z0-9_]+)\s*=\s*JS_GetPropertyStr\(ctx,\s*argv\[%d\],\s*\"([A-Za-z0-9_]+)\"\)"
        % index,
        body,
    )
    if not reads:
        return None
    conversions = [
        (r"JS_ToCString\(ctx,\s*%s\)", "string"),
        (r"JS_ToFloat64\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*%s\)", "number"),
        (r"JS_ToInt32\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*%s\)", "number"),
        (r"JS_ToUint32\(ctx,\s*&[A-Za-z0-9_.\[\]]+,\s*%s\)", "number"),
        (r"js_to_ImVec2\(ctx,\s*%s", "ImGui.Vec2"),
        (r"js_to_ImVec4\(ctx,\s*%s", "ImGui.Vec4"),
        (r"js_get_float_array\(ctx,\s*%s", "number[]"),
        (r"JS_ToBool\(ctx,\s*%s\)", "boolean"),
    ]
    fields = []
    for var, key in reads:
        if key in [f[0] for f in fields]:
            continue
        ts = "any"
        for pattern, candidate in conversions:
            if re.search(pattern % re.escape(var), body):
                ts = candidate
                break
        if ts == "any":
            # Some bindings inline the vector read instead of calling
            # js_to_ImVec2: {x, y} off the option value directly.
            components = set(re.findall(
                r"JS_GetPropertyStr\(ctx,\s*%s,\s*\"([xyzw])\"\)" % re.escape(var),
                body))
            if {"x", "y"} <= components:
                ts = "ImGui.Vec4" if {"z", "w"} <= components else "ImGui.Vec2"
        fields.append((key, ts))
    return "{ " + "; ".join("%s?: %s" % f for f in fields) + " }"


def object_return(body, var):
    """The {key: type} shape a binding builds in `var`, or None."""
    fields = []
    for key, ctor in re.findall(
        r"JS_SetPropertyStr\(ctx,\s*" + re.escape(var) +
        r",\s*\"([A-Za-z0-9_]+)\",\s*([A-Za-z0-9_]+)\(", body):
        ts = NEW_TO_TS.get(ctor)
        if ts is None:
            ts = "any"
        if key not in [f[0] for f in fields]:
            fields.append((key, ts))
    if not fields:
        return None
    return "{ " + "; ".join("%s: %s" % f for f in fields) + " }"


def return_type(body):
    """The union of every non-error return in a binding."""
    kinds = []

    def add(ts):
        if ts and ts not in kinds:
            kinds.append(ts)

    for expr in re.findall(r"return +([^;]+);", body, re.S):
        expr = " ".join(expr.split())
        if expr.startswith("JS_Throw") or expr == "JS_EXCEPTION":
            continue  # an exception is not part of the type
        if expr == "JS_UNDEFINED":
            add("void")
            continue
        if expr == "JS_NULL":
            add("null")
            continue
        ctor = re.match(r"([A-Za-z0-9_]+)\(", expr)
        if ctor and ctor.group(1) in NEW_TO_TS:
            add(NEW_TO_TS[ctor.group(1)])
            continue
        if re.fullmatch(r"[A-Za-z0-9_]+", expr):
            shape = object_return(body, expr)
            add(shape if shape else "any")
            continue
        add("any")

    if not kinds:
        return "void"
    if len(kinds) > 1 and "void" in kinds:
        # `return JS_UNDEFINED` after an early-out is a bail-out, not a result.
        kinds = [k for k in kinds if k != "void"] or ["void"]
    return " | ".join(kinds)


def required_count(body):
    """How many leading arguments the binding rejects a call without."""
    counts = [int(n) for n in re.findall(r"argc *< *(\d+)", body)]
    return max(counts) if counts else 0


def max_arg(body):
    indices = [int(n) for n in re.findall(r"argv\[(\d+)\]", body)]
    return max(indices) + 1 if indices else 0


def comment_param_names(comment, name, count):
    """Parameter names out of the doc comment's JS signature line, if it has one
    and it agrees with the body about how many there are."""
    for match in re.finditer(re.escape(name) + r"\(([^)]*)\)", comment):
        # Skip the C++ signature the comment usually opens with.
        if comment[:match.start()].rstrip().endswith("ImGui::"):
            continue
        raw = match.group(1).strip()
        if not raw:
            names = []
        else:
            names = [p.strip().rstrip("?") for p in raw.split(",")]
        if len(names) != count:
            continue
        if all(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n) for n in names):
            return [sanitize(n) for n in names]
    return cpp_param_names(comment, name, count)


def cpp_param_names(comment, name, count):
    """Fall back to the C++ signature the comment usually opens with. Only used
    when it has exactly as many parameters as the wrapper takes - that agreement
    is the evidence it was parsed correctly, since the JS shape usually differs
    (out-pointers become return values, defaults become an options object)."""
    flat = " ".join(comment.split())
    marker = "ImGui::" + name + "("
    at = flat.find(marker)
    if at == -1:
        return None
    depth = 0
    start = at + len(marker)
    for i in range(start - 1, len(flat)):
        if flat[i] == "(":
            depth += 1
        elif flat[i] == ")":
            depth -= 1
            if depth == 0:
                return cpp_split(flat[start:i], count)
    return None


def cpp_split(params, count):
    parts, depth, current = [], 0, ""
    for ch in params:
        if ch == "," and depth == 0:
            parts.append(current)
            current = ""
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        current += ch
    if current.strip():
        parts.append(current)
    if len(parts) != count:
        return None

    names = []
    for part in parts:
        part = part.split("=")[0].strip().rstrip("[]").strip()
        identifier = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", part)
        if not identifier:
            return None
        names.append(sanitize(identifier[-1]))
    return names


RESERVED = {"function", "default", "var", "new", "class", "in", "of", "for"}


def sanitize(name):
    return name + "_" if name in RESERVED else name


# --- emitting ----------------------------------------------------------------


def main():
    src = read_source()
    bodies = function_bodies(src)
    entries = export_list(src)

    out = []
    out.append("// GENERATED by types/gen-imgui-dts.py - do not edit by hand.")
    out.append("//")
    out.append("// The object src/Script.cpp passes to the entry module's")
    out.append("// draw_gui export, built by imgui-quickjs/imgui-quickjs.cpp.")
    out.append("// There is deliberately no \"ImGui\" module to import: these")
    out.append("// calls are only valid inside the overlay's frame, which is")
    out.append("// exactly when draw_gui runs. So this is a type, not a value -")
    out.append("// the only ImGui in scope is the argument you were handed.")
    out.append("//")
    out.append("// These wrappers are NOT the C++ ImGui API: there are no output")
    out.append("// pointers, so a widget returns its new state - `const r =")
    out.append("// ImGui.SliderFloat(...); if (r.changed) x = r.value;` - and the")
    out.append("// optional trailing arguments are an options object rather than")
    out.append("// positional parameters.")
    out.append("")
    out.append("// Interfaces only, so this declares no runtime value to call.")
    out.append("declare namespace ImGui {")
    out.append("  /** An ImVec2, as {x, y}. */")
    out.append("  interface Vec2 {")
    out.append("    x: number;")
    out.append("    y: number;")
    out.append("  }")
    out.append("")
    out.append("  /** An ImVec4 / ImColor, as {x, y, z, w}. */")
    out.append("  interface Vec4 {")
    out.append("    x: number;")
    out.append("    y: number;")
    out.append("    z: number;")
    out.append("    w: number;")
    out.append("  }")
    out.append("}")
    out.append("")
    out.append("interface ImGui {")

    section = None
    anys = 0
    functions = 0
    enums = 0

    for kind, name, impl, entry_section in entries:
        if entry_section != section:
            section = entry_section
            out.append("")
            out.append("  // --- %s %s" % (section, "-" * max(0, 60 - len(section))))
            out.append("")

        if kind == "enum":
            members = enum_members(src, name)
            enums += 1
            out.append("  readonly %s: {" % name)
            for member in members:
                out.append("    readonly %s: number;" % sanitize(member))
            out.append("  };")
            continue

        comment, body = bodies[impl]
        functions += 1
        count = max(max_arg(body), 0)
        required = min(required_count(body), count)
        names = comment_param_names(comment, name, count)

        params = []
        for i in range(count):
            ts = arg_type(body, i)
            if ts in (None, "object"):
                shape = options_shape(body, i)
                if shape:
                    ts = shape
            if ts is None:
                ts = "any"
                anys += 1
            label = names[i] if names else "arg%d" % i
            optional = "" if i < required else "?"
            params.append("%s%s: %s" % (label, optional, ts))

        ret = return_type(body)
        summary = first_sentence(comment, name)
        if summary:
            out.append("  /** %s */" % summary)
        out.append("  %s(%s): %s;" % (name, ", ".join(params), ret))

    out.append("}")
    out.append("")

    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("wrote %s" % os.path.normpath(OUTPUT))
    print("  %d functions, %d enums, %d parameters left as `any`"
          % (functions, enums, anys))


def first_sentence(comment, name):
    """The C++ signature line from the doc comment, which is the most useful
    single line to keep: it names the real ImGui function behind the wrapper."""
    for line in comment.splitlines():
        line = line.strip()
        if line.startswith("ImGui::"):
            return " ".join(line.split())[:160]
    for line in comment.splitlines():
        line = line.strip()
        if line.startswith(name + "("):
            return " ".join(line.split())[:160]
    return None


if __name__ == "__main__":
    sys.exit(main())
