#!/usr/bin/env python3
"""Compile src/shaders/*.slang to SPIR-V and embed the result in a header.

Slang rather than GLSL, and it is worth saying why beyond taste: it is Khronos's own shading
language now, it ships in the Vulkan SDK, and it gives this project two things GLSL does not -
generics and interfaces for the übershader's stage ops, and one file per pass holding every
entry point, so a push constant block shared across stages cannot drift between two files.

Shaders are compiled OFFLINE and the generated header is checked in, so `d3d8.dll` depends on
no shader toolchain at runtime - the same reasoning that makes the renderer reach Vulkan through
volk rather than the loader's import library. GkPlus *is* `d3d8.dll`, and a missing build-time
tool must not stop the game launching.

CMake drives this now, and the reason is a defect it caused twice: for as long as nothing in the
build ran the generator, editing a shader and running `cmake --build` **reported success having
changed nothing** (vulkan_renderer_notes.md 4.46 - a real fix that measured as having no effect,
whose only tell was that the screenshots were byte-identical). Three entry points:

    python3 src/gen-shaders.py            # compile and write the header
    python3 src/gen-shaders.py --check    # is the checked-in header stale? no slangc needed
    python3 src/gen-shaders.py --deps     # the source files, one per line, for CMake

`--check` is the half that matters on a machine with no Vulkan SDK: the build cannot regenerate
the header there, so it must refuse to use a stale one rather than silently succeed. Staleness is
a content **hash**, embedded in the header and recomputed here - not a timestamp, because a git
checkout shuffles mtimes and would report stale on a clean tree and fresh after a branch switch
that changed a shader.

Output is `src/Shaders.gen.inc.h`, one `const uint32_t kXxxSpv[]` per entry point.
"""

import argparse
import glob
import hashlib
import pathlib
import subprocess
import sys

# Resolved from this file rather than from the working directory, because CMake invokes the
# script from the build tree. `python3 src/gen-shaders.py` from the repo root is unchanged.
ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = ROOT / "src" / "shaders"
OUTPUT = ROOT / "src" / "Shaders.gen.inc.h"

# One SPIR-V module per entry point rather than one multi-entry module: VkPipelineShaderStage
# names an entry point, so either shape works, but separate modules keep the C++ side from
# having to know which names live in which blob.
#
# `-fvk-use-entrypoint-name` stops Slang renaming every entry to "main", which matters because
# the C++ passes the name through verbatim.
#
# THIS LIST IS THE SINGLE SOURCE OF TRUTH for which files the build depends on - `--deps`
# derives that from here, so CMakeLists.txt never names a shader and the two cannot drift.
ENTRY_POINTS = [
    ("world.slang", "vertex_main", "vertex"),
    ("world.slang", "fragment_main", "fragment"),
    # The PN-triangle amplification pass (§4.71). A vertex stage of its own rather than a reuse of
    # `vertex_main`: a tessellated pipeline's vertex shader outputs a control point, not a
    # VertexOut, so leaving `vertex_main` untouched is what makes the untessellated path
    # bit-identical by construction rather than by inspection.
    ("world.slang", "tess_vertex_main", "vertex"),
    ("world.slang", "hull_main", "hull"),
    ("world.slang", "domain_main", "domain"),
    # The first compute entry point here. The `stage` string goes straight to `slangc -stage`, so
    # this list is all it took - nothing else in this script is stage-aware.
    ("lightgrid.slang", "build_grid", "compute"),
    ("shadow.slang", "shadow_vertex", "vertex"),
    # The same pass driven by vkCmdDrawIndexedIndirect - the map lights' bake, which needs the
    # record out of a buffer rather than out of the push (§4.62).
    ("shadow.slang", "map_shadow_vertex", "vertex"),
    # The tessellated twins (§4.71). Two vertex stages, because that is the only place the direct
    # and indirect paths differ - one hull and one domain then serve all four shadow pipelines.
    ("shadow.slang", "shadow_tess_vertex", "vertex"),
    ("shadow.slang", "map_shadow_tess_vertex", "vertex"),
    ("shadow.slang", "shadow_hull", "hull"),
    ("shadow.slang", "shadow_domain", "domain"),
    # Screen-space ambient occlusion (§4.86). Two passes and therefore four entry points: a
    # prepass that writes world position and normal per pixel, and a full-screen resolve that
    # walks one fixed disc over the first's output.
    ("ao.slang", "ao_prepass_vertex", "vertex"),
    ("ao.slang", "ao_prepass_fragment", "fragment"),
    ("ao.slang", "ao_fullscreen_vertex", "vertex"),
    ("ao.slang", "ao_resolve_fragment", "fragment"),
]

SLANGC_ARGS = [
    "-target", "spirv",
    "-profile", "spirv_1_5",
    "-matrix-layout-row-major",
    "-fvk-use-entrypoint-name",
    "-O2",
]

# The hashes `--check` reads back. They are comments, so they cost the C++ side nothing.
RECIPE_MARKER = "// recipe-hash: "
SOURCE_MARKER = "// source-hash: "


def find_slangc(required=True):
    from shutil import which
    found = which("slangc")
    if found:
        return found
    candidates = sorted(glob.glob("C:/VulkanSDK/*/Bin/slangc.exe"))
    if candidates:
        return candidates[-1]
    if required:
        sys.exit("slangc not found - install the Vulkan SDK or put slangc on PATH")
    return None


def sources():
    """The distinct .slang files ENTRY_POINTS names, in first-mention order."""
    names = []
    for source_name, _, _ in ENTRY_POINTS:
        if source_name not in names:
            names.append(source_name)
    return [SHADER_DIR / name for name in names]


def stamp_lines():
    """The hash comments describing what the header should have been generated from.

    The recipe is hashed as well as the sources, so adding an entry point or changing a
    slangc flag marks the header stale even though no shader file moved.
    """
    recipe = hashlib.sha256()
    recipe.update(repr(ENTRY_POINTS).encode("utf-8"))
    recipe.update(repr(SLANGC_ARGS).encode("utf-8"))
    lines = [RECIPE_MARKER + recipe.hexdigest()]
    for path in sources():
        if not path.exists():
            sys.exit("%s not found" % path)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append("%s%s %s"
                     % (SOURCE_MARKER, path.relative_to(ROOT).as_posix(), digest))
    return lines


def describe_staleness():
    """None if the checked-in header matches the sources, else why it does not."""
    if not OUTPUT.exists():
        return "%s does not exist" % OUTPUT.relative_to(ROOT).as_posix()
    text = OUTPUT.read_text(encoding="utf-8")
    have = [line for line in text.splitlines()
            if line.startswith(RECIPE_MARKER) or line.startswith(SOURCE_MARKER)]
    want = stamp_lines()
    if not have:
        return ("%s carries no hashes - it predates this check and must be regenerated once"
                % OUTPUT.relative_to(ROOT).as_posix())
    if have == want:
        return None
    have_recipe = [line for line in have if line.startswith(RECIPE_MARKER)]
    if have_recipe != want[:1]:
        return "the entry-point list or the slangc flags have changed"
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


def symbol(entry):
    """vertex_main -> kVertexMainSpv"""
    return "k" + "".join(part.capitalize() for part in entry.split("_")) + "Spv"


def compile_all():
    slangc = find_slangc()
    out = [
        "// Generated by src/gen-shaders.py from src/shaders/*.slang. Do not edit.",
        "//",
        "// SPIR-V is embedded rather than compiled at runtime so that d3d8.dll depends on no",
        "// shader toolchain. The hashes below are what `--check` reads to decide whether this",
        "// file is stale, so the build can refuse a stale one where it cannot regenerate it.",
    ]
    out += stamp_lines()
    out += [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
    ]

    for source_name, entry, stage in ENTRY_POINTS:
        source = SHADER_DIR / source_name
        if not source.exists():
            sys.exit("%s not found" % source)
        output = SHADER_DIR / ("%s.%s.spv" % (source.stem, entry))
        result = subprocess.run(
            [slangc, str(source), "-entry", entry, "-stage", stage] + SLANGC_ARGS +
            ["-o", str(output)],
            capture_output=True)
        if result.returncode != 0:
            sys.exit("%s:%s failed to compile:\n%s"
                     % (source_name, entry,
                        (result.stderr + result.stdout).decode("utf-8", "replace")))
        blob = output.read_bytes()
        output.unlink()
        if len(blob) % 4 != 0:
            sys.exit("%s:%s produced %d bytes, not a whole number of SPIR-V words"
                     % (source_name, entry, len(blob)))
        words = ["0x%08x" % int.from_bytes(blob[i:i + 4], "little")
                 for i in range(0, len(blob), 4)]
        out.append("// %s : %s (%s, %d words)"
                   % (source.relative_to(ROOT).as_posix(), entry, stage, len(words)))
        out.append("inline const uint32_t %s[] = {" % symbol(entry))
        for i in range(0, len(words), 8):
            out.append("    " + ", ".join(words[i:i + 8]) + ",")
        out.append("};")
        out.append("")
        print("%-22s %-16s %5d words" % (source_name, entry, len(words)))

    # Write only on a real change. CMake's rule stamps a file in the build tree instead of
    # depending on this one, so an unchanged header keeps its mtime and every TU that includes
    # it stays out of the rebuild.
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
                       help="exit non-zero if the checked-in header is stale (no slangc needed)")
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
        hint = ("" if find_slangc(required=False) else
                "\nslangc was not found, so this build cannot regenerate it for you - install "
                "the\nVulkan SDK or put slangc on PATH.")
        sys.exit("%s is out of date: %s.\nRun:  python3 src/gen-shaders.py%s"
                 % (OUTPUT.relative_to(ROOT).as_posix(), reason, hint))

    compile_all()


if __name__ == "__main__":
    main()
