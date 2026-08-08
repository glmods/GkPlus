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
(``SHPMRGDT``, ``SHPVTINT``), and preprocessed render data nothing in the engine
reads is dropped (``SHPPCINF``, which 681 of the 9,357 shipped shapes omit anyway).

``SHPMRGDT`` is a *pairing between polygons*, so it rides as a shared pair id
rather than the index the file stores -- an index does not survive this addon
renumbering polygons, and carrying one used to crash Gunlok on 15 of the 24
shipped levels. Export validates the rebuilt table before writing it, because
the engine's merge pass has no bounds check. See ``blender/CLAUDE.md``.
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


def _rif_collection_name(context):
    """The RIF collection the Properties editor is looking at, by name.

    The sound table lives on the collection, so its panel is drawn in the
    Collection tab -- where ``context.collection`` is whatever the outliner has
    active, which need not be a RIF at all.
    """
    coll = getattr(context, "collection", None)
    return coll.name if coll is not None and coll.get("rif_id") == "REBINFF2" else ""


def _every_sound_event(collection):
    """Every keyframe sound event in the file, across every sequence."""
    out = []
    for action in bpy.data.actions:
        if action.get("rif_id") != "OBANSEQC":
            continue
        out.extend(sc.sound_events(action))
    return out


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
    textures: EnumProperty(
        name="Textures",
        description=(
            "Write the images as .RIM files too. The .rif itself only ever "
            "stores their names"
        ),
        items=(
            ("NONE", "None", "Write only the .rif. The names still go in it"),
            ("CHANGED", "Changed only",
             "Write the textures whose pixels no longer match the .RIM they were "
             "imported from - what a mod needs and nothing else"),
            ("ALL", "All", "Write every texture the file names, edited or not"),
        ),
        default="NONE",
    )
    texture_dir: StringProperty(
        name="Textures folder",
        description=(
            "Where the .RIM files go, laid out the way the game's Graphics "
            "folder is. Point it at a mod (gkplus\\mods\\<name>\\graphics), not "
            "at the install, unless you mean to overwrite it"
        ),
        default="",
        subtype="DIR_PATH",
    )
    compress_textures: BoolProperty(
        name="Compress textures",
        description=(
            "Pack the written .RIM files with ByteRun1, which 36 of the shipped "
            "textures use. Off writes them raw - larger, and the form verified "
            "end to end in the running game"
        ),
        default=True,
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

        # Vertex lighting is read from `rif_light` by name and never averaged or
        # padded at export, so a mesh whose attribute no longer describes it is
        # refused here rather than discovered as a raise mid-write.
        lighting = sc.lighting_problems(target)
        if lighting:
            self.report({"ERROR"}, lighting[0] if len(lighting) == 1
                        else "%s (and %d other mesh(es))" % (lighting[0],
                                                             len(lighting) - 1))
            return {"CANCELLED"}

        # A dummy with no DUMOBJDT, or with an empty name, is a shape the game's
        # own data never takes -- and the first of the two is an access violation
        # during level load rather than a quiet failure. Both are refused here,
        # for the same reason the shared shape id is: discovering it in Gunlok
        # costs a crash that names neither the file nor the object.
        dummy_errors, dummy_warnings = sc.dummy_problems(target)
        if dummy_errors:
            self.report({"ERROR"}, dummy_errors[0] if len(dummy_errors) == 1
                        else "%s (and %d other problem(s))"
                             % (dummy_errors[0], len(dummy_errors) - 1))
            return {"CANCELLED"}

        # Checked before anything is written, so a missing folder does not leave
        # a .rif on disk whose textures never followed it.
        texture_root = bpy.path.abspath(self.texture_dir) if self.texture_dir else ""
        if self.textures != "NONE" and not os.path.isdir(texture_root):
            self.report({"ERROR"},
                        "Set the Textures folder to write .RIM files, or set "
                        "Textures to None")
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
        # Counted apart, because they are two systems that share nothing: a
        # table of definitions an animation names, and a placement in the level.
        if stats.get("sounds"):
            summary += ", %d INDSOUND entr(ies)" % stats["sounds"]
        if stats.get("emitters"):
            summary += ", %d ambient emitter(s)" % stats["emitters"]

        if self.textures != "NONE":
            written = sc.write_textures(target, texture_root,
                                        changed_only=self.textures == "CHANGED",
                                        compress=self.compress_textures)
            summary += ("; %d .RIM written (%.1f MB)"
                        % (written["written"], written["bytes"] / (1024.0 * 1024.0)))
            if written["unchanged"]:
                summary += ", %d unchanged" % written["unchanged"]
            if written["failed"]:
                name, why = written["failed"][0]
                self.report(
                    {"WARNING"},
                    "%s; %d texture(s) could not be written (%s: %s)"
                    % (summary, len(written["failed"]), name, why),
                )
                return {"FINISHED"}
            if written["no_image"]:
                # The .rif is complete either way -- it stores the name -- so this
                # is a warning about the mod, not about the model.
                self.report(
                    {"WARNING"},
                    "%s; %d material(s) name a texture but carry no image, so "
                    "nothing was written for them (%s)"
                    % (summary, len(written["no_image"]),
                       ", ".join(written["no_image"][:3])),
                )
                return {"FINISHED"}
        # Never let this one pass as INFO: the faces are gone from the file, and
        # keeping them would have crashed the game while it built section
        # adjacency, with a fault that names neither this file nor the polygon.
        if stats.get("degenerate_faces"):
            self.report(
                {"WARNING"},
                "%s; dropped %d face(s) whose corners coincide once quantized to "
                "RIF integer units -- keeping them crashes Gunlok during level "
                "load. Merge by Distance and check for zero-area faces if you "
                "did not expect any." % (summary, stats["degenerate_faces"]),
            )
            return {"FINISHED"}
        # Never INFO either, and this one is a *saved* crash rather than a loss:
        # a SHPMRGDT the validator could not prove is an out-of-bounds heap write
        # inside the engine's quad merge during level load. Dropping it is always
        # legal; being told is what stops it going unnoticed.
        if stats.get("merge_dropped"):
            self.report(
                {"WARNING"},
                "%s; dropped the polygon-merge data on %d shape(s) because it "
                "would not have been safe to write -- the engine's merge pass has "
                "no bounds check, so a bad pairing crashes it during level load "
                "(%s)" % (summary, stats["merge_dropped"],
                          (stats.get("merge_reasons") or ["?"])[0]),
            )
            return {"FINISHED"}
        # Likewise never INFO: the object was marked as carrying lighting and
        # now does not, which is a chunk that used to be in the file and is not.
        if stats.get("lighting_dropped"):
            self.report(
                {"WARNING"},
                "%s; %d object(s) lost their vertex lighting because the %r "
                "attribute was deleted. Re-create it with Enable Vertex Lighting "
                "if that was not deliberate." % (summary, stats["lighting_dropped"],
                                                 sc.LIGHT_COLOR_ATTR),
            )
            return {"FINISHED"}
        if dummy_warnings:
            self.report({"WARNING"}, "%s; %s%s"
                        % (summary, dummy_warnings[0],
                           " (and %d more)" % (len(dummy_warnings) - 1)
                           if len(dummy_warnings) > 1 else ""))
            return {"FINISHED"}
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
    """Add an entry to this file's INDSOUND table, which an animation keyframe names"""

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
        entry = sc.add_sound(target, path, bpy.path.abspath(self.filepath)
                             if self.filepath else None)
        self.report({"INFO"}, "Added %s as sound %d" % (path, entry["index"]))
        return {"FINISHED"}


class SCENE_OT_rif_remove_sound(bpy.types.Operator):
    """Drop the selected entry from this file's INDSOUND table"""

    bl_idname = "scene.rif_remove_sound"
    bl_label = "Remove RIF Sound"
    bl_options = {"REGISTER", "UNDO"}

    collection: StringProperty(name="Collection", default="")

    def execute(self, context):
        target, error = _sole_collection(self.collection or _rif_collection_name(context))
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        _at, entry = sc.active_sound(target)
        if entry is None or not sc.remove_sound(target):
            self.report({"WARNING"}, "no table entry selected")
            return {"CANCELLED"}
        # A dangling index is legal shipped data, so removing an entry a keyframe
        # still names is a warning rather than a refusal -- the engine's null-slot
        # check makes it silent, and 12 shipped files rely on exactly that.
        using = [e for e in _every_sound_event(target) if e["index"] == entry["index"]]
        if using:
            self.report({"WARNING"},
                        "removed slot %d; %d keyframe(s) still name it and will "
                        "now play nothing" % (entry["index"], len(using)))
            return {"FINISHED"}
        self.report({"INFO"}, "removed slot %d (%s)" % (entry["index"], entry["path"]))
        return {"FINISHED"}


class SCENE_OT_rif_select_sound(bpy.types.Operator):
    """Edit this entry of the sound table"""

    bl_idname = "scene.rif_select_sound"
    bl_label = "Select RIF Sound"
    bl_options = {"REGISTER", "UNDO"}

    index: IntProperty(name="Row", default=0, min=0)
    collection: StringProperty(name="Collection", default="")

    def execute(self, context):
        target, error = _sole_collection(self.collection or _rif_collection_name(context))
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        target[sc.SOUND_ACTIVE_PROP] = self.index
        return {"FINISHED"}


class OBJECT_OT_rif_add_dummy(bpy.types.Operator):
    """Make the selected empties top-level DUMMYOBJ locators"""

    bl_idname = "object.rif_add_dummy"
    bl_label = "Add as Locator"
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

        targets = context.selected_objects or ([context.object] if context.object else [])
        added, refused = 0, []
        for obj in targets:
            why = sc.adopt_dummy(target, obj)
            if why is None:
                added += 1
            else:
                refused.append(why)

        if refused and not added:
            self.report({"ERROR"}, refused[0])
            return {"CANCELLED"}
        summary = ("Added %d locator(s) to %s; the engine finds these by name"
                   % (added, target.name))
        if refused:
            summary += "; skipped %d (%s)" % (len(refused), refused[0])
        self.report({"WARNING"} if refused else {"INFO"}, summary)
        return {"FINISHED"}


class SCENE_OT_rif_add_emitter(bpy.types.Operator):
    """Add a looping ambient sound at the 3D cursor: a DUMMYOBJ carrying a DUMOBJTX"""

    bl_idname = "scene.rif_add_emitter"
    bl_label = "Add Ambient Emitter"
    bl_options = {"REGISTER", "UNDO"}

    wav: StringProperty(
        name="Sound",
        description=(
            "The .wav this emitter loops, as a bare file name. The engine "
            "resolves it against the sound system's own directory list; the "
            "shipped ambient set lives in Sound\\environ"
        ),
        default="GL_Wind03.wav",
    )
    name: StringProperty(
        name="Name",
        description=(
            "The dummy's name in the file. An emitter's name never resolves - "
            "ToMap frees the record - but an empty one is stored as NULL, which "
            "no shipped dummy is"
        ),
        default="",
    )
    collection: StringProperty(name="Collection", default="")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        target, error = _sole_collection(self.collection)
        if target is None:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        obj, why = sc.add_emitter(target, self.wav, self.name or None)
        if obj is None:
            self.report({"ERROR"}, why)
            return {"CANCELLED"}
        obj.location = context.scene.cursor.location
        if obj.data.sound is None:
            self.report({"WARNING"},
                        "%s added, but %s was not found under the install's Sound "
                        "folder -- the name still exports" % (obj.name, self.wav))
            return {"FINISHED"}
        self.report({"INFO"}, "Added %s, looping %s" % (obj.name, self.wav))
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


def _lighting_targets(context):
    """The mesh objects these two operators act on: the selection, else the active."""
    objs = [o for o in (context.selected_objects or []) if o.type == "MESH"]
    if not objs and context.object is not None and context.object.type == "MESH":
        objs = [context.object]
    return objs


class OBJECT_OT_rif_enable_lighting(bpy.types.Operator):
    """Give this mesh baked vertex lighting: a white rif_light to paint or bake into"""

    bl_idname = "object.rif_enable_lighting"
    bl_label = "Enable Vertex Lighting"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return bool(_lighting_targets(context))

    def execute(self, context):
        done, fresh = 0, 0
        for obj in _lighting_targets(context):
            _name, had = sc.enable_lighting(obj.data)
            done += 1
            fresh += 0 if had else 1
        if not done:
            self.report({"ERROR"}, "No mesh selected")
            return {"CANCELLED"}
        msg = "%s on %d mesh(es)" % (sc.LIGHT_COLOR_ATTR, done)
        if fresh:
            msg += "; %d had no lighting and start white" % fresh
        msg += (". It is active and it is what exports, so Bake > Target > "
                "Active Color Attribute writes the file's lighting directly.")
        self.report({"INFO"}, msg)
        return {"FINISHED"}


class OBJECT_OT_rif_adopt_color_attribute(bpy.types.Operator):
    """Fold the active colour attribute into rif_light, the one that exports"""

    bl_idname = "object.rif_adopt_color_attribute"
    bl_label = "Use Active Color Attribute"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return bool(_lighting_targets(context))

    def execute(self, context):
        done, refused, last = 0, [], None
        for obj in _lighting_targets(context):
            source, why = sc.adopt_color_attribute(obj.data)
            if why is None:
                done += 1
                last = source
            else:
                refused.append(why)
        if not done:
            self.report({"ERROR"}, refused[0] if refused else "No mesh selected")
            return {"CANCELLED"}
        msg = "Adopted %s into %s on %d mesh(es)" % (last, sc.LIGHT_COLOR_ATTR, done)
        if refused:
            msg += "; skipped %d (%s)" % (len(refused), refused[0])
        self.report({"WARNING"} if refused else {"INFO"}, msg)
        return {"FINISHED"}


class OBJECT_OT_rif_navmesh_preview(bpy.types.Operator):
    """Mark the faces a character can stand on, the way the engine decides it"""

    bl_idname = "object.rif_navmesh_preview"
    bl_label = "Preview Navmesh"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return bool(_lighting_targets(context))

    def execute(self, context):
        done, refused, last = 0, [], None
        for obj in _lighting_targets(context):
            stats, why = sc.navmesh_preview(obj)
            if why is None:
                done += 1
                last = stats
            else:
                refused.append(why)
        if not done:
            self.report({"ERROR"}, refused[0] if refused else "No mesh selected")
            return {"CANCELLED"}

        msg = ("%d/%d faces walkable; %d island(s), largest %d. Blocked: %d facing down, "
               "%d steeper than 45 deg, %d flagged 0x100. Walkable faces are selected."
               % (last["walkable"], last["faces"], last["islands"], last["largest"],
                  last["faces_down"], last["too_steep"], last["blocked_flag"]))
        # A navmesh in many pieces is the thing worth shouting about: units can
        # only path within one island, so a fragmented one looks fine and plays
        # broken. The threshold is calibrated against shipped levels rather than
        # chosen -- level01/02/03/06 put 57.8%, 61.6%, 66.1% and 66.7% of their
        # walkable faces in the largest island (and hundreds of islands is
        # normal: 523, 420, 699, 1014). So the count means little and the share
        # means a lot, and 40% sits clear of the observed range in both
        # directions.
        fragmented = last["walkable"] and last["largest"] < 0.4 * last["walkable"]
        self.report({"WARNING"} if fragmented or refused else {"INFO"}, msg)
        return {"FINISHED"}


class SCENE_OT_rif_preview_setup(bpy.types.Operator):
    """Shade the RIF the way Gunlok does: texture times baked vertex lighting"""

    # The docstring is the tooltip, so the reasoning lives here instead: this
    # builds one Emission material per texture that multiplies `rif_light` by
    # that texture **in gamma space**, the way D3D8 fixed function does, and puts
    # the viewport in Material Preview under the Standard view transform. Why
    # each of those is forced rather than chosen is in `scene.py`'s preview
    # section. Reversible: the authored materials are recorded, never lost.

    bl_idname = "scene.rif_preview_setup"
    bl_label = "Set Up Gunlok Preview"
    bl_options = {"REGISTER", "UNDO"}

    shadow_casters: BoolProperty(
        name="Shadow objects cast only",
        description=(
            "Turn camera visibility off and shadow visibility on for a "
            "`_shadow` object, which is the role the engine gives it: a "
            "low-polygon silhouette for shadow volumes, never drawn"
        ),
        default=True)

    @classmethod
    def poll(cls, context):
        return bool(sc.rif_collections())

    def execute(self, context):
        stats, why = sc.preview_setup(shadow_casters=self.shadow_casters)
        if why is not None:
            self.report({"ERROR"}, why)
            return {"CANCELLED"}

        msg = ("%d object(s), %d material(s) previewing texture x %s"
               % (stats["objects"], stats["materials"], sc.LIGHT_COLOR_ATTR))
        if stats["lit"]:
            msg += "; %d mesh(es) had no %s and got one" % (stats["lit"],
                                                            sc.LIGHT_COLOR_ATTR)
        if stats["untextured"]:
            msg += "; %d material(s) have no texture and show the light alone" % (
                stats["untextured"])
        if stats["shadow"]:
            msg += "; %d shadow object(s) now cast only" % stats["shadow"]
        self.report({"INFO"}, msg)
        return {"FINISHED"}


class SCENE_OT_rif_preview_restore(bpy.types.Operator):
    """Put the authored materials and the colour management back"""

    bl_idname = "scene.rif_preview_restore"
    bl_label = "Restore Authored Materials"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return sc.PREVIEW_STATE_PROP in context.scene or sc.preview_is_active()

    def execute(self, context):
        stats, why = sc.preview_restore()
        if why is not None:
            self.report({"ERROR"}, why)
            return {"CANCELLED"}
        self.report({"INFO"},
                    "Restored %d slot(s) on %d object(s); removed %d unused preview "
                    "material(s)" % (stats["materials"], stats["objects"],
                                     stats["removed"]))
        return {"FINISHED"}


class OBJECT_OT_rif_new_light_id(bpy.types.Operator):
    """Give this light an id no other light in the file claims"""

    bl_idname = "object.rif_new_light_id"
    bl_label = "Assign Fresh Light ID"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.get("rif_id") == "STDLIGHT"

    def execute(self, context):
        obj = context.object
        target = sc.collection_for(obj)
        if target is None:
            self.report({"ERROR"}, "%s is not in a RIF collection" % obj.name)
            return {"CANCELLED"}
        light_id = sc.next_light_id(target)
        obj["rif_light_id"] = light_id
        self.report({"INFO"}, "%s is now light %d" % (obj.name, light_id))
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


def _active_collection(context):
    obj = context.object
    for coll in (obj.users_collection if obj else ()):
        if coll.get("rif_id") == "REBINFF2":
            return coll
    return next((c for c in bpy.data.collections if c.get("rif_id") == "REBINFF2"),
                None)


def _cutscene_root(context):
    """The cutscene Empty at or above the active object."""
    obj = context.object
    while obj is not None:
        if obj.get("rif_cut_role") == sc.CUT_SCENE:
            return obj
        obj = obj.parent
    return None


class SCENE_OT_rif_add_cutscene(bpy.types.Operator):
    """Add a cutscene: a camera, the empty it looks at, and an end event"""

    bl_idname = "scene.rif_add_cutscene"
    bl_label = "Add Cutscene"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(
        name="Name",
        description=("What PLAY CUTSCENE matches, and what the level's GLS "
                     "`camera track` section must name"),
        default="new cutscene")

    @classmethod
    def poll(cls, context):
        return _active_collection(context) is not None

    def execute(self, context):
        collection = _active_collection(context)
        if collection is None:
            self.report({"ERROR"}, "No Gunlok RIF collection")
            return {"CANCELLED"}
        existing = {r.get("rif_cut_name", "") for r in sc.cutscene_roots(collection)}
        if self.name in existing:
            self.report({"ERROR"}, "A cutscene called %r already exists; the "
                                   "name is the key PLAY CUTSCENE matches"
                        % self.name)
            return {"CANCELLED"}

        camera = context.object if (context.object
                                    and context.object.type == "CAMERA") else None
        root = sc.add_cutscene(collection, self.name, camera=camera)
        for obj in context.selected_objects:
            obj.select_set(False)
        root.select_set(True)
        context.view_layer.objects.active = root
        self.report({"INFO"},
                    "Added %r. Key the camera's location to build the path, then "
                    "declare it in the level's .gls with: camera track { file "
                    "\"...\" name \"%s\" }" % (self.name, self.name))
        return {"FINISHED"}


class OBJECT_OT_rif_cutscene_preview(bpy.types.Operator):
    """Draw the Catmull-Rom path the engine will actually follow"""

    bl_idname = "object.rif_cutscene_preview"
    bl_label = "Preview Cutscene Path"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return _cutscene_root(context) is not None

    def execute(self, context):
        root = _cutscene_root(context)
        made = sc.preview_cutscene_path(root)
        if not made:
            self.report({"WARNING"},
                        "No track has two or more location keyframes yet")
            return {"CANCELLED"}
        self.report({"INFO"},
                    "Drew %d path(s). The keyframes are spline control points, "
                    "so the curve does not pass straight between them - on the "
                    "shipped paths it bulges a median 5.6%% of segment length "
                    "(max 47%%)." % made)
        return {"FINISHED"}


class OBJECT_OT_rif_cutscene_add_event(bpy.types.Operator):
    """Add an event to this cutscene's camera track"""

    bl_idname = "object.rif_cutscene_add_event"
    bl_label = "Add Cutscene Event"
    bl_options = {"REGISTER", "UNDO"}

    kind: EnumProperty(
        name="Event",
        items=(("END", "End", "End the cutscene and hand control back"),
               ("CONSOLE", "Console Command", "Queue a console line")),
        default="END")
    command: StringProperty(
        name="Command",
        description="The console line to queue, for a Console Command event",
        default="")
    position: FloatProperty(
        name="At point",
        description=("Where in the path this fires, in point index space: 2.5 "
                     "is halfway between the third and fourth control points"),
        default=0.0, min=0.0)

    @classmethod
    def poll(cls, context):
        return _cutscene_root(context) is not None

    def execute(self, context):
        root = _cutscene_root(context)
        if self.kind == "CONSOLE" and not self.command.strip():
            self.report({"ERROR"}, "A Console Command event needs a command")
            return {"CANCELLED"}
        added = sc.add_cutscene_event(root, self.kind, self.command,
                                      self.position)
        if added is None:
            self.report({"ERROR"}, "This cutscene has no camera track")
            return {"CANCELLED"}
        self.report({"INFO"}, "Added %s event at point %.2f"
                    % (self.kind.lower(), self.position))
        return {"FINISHED"}


class OBJECT_PT_rif_cutscene(bpy.types.Panel):
    bl_label = "Gunlok Cutscene"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return (_cutscene_root(context) is not None
                or _active_collection(context) is not None)

    def draw(self, context):
        layout = self.layout
        root = _cutscene_root(context)
        if root is None:
            layout.operator(SCENE_OT_rif_add_cutscene.bl_idname, icon="ADD")
            layout.label(text="Select a cutscene to edit it", icon="INFO")
            return

        col = layout.column()
        col.prop(root, '["rif_cut_name"]', text="Name")
        col.label(text="PLAY CUTSCENE %s" % root.get("rif_cut_name", ""),
                  icon="CONSOLE")

        obj = context.object
        if obj is not None and obj.get("rif_cut_role") == sc.CUT_TRACK:
            box = layout.box()
            box.label(text="Track: %s" % obj.get("rif_track_name", ""))
            if obj.type == "CAMERA":
                box.prop(obj.data, "angle", text="Field of view")
            keys = len(sc.track_frames(obj))
            box.label(text="%d control point(s)" % keys,
                      icon="KEYFRAME" if keys else "ERROR")

        layout.operator(OBJECT_OT_rif_cutscene_preview.bl_idname, icon="CURVE_PATH")
        layout.operator(OBJECT_OT_rif_cutscene_add_event.bl_idname, icon="ADD")

        problems = sc.cutscene_problems_for(root)
        for why in problems:
            layout.label(text=why, icon="ERROR")


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
                if obj.type in ("EMPTY", "SPEAKER"):
                    col.operator(OBJECT_OT_rif_add_dummy.bl_idname, icon="EMPTY_AXIS")
            else:
                col.operator(SCENE_OT_rif_new.bl_idname, icon="FILE_NEW")
            return

        layout.label(text="Chunk: %s" % chunk_id)
        if obj.type == "ARMATURE":
            self._draw_rig(obj, layout)
            return
        if chunk_id == "STDLIGHT":
            self._draw_light(obj, layout)
            return
        if chunk_id in ("RBOBJECT", "DUMMYOBJ"):
            # Deliberately not obj.name: the outliner name is uniquified by
            # Blender and is not what the engine resolves by strcmp.
            layout.prop(obj, "rif_object_name", text="Name in file")
        if chunk_id == "DUMMYOBJ":
            self._draw_dummy(obj, layout)
            return
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

        self._draw_vertex_lighting(obj, layout)

    @staticmethod
    def _draw_vertex_lighting(obj, layout):
        """The baked lighting: the only lighting in a .rif the game actually reads."""
        me = obj.data
        box = layout.box()
        box.label(text="Vertex lighting (SHPVTINT)", icon="LIGHT")

        # Two separate questions, and conflating them is the bug this design
        # avoids: does the object carry a chunk (the marker), and is there an
        # attribute to paint (which the preview also mints, without the marker).
        paint = me.color_attributes.get(sc.LIGHT_COLOR_ATTR)
        if not sc.has_lighting(me):
            box.label(text="None. Enable to start one, white.", icon="INFO")
            if paint is not None:
                box.label(text="%s exists for the preview but is not exported yet."
                               % sc.LIGHT_COLOR_ATTR, icon="INFO")
        else:
            why = sc.lighting_refusal(me) if paint is not None else None
            if paint is None:
                box.label(text="Marked, but %s is gone; export drops it."
                               % sc.LIGHT_COLOR_ATTR, icon="ERROR")
            elif why is not None:
                box.label(text="Export refuses this mesh's lighting.", icon="ERROR")
                box.label(text=why)
            else:
                box.label(text="%d value(s), one per vertex -- paint or bake %s"
                               % (len(me.vertices), sc.LIGHT_COLOR_ATTR))
        col = box.column(align=True)
        col.operator(OBJECT_OT_rif_enable_lighting.bl_idname,
                     text="Enable Vertex Lighting", icon="COLOR")
        col.operator(OBJECT_OT_rif_adopt_color_attribute.bl_idname,
                     text="Use Active Color Attribute", icon="IMPORT")

        # Seeing it is a separate act from editing it: Solid mode draws vertex
        # colour *or* texture and has no way to combine them, so the engine's
        # `texture x diffuse` needs a material and Material Preview.
        row = box.row(align=True)
        row.operator(SCENE_OT_rif_preview_setup.bl_idname,
                     text="Preview As In Game", icon="SHADING_RENDERED")
        if sc.preview_is_active():
            row.operator(SCENE_OT_rif_preview_restore.bl_idname,
                         text="", icon="LOOP_BACK")

        nav = layout.box()
        nav.label(text="Navmesh", icon="MESH_GRID")
        walk = me.attributes.get(sc.NAV_WALKABLE_ATTR)
        if walk is None:
            nav.label(text="Characters walk on the level's own polygons.", icon="INFO")
        else:
            n = sum(1 for d in walk.data if d.value)
            nav.label(text="%d of %d face(s) walkable" % (n, len(me.polygons)))
        nav.operator(OBJECT_OT_rif_navmesh_preview.bl_idname,
                     text="Preview Navmesh", icon="MESH_GRID")

    @staticmethod
    def _draw_dummy(obj, layout):
        """A locator's panel. The name *is* the API, so it is what this is about.

        The engine builds strings like ``Goodie A2``, ``baddie c`` and ``Flag_3``
        itself and scans ``MapAuxObjectList`` for them, always case-insensitively.
        A dummy that is an emitter is a different thing entirely -- ``ToMap``
        frees the record before any name-matching consumer runs -- so the panel
        says which of the two this one is rather than showing both.
        """
        box = layout.box()
        if sc.EMITTER_TEXT_PROP in obj:
            box.label(text="Ambient sound emitter (DUMOBJTX)", icon="SPEAKER")
            box.label(text="ToMap turns this into a looping emitter and frees the")
            box.label(text="record, so its name never resolves. Edit it in the")
            box.label(text="Speaker's own Data tab.")
        else:
            box.label(text="Named locator", icon="EMPTY_AXIS")
            box.label(text="Found by name: console and trigger positions,")
            box.label(text="MP and enemy spawns (Goodie/Baddie), CTF points.")
            box.label(text="A `for \"<rif object>\"` spawn point is an RBOBJECT,")
            box.label(text="never this -- the two are disjoint namespaces.")

        if "rif_dumobjdt" not in obj:
            err = layout.box()
            err.label(text="No DUMOBJDT -- export refuses this", icon="ERROR")
            err.label(text="A dummy without one is an unchecked null dereference")
            err.label(text="during level load. Re-add it with Add as Locator.")

        collection = sc.collection_for(obj)
        if collection is None:
            return
        name = (obj.get("rif_name", "") or "").strip()
        sharing = [o for o in collection.objects
                   if o.get("rif_id") == "DUMMYOBJ" and name
                   and (o.get("rif_name", "") or "").strip().lower() == name.lower()]
        if len(sharing) > 1:
            warn = layout.box()
            warn.label(text="%d dummies share this name" % len(sharing), icon="INFO")
            warn.label(text="Legal and shipped (62 files), but the console takes")
            warn.label(text="the first match where triggers take the last.")

    @staticmethod
    def _draw_light(obj, layout):
        """A light's panel: what the datablock carries, and what it does not.

        Colour, brightness and range are real light settings, so they are edited
        in the Light data tab rather than duplicated here. What is left is the
        three chunk fields Blender has nowhere to put.
        """
        collection = sc.collection_for(obj)
        row = layout.row(align=True)
        row.label(text="Light ID %s" % obj.get("rif_light_id", "?"))
        row.operator(OBJECT_OT_rif_new_light_id.bl_idname, text="", icon="FILE_REFRESH")

        # Duplicating a light in Blender copies its id along with everything
        # else, and the id is unique within the file in all 38 shipped files
        # that have lights. Whether the engine minds is not measured, so this
        # warns rather than refusing the export the way a shared shape id does.
        if collection is not None:
            mine = obj.get("rif_light_id")
            sharing = [o for o in collection.objects
                       if o.get("rif_id") == "STDLIGHT" and o.get("rif_light_id") == mine]
            if len(sharing) > 1:
                box = layout.box()
                box.label(text="Light ID shared with %d other light(s)" % (len(sharing) - 1),
                          icon="ERROR")
                box.label(text="No shipped file does this; assign a fresh id.")

        light = obj.data
        if not light.use_custom_distance:
            box = layout.box()
            box.label(text="Custom Distance is off; range exports as 0", icon="ERROR")
            box.label(text="Set it in Light > Custom Distance.")
        scale = (collection.get("rif_scale", sc.DEFAULT_SCALE) if collection
                 else sc.DEFAULT_SCALE)
        col = layout.column(align=True)
        col.label(text="Brightness %.3f (energy; 0.2 - 2.0 in the game)" % (light.energy,))
        col.label(text="Range %d rif units"
                       % (int(round(light.cutoff_distance / scale))
                          if light.use_custom_distance else 0))

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


class COLLECTION_PT_rif_sounds(bpy.types.Panel):
    """The file's INDSOUND table, which is file data and so lives on the collection."""

    bl_label = "Gunlok RIF Sounds"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "collection"

    @classmethod
    def poll(cls, context):
        coll = getattr(context, "collection", None)
        return coll is not None and coll.get("rif_id") == "REBINFF2"

    def draw(self, context):
        coll = context.collection
        layout = self.layout
        entries = sc.sound_table(coll)

        box = layout.box()
        box.label(text="INDSOUND: sounds an animation keyframe names", icon="SEQ_HISTOGRAM")
        box.label(text="No position -- these play wherever the model is.", icon="INFO")

        # Drawn by hand rather than with `template_list`, and that is a measured
        # decision, not a preference. The table is a plain ID property -- the
        # same storage `rif_bmpnames` uses, and the reason `scene.py` needs no
        # registered RNA at all, which is what lets every test drive it with the
        # addon unregistered. `template_list` cannot take one: it resolves the
        # bracket form `["rif_indsound"]` and then **crashes Blender 5.2** in
        # `RNA_property_collection_length` (a null dereference through
        # `rna_property_rna_or_id_get`), which a background test cannot see
        # because nothing there ever draws. A registered `CollectionProperty`
        # would satisfy it and would mean a second place the table lives.
        #
        # So this is the approximation the design accepts: a row per entry,
        # clicking one selects it. No filtering, no sorting, no drag-reorder --
        # and reordering would mean nothing anyway, since a slot is an id rather
        # than a position.
        active_at = int(coll.get(sc.SOUND_ACTIVE_PROP, 0))
        box = layout.box()
        rows = box.column(align=True)
        for i, item in enumerate(entries):
            row = rows.row(align=True)
            op = row.operator(SCENE_OT_rif_select_sound.bl_idname,
                              text="%3d   %s" % (item["index"],
                                                 item.get("path") or "(no path)"),
                              icon="PLAY_SOUND" if sc.sound_audio(item) else "BLANK1",
                              depress=(i == active_at))
            op.index = i
        controls = box.row(align=True)
        controls.operator(SCENE_OT_rif_add_sound.bl_idname, text="Add", icon="ADD")
        controls.operator(SCENE_OT_rif_remove_sound.bl_idname, text="Remove",
                          icon="REMOVE")

        _at, entry = sc.active_sound(coll)
        if entry is None:
            layout.label(text="No entries. A file may legitimately have none.", icon="INFO")
            return

        sub = layout.column()
        sub.use_property_split = True
        sub.prop(coll, "rif_sound_slot", text="Slot")
        sub.prop(coll, "rif_sound_path", text="Path in file")
        sub.prop(coll, "rif_sound_min_distance", text="Min distance (mm)")
        sub.prop(coll, "rif_sound_max_distance", text="Max distance (mm)")
        sub.prop(coll, "rif_sound_volume", text="Volume (0-127)")
        sub.prop(coll, "rif_sound_pitch", text="Pitch offset")

        clashes = [e for e in entries if e["index"] == entry["index"]]
        if len(clashes) > 1:
            warn = layout.box()
            warn.label(text="Slot %d is claimed by %d entries" % (entry["index"], len(clashes)),
                       icon="ERROR")
            warn.label(text="The loader installs them into one array slot; the last wins.")

        audio = sc.sound_audio(entry)
        if audio is None:
            layout.label(text="No audio found under the install's Sound folder. "
                              "The path still exports.", icon="INFO")
        else:
            layout.label(text="Auditioning %s" % os.path.basename(audio.filepath),
                         icon="PLAY_SOUND")
        layout.label(text="Export writes the path; the wave is never written back.",
                     icon="INFO")


class DATA_PT_rif_emitter(bpy.types.Panel):
    """A Speaker's panel: the ambient emitter, which is the positional sound system."""

    bl_label = "Gunlok RIF"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == "SPEAKER" and obj.data is not None

    def draw(self, context):
        obj = context.object
        layout = self.layout
        layout.use_property_split = True
        if sc.EMITTER_TEXT_PROP not in obj:
            layout.label(text="Not a RIF ambient emitter", icon="INFO")
            layout.label(text="A positional sound is a DUMMYOBJ carrying a DUMOBJTX.")
            layout.operator(SCENE_OT_rif_add_emitter.bl_idname, icon="ADD")
            return

        spk = obj.data
        layout.prop(obj, "rif_emitter_wav", text="Sound (.wav)")
        # These three ARE the stored directives, shown as what they are rather
        # than duplicated into custom properties: export splices them back into
        # the text, and only the ones that changed.
        col = layout.column(align=True)
        col.prop(spk, "distance_reference", text="Min distance (I)")
        col.prop(spk, "distance_max", text="Max distance (R)")
        col.prop(spk, "pitch", text="Pitch (P)")

        vol = layout.box()
        vol.label(text="Volume (V) is parsed and discarded", icon="ERROR")
        vol.prop(spk, "volume", text="Volume")
        vol.label(text="SoundSystem_AddAmbientEmitter never reads it -- the sample's "
                       "own default wins. 514 shipped emitters carry a V and not one "
                       "does anything. Written for fidelity only.")

        info = layout.column(align=True)
        vals = sc.emitter_values(obj)
        info.label(text="Writes: %s" % (sc.emitter_text_from_speaker(obj)
                                        .replace("\r\n", " / ")))
        if not vals["R"]:
            info.label(text="Max distance 0 means the sample's own, not silence.",
                       icon="INFO")
        for why in sc.emitters.problems(sc.emitter_text_from_speaker(obj)):
            info.label(text=why, icon="ERROR")
        if spk.sound is None:
            info.label(text="No audio loaded; the name still exports.", icon="INFO")


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


# The INDSOUND table's fields, as accessors over the *active* row.
#
# The table is one ID property holding every entry, which is what keeps it whole
# and in order -- so there is no per-entry RNA path to draw a field from, and
# these read and write whichever row the UIList has selected. The same get/set
# shape `rif_texture_name` and `rif_duration_ms` already use.


def _sound_field(name, default=0):
    def get(self):
        _at, entry = sc.active_sound(self)
        return default if entry is None else entry.get(name, default)

    def set_(self, value):
        sc.set_sound_field(self, name, value)

    return get, set_


def _get_sound_slot(self):
    _at, entry = sc.active_sound(self)
    return 0 if entry is None else int(entry["index"])


def _set_sound_slot(self, value):
    # Not guarded against a clash: an index is a stable id the *file* assigns,
    # duplicates are visible in the panel, and refusing an edit mid-drag is
    # worse than saying what it did.
    sc.set_sound_field(self, "index", max(0, min(int(value), 127)))


def _get_sound_path(self):
    _at, entry = sc.active_sound(self)
    return "" if entry is None else entry.get("path", "")


def _set_sound_path(self, value):
    sc.set_sound_field(self, "path", (value or "").strip())


def _get_emitter_wav(self):
    return sc.emitters.wav(sc.emitter_text(self))


def _set_emitter_wav(self, value):
    sc.set_emitter_wav(self, (value or "").strip())


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
    self.layout.operator(OBJECT_OT_rif_add_dummy.bl_idname,
                         text="Add as Gunlok RIF Locator")
    self.layout.operator(OBJECT_OT_rif_add_sequence.bl_idname,
                         text="Add Action to Gunlok RIF")
    self.layout.operator(SCENE_OT_rif_add_sound.bl_idname, text="Add Gunlok RIF Sound")
    self.layout.operator(SCENE_OT_rif_add_emitter.bl_idname,
                         text="Add Gunlok Ambient Emitter")
    self.layout.operator(SCENE_OT_rif_preview_setup.bl_idname,
                         text="Set Up Gunlok Preview")
    self.layout.operator(SCENE_OT_rif_preview_restore.bl_idname,
                         text="Restore Authored Materials")
    self.layout.operator(SCENE_OT_rif_add_cutscene.bl_idname,
                         text="Add Gunlok Cutscene")


_CLASSES = (IMPORT_SCENE_OT_rif, EXPORT_SCENE_OT_rif, SCENE_OT_rif_new,
            OBJECT_OT_rif_add, OBJECT_OT_rif_add_sequence, OBJECT_OT_rif_new_shape_id,
            OBJECT_OT_rif_new_light_id, OBJECT_OT_rif_navmesh_preview,
            OBJECT_OT_rif_enable_lighting, OBJECT_OT_rif_adopt_color_attribute,
            SCENE_OT_rif_preview_setup, SCENE_OT_rif_preview_restore,
            SCENE_OT_rif_add_sound, SCENE_OT_rif_remove_sound,
            SCENE_OT_rif_select_sound, POSE_OT_rif_set_sound,
            OBJECT_OT_rif_add_dummy, SCENE_OT_rif_add_emitter,
            ACTION_OT_rif_toggle_setting,
            SCENE_OT_rif_add_cutscene, OBJECT_OT_rif_cutscene_preview,
            OBJECT_OT_rif_cutscene_add_event,
            COLLECTION_PT_rif_sounds,
            OBJECT_PT_rif, OBJECT_PT_rif_cutscene, DATA_PT_rif_emitter,
            MATERIAL_PT_rif)


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
    # The INDSOUND table's active row. Named `rif_sound_*` on the Collection,
    # while the table itself is `rif_indsound` -- an RNA property may not shadow
    # the ID property it reads, which is the same trap `rif_texture_name` avoids.
    bpy.types.Collection.rif_sound_slot = IntProperty(
        name="Slot",
        description=(
            "The table index an animation keyframe names. A stable, sparse id "
            "that means nothing outside this file; 0 is how a frame says "
            "\"no sound\", so it is never allocated"
        ),
        min=0, max=127, get=_get_sound_slot, set=_set_sound_slot)
    bpy.types.Collection.rif_sound_path = StringProperty(
        name="Path in file",
        description=(
            "What the .rif stores for this sound, relative to the install's "
            "Sound folder and backslash-separated. This is what export writes - "
            "the loaded audio is for audition only"
        ),
        get=_get_sound_path, set=_set_sound_path)
    _min_get, _min_set = _sound_field("min_distance")
    bpy.types.Collection.rif_sound_min_distance = IntProperty(
        name="Min distance",
        description="Millimetres. 5,000 in 231 of the 240 shipped entries",
        min=0, soft_max=100000, get=_min_get, set=_min_set)
    _max_get, _max_set = _sound_field("max_distance")
    bpy.types.Collection.rif_sound_max_distance = IntProperty(
        name="Max distance",
        description="Millimetres. 40,000 in 237 of the 240 shipped entries",
        min=0, soft_max=100000, get=_max_get, set=_max_set)
    _vol_get, _vol_set = _sound_field("volume")
    bpy.types.Collection.rif_sound_volume = IntProperty(
        name="Volume",
        description=(
            "0-127, and unlike an ambient emitter's V directive this one is "
            "read: the shipped entries take 20 distinct values"
        ),
        min=0, max=127, get=_vol_get, set=_vol_set)
    _pitch_get, _pitch_set = _sound_field("pitch")
    bpy.types.Collection.rif_sound_pitch = IntProperty(
        name="Pitch offset",
        description=(
            "0 in 225 of the 240 shipped entries; the rest are -640, 512 or "
            "-1280. The unit is not measured"
        ),
        soft_min=-4096, soft_max=4096, get=_pitch_get, set=_pitch_set)
    bpy.types.Object.rif_emitter_wav = StringProperty(
        name="Sound (.wav)",
        description=(
            "Line 2 of this emitter's DUMOBJTX: the looping ambient sound, as a "
            "bare file name the sound system resolves against its own directory "
            "list. Editing it rewrites that line and nothing else"
        ),
        get=_get_emitter_wav, set=_set_emitter_wav)
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
    del bpy.types.Object.rif_emitter_wav
    del bpy.types.Collection.rif_sound_pitch
    del bpy.types.Collection.rif_sound_volume
    del bpy.types.Collection.rif_sound_max_distance
    del bpy.types.Collection.rif_sound_min_distance
    del bpy.types.Collection.rif_sound_path
    del bpy.types.Collection.rif_sound_slot
    del bpy.types.Material.rif_texture_name
    del bpy.types.Object.rif_shape_id
    del bpy.types.Object.rif_object_name
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
