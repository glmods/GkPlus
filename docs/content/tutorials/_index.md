---
title: "Tutorials"
description: "Follow one start to finish and it ends with something visibly working. Three lessons, one per audience."
weight: 10
---

A tutorial is a lesson. Every command is given, every expected output is shown, and every
choice is made for you, so follow one literally, in order, without substituting your own
paths or values, and it will work. None of them explains why; the "why" is linked at the
end of each one.

There are three, and you want the one for the thing you are: play the game under GkPlus,
write scripts for it, or work on it.

- **[Installing GkPlus](/tutorials/installing-gkplus/)**: *for players.* Put GkPlus into a
  stock Steam copy of Gunlok, read the version stamp that proves it loaded, and change a
  setting in the profile the game writes for you. Needs a built `d3d8.dll`.
- **[Your first GkPlus script](/tutorials/your-first-script/)**: *for mod authors.* Write
  one `main.mjs` that adds a row to Gunlok's own menus, adds a toggle to Options, and then
  draws a live panel over a running level.
- **[Building GkPlus](/tutorials/building-gkplus/)**: *for developers.* Clone, configure
  with the CMake preset, build `d3d8.dll`, deploy it into the game, then change one string
  in `src/Font.cpp` and watch your own text appear on the main menu.

Take them in that order if all three apply to you. Each one ends by pointing at the
[how-to guides](/how-to/) for the same audience, which is where the branching, the options
and the real work begin.
