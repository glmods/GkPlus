"""Blender import/export for Rebellion RIF assets (Gunlok, 2000).

The format work lives in :mod:`rif` (container), :mod:`schema` (chunk bodies as
typed fields) and :mod:`shapes` (geometry), none of which import ``bpy`` -- they
are exercised by ``blender/tests/`` without Blender running. :mod:`scene` is the
Blender adapter; this module is only the operators.

**The scene is the whole file.** Import builds every chunk into a Blender
datablock -- objects, meshes, lights, Actions, and typed properties for the rest --
and export reads nothing but the scene. There is no source-file parameter and no
opaque byte storage, so a ``.blend`` can be moved to a machine that has never seen
the original ``.rif`` and still produce one.

The bar is *semantic* equivalence, not byte equivalence: derived chunks are
regenerated (``SHPCENTR``), authored per-element data becomes mesh attributes
(``SHPMRGDT``, ``SHPVTINT``), and preprocessed render data with no known generator
is dropped (``SHPPCINF``, which 681 of the 9,357 shipped shapes omit anyway).
"""

import os

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import rif
from . import scene as sc


class IMPORT_SCENE_OT_rif(bpy.types.Operator, ImportHelper):
    """Import a Rebellion RIF file"""

    bl_idname = "import_scene.rif"
    bl_label = "Import RIF"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".rif"
    filter_glob: StringProperty(default="*.rif;*.RIF", options={"HIDDEN"})

    scale: FloatProperty(
        name="Scale",
        description="Multiplier from RIF integer units to Blender metres",
        default=sc.DEFAULT_SCALE,
        min=1e-6,
        max=1000.0,
    )
    y_down: BoolProperty(
        name="Y-down source",
        description="Treat RIF coordinates as Y-down and convert to Blender's Z-up",
        default=True,
    )
    load_textures: BoolProperty(
        name="Load textures",
        description=(
            "Decode each .RIM the file names and pack it into the .blend. The "
            "texture names are imported either way - this only decides whether "
            "the images come with them"
        ),
        default=True,
    )
    texture_dir: StringProperty(
        name="Textures",
        description=(
            "The directory a texture name is relative to, normally the install's "
            "Graphics folder. Left empty, it is searched for above the .rif"
        ),
        default="",
        subtype="DIR_PATH",
    )

    def execute(self, context):
        try:
            root = rif.load(self.filepath)
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"},
                        "Could not read %s: %s" % (os.path.basename(self.filepath), exc))
            return {"CANCELLED"}

        name = os.path.basename(self.filepath)
        _collection, stats = sc.build_scene(
            root, name, scale=self.scale, y_down=self.y_down,
            fps=context.scene.render.fps, source_path=self.filepath,
            texture_dir=bpy.path.abspath(self.texture_dir) if self.texture_dir else "",
            load_images=self.load_textures)

        summary = "Imported %s: %d top-level chunks" % (name, stats["objects"])
        if stats.get("textures"):
            summary += "; %d texture(s), %d image(s) loaded" % (stats["textures"],
                                                               stats.get("images", 0))
        if self.load_textures and stats.get("textures") and not stats.get("texture_root"):
            # Otherwise the model just quietly imports untextured and the reason
            # (nothing named Graphics above the file) is invisible.
            self.report(
                {"WARNING"},
                "No textures directory found above %s; import again with one set "
                "in the Textures field to see them" % os.path.basename(self.filepath),
            )
        if stats.get("lod_rigged"):
            # Otherwise these read as mysterious duplicates of the parts they sit on.
            summary += ("; %d level-of-detail part(s) put on their base part's bone"
                        % stats["lod_rigged"])
        self.report({"INFO"}, summary)
        if stats["lost_faces"]:
            self.report(
                {"WARNING"},
                "%d face(s) share their vertices with another and cannot be represented "
                "in Blender; they will not be exported" % stats["lost_faces"],
            )
        unreadable = stats.get("missing_textures", []) + stats.get("undecodable_textures", [])
        if unreadable:
            # Named but not shown: the material and its place in the table are
            # still there, so the export is unaffected either way.
            self.report(
                {"WARNING"},
                "%d texture(s) could not be read (%s); their materials carry the "
                "name but no image" % (len(unreadable), ", ".join(unreadable[:3])),
            )
        return {"FINISHED"}


class EXPORT_SCENE_OT_rif(bpy.types.Operator, ExportHelper):
    """Write a RIF file from the scene"""

    bl_idname = "export_scene.rif"
    bl_label = "Export RIF"
    bl_options = {"REGISTER"}

    filename_ext = ".rif"
    filter_glob: StringProperty(default="*.rif;*.RIF", options={"HIDDEN"})

    collection: StringProperty(
        name="Collection",
        description="Which imported collection to write. Empty uses the only one present",
        default="",
    )

    def execute(self, context):
        candidates = [c for c in bpy.data.collections if c.get("rif_id") == "REBINFF2"]
        if self.collection:
            candidates = [c for c in candidates if c.name == self.collection]

        if not candidates:
            self.report({"ERROR"}, "No imported RIF collection in this scene")
            return {"CANCELLED"}
        if len(candidates) > 1:
            self.report(
                {"ERROR"},
                "%d RIF collections present (%s); name one in the Collection field"
                % (len(candidates), ", ".join(c.name for c in candidates[:4])),
            )
            return {"CANCELLED"}

        try:
            root, stats = sc.rebuild_tree(candidates[0])
            rif.save(self.filepath, root)
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, "Could not write: %s" % exc)
            return {"CANCELLED"}

        summary = ("Wrote %s: %d objects, %d shapes, %d lights, %d texture(s)"
                   % (os.path.basename(self.filepath), stats["objects"], stats["shapes"],
                      stats["lights"], stats["textures"]))
        if stats.get("new_textures"):
            summary += " (%d added)" % stats["new_textures"]
        self.report({"INFO"}, summary)
        return {"FINISHED"}


def _menu_import(self, context):
    self.layout.operator(IMPORT_SCENE_OT_rif.bl_idname, text="Gunlok RIF (.rif)")


def _menu_export(self, context):
    self.layout.operator(EXPORT_SCENE_OT_rif.bl_idname, text="Gunlok RIF (.rif)")


_CLASSES = (IMPORT_SCENE_OT_rif, EXPORT_SCENE_OT_rif)


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
