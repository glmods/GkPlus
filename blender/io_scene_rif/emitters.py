"""``DUMOBJTX`` -- the ambient sound **placement** system. Imports no ``bpy``.

This is not :mod:`sounds`, and the distinction is the reason both modules exist.
Gunlok has exactly two ways to put a sound in the world and they share nothing --
not a file, a directory, a chunk parent or a trigger:

* ``INDSOUND`` (:mod:`sounds`) is an indexed **table of definitions** at the rif
  root, addressed by a slot number an animation keyframe names, and it plays
  wherever the animating model happens to be. It occurs only in ``Objects\\``
  and ``Units\\``.
* ``DUMOBJTX`` is a **placement**: a text directive on a ``DUMMYOBJ`` that
  becomes one looping emitter at that dummy's fixed world position, started once
  from ``LoadLevel``. It occurs only in ``Levels\\`` -- 1,097 of them across 24
  files, and every single one is a sound.

The two name **not one file in common**. Full comparison in
``rif_chunk_format.md``, "``DUMOBJTX`` vs ``INDSOUND``".

The wire form is a NUL-terminated, CRLF-separated string padded to
``(strlen + 4) & ~3``::

    Sound\\r\\nGL_Wind03.wav\\r\\nV40 P0 R0\\0\\0\\0

**The parser has no function of its own** -- it is open-coded inside ``ToMap``
@ 0x004804bf-0x00480b13, which is why searching for a ``lookup_child`` on this id
finds only the getter and makes the chunk look inert. It is not.

Five measured facts shape everything here:

- **Line 1 is compared with ``lstrcmpiA``**, so case does not matter: 1,083 ship
  as ``Sound`` and 14 as ``sound``. Anything else and the record stays in
  ``MapAuxObjectList`` as a plain named marker.
- **The directive letters are uppercase-only.** The dispatch @ 0x00481c10 does
  ``ADD -0x49`` / ``CMP 0xd`` / ``JA``, so a lowercase ``v``/``p``/``r`` is
  skipped in silence. All 1,540 shipped directives are uppercase, so nothing in
  the game exercises it -- which makes it purely a trap for a generator, and
  :func:`format_directives` therefore always emits uppercase.
- **``V`` is parsed and then discarded.** ``SoundSystem_AddAmbientEmitter``
  @ 0x0058b9e0 takes the volume as its 5th argument and never reads that stack
  slot; the emitter takes the sample's own default instead. 514 shipped emitters
  carry a ``V`` and not one of them does anything. It is carried for fidelity
  and shown as inert, the way ``CUTEVENT`` kind 5 is.
- **``P`` is divided by 12** -- semitones to octaves -- and ``I``/``R`` are the
  min/max distance. A ``0.0f`` argument means "use the sample's own default",
  not zero.
- **The text is free-form and must be carried verbatim.** The shipped set is not
  the tidy three-line form the documentation suggests: 362 chunks have no third
  line at all, 221 end with a trailing CRLF that leaves an empty one, 29 end with
  two, and ``level06.RIF`` has one whose directives are split across two lines
  with a leading space (``Sound\\r\\nloudcreak01.wav\\r\\nV30\\r\\n P0 R60``).
  Reformatting from parsed values would rewrite all 1,097; :func:`retext`
  therefore **splices**, replacing only the argument text of a directive whose
  value actually changed and leaving every other byte alone.
"""

import math

#: What line 1 must say for the record to become an emitter. Compared with
#: ``lstrcmpiA``, so this is the spelling *written*, never the one *matched*.
KIND = "Sound"

#: The engine's line separator, in all 1,097.
SEP = "\r\n"

#: The directive letters, in the order :func:`format_directives` writes them --
#: which is the order all 514 shipped directive runs use.
ORDER = ("V", "P", "R")

#: Every letter the jump table @ 0x00481c10 accepts, with what it means and what
#: the engine uses when it is absent. ``I`` never appears in shipped data; its
#: meaning is inferred from the field it lands in.
MEANING = {
    "I": "min distance",
    "V": "volume (parsed and discarded)",
    "P": "pitch, in semitones",
    "R": "max distance",
}

#: The value the engine uses for a directive the text does not carry. A zero
#: distance or pitch is not literal zero -- it means "the sample's own default".
DEFAULTS = {"I": 0.0, "V": 100.0, "P": 0.0, "R": 0.0}

#: Parsed and thrown away by the engine. Authored anyway, so a re-export of a
#: shipped level does not quietly rewrite 514 emitters.
INERT = frozenset("V")

#: The volume a ``V`` of 100 stands for, which is what makes it a 0..1 fraction
#: in Blender. Shipped values run 5..100.
VOLUME_FULL = 100.0

#: Semitones per octave: ``P`` is divided by this before reaching the sample.
SEMITONES_PER_OCTAVE = 12.0


class EmitterError(Exception):
    pass


def is_emitter(text):
    """True when this text makes the dummy a sound rather than a marker.

    Case-insensitive on purpose: the engine's test is ``lstrcmpiA``, and 14 of
    the 1,097 shipped chunks say ``sound``.
    """
    return _lines(text)[0].strip().lower() == KIND.lower()


def _lines(text):
    return (text or "").replace("\r\n", "\n").split("\n")


def kind(text):
    """Line 1 exactly as written, so a lowercase ``sound`` survives a re-export."""
    return _lines(text)[0]


def wav(text):
    """Line 2: the ``.wav``, resolved by the sound system's own directory list.

    **Not** against ``SOUNDDIR``, which is inert. These live in ``Sound\\environ``
    and are indexed by ``Sound\\environ.dat``; all 1,097 shipped names are bare
    files with no folder part.
    """
    lines = _lines(text)
    return lines[1].strip() if len(lines) > 1 else ""


def tokens(text):
    """``[(letter, argument, start, end)]`` for every directive, with its span.

    The span is into ``text`` itself, which is what lets :func:`retext` rewrite
    one argument without touching a byte of anything else. Everything from line
    3 onwards is scanned, because one shipped chunk splits its directives across
    two lines.
    """
    out = []
    body = text or ""
    at = 0
    for _ in range(2):  # skip lines 1 and 2
        nxt = body.find("\n", at)
        if nxt < 0:
            return out
        at = nxt + 1
    i = at
    while i < len(body):
        if body[i].isspace():
            i += 1
            continue
        start = i
        while i < len(body) and not body[i].isspace():
            i += 1
        out.append((body[start], body[start + 1:i], start, i))
    return out


def values(text):
    """``{letter: float}`` for every directive the engine would act on.

    Lowercase letters are **excluded**, because the engine skips them in
    silence -- reporting them as if they worked would be the one lie this module
    exists to avoid. :func:`problems` names them instead.
    """
    out = {}
    for letter, arg, _s, _e in tokens(text):
        if letter not in MEANING:
            continue
        try:
            out[letter] = float(arg)
        except ValueError:
            continue
    return out


def effective(text):
    """``{letter: float}`` for all four, absent directives filled from the engine.

    This is what the emitter actually sounds like, which is what a UI showing
    distances and pitch has to display.
    """
    out = dict(DEFAULTS)
    out.update(values(text))
    return out


def problems(text):
    """Everything about this text the engine would ignore, as messages.

    A lowercase directive is the one that matters: it looks authored, parses
    fine to a reader, and does nothing at all.
    """
    out = []
    if not is_emitter(text):
        out.append("line 1 is %r, not %r -- this dummy is a marker, not a sound"
                   % (kind(text), KIND))
        return out
    if not wav(text):
        out.append("line 2 is empty; the emitter names no .wav")
    for letter, arg, _s, _e in tokens(text):
        if letter.upper() in MEANING and letter not in MEANING:
            out.append("%r is lowercase; the engine's jump table only accepts %s, "
                       "so it is skipped in silence"
                       % (letter + arg, "/".join(sorted(MEANING))))
        elif letter not in MEANING:
            out.append("%r is not a directive the engine knows" % (letter + arg))
        else:
            try:
                float(arg)
            except ValueError:
                out.append("%r has no number after it" % (letter + arg))
    return out


def format_number(value):
    """A directive argument, spelled the way the shipped ones are.

    Every one of the 1,540 shipped arguments is a canonical integer (``0``,
    ``20``, ``-2``, ``5000``), so a whole number must come out without a decimal
    point or an untouched emitter re-exports differently. The tolerance absorbs
    the float32 round trip through a Blender property -- a pitch of 2 semitones
    is stored as ``2**(2/12)`` and comes back as 1.9999998.
    """
    if abs(value - round(value)) < 1e-4:
        return "%d" % int(round(value))
    return ("%.4f" % value).rstrip("0").rstrip(".")


def format_directives(values_by_letter):
    """``"V40 P0 R0"`` -- **uppercase**, because lowercase is silently skipped."""
    parts = []
    for letter in ORDER + tuple(sorted(set(values_by_letter) - set(ORDER))):
        if letter in values_by_letter:
            parts.append("%s%s" % (letter.upper(), format_number(values_by_letter[letter])))
    return " ".join(parts)


def new_text(wav_name, values_by_letter=None):
    """The text for an emitter authored from nothing.

    Written in the majority shipped form -- ``Sound``, the wav, then the
    directives on one line -- and with no trailing CRLF, which is what 452 of
    the 514 directive-carrying chunks do.
    """
    wav_name = (wav_name or "").strip()
    if not wav_name:
        raise EmitterError("an emitter needs a .wav name; that is what line 2 stores")
    line3 = format_directives(values_by_letter or {})
    lines = [KIND, wav_name] + ([line3] if line3 else [])
    return SEP.join(lines)


def retext(text, wav_name=None, values_by_letter=None):
    """``text`` with only the parts that actually changed rewritten.

    Byte-exactness is the point. An emitter whose Speaker nobody touched has to
    come back out of Blender as the same bytes it went in as, and the shipped
    texts are too irregular to reproduce by reformatting -- so this replaces an
    argument in place, appends a directive the text did not carry, and otherwise
    returns its input unchanged.

    A value equal to the engine's own default for an *absent* directive is not
    written, which is what keeps "the file specifies no radius" distinguishable
    from "the file specifies zero".
    """
    if not text:
        return new_text(wav_name or "", values_by_letter)

    out = text
    wanted = dict(values_by_letter or {})
    present = {letter: (arg, s, e) for letter, arg, s, e in tokens(out)
               if letter in MEANING}

    # Back to front, so an earlier span is still valid after a later splice.
    edits = []
    appended = []
    for letter, value in wanted.items():
        letter = letter.upper()
        formatted = format_number(value)
        if letter in present:
            arg, start, end = present[letter]
            if formatted != arg:
                edits.append((start, end, letter + formatted))
        elif formatted != format_number(DEFAULTS.get(letter, 0.0)):
            appended.append(letter + formatted)

    for start, end, replacement in sorted(edits, reverse=True):
        out = out[:start] + replacement + out[end:]

    if appended:
        extra = " ".join(appended)
        # Re-tokenized rather than reusing the spans above: a replacement of a
        # different width has already moved every offset after it.
        spans = [e for _l, _a, _s, e in tokens(out)]
        if spans:
            # Onto the end of the existing directive run, not a fresh line.
            last = max(spans)
            out = out[:last] + " " + extra + out[last:]
        elif out.endswith(SEP):
            out += extra
        else:
            out += SEP + extra

    if wav_name is not None:
        lines = out.split(SEP)
        if len(lines) > 1 and lines[1].strip() != wav_name.strip():
            lines[1] = wav_name.strip()
            out = SEP.join(lines)

    return out


# --------------------------------------------------------------------------
# The engine's units, and Blender's
# --------------------------------------------------------------------------
#
# `P` is semitones and a Blender Speaker's `pitch` is a frequency multiplier, so
# the two are an octave apart by construction. The distances are passed straight
# through: the unit the engine reads them in is **not measured**, and the shipped
# values do not settle it either -- `R` takes 5, 10, 15, 20 (metres, if the map's
# units are metres) *and* 500 and 5000 (millimetres, like INDSOUND's) in the same
# game. So a value is shown as the number the file holds and nothing is scaled.


def pitch_to_factor(semitones):
    """``P`` -> a playback rate multiplier, which is what Blender's pitch is."""
    return 2.0 ** (float(semitones) / SEMITONES_PER_OCTAVE)


def factor_to_pitch(factor):
    """The inverse. A factor of 0 or less has no logarithm; it reads as no shift."""
    if factor <= 0.0:
        return 0.0
    return SEMITONES_PER_OCTAVE * math.log(float(factor), 2.0)


def volume_to_fraction(volume):
    """``V`` -> Blender's 0..1 speaker volume. Inert in the engine either way."""
    return max(0.0, min(1.0, float(volume) / VOLUME_FULL))


def fraction_to_volume(fraction):
    return max(0.0, min(1.0, float(fraction))) * VOLUME_FULL
