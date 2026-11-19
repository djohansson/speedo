# Backlog

What is left to do across the plans and the notes, in the order the user chose (2026-11-19): the smaller items first,
the larger ones after. Each item points at where its detail is kept; this file only tracks what is open. Remove an item
when it is done (its plan or note says how), and add one when a plan or a report leaves something open.

## Smaller

None open (2026-11-19: the preview's reload confirmed by the user; rhitest's NVIDIA device loss was fixed on 2026-11-15,
its draw recorded on a queue without graphics, and passes 10 of 10 and under validation on driver 610.88; a reload's
wait for the queues measured and left, docs/shader-compile-plan.md, step 6).

## Larger

- **The indirect lighting's light anchored in the world** rather than per clip of the clipmap (docs/world-cache-plan.md, side by side with the clipmap until shown better, the user's call 2026-11-19): the clip levels disagree
  about the same place by 4 to 5 of 255, which shows while walking as darker patches on Sponza's corridor floor beside
  the columns (moving against settled 1.19, 1.52 and 1.65 of 255 at the walk's three poses, 2026-11-19,
  `temp/gi-disocclusion/walkp.ps1`). The user chose a world-anchored replacement (multi-resolution cells of means) over
  a fade between the levels. docs/notes/indirect-lighting.md.
- **The surface cache's remaining noise**: MovingInterior's far wall, 0.44 of 255 between seeds with the window of 4096
  once settled; Bistro at night, the leading strip of a fast turn about twice the rest (more samples for new pixels: 32
  gives 15.6 in the emitters' view against 18.1). docs/notes/indirect-lighting.md.
- **Visual testing, steps 4 to 7**: a reference for the indirect lighting (a progressive path tracer, likely), golden
  images on lavapipe in CI, scenes with known answers, and CI's small additions; with the user's open calls (where
  stored references live, CI's time budget, which reference). docs/visual-testing-plan.md.
- **Performance**: the shadow maps of static casters cached (virtual shadow maps longer term), and step 5's rich
  worlds (model instances, streaming and level of detail, async compute, recording on several threads, quality presets
  aimed at a frame time). docs/performance-plan.md.
