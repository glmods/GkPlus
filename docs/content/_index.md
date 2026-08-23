---
title: "Documentation"
description: "Tutorials, how-to guides, reference and explanation for GkPlus, the modding framework for Gunlok (2000)."
---

Every page here is one of four kinds, and they are not interchangeable: they answer
different questions and are written to be read in different states of mind. If you are not
sure which one you want, [the table below](#which-one-do-i-want) decides it in one look.

Nothing here assumes you have read anything else, but everything assumes you own a copy of
Gunlok, since GkPlus ships no game content. What GkPlus actually is, and why it is shaped like a
graphics driver, is [explanation](/explanation/why-gkplus-is-a-d3d8-dll/) rather than
something you need before you start.

## The four kinds of page

### [Tutorials](/tutorials/): learning, by doing something that works

Lessons you follow start to finish. Every command is given, every expected output is
shown, and no step asks you to choose. Follow one literally and it ends with something
visibly working: a version stamp on the main menu, a panel of your own drawn over a
running level, a DLL you built appearing in the game.

Three of them, one per audience: [Installing GkPlus](/tutorials/installing-gkplus/) for
players, [Your first GkPlus script](/tutorials/your-first-script/) for mod authors,
[Building GkPlus](/tutorials/building-gkplus/) for developers.

### [How-to guides](/how-to/): getting a specific job done

Recipes for a goal you already have. They assume competence, skip the teaching, cover the
variations that matter, and stop when the goal is met. Titled with a verb, so you can scan
for yours.

They are split by audience: [modding](/how-to/modding/): profiles, mods, textures,
renderer features, scripts and levels; and [development](/how-to/development/): builds,
tests, detours, bindings, crashes, profiling, Ghidra.

### [Reference](/reference/): looking something up

Descriptions of the machinery: what a key is, what a flag does, what a function takes.
Accurate, complete, and deliberately boring. It does not explain and it does not instruct.

Four sets: the [C++ API](/reference/cpp/) and the [JavaScript API](/reference/javascript/),
both generated from the source; and the hand-written
[configuration and tooling reference](/reference/data/): environment variables,
`settings.json`, the mod contract and the CLI tools, plus an
[index of the repository's design records](/reference/data/notes-index/).

### [Explanation](/explanation/): understanding why

Background you can read away from the keyboard. Why the framework is shaped like a
graphics driver, why a mod is never discovered by scanning a folder, why the renderer
intercepts at the device, what a pixel-difference number can and cannot prove. Rejected
alternatives are included, because the ones that were tried are the argument.

Start with [Why GkPlus is a d3d8.dll](/explanation/why-gkplus-is-a-d3d8-dll/).

## Which one do I want?

|  | Practical steps | Theoretical knowledge |
|---|---|---|
| **Serving your study** | [Tutorials](/tutorials/) | [Explanation](/explanation/) |
| **Serving your work** | [How-to guides](/how-to/) | [Reference](/reference/) |

If you are stuck partway through a job, you want a how-to. If a how-to told you to do
something and you want to know why, the explanation page is linked from it. If you need
the exact spelling of a key, that is reference. If you want to be walked through the
whole thing once, that is a tutorial.

The four-quadrant split is [Diátaxis](https://diataxis.fr/).

## Things that are not on this site

- **The reverse-engineering record.** Gunlok's own internals (the actor hierarchy, the
  trigger system, the GLS parser, the wire protocol, the save format, the renderer's
  measurements) live in the `*_notes.md` files at the repository root.
  [The design records index](/reference/data/notes-index/) says which file answers which
  question.
- **Anything from the game itself.** No Gunlok assets, text or code are redistributed
  here.
- **A prebuilt download.** There is no release channel; the DLL is built from source, and
  [Building GkPlus](/tutorials/building-gkplus/) is how.
