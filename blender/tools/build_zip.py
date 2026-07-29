"""Build the installable extension zip.

    uv run tools/build_zip.py [-o DIR]

Blender wants a zip whose single top-level directory is the extension id, holding
``blender_manifest.toml`` beside the modules. Only the files the addon actually
needs go in -- no tests, no ``__pycache__``, no ``pyproject.toml``.

``blender_manifest.toml`` is the source of truth for the addon's version, because
that is what Blender reads. This script fails if ``pyproject.toml`` disagrees,
which is the only thing keeping the two from drifting.
"""

import argparse
import sys
import zipfile
from pathlib import Path

try:  # 3.11+
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib

ROOT = Path(__file__).resolve().parent.parent
ADDON = ROOT / "io_scene_rif"

#: Everything that belongs in the installable extension. Listed rather than
#: globbed so a stray file in the addon directory cannot silently ship.
PAYLOAD = (
    "blender_manifest.toml",
    "__init__.py",
    "bmpnames.py",
    "rif.py",
    "rim.py",
    "schema.py",
    "scene.py",
    "shapes.py",
)


def read_toml(path):
    with path.open("rb") as fh:
        return tomllib.load(fh)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--outdir", type=Path, default=ROOT / "dist")
    args = parser.parse_args(argv)

    manifest = read_toml(ADDON / "blender_manifest.toml")
    project = read_toml(ROOT / "pyproject.toml")["project"]

    addon_id = manifest["id"]
    version = manifest["version"]
    if project["version"] != version:
        print(
            "version mismatch: blender_manifest.toml has %s, pyproject.toml has %s"
            % (version, project["version"]),
            file=sys.stderr,
        )
        return 1

    missing = [name for name in PAYLOAD if not (ADDON / name).is_file()]
    if missing:
        print("missing from %s: %s" % (ADDON, ", ".join(missing)), file=sys.stderr)
        return 1

    args.outdir.mkdir(parents=True, exist_ok=True)
    out = args.outdir / ("%s-%s.zip" % (addon_id, version))

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in PAYLOAD:
            zf.write(ADDON / name, "%s/%s" % (addon_id, name))

    print("%s  (%d files, %d bytes)" % (out, len(PAYLOAD), out.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
