#!/usr/bin/env python3
"""Checks that `types/gk.d.ts` declares exactly what `src/Js*.cpp` exposes.

`tsc` proves the declarations are *self-consistent* and that the shipped scripts
type-check against them. It cannot prove they match the bindings, because the
bindings are C++ - so a member added to a `JSCFunctionListEntry` table and not to
the `.d.ts` was invisible, and so was the reverse. Both happen: the reverse is
what left `units.remove_trigger` declared for a while after the C++ entry was
deleted, and the forward case is how `render` accumulated 50 undeclared members
behind an index signature.

Run it from the repo root:

    python3 types/check-surface.py

Exit status is 0 when every pair agrees. It is a *name* check only - nothing here
looks at types, which is `tsc`'s job.
"""

import io
import re
import sys

# (source file, C++ table(s), .d.ts interface).
#
# Several names joined with `+` are checked as one set, which is how a surface
# that is a prototype *chain* in C++ but several mixin interfaces in the .d.ts
# gets compared - see UNIONS for the other half of that. An interface that is
# deliberately open belongs in OPEN_INTERFACES, and a single justified difference
# in EXPECTED. Leaving a namespace out of this list is never the right answer:
# "not checked" would then be the silent default.
PAIRS = [
    ("src/JsCamera.cpp", "CameraProps", "Camera"),
    ("src/JsConsole.cpp", "ConsoleProps", "Console"),
    ("src/JsGame.cpp", "GameProps", "Game"),
    ("src/JsWorld.cpp", "WorldProps", "World"),
    ("src/JsText.cpp", "TextProps", "Text"),
    ("src/JsRepl.cpp", "ReplProps", "Repl"),
    ("src/JsCommands.cpp", "FxProps", "Fx"),
    ("src/JsCommands.cpp", "LightProps", "Light"),
    ("src/JsCommands.cpp", "ObjectiveProps", "Objectives"),
    ("src/JsCommands.cpp", "MusicProps", "Music"),
    ("src/JsCommands.cpp", "ScreenProps", "Screen"),
    ("src/JsCommands.cpp", "UnitsProps", "Units"),
    ("src/JsCommands.cpp", "InventoryProps", "Inventory"),
    ("src/JsCommands.cpp", "TrackProps", "Tracks"),
    ("src/JsCommands.cpp", "DemoProps", "Demo"),
    ("src/JsCommands.cpp", "ScriptProps", "ScriptPacing"),
    ("src/JsTriggers.cpp", "TriggersProps", "Triggers"),
    ("src/JsTriggers.cpp", "TriggerProto", "Trigger"),
    ("src/JsRoles.cpp", "RoleProto", "Role"),
    # The Actor surface is one C++ prototype but several .d.ts interfaces: TS
    # cannot narrow an inherited property to a different `kind` literal, so the
    # members are composed from mixins. UNIONS below is what puts them back
    # together - leaving Actor unchecked would exempt the largest namespace.
    ("src/JsActors.cpp", "ActorProto+MobileActorProto+CharacterActorProto+"
                         "TurretActorProto+PickupActorProto", "ActorBase"),
    ("src/JsRender.cpp", "RenderProps", "Render"),
    # `render`'s families, one sub-object each. The group itself is declared on
    # `Render` and built by NewRenderNamespace, so each name is an EXPECTED
    # difference below rather than a missing binding.
    ("src/JsRender.cpp", "RenderTessProps", "RenderTess"),
    ("src/JsRender.cpp", "RenderHdrProps", "RenderHdr"),
    ("src/JsRender.cpp", "RenderBloomProps", "RenderBloom"),
    ("src/JsRender.cpp", "RenderAoProps", "RenderAo"),
    ("src/JsRender.cpp", "RenderSunShadowProps", "RenderSunShadow"),
    ("src/JsRender.cpp", "RenderMapShadowProps", "RenderMapShadow"),
    ("src/JsRender.cpp", "RenderMapLightProps", "RenderMapLight"),
    ("src/JsRender.cpp", "RenderLocalLightProps", "RenderLocalLight"),
    ("src/JsRender.cpp", "RenderDynamicShadowProps", "RenderDynamicShadow"),
    ("src/JsRender.cpp", "RenderLightingMapProps", "RenderLightingMap"),
    ("src/JsRender.cpp", "RenderMaterialProps", "RenderMaterial"),

    ("src/JsRender.cpp", "RenderDebugProps", "RenderDebug"),
    ("src/JsMake.cpp", "MakeProps", "Make"),
    ("src/JsGls.cpp", "GlsProps", "Gls"),
    ("src/JsProf.cpp", "ProfProps", "Prof"),
]

# Differences that are correct, with the reason. Anything not listed here is a
# finding.
EXPECTED = {
    # Built by NewRenderNamespace rather than sitting in a table.
    ("Render", "dts", "debug"),
    ("Render", "dts", "tess"),
    ("Render", "dts", "hdr"),
    ("Render", "dts", "bloom"),
    ("Render", "dts", "ao"),
    ("Render", "dts", "sun_shadow"),
    ("Render", "dts", "map_shadow"),
    ("Render", "dts", "map_light"),
    ("Render", "dts", "local_light"),
    ("Render", "dts", "dynamic_shadow"),
    ("Render", "dts", "lighting_map"),
    ("Render", "dts", "material"),
}

# An interface that is deliberately open: its C++-only members are reachable
# through an index signature, so they need no declaration.
OPEN_INTERFACES = {"RenderDebug"}

# An interface whose declared members are spread over several. The check runs
# against the union.
UNIONS = {
    "ActorBase": ["ActorBase", "MobileMembers", "CharacterMembers",
                  "TurretMembers", "PickupMembers"],
}

MEMBER_RE = re.compile(
    r"""^\s{4}
        (?:readonly\s+)?
        (?:(?:get|set)\s+)?          # accessor pairs are declared this way
        ([a-z_][A-Za-z0-9_]*)
        \s*\??\s*[:(<]""",
    re.M | re.X,
)

ENTRY_RE = re.compile(
    r'JS_C(?:GETSET|FUNC)(?:_MAGIC)?_DEF2?\("([A-Za-z0-9_]+)"'
    r'|JS_(?:OBJECT|PROP_INT32)_DEF\("([A-Za-z0-9_]+)"'
)


def cpp_tables(path):
    src = io.open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"(?:const\s+)?JSCFunctionListEntry\s+(\w+)\[\]\s*=\s*\{", src):
        body = src[m.end() : src.index("\n};", m.end())]
        out[m.group(1)] = {a or b for a, b in ENTRY_RE.findall(body)}
    return out


def dts_interface(dts, name):
    key = "  export interface %s {" % name
    if key not in dts:
        return None
    body = dts[dts.index(key) + len(key) :]
    return set(MEMBER_RE.findall(body[: body.index("\n  }\n")]))


def main():
    dts = io.open("types/gk.d.ts", encoding="utf-8").read()
    cache = {}
    problems = 0

    for path, table, iface in PAIRS:
        if path not in cache:
            cache[path] = cpp_tables(path)
        # A `+`-joined table name is a union, for the surfaces that are one
        # prototype chain in C++ and several interfaces in the .d.ts.
        cpp = set()
        for part in table.split("+"):
            members = cache[path].get(part)
            if members is None:
                cpp = None
                break
            cpp |= members
        declared = set()
        for part in UNIONS.get(iface, [iface]):
            members = dts_interface(dts, part)
            if members is None:
                declared = None
                break
            declared |= members
        if cpp is None:
            print("%s: no C++ table named %s" % (path, table))
            problems += 1
            continue
        if declared is None:
            print("types/gk.d.ts: no interface named %s" % iface)
            problems += 1
            continue

        missing = {n for n in cpp - declared if (iface, "cpp", n) not in EXPECTED}
        extra = {n for n in declared - cpp if (iface, "dts", n) not in EXPECTED}
        if iface in OPEN_INTERFACES:
            missing = set()

        for name in sorted(missing):
            print("%s.%s is in %s but not declared" % (iface.lower(), name, table))
            problems += 1
        for name in sorted(extra):
            print("%s.%s is declared but not in %s" % (iface.lower(), name, table))
            problems += 1

    if problems:
        print("\n%d mismatch(es)." % problems)
        return 1
    print("gk.d.ts matches the bindings: %d namespaces checked." % len(PAIRS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
