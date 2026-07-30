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
from bpy.props import (BoolProperty, EnumProperty, FloatProperty, IntProperty,
                       StringProperty)
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import heads
from . import rif
from . import scene as sc


def _sole_collection(name=""):
    """``(collection, error)`` for the RIF this operator should act on."""
    candidates = sc.rif_collections()
    if name:
        candidates = [c for c in candidates if c.name == name]
    if not candidates:
        return None, "No RIF collection in this scene (File > New > Gunlok RIF)"
    if len(candidates) > 1:
        return None, ("%d RIF collections present (%s); name one in the Collection field"
                      % (len(candidates), ", ".join(c.name for c in candidates[:4])))
    return candidates[0], None


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
        target, error = _sole_collection(self.collection)
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}

        # Two objects claiming one shape id is not representable: re-import pairs
        # both with the first shape and the second one's geometry is orphaned.
        # It is what duplicating an object in Blender produces, so it is checked
        # rather than left to be discovered in the game.
        clashes = {sid: names for sid, names in sc.shape_id_users(target).items()
                   if len(names) > 1}
        if clashes:
            sid, names = sorted(clashes.items())[0]
            self.report(
                {"ERROR"},
                "shape id %d is claimed by %d objects (%s); give each one a fresh "
                "id in Object Properties > Gunlok RIF" % (sid, len(names),
                                                          ", ".join(sorted(names)[:3])),
            )
            return {"CANCELLED"}

        try:
            root, stats = sc.rebuild_tree(target)
            rif.save(self.filepath, root)
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, "Could not write: %s" % exc)
            return {"CANCELLED"}

        summary = ("Wrote %s: %d objects, %d shapes, %d lights, %d texture(s)"
                   % (os.path.basename(self.filepath), stats["objects"], stats["shapes"],
                      stats["lights"], stats["textures"]))
        if stats.get("new_textures"):
            summary += " (%d added)" % stats["new_textures"]
        if stats.get("sounds"):
            summary += ", %d sound(s)" % stats["sounds"]
        self.report({"INFO"}, summary)
        return {"FINISHED"}


# --------------------------------------------------------------------------
# authoring
# --------------------------------------------------------------------------
#
# Export reads `rif_id`, `rif_index`, `rif_objhead` and `rif_absorbed`, and
# nothing else in a Blender scene creates them -- so without these operators a
# mesh added by hand is silently missing from the file it was meant to be in.
# These are the two entry points that mint them, plus the panel for the two
# identities the format needs and Blender has no field for.


class SCENE_OT_rif_new(bpy.types.Operator):
    """Start a new RIF file: an empty collection ready to have objects added"""

    bl_idname = "scene.rif_new"
    bl_label = "New Gunlok RIF"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(
        name="File name",
        description="Names the collection, and seeds the file's RIFFNAME chunk",
        default="untitled.rif",
    )
    scale: FloatProperty(
        name="Scale",
        description=(
            "Multiplier from RIF integer units to Blender metres. The engine's "
            "own factor is per-rif data, so this is a convention - match the "
            "files you are authoring alongside"
        ),
        default=sc.DEFAULT_SCALE,
        min=1e-6,
        max=1000.0,
    )
    y_down: BoolProperty(
        name="Y-down target",
        description="Write RIF coordinates Y-down, as every shipped file is",
        default=True,
    )

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        collection = sc.new_collection(self.name, scale=self.scale, y_down=self.y_down,
                                       fps=context.scene.render.fps)
        self.report({"INFO"}, "New RIF collection %r; add objects with Object > "
                              "Gunlok RIF > Add to RIF" % collection.name)
        return {"FINISHED"}


class OBJECT_OT_rif_add(bpy.types.Operator):
    """Make the selected objects part of a RIF file"""

    bl_idname = "object.rif_add"
    bl_label = "Add to RIF"
    bl_options = {"REGISTER", "UNDO"}

    collection: StringProperty(
        name="Collection",
        description="Which RIF to add to. Empty uses the only one present",
        default="",
    )

    @classmethod
    def poll(cls, context):
        return bool(context.selected_objects or context.object)

    def execute(self, context):
        target, error = _sole_collection(self.collection)
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}

        # Falling back to the active object matters for the button in the Object
        # Properties panel: you are looking at an object there without
        # necessarily having it selected in the viewport.
        targets = context.selected_objects or ([context.object] if context.object else [])
        added, refused = 0, []
        for obj in targets:
            why = sc.adopt_object(target, obj)
            if why is None:
                added += 1
            else:
                refused.append(why)

        if refused and not added:
            self.report({"ERROR"}, refused[0])
            return {"CANCELLED"}
        summary = "Added %d object(s) to %s" % (added, target.name)
        if refused:
            summary += "; skipped %d (%s)" % (len(refused), refused[0])
        self.report({"WARNING"} if refused else {"INFO"}, summary)
        return {"FINISHED"}


class OBJECT_OT_rif_add_sequence(bpy.types.Operator):
    """Make the armature's active Action one of this rig's animation sequences"""

    bl_idname = "object.rif_add_sequence"
    bl_label = "Add Action to RIF"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.type == "ARMATURE"
                and obj.animation_data is not None
                and obj.animation_data.action is not None)

    def execute(self, context):
        obj = context.object
        action = obj.animation_data.action
        why = sc.adopt_action(obj, action)
        if why is not None:
            self.report({"WARNING"}, why)
            return {"CANCELLED"}
        bones = sorted(sc._animated_bones(action))
        if not bones:
            self.report(
                {"WARNING"},
                "%s has no pose-bone curves yet; it will export as an empty sequence"
                % action.name)
        else:
            self.report({"INFO"}, "%s is now a sequence over %d bone(s)"
                                  % (action.name, len(bones)))
        return {"FINISHED"}


class SCENE_OT_rif_add_sound(bpy.types.Operator):
    """Add an entry to this file's sound table, as a Speaker"""

    bl_idname = "scene.rif_add_sound"
    bl_label = "Add RIF Sound"
    bl_options = {"REGISTER", "UNDO"}

    path: StringProperty(
        name="Path",
        description=(
            "What the .rif stores, relative to the install's Sound folder and "
            "backslash-separated, e.g. Robots\\GL_click08.wav"
        ),
        default="Robots\\",
    )
    filepath: StringProperty(
        name="Audio file",
        description="Optional .wav to load for audition. Never written back",
        default="",
        subtype="FILE_PATH",
    )
    collection: StringProperty(name="Collection", default="")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        target, error = _sole_collection(self.collection)
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        path = (self.path or "").strip()
        if not path:
            self.report({"ERROR"}, "A sound needs a path; that is what the file stores")
            return {"CANCELLED"}
        obj = sc.add_sound(target, path, bpy.path.abspath(self.filepath)
                           if self.filepath else None)
        self.report({"INFO"}, "Added %s as sound %d"
                              % (path, obj.data.get("rif_sound_index", 0)))
        return {"FINISHED"}


class POSE_OT_rif_set_sound(bpy.types.Operator):
    """Play a sound at the active bone's keyframe on the current frame"""

    bl_idname = "pose.rif_set_sound"
    bl_label = "Set Keyframe Sound"
    bl_options = {"REGISTER", "UNDO"}

    index: IntProperty(
        name="Sound",
        description="Table index to play here; 0 removes the event",
        default=0, min=0, max=127,
    )

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.type == "ARMATURE"
                and context.active_pose_bone is not None
                and obj.animation_data is not None
                and obj.animation_data.action is not None
                and obj.animation_data.action.get("rif_id") == "OBANSEQC")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        obj = context.object
        action = obj.animation_data.action
        bone = context.active_pose_bone.name
        frame = float(context.scene.frame_current)

        target = sc.collection_for(obj)
        known = {e["index"] for e in sc.sound_table(target)} if target else set()
        if self.index and self.index not in known:
            # A dangling index is legal -- 12 shipped files have one and the
            # engine skips it silently -- so this is a warning, not a refusal.
            self.report({"WARNING"},
                        "no table entry %d; the engine will play nothing" % self.index)

        sc.set_sound_event(action, bone, frame, self.index)
        self.report({"INFO"}, "%s frame %g: sound %d"
                              % (bone, frame, self.index) if self.index else
                              "%s frame %g: sound removed" % (bone, frame))
        return {"FINISHED"}


class ACTION_OT_rif_toggle_setting(bpy.types.Operator):
    """Give this sequence one of its optional chunks, or take it away"""

    bl_idname = "action.rif_toggle_setting"
    bl_label = "Toggle Sequence Setting"
    bl_options = {"REGISTER", "UNDO"}

    setting: StringProperty(name="Chunk", default="OBASEQTM")

    #: What a newly added chunk starts at: the commonest shipped value, so
    #: adding one does not also invent a number.
    DEFAULTS = {"OBASEQTM": 1000, "OBASEQFL": 0x04, "OBASEQSP": [0, 0, 0]}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.animation_data is not None
                and obj.animation_data.action is not None
                and obj.animation_data.action.get("rif_id") == "OBANSEQC")

    def execute(self, context):
        action = context.object.animation_data.action
        cid = self.setting.encode("ascii")
        if cid not in sc.SEQUENCE_SETTINGS:
            self.report({"ERROR"}, "unknown setting %r" % self.setting)
            return {"CANCELLED"}
        if sc.sequence_setting(action, cid) is None:
            sc.set_sequence_setting(action, cid, self.DEFAULTS[self.setting])
            self.report({"INFO"}, "%s added to %s" % (self.setting, action.name))
        else:
            sc.set_sequence_setting(action, cid, None)
            self.report({"INFO"}, "%s removed from %s" % (self.setting, action.name))
        return {"FINISHED"}


class OBJECT_OT_rif_new_shape_id(bpy.types.Operator):
    """Give this object an unused shape id, on both halves of the pair"""

    bl_idname = "object.rif_new_shape_id"
    bl_label = "Assign Fresh Shape ID"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.get("rif_id") == "RBOBJECT" and obj.data is not None

    def execute(self, context):
        obj = context.object
        target = sc.collection_for(obj)
        if target is None:
            self.report({"ERROR"}, "%s is not in a RIF collection" % obj.name)
            return {"CANCELLED"}
        shape_id = sc.next_shape_id(target)
        sc.set_rif_shape_id(obj, shape_id)
        self.report({"INFO"}, "%s now claims shape %d" % (obj.name, shape_id))
        return {"FINISHED"}


# The two identities live inside chunk bodies stored as int32 arrays, so without
# these accessors editing one means hand-packing bytes into a custom property.
def _get_object_name(self):
    return sc.rif_object_name(self)


def _set_object_name(self, value):
    sc.set_rif_object_name(self, value)


def _get_shape_id(self):
    return sc.rif_shape_id(self)


def _set_shape_id(self, value):
    sc.set_rif_shape_id(self, value)


class OBJECT_PT_rif(bpy.types.Panel):
    bl_label = "Gunlok RIF"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def draw(self, context):
        obj = context.object
        layout = self.layout
        layout.use_property_split = True

        chunk_id = obj.get("rif_id")
        if chunk_id is None:
            col = layout.column()
            col.label(text="Not part of a RIF file", icon="INFO")
            if sc.rif_collections():
                col.operator(OBJECT_OT_rif_add.bl_idname, icon="ADD")
            else:
                col.operator(SCENE_OT_rif_new.bl_idname, icon="FILE_NEW")
            return

        layout.label(text="Chunk: %s" % chunk_id)
        if obj.type == "ARMATURE":
            self._draw_rig(obj, layout)
            return
        if chunk_id in ("RBOBJECT", "DUMMYOBJ"):
            # Deliberately not obj.name: the outliner name is uniquified by
            # Blender and is not what the engine resolves by strcmp.
            layout.prop(obj, "rif_object_name", text="Name in file")
        if chunk_id != "RBOBJECT" or obj.data is None:
            return

        row = layout.row(align=True)
        row.prop(obj, "rif_shape_id", text="Shape ID")
        row.operator(OBJECT_OT_rif_new_shape_id.bl_idname, text="", icon="FILE_REFRESH")

        collection = sc.collection_for(obj)
        if collection is None:
            return
        sharing = sc.shape_id_users(collection).get(sc.rif_shape_id(obj), ())
        if len(sharing) > 1:
            box = layout.box()
            box.label(text="Shape ID shared with %d other object(s)" % (len(sharing) - 1),
                      icon="ERROR")
            box.label(text="Export refuses this; assign a fresh id.")

    @staticmethod
    def _draw_rig(obj, layout):
        """The rig's own panel: which Actions export, and which do not."""
        marked = sc._rif_actions(obj)
        layout.label(text="%d bone(s), %d sequence(s)" % (len(obj.data.bones), len(marked)))

        anim = obj.animation_data
        action = anim.action if anim else None
        if action is None:
            layout.label(text="No active Action.", icon="INFO")
            return
        if action.get("rif_id") != "OBANSEQC":
            box = layout.box()
            box.label(text="%s is not a sequence and will not export" % action.name,
                      icon="ERROR")
            box.operator(OBJECT_OT_rif_add_sequence.bl_idname, icon="ADD")
            return

        layout.label(text="%s: sequence %s over %d bone(s)"
                          % (action.name, action.get("rif_sub_sequence", "?"),
                             len(sc._animated_bones(action))),
                     icon="CHECKMARK")

        # The three optional per-sequence chunks. Present and absent are
        # different states -- most sequences carry none -- so each row is a
        # toggle rather than a field with a "none" value.
        box = layout.box()
        box.label(text="Sequence settings", icon="SEQUENCE")
        for cid, label, unit in ((b"OBASEQTM", "Duration", "ms"),
                                 (b"OBASEQFL", "Flags", ""),
                                 (b"OBASEQSP", "Speed", "mm/s")):
            value = sc.sequence_setting(action, cid)
            row = box.row(align=True)
            if value is None:
                row.label(text="%s: not set" % label)
            elif cid == b"OBASEQFL":
                row.prop(action, "rif_loop_mode", text="Loop")
            elif cid == b"OBASEQSP":
                row.prop(action, "rif_sequence_speed", text="%s (%s)" % (label, unit))
            else:
                row.prop(action, "rif_duration_ms", text="%s (%s)" % (label, unit))
            op = row.operator(ACTION_OT_rif_toggle_setting.bl_idname, text="",
                              icon="X" if value is not None else "ADD")
            op.setting = cid.decode("ascii")

        events = sc.sound_events(action)
        box = layout.box()
        box.label(text="Sounds (%d event(s))" % len(events), icon="SPEAKER")
        collection = sc.collection_for(obj)
        table = {e["index"]: e for e in sc.sound_table(collection)} if collection else {}
        for ev in events[:12]:
            entry = table.get(ev["index"])
            row = box.row()
            row.label(text="%s @ %g" % (ev["bone"], ev["frame"]))
            # A dangling index is legal shipped data, so it is flagged rather
            # than hidden: the engine skips it silently and so would you.
            row.label(text="%d %s" % (ev["index"],
                                      entry["path"] if entry else "(no table entry)"),
                      icon="NONE" if entry else "ERROR")
        if len(events) > 12:
            box.label(text="... and %d more" % (len(events) - 12))
        box.operator(POSE_OT_rif_set_sound.bl_idname, icon="ADD")


class DATA_PT_rif_sound(bpy.types.Panel):
    bl_label = "Gunlok RIF"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == "SPEAKER" and obj.data is not None

    def draw(self, context):
        spk = context.object.data
        layout = self.layout
        layout.use_property_split = True
        if "rif_sound_index" not in spk:
            layout.label(text="Not a RIF sound", icon="INFO")
            layout.operator(SCENE_OT_rif_add_sound.bl_idname, icon="ADD")
            return
        layout.label(text="Table slot %d" % spk["rif_sound_index"])
        layout.prop(spk, "rif_sound_file", text="Path in file")
        # These three ARE the stored fields, shown as what they are rather than
        # duplicated into custom properties: the exporter reads them back off the
        # speaker, in millimetres and 0..127.
        col = layout.column(align=True)
        col.prop(spk, "distance_reference", text="Min distance (m)")
        col.prop(spk, "distance_max", text="Max distance (m)")
        col.prop(spk, "volume", text="Volume")
        layout.label(text="Writes %d / %d mm, volume %d"
                          % (round(spk.distance_reference * 1000),
                             round(spk.distance_max * 1000),
                             round(max(0.0, min(1.0, spk.volume)) * 127)),
                     icon="INFO")
        if spk.sound is None:
            layout.label(text="No audio loaded; the path still exports.", icon="INFO")


def _get_duration_ms(self):
    v = sc.sequence_setting(self, b"OBASEQTM")
    return 0 if v is None else v


def _set_duration_ms(self, value):
    sc.set_sequence_setting(self, b"OBASEQTM", max(0, int(value)))


def _get_sequence_speed(self):
    v = sc.sequence_setting(self, b"OBASEQSP")
    return 0 if v is None else int(v[0])


def _set_sequence_speed(self, value):
    # Only the speed is exposed: `angle` and `spare` are zero in all 582 shipped
    # chunks, and a heading nothing has ever set is not something to invent a UI
    # for. They are preserved either way.
    cur = sc.sequence_setting(self, b"OBASEQSP") or [0, 0, 0]
    sc.set_sequence_setting(self, b"OBASEQSP", [max(0, int(value)), cur[1], cur[2]])


def _get_loop_mode(self):
    v = sc.sequence_setting(self, b"OBASEQFL")
    return {"LOOP": 1, "ONCE": 2}.get(heads.seq_loop_mode(0 if v is None else v), 0)


def _set_loop_mode(self, value):
    cur = sc.sequence_setting(self, b"OBASEQFL")
    mode = {1: "LOOP", 2: "ONCE"}.get(value, "UNSET")
    sc.set_sequence_setting(self, b"OBASEQFL",
                            heads.set_seq_loop_mode(0 if cur is None else cur, mode))


def _get_sound_file(self):
    return self.get("rif_sound_path", "") or ""


def _set_sound_file(self, value):
    self["rif_sound_path"] = (value or "").strip()


class MATERIAL_PT_rif(bpy.types.Panel):
    bl_label = "Gunlok RIF"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def draw(self, context):
        mat = context.material
        layout = self.layout
        layout.use_property_split = True
        # The .rif stores a texture *name*, never the image, so this field is the
        # retexturing knob and assigning a different image to the shader is not.
        layout.prop(mat, "rif_texture_name", text="Texture (.RIM)")
        index = mat.get("rif_texture_index")
        if index is not None:
            layout.label(text="Table index: %s"
                              % ("untextured" if index == 0xFFF else index))
        layout.label(text="Export writes the name; the image is display only.",
                     icon="INFO")

        # A stored UV is a texel coordinate, so export multiplies by the texture's
        # size. With no image wired up and nothing recorded, that size is 1x1 and
        # every UV writes as a single texel -- which looks like nothing at all
        # in-game and is invisible from here unless it is shown.
        width, height = sc.uv_scale(mat)
        row = layout.row()
        row.label(text="UV scale: %d x %d texels" % (width, height))
        if mat.rif_texture_name and (width, height) == (1.0, 1.0):
            box = layout.box()
            box.label(text="No texture size known; UVs will export 1:1", icon="ERROR")
            box.label(text="Wire the .RIM's image into this material to fix it.")


def _get_bmp_name(self):
    return self.get("rif_bmp_name", "") or ""


def _set_bmp_name(self, value):
    self["rif_bmp_name"] = (value or "").strip()


def _menu_import(self, context):
    self.layout.operator(IMPORT_SCENE_OT_rif.bl_idname, text="Gunlok RIF (.rif)")


def _menu_export(self, context):
    self.layout.operator(EXPORT_SCENE_OT_rif.bl_idname, text="Gunlok RIF (.rif)")


def _menu_object(self, context):
    self.layout.separator()
    self.layout.operator(SCENE_OT_rif_new.bl_idname, text="New Gunlok RIF")
    self.layout.operator(OBJECT_OT_rif_add.bl_idname, text="Add to Gunlok RIF")
    self.layout.operator(OBJECT_OT_rif_add_sequence.bl_idname,
                         text="Add Action to Gunlok RIF")
    self.layout.operator(SCENE_OT_rif_add_sound.bl_idname, text="Add Gunlok RIF Sound")


_CLASSES = (IMPORT_SCENE_OT_rif, EXPORT_SCENE_OT_rif, SCENE_OT_rif_new,
            OBJECT_OT_rif_add, OBJECT_OT_rif_add_sequence, OBJECT_OT_rif_new_shape_id,
            SCENE_OT_rif_add_sound, POSE_OT_rif_set_sound,
            ACTION_OT_rif_toggle_setting,
            OBJECT_PT_rif, DATA_PT_rif_sound, MATERIAL_PT_rif)


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.rif_object_name = StringProperty(
        name="Name in file",
        description=(
            "The name this object answers to inside the .rif - what a map "
            "section's `name`, a `for \"<rif object>\"` spawn point and an "
            "OBJHIERD node binding all resolve by. Renaming the Blender object "
            "does not change it"
        ),
        get=_get_object_name, set=_set_object_name)
    bpy.types.Object.rif_shape_id = IntProperty(
        name="Shape ID",
        description=(
            "Pairs this object with its geometry (OBJHEAD1+0x38 against "
            "SHPHEAD1+0x14). Setting it here writes both halves; two objects "
            "must never share one"
        ),
        get=_get_shape_id, set=_set_shape_id)
    # Named `rif_texture_name`, not `rif_bmp_name`: the latter is the ID property
    # this reads and writes, and an RNA property of the same name shadows it.
    bpy.types.Material.rif_texture_name = StringProperty(
        name="Texture (.RIM)",
        description=(
            "The texture path this material's polygons wear, relative to the "
            "install's Graphics folder. This is what export writes - swapping "
            "the image in the shader does nothing"
        ),
        get=_get_bmp_name, set=_set_bmp_name)
    # Same reason as rif_texture_name: the ID property it reads is
    # `rif_sound_path`, and an RNA property of that name would shadow it.
    bpy.types.Speaker.rif_sound_file = StringProperty(
        name="Path in file",
        description=(
            "What the .rif stores for this sound, relative to the install's "
            "Sound folder and backslash-separated. This is what export writes - "
            "the loaded audio is for audition only"
        ),
        get=_get_sound_file, set=_set_sound_file)
    bpy.types.Action.rif_duration_ms = IntProperty(
        name="Duration",
        description="OBASEQTM: how long this sequence runs, in milliseconds",
        min=0, soft_max=10000, get=_get_duration_ms, set=_set_duration_ms)
    bpy.types.Action.rif_sequence_speed = IntProperty(
        name="Speed",
        description=(
            "OBASEQSP: how fast this sequence moves the model, in mm/second. "
            "The shipped walks and runs are 1400-3000"
        ),
        min=0, soft_max=5000, get=_get_sequence_speed, set=_set_sequence_speed)
    bpy.types.Action.rif_loop_mode = EnumProperty(
        name="Loop",
        description=(
            "OBASEQFL's loop bits. Every other bit in the word - including the "
            "0x80 that 181 shipped chunks carry and nobody has explained - is "
            "preserved untouched"
        ),
        items=[("UNSET", "Unset", "Neither loop bit set"),
               ("LOOP", "Loops", "SequenceFlag_Loops"),
               ("ONCE", "Once", "SequenceFlag_NoLoop")],
        get=_get_loop_mode, set=_set_loop_mode)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)
    bpy.types.VIEW3D_MT_object.append(_menu_object)


def unregister():
    bpy.types.VIEW3D_MT_object.remove(_menu_object)
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    del bpy.types.Action.rif_loop_mode
    del bpy.types.Action.rif_sequence_speed
    del bpy.types.Action.rif_duration_ms
    del bpy.types.Speaker.rif_sound_file
    del bpy.types.Material.rif_texture_name
    del bpy.types.Object.rif_shape_id
    del bpy.types.Object.rif_object_name
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
