#!/usr/bin/env python3
"""Turn the shader-side struct declarations into C++ `static_assert`s.

**This exists because of vulkan_renderer_notes.md 4.67, which cost two sections.** Twelve structs
are declared twice - once in `src/shaders/*.slang` and once in C++ - with nothing linking the two.
`GpuFrameData` drifted by one field, and nothing caught it: a permutation preserves `sizeof`, so
`sizeof(GpuFrameData) == 272` still held and so did both `offsetof` asserts, which pin fields
*after* the disturbance. The two symptoms had nothing in common - three knobs silently read as
permanently on (4.67), and the fragment shader indexing a five-entry bindless sampler array at
1,065,353,216, which is a lost device (4.66). Fixing it from one symptom said nothing about the
other.

So: parse the Slang structs, compute every field's byte offset, and emit an `offsetof` assert per
field plus a `sizeof` assert per struct. Any drift is then a compile error naming the field, which
is what the C++ side could never say for itself.

Direction matters and is deliberate. Generating the *Slang* from the C++ header was the obvious
reading, and it is wrong: the shader declares `ConstBufferPointer<GpuMapLight>` where the header
has `uint64_t`, so a mechanical translation would lose the types the shader actually dereferences
through, and it would only ever cover the struct it was written for. Asserts cover all twelve
pairs, including the three structs declared in *two* shader files, and leave both sides their own
documentation - which differs on purpose, the shader's explaining shader semantics.

No shader toolchain is needed: this reads the `.slang` text, so unlike `gen-shaders.py` it can
always run. Three entry points, the same three that script has:

    python3 src/gen-shader-abi.py            # write the header
    python3 src/gen-shader-abi.py --check    # is the checked-in header stale?
    python3 src/gen-shader-abi.py --deps     # the source files, one per line, for CMake

Output is `src/ShaderAbi.gen.inc.h`, included at the end of VkDraw.cpp's anonymous namespace -
the one point where all three of `src/VkDraw.h`, `src/VertexFormat.h` and the file's own three
push blocks are in scope.
"""

import argparse
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = ROOT / "src" / "shaders"
OUTPUT = ROOT / "src" / "ShaderAbi.gen.inc.h"

RECIPE_MARKER = "// recipe-hash: "
SOURCE_MARKER = "// source-hash: "

# Which Slang struct checks which C++ one. **This is the whole configuration**; a struct absent
# from here is shader-internal (VertexOut, LightGeometry, ...) and has no counterpart to drift
# from.
#
# `fields` renames where the two sides decompose the same bytes differently, and it is needed
# exactly once: Slang's `Vertex` packs the normal and the packed D3DCOLOR into one `float4`
# because three float4s land at 0/16/32 under every layout rule there is (see the comment above
# `Vertex` in world.slang), where `CanonicalVertex` spells the four members out. A field mapped
# to None is not checked - there is no single C++ member starting at those bytes.
#
# A struct listed against two shader files is checked twice, which is the point: `GpuDrawRecord`
# and `Vertex` are each declared three times, and shadow.slang's copy carries its own warning
# that getting the stride wrong makes every draw past the first read the wrong matrix.
PAIRS = [
    ("world.slang", "GpuFrameData", "GpuFrameData", {}),
    ("world.slang", "GpuLight", "GpuLight", {}),
    ("world.slang", "GpuDrawRecord", "GpuDrawRecord", {}),
    ("world.slang", "GpuMaterial", "GpuMaterial", {}),
    ("world.slang", "GpuMapLight", "GpuMapLight", {}),
    ("world.slang", "Vertex", "CanonicalVertex",
     {"pos": "pos", "normal_and_color": "normal", "uv": "uv0"}),
    ("world.slang", "Push", "PushConstants", {}),
    ("shadow.slang", "Vertex", "CanonicalVertex",
     {"pos": "pos", "normal_and_color": "normal", "uv": "uv0"}),
    ("shadow.slang", "GpuDrawRecord", "GpuDrawRecord", {}),
    ("shadow.slang", "ShadowPush", "ShadowPushConstants", {}),
    ("lightgrid.slang", "GpuMapLight", "GpuMapLight", {}),
    ("lightgrid.slang", "GridPush", "LightGridPush", {}),
]

# size and the two alignments: (bytes, scalar align, std430 align).
#
# **Both rules are computed and required to agree**, which is not belt-and-braces - it enforces a
# design rule the shader sources already state and rely on. Slang does not say which layout it
# picked for a `ConstBufferPointer<T>`, so world.slang's `Vertex` is three float4s and its
# GpuDrawRecord is float4 arrays specifically so that "the agreement is structural instead of a
# bet". A struct where the rules diverge is one where that reasoning has been broken, and it is
# worth a build failure at the point it happens rather than a wrong pixel later.
# Constants declared on both sides, as (source, slang name, C++ expression). The struct asserts
# above say the two agree about where a field *is*; these say they agree about what is *in* it, and
# it is the same hazard one level down - `kDynShadowFace` disagreeing would put every cube lookup
# in the wrong tile, silently, with no size to preserve and nothing to catch it.
#
# The C++ side is an expression rather than a name because some are derived (`kMapShadowAtlas /
# kMapShadowFace`) and one is a reciprocal the shader spells out. A float constant is compared with
# its C++ value promoted, so 64 and 64.0 agree.
CONSTANTS = [
    ("world.slang", "kMaxShadowCascades", "kMaxShadowCascades"),
    ("world.slang", "kMapShadowFace", "kMapShadowFace"),
    ("world.slang", "kMapShadowTilesPerRow", "kMapShadowTilesPerRow"),
    ("world.slang", "kDynShadowFace", "kDynShadowFace"),
    ("world.slang", "kDynShadowTilesPerRow", "kDynShadowTilesPerRow"),
    ("world.slang", "kNoTexture", "kNoTexture"),
]

SCALARS = {
    "float": (4, 4, 4),
    "uint": (4, 4, 4),
    "int": (4, 4, 4),
    "bool": (4, 4, 4),
}
for _base, (_size, _a, _b) in list(SCALARS.items()):
    # A vector's alignment is its component's under scalar rules and the next power of two up
    # under std430 - so float3 is 4 there and 16 here, which is the classic disagreement.
    SCALARS["%s2" % _base] = (_size * 2, _a, _b * 2)
    SCALARS["%s3" % _base] = (_size * 3, _a, _b * 4)
    SCALARS["%s4" % _base] = (_size * 4, _a, _b * 4)

FIELD_RE = re.compile(r"^\s*([A-Za-z_][\w:]*(?:\s*<[^;]*>)?)\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*;")
CONST_RE = re.compile(r"^\s*static\s+const\s+(?:u?int|float)\s+([A-Za-z_]\w*)\s*=\s*([^;]+);",
                      re.MULTILINE)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def as_int(literal):
    """The value of a Slang integer literal, or None if it is not one.

    Handles the `u` suffix and hex, and returns None for anything with an operator in it -
    `kMapShadowNear` is `1.0 / 64.0`, which is a fine constant and not an array extent.
    """
    text = literal.strip().rstrip("uU")
    try:
        return int(text, 16) if text.lower().startswith("0x") else int(text)
    except ValueError:
        return None


def parse_source(path):
    """{name: [(type, field, extent)]} and {constant: literal} for one .slang file."""
    text = strip_comments(path.read_text(encoding="utf-8"))
    constants = dict((m.group(1), m.group(2).strip()) for m in CONST_RE.finditer(text))
    structs = {}
    for match in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{(.*?)\n\}\s*;", text, flags=re.S):
        fields = []
        for line in match.group(2).splitlines():
            found = FIELD_RE.match(line)
            if found:
                extent = found.group(3)
                fields.append((re.sub(r"\s+", "", found.group(1)), found.group(2),
                               extent[1:-1].strip() if extent else None))
        structs[match.group(1)] = fields
    return structs, constants


def type_layout(kind):
    """(size, scalar align, std430 align) for one field type."""
    if kind.startswith("ConstBufferPointer<"):
        return (8, 8, 8)  # a device address, and 8-aligned under every rule
    if kind in SCALARS:
        return SCALARS[kind]
    sys.exit("gen-shader-abi.py does not know the type %r - add it to SCALARS" % kind)


def layout(fields, constants, std430):
    """[(field, offset)] and the total size, under one of the two rules."""
    offset = 0
    largest = 1
    out = []
    for kind, name, extent in fields:
        size, scalar_align, wide_align = type_layout(kind)
        align = wide_align if std430 else scalar_align
        if extent is not None:
            count = as_int(constants.get(extent, extent))
            if count is None:
                sys.exit("array extent %r is not a literal or a known integer constant" % extent)
            # The array stride is the element size rounded up to the element's own alignment.
            # std140 would round it to 16 as well; nothing here is a uniform block, and a stride
            # that needed it would fail the two-rule agreement below anyway.
            stride = (size + align - 1) // align * align
            size = stride * count
        offset = (offset + align - 1) // align * align
        out.append((name, offset))
        offset += size
        largest = max(largest, align)
    return out, (offset + largest - 1) // largest * largest


def check_pairs(parsed):
    """[(source, slang name, cpp name, [(cpp field, offset)], size)], or exit with the reason."""
    results = []
    for source_name, slang_name, cpp_name, renames in PAIRS:
        if source_name not in parsed:
            parsed[source_name] = parse_source(SHADER_DIR / source_name)
        structs, constants = parsed[source_name]
        if slang_name not in structs:
            sys.exit("%s declares no struct %s - PAIRS is out of date"
                     % (source_name, slang_name))
        fields = structs[slang_name]
        scalar_fields, scalar_size = layout(fields, constants, std430=False)
        wide_fields, wide_size = layout(fields, constants, std430=True)
        if scalar_fields != wide_fields or scalar_size != wide_size:
            differing = [a[0] for a, b in zip(scalar_fields, wide_fields) if a != b]
            sys.exit(
                "%s:%s is laid out differently under scalar and std430 rules (%s), so its C++\n"
                "counterpart can only match one of them. Slang does not say which it picked -\n"
                "reshape the struct so every rule agrees, as world.slang's Vertex and\n"
                "GpuDrawRecord already do."
                % (source_name, slang_name, ", ".join(differing) or "the size"))
        checked = []
        for name, offset in scalar_fields:
            mapped = renames.get(name, name) if renames else name
            if mapped is not None:
                checked.append((mapped, offset))
        results.append((source_name, slang_name, cpp_name, checked, scalar_size))
    return results


def check_constants(parsed):
    """[(source, slang name, cpp expression, literal)] for CONSTANTS."""
    out = []
    for source_name, slang_name, cpp_expr in CONSTANTS:
        if source_name not in parsed:
            parsed[source_name] = parse_source(SHADER_DIR / source_name)
        constants = parsed[source_name][1]
        if slang_name not in constants:
            sys.exit("%s declares no constant %s - CONSTANTS is out of date"
                     % (source_name, slang_name))
        # Slang's `u` suffix is not C++ syntax on a float, and a bare `4u` compared against a
        # `constexpr uint32_t` needs no suffix at all - so it is dropped and the comparison is left
        # to the usual arithmetic conversions. 64 == 64.0 either way.
        out.append((source_name, slang_name, cpp_expr, constants[slang_name].rstrip("uU")))
    return out


def sources():
    names = []
    for source_name, _, _, _ in PAIRS:
        if source_name not in names:
            names.append(source_name)
    for source_name, _, _ in CONSTANTS:
        if source_name not in names:
            names.append(source_name)
    return [SHADER_DIR / name for name in names]


def stamp_lines():
    recipe = hashlib.sha256()
    recipe.update(repr(PAIRS).encode("utf-8"))
    recipe.update(repr(CONSTANTS).encode("utf-8"))
    recipe.update(repr(sorted(SCALARS.items())).encode("utf-8"))
    lines = [RECIPE_MARKER + recipe.hexdigest()]
    for path in sources():
        if not path.exists():
            sys.exit("%s not found" % path)
        lines.append("%s%s %s" % (SOURCE_MARKER, path.relative_to(ROOT).as_posix(),
                                  hashlib.sha256(path.read_bytes()).hexdigest()))
    return lines


def describe_staleness():
    if not OUTPUT.exists():
        return "%s does not exist" % OUTPUT.relative_to(ROOT).as_posix()
    have = [line for line in OUTPUT.read_text(encoding="utf-8").splitlines()
            if line.startswith(RECIPE_MARKER) or line.startswith(SOURCE_MARKER)]
    want = stamp_lines()
    if not have:
        return "%s carries no hashes" % OUTPUT.relative_to(ROOT).as_posix()
    if have == want:
        return None
    if have[:1] != want[:1]:
        return "the struct pairing table or the layout rules have changed"
    have_sources = dict(line[len(SOURCE_MARKER):].split(" ", 1)
                        for line in have if line.startswith(SOURCE_MARKER))
    want_sources = dict(line[len(SOURCE_MARKER):].split(" ", 1)
                        for line in want if line.startswith(SOURCE_MARKER))
    for name, digest in want_sources.items():
        if name not in have_sources:
            return "%s is not in the header at all" % name
        if have_sources[name] != digest:
            return "%s has changed since the header was generated" % name
    for name in have_sources:
        if name not in want_sources:
            return "%s is in the header but is no longer a source" % name
    return "the header's hashes do not match its sources"


def generate():
    parsed = {}
    pairs = check_pairs(parsed)
    constants = check_constants(parsed)
    out = [
        "// Generated by src/gen-shader-abi.py from src/shaders/*.slang. Do not edit.",
        "//",
        "// One `offsetof` per field and one `sizeof` per struct, derived from the SHADER's own",
        "// declaration - so a C++ struct that drifts from the shader it feeds is a compile error",
        "// naming the field. That is the link vulkan_renderer_notes.md 4.67 did not have: a",
        "// permutation preserves the size, and the two hand-written asserts it did have pinned",
        "// fields after the disturbance, so nothing failed until a float's bit pattern reached a",
        "// bindless sampler index and took the device down (4.66).",
        "//",
        "// Included from VkDraw.cpp, inside its anonymous namespace - the one point where",
        "// src/VkDraw.h, src/VertexFormat.h and that file's three push blocks are all in scope.",
    ]
    out += stamp_lines()
    out += ["#pragma once", "", "#include <cstddef>", ""]

    for source_name, slang_name, cpp_name, fields, size in pairs:
        out.append("// src/shaders/%s : struct %s" % (source_name, slang_name))
        for name, offset in fields:
            out.append("static_assert(offsetof(%s, %s) == %d," % (cpp_name, name, offset))
            out.append("              \"%s::%s moved away from %s's %s\");"
                       % (cpp_name, name, source_name, slang_name))
        out.append("static_assert(sizeof(%s) == %d," % (cpp_name, size))
        out.append("              \"%s is not the size %s declares\");" % (cpp_name, slang_name))
        out.append("")
        print("%-16s %-16s -> %-20s %2d fields, %3d bytes"
              % (source_name, slang_name, cpp_name, len(fields), size))

    # The constants, which the struct asserts cannot reach: where a field IS, against what is in
    # it. `kDynShadowFace` disagreeing would put every cube lookup in the wrong tile with no size
    # to preserve and nothing to catch it.
    out.append("// Constants declared on both sides")
    for source_name, slang_name, cpp_expr, literal in constants:
        out.append("static_assert(%s == %s," % (cpp_expr, literal))
        out.append("              \"%s disagrees with %s's %s\");"
                   % (cpp_expr, source_name, slang_name))
        print("%-16s %-16s -> %-20s == %s" % (source_name, slang_name, cpp_expr, literal))
    out.append("")

    text = "\n".join(out)
    relative = OUTPUT.relative_to(ROOT).as_posix()
    if OUTPUT.exists() and OUTPUT.read_text(encoding="utf-8") == text:
        print("%s unchanged" % relative)
    else:
        OUTPUT.write_text(text, encoding="utf-8")
        print("wrote %s" % relative)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true",
                       help="exit non-zero if the checked-in header is stale")
    group.add_argument("--deps", action="store_true",
                       help="print the shader sources, one absolute path per line")
    args = parser.parse_args()

    if args.deps:
        for path in sources():
            print(path.as_posix())
        return
    if args.check:
        reason = describe_staleness()
        if reason is None:
            return
        sys.exit("%s is out of date: %s.\nRun:  python3 src/gen-shader-abi.py"
                 % (OUTPUT.relative_to(ROOT).as_posix(), reason))
    generate()


if __name__ == "__main__":
    main()
