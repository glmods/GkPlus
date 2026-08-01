#!/usr/bin/env python3
"""Generate the X-macro method lists for the D3D8 capture device.

`d3d8to9` ships its own `d3d8.hpp` with the COM interfaces declared as plain pure-virtual
classes, which is where these come from - GkPlus does not depend on the DX8 SDK. The file is
perfectly regular (one `virtual <ret> STDMETHODCALLTYPE <name>(<params>) = 0;` per line, no
array parameters, no templates), so parsing it is a dozen lines rather than a C++ parser.

Re-run after a `d3d8to9` version bump:

    python3 src/gen-d3d8-forwarders.py

Output is `src/D3D8Device.gen.inc.h`, which defines two X-macros per interface:

    GK_D3D8_<IFACE>_METHODS(X)    every method  -> the class declarations
    GK_D3D8_<IFACE>_FORWARDED(X)  the ones we do not intercept -> the forwarding bodies

`X` is invoked as `X(ret, Name, (params), (args))`. `IUnknown`'s three slots are deliberately
absent: reference counting is the capture device's own, never forwarded.

To start intercepting a method, add its name to INTERCEPTED below and write the body by hand -
it drops out of the FORWARDED list, so a missing definition is a link error rather than a
silently-still-forwarding method.
"""

import pathlib
import re
import sys

HEADER = pathlib.Path(
    "build/vcpkg_installed/x86-windows-static-md/include/d3d8.hpp"
)
OUTPUT = pathlib.Path("src/D3D8Device.gen.inc.h")

# Interfaces we implement ourselves. Everything else in d3d8.hpp is passed through as the
# d3d8to9 object it already is.
#
# The buffers are wrapped so their *destruction* is visible: CreateVertexBuffer tells us how
# much was allocated, but only Release reaching zero says how much is live, and live is what
# sizes the Vulkan arena. Textures are wrapped because LockRect is the only place their
# pixels exist in a form this layer can read.
#
# IDirect3DSurface8 is wrapped to answer one question: GetSurfaceLevel runs exactly twice per
# texture LockRect (notes section 4.11), which is equally consistent with a harmless query and
# with a second write path invisible here. Its own LockRect counter is what tells them apart.
INTERFACES = ["IDirect3D8", "IDirect3DDevice8", "IDirect3DVertexBuffer8",
              "IDirect3DIndexBuffer8", "IDirect3DTexture8", "IDirect3DSurface8"]

# A COM vtable is base methods then derived methods, so a wrapper has to declare and forward
# the base's slots too or every index past the base is wrong. d3d8.hpp spells the inheritance
# in the interface declaration; this is that, made explicit.
BASES = {
    "IDirect3DVertexBuffer8": ["IDirect3DResource8"],
    "IDirect3DIndexBuffer8": ["IDirect3DResource8"],
    "IDirect3DTexture8": ["IDirect3DResource8", "IDirect3DBaseTexture8"],
}

# Methods with a hand-written body in src/D3D8Capture.cpp. Keep each list in the order it
# appears in the interface so a diff against the header stays readable.
INTERCEPTED = {
    "IDirect3D8": [
        # The whole reason IDirect3D8 is wrapped at all: it is where the device is minted.
        "CreateDevice",
    ],
    # Lock is where the geometry actually arrives, and Unlock is where it is complete. Phase
    # 2b only measures the volume; Phase 2c uploads it.
    #
    # GetDevice appears on every resource and must hand back the capture device, not the
    # d3d8to9 one - see check_wrapped_params.
    "IDirect3DVertexBuffer8": ["GetDevice", "Lock", "Unlock"],
    "IDirect3DIndexBuffer8": ["GetDevice", "Lock", "Unlock"],
    # LockRect is where the pixels arrive; GetSurfaceLevel is the other way in, and now hands
    # back a wrapped surface so that route is visible too.
    "IDirect3DTexture8": ["GetDevice", "LockRect", "UnlockRect", "GetSurfaceLevel"],
    # The whole point of this interface: its own LockRect is the counter that says whether
    # IDirect3DTexture8::LockRect sees every pixel (notes section 4.11).
    #
    # GetContainer is here for a reason the checker cannot see: it returns the containing
    # texture through a `void **`, so no parameter names a wrapped interface and the scan
    # above waves it through. Handing back the inner texture would leak an unwrapped object
    # into the game exactly the way a forwarded GetDevice does.
    "IDirect3DSurface8": ["GetDevice", "GetContainer", "LockRect", "UnlockRect"],
    "IDirect3DDevice8": [
        # Hands back an IDirect3D8; must hand back OUR wrapper, or a second device created
        # through it would be built by the unwrapped d3d8to9 object and never be seen here.
        "GetDirect3D",
        # Resource creation - where geometry and textures enter the renderer.
        "CreateTexture",
        "CreateVertexBuffer",
        "CreateIndexBuffer",
        # Take or return a texture, so they unwrap on the way in and re-wrap on the way out.
        "SetTexture",
        "GetTexture",
        "UpdateTexture",
        # Everything that takes or returns a surface. All ten were enumerated by
        # check_wrapped_params rather than by reading the header, which is what the check is
        # for; `SetCursorProperties` in particular does not look like a resource call.
        "SetCursorProperties",
        "CreateRenderTarget",
        "CreateDepthStencilSurface",
        "CreateImageSurface",
        "CopyRects",
        "GetFrontBuffer",
        "GetBackBuffer",
        "SetRenderTarget",
        "GetRenderTarget",
        "GetDepthStencilSurface",
        # Frame boundaries.
        "BeginScene",
        "EndScene",
        "Present",
        "Reset",
        "Clear",
        # Fixed-function state. This is the set the recorder snapshots per draw; see
        # vulkan_renderer_notes.md section 1.
        "SetTransform",
        "SetViewport",
        "SetMaterial",
        "SetLight",
        "LightEnable",
        "SetRenderState",
        "SetTextureStageState",
        # Everything that takes or returns a wrapped buffer. check_wrapped_params() below
        # enforces that this list is complete - ProcessVertices was missed twice by reading,
        # and cost an access violation inside d3d9.dll to find.
        "SetStreamSource",
        "GetStreamSource",
        "SetIndices",
        "GetIndices",
        "ProcessVertices",
        "SetVertexShader",
        # State blocks. Measured, not anticipated: 873,200 steady-state draws went past a
        # recorder watching only the individual setters and produced 11 render states, 2
        # stage states and a max texture stage of 0 - impossible for geometry with two UV
        # sets. AwMaterial::state_block (+0x30) is where the material state actually lives,
        # and ApplyStateBlock sets it without touching any Set* method.
        "BeginStateBlock",
        "EndStateBlock",
        "ApplyStateBlock",
        "CaptureStateBlock",
        "DeleteStateBlock",
        "CreateStateBlock",
        # The four draws. rendering_notes.md section 4.1: these are the total funnel, and the
        # only four call sites in the whole binary reach them.
        "DrawPrimitive",
        "DrawIndexedPrimitive",
        "DrawPrimitiveUP",
        "DrawIndexedPrimitiveUP",
    ],
}

METHOD_RE = re.compile(
    r"^\s*virtual\s+(?P<ret>.+?)\s+STDMETHODCALLTYPE\s+"
    r"(?P<name>\w+)\((?P<params>.*)\)\s*=\s*0;"
)


def parse_interface(text, name):
    """Return [(ret, name, params, args)] for one interface block."""
    start = text.index("interface %s : public" % name)
    end = text.index("\n};", start)
    methods = []
    for line in text[start:end].splitlines():
        m = METHOD_RE.match(line)
        if not m:
            continue
        params = m.group("params").strip()
        args = []
        if params:
            for p in params.split(","):
                # The argument name is the trailing identifier; `*` and `&` bind left.
                args.append(re.findall(r"(\w+)\s*$", p.strip())[0])
        methods.append((m.group("ret"), m.group("name"), params, ", ".join(args)))
    return methods


def check_wrapped_params(iface, methods, intercepted):
    """Every method mentioning a wrapped interface must be hand-written.

    A forwarded one hands our wrapper straight to d3d8to9, which static_casts it to its own
    concrete class and reads a garbage proxy pointer - surfacing as an access violation deep
    inside d3d9.dll with nothing pointing back here. That is exactly how `ProcessVertices`
    was missed: it takes an IDirect3DVertexBuffer8 among five parameters and looks nothing
    like a resource call.

    So the omission is made impossible rather than merely fixed. EVERY wrapped interface
    counts, including IDirect3D8 and IDirect3DDevice8: an earlier revision excluded those two
    on the grounds that they "never appear as another method's parameter", which was wrong -
    every resource has a `GetDevice(IDirect3DDevice8 **)`, and forwarding it hands the game
    the unwrapped d3d8to9 device, whose every subsequent call is invisible here. That is the
    ProcessVertices failure in its quiet form: no crash, just capture that silently stops.

    Their BASES count too, and that is not a detail: `SetTexture` takes an
    IDirect3DBaseTexture8, so a wrapped IDirect3DTexture8 reaches it as a base pointer. A
    check that matched only the exact wrapped names would wave all four texture methods
    through - the same blind spot that let ProcessVertices past, one level up the hierarchy.
    """
    wrapped = list(INTERFACES)
    for iface_name in list(wrapped):
        wrapped += BASES.get(iface_name, [])
    missing = []
    for _ret, name, params, _args in methods:
        if name in intercepted:
            continue
        if any(w in params for w in wrapped):
            missing.append("%s(%s)" % (name, params))
    if missing:
        sys.exit(
            "%s: these take or return a wrapped interface but are not in INTERCEPTED, so "
            "they would forward a wrapper to d3d8to9:\n  %s"
            % (iface, "\n  ".join(missing)))


def emit(out, iface, methods, intercepted):
    slug = "GK_" + iface.upper()
    unknown = set(intercepted)
    for macro, keep in (("METHODS", None), ("FORWARDED", intercepted)):
        out.append("// %s: %s" % (iface, "all methods" if keep is None
                                  else "pass-through only"))
        out.append("#define %s_%s(X) \\" % (slug, macro))
        rows = [m for m in methods if keep is None or m[1] not in keep]
        for i, (ret, name, params, args) in enumerate(rows):
            unknown.discard(name)
            out.append("  X(%s, %s, (%s), (%s))%s"
                       % (ret, name, params, args,
                          " \\" if i + 1 < len(rows) else ""))
        out.append("")
    if unknown:
        sys.exit("%s: INTERCEPTED names not found in the header: %s"
                 % (iface, ", ".join(sorted(unknown))))


def main():
    if not HEADER.exists():
        sys.exit("%s not found - configure the build first (cmake --preset builtin-vcpkg)"
                 % HEADER)
    text = HEADER.read_text(encoding="utf-8")

    out = [
        "// Generated by src/gen-d3d8-forwarders.py from d3d8to9's d3d8.hpp. Do not edit.",
        "//",
        "// X is invoked as X(ret, Name, (params), (args)). See the generator's docstring.",
        "#pragma once",
        "",
    ]
    total = 0
    for iface in INTERFACES:
        # Base methods first: that is the vtable order, and getting it wrong shifts every
        # derived slot.
        methods = []
        for base in BASES.get(iface, []):
            methods += parse_interface(text, base)
        methods += parse_interface(text, iface)
        if not methods:
            sys.exit("no methods parsed for %s - has d3d8.hpp changed shape?" % iface)
        check_wrapped_params(iface, methods, INTERCEPTED.get(iface, []))
        emit(out, iface, methods, INTERCEPTED.get(iface, []))
        total += len(methods)
        print("%-20s %3d methods, %2d intercepted, %2d forwarded"
              % (iface, len(methods), len(INTERCEPTED.get(iface, [])),
                 len(methods) - len(INTERCEPTED.get(iface, []))))

    OUTPUT.write_text("\n".join(out), encoding="utf-8")
    print("wrote %s (%d methods total)" % (OUTPUT, total))


if __name__ == "__main__":
    main()
