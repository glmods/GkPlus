# Dumps a GkPlus symbol map from a Ghidra program, for `prof.symbols`.
#
# The sampling profiler in `src/Profiler.cpp` records raw addresses and resolves them to
# `module+0xrva` at read time. gl.exe ships no symbols, but its Ghidra database is heavily named
# - so exporting that database is what makes a sampled profile read in the game's own function
# names instead of hex.
#
# Output is one function per line, sorted by RVA, in the format `src/Profiler.cpp`'s LoadSymbols
# parses:
#
#     <hex rva> <hex size> <name>
#
# with `#` comment lines carrying the image base and the source file's size and timestamp, which
# LoadSymbols compares against the module actually loaded so a stale map is reported rather than
# silently believed.
#
# **RVAs, not absolute addresses** - the profiler subtracts the module's runtime base, so the map
# stays correct under ASLR and does not care what Ghidra's image base was.
#
# Run it from the Script Manager, or headless:
#
#     analyzeHeadless <project dir> <project> -process gl.exe -noanalysis \
#         -scriptPath utils/symdump -postScript gl_symbols.py <output path>
#
# With no argument it writes `<program name>.sym` next to the executable Ghidra imported.
#
# @category GkPlus
# @runtime Jython

import os


def executable_path():
    """Where the imported binary actually lives, or None if Ghidra does not know."""
    try:
        path = currentProgram.getExecutablePath()
    except Exception:
        return None
    if not path:
        return None
    # Ghidra stores a leading-slash form for Windows paths ("/C:/games/gl.exe").
    if len(path) > 2 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return path.replace("/", os.sep)


def output_path():
    args = getScriptArgs()
    if len(args) > 0 and args[0]:
        return args[0]
    exe = executable_path()
    name = currentProgram.getName()
    if exe and os.path.isdir(os.path.dirname(exe)):
        return os.path.join(os.path.dirname(exe), name + ".sym")
    return os.path.join(os.path.expanduser("~"), name + ".sym")


def collect():
    """(rva, size, name) per function, sorted by rva. Names are namespace-qualified."""
    base = currentProgram.getImageBase().getOffset()
    rows = []
    skipped_empty = 0
    # getFunctions(True) iterates the program's INTERNAL functions only - externals are reached
    # through getExternalFunctions() and never enter this loop, so they are counted separately
    # rather than filtered here. (Measured on the gl.exe database: getFunctionCount() 12703,
    # getExternalFunctions() 216, and this loop yields exactly 12487.)
    external = 0
    for _ in currentProgram.getFunctionManager().getExternalFunctions():
        external += 1
    for function in currentProgram.getFunctionManager().getFunctions(True):
        entry = function.getEntryPoint().getOffset()
        # getNumAddresses rather than max-minus-entry: a body Ghidra has split into several
        # ranges would otherwise claim every byte between them, and swallow its neighbours.
        size = function.getBody().getNumAddresses()
        if size <= 0:
            skipped_empty += 1
            continue
        # True asks for the namespace-qualified form, which is what makes `Actor::Update`
        # readable rather than a bare `Update` shared by sixteen classes.
        name = function.getName(True)
        # The format is whitespace-delimited with the name last, so only newlines are fatal -
        # but a stray space in a demangled name would still read oddly. Collapse them.
        name = " ".join(name.split())
        rows.append((entry - base, size, name))
    rows.sort()
    return rows, external, skipped_empty


def main():
    target = output_path()
    rows, external, skipped_empty = collect()
    exe = executable_path()

    size_on_disk = -1
    mtime = -1
    if exe and os.path.isfile(exe):
        stat = os.stat(exe)
        size_on_disk = int(stat.st_size)
        mtime = int(stat.st_mtime)

    handle = open(target, "wb")
    try:
        handle.write("# gkplus symbol map\n")
        handle.write("# module %s\n" % currentProgram.getName())
        handle.write("# image_base 0x%08x\n" % currentProgram.getImageBase().getOffset())
        handle.write("# file_size %d\n" % size_on_disk)
        handle.write("# file_time %d\n" % mtime)
        handle.write("# functions %d\n" % len(rows))
        handle.write("# format: <hex rva> <hex size> <name>\n")
        for rva, size, name in rows:
            handle.write("%x %x %s\n" % (rva, size, name))
    finally:
        handle.close()

    named = 0
    for _, _, name in rows:
        if not name.startswith("FUN_"):
            named += 1
    print("wrote %s" % target)
    print("  %d functions, %d named (%.1f%%), %d still FUN_" %
          (len(rows), named, 100.0 * named / max(1, len(rows)), len(rows) - named))
    print("  %d external functions not exported, %d skipped for an empty body" %
          (external, skipped_empty))
    print("  file_size %d, file_time %d" % (size_on_disk, mtime))


main()
