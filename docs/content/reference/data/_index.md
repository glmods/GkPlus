---
title: "Configuration and tooling"
description: "Environment variables, settings.json, the mod contract, the CLI tools, and the index of the repository's design records."
weight: 30
---

The reference that is not a symbol table. Every page here is hand-written and every claim
on it cites the file and, where it matters, the line it was read from. These are the
surfaces a generator cannot see: variables read with `getenv`, keys in a JSON document,
flags parsed by `argparse`, panels registered with Blender.

Where a page would otherwise state a hand-maintained count, it gives the one-line command
that re-derives it instead. [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/) is the reason.

## Configuring a launch

- **[Environment variables](/reference/data/environment-variables/)**: every `GKPLUS_*`
  variable `d3d8.dll` reads, plus the ones the Python tools read, with accepted values,
  defaults and read sites.
- **[settings.json](/reference/data/settings-json/)**: the shared settings document: where
  it is, whose sections are in it, the `core` keys, when it is written, and what outranks
  it.
- **[Renderer setting keys](/reference/data/render-settings-keys/)**: every
  `core.render.*` key that persists, its type, and the two that behave differently from the
  rest.
- **[The profile directory](/reference/data/profile-directory/)**: what `GKPLUS_PROFILE`
  names, what GkPlus reads out of it, and which paths resolve against it.
- **[Mod metadata](/reference/data/mod-metadata/)**: the `metadata/` contract: `info.json`
  fields, the README, the icons, the `script` field, and every problem string a mod can
  report.

## The tools

- **[Blender addon](/reference/data/blender-addon/)**: `io_scene_rif`: its operators,
  their options, and the property panels it adds.
- **[gkpbr](/reference/data/gkpbr-cli/)**: the PBR map generator's subcommands, flags,
  environment and outputs.
- **[gklightmap](/reference/data/gklightmap-cli/)**: the lighting-map generator's
  subcommands, flags, channel layout and output paths.
- **[Command-line utilities](/reference/data/cli-utilities/)**: `rimutil`, `rifutil`,
  `riflights`, `vfdiff` and `symdump`: invocation, options, output and exit codes.
- **[The rendertest harness](/reference/data/rendertest-harness/)**: the seven PowerShell
  scripts in `utils/rendertest`, the functions each defines, and their parameters.

## The repository's own record

- **[Design records index](/reference/data/notes-index/)**: every `*_notes.md` at the
  repository root and the question it answers. The reverse-engineering record is not
  reproduced on this site; this is the map into it.
