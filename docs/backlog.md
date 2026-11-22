# Backlog

What is left to do across the plans and the notes, in the order the user chose (2026-11-19): the smaller items first,
the larger ones after. Each item points at where its detail is kept; this file only tracks what is open. Remove an item
when it is done (its plan or note says how), and add one when a plan or a report leaves something open.

## Smaller

None open (2026-11-19: the preview's reload confirmed by the user; rhitest's NVIDIA device loss was fixed on 2026-11-15,
its draw recorded on a queue without graphics, and passes 10 of 10 and under validation on driver 610.88; a reload's
wait for the queues measured and left, docs/shader-compile-plan.md, step 6).

## Larger

- **bathroom2's remaining light where it isn't** (the user's report, 2026-11-21): the line along the ceiling's right
  edge, the space above the ceiling seen through a slot, which the last level lights through the bounce at 1.6 times the
  ceiling (dark at resolution 256); the statuettes in the niche under the window 47 to 56 of 255 where the finer
  voxels give 35 to 44 (the hits test only the voxels marked apart); a dark blob low on the bath's inner wall at
  `5.8027,7.2424,-12.9784,27.193,-39.740`, settled (43 to 47 of 255 where the finer voxels give 55: the last level
  through the finer clip's fade, a third darker there, the levels' disagreement below). And what testing
  where a voxel's light was gathered costs: 0.4 ms at bathroom2's pose, 0.65 at Sponza's, 1.3 at Bistro's (the marks
  made once a voxelizing, fewer rays at corners). docs/notes/indirect-lighting.md, "A voxel's light is taken only where
  it was gathered".
- **The clip levels' disagreement**: the levels disagree about the same place by 4 to 5 of 255, which shows while
  walking as darker patches on Sponza's corridor floor beside the columns (moving against settled 1.19, 1.52 and 1.65
  of 255 at the walk's three poses, 2026-11-19, `temp/gi-disocclusion/walkp.ps1`). The world-anchored grid built for it
  (docs/world-cache-plan.md) steadied the walk (1.13 at the far pose against 1.59) but costs 1.3 to 3.4 ms a frame and
  176 MB, so it is off by default (2026-11-20, step 5); next, a fade between the levels in the clipmap.
  docs/notes/indirect-lighting.md.
- **The surface cache's remaining noise**: MovingInterior's far wall, 0.44 of 255 between seeds with the window of 4096
  once settled; Bistro at night, the leading strip of a fast turn about twice the rest (more samples for new pixels: 32
  gives 15.6 in the emitters' view against 18.1). docs/notes/indirect-lighting.md.
- **Visual testing, steps 4 to 7**: a reference for the indirect lighting (a progressive path tracer, likely), golden
  images on lavapipe in CI, scenes with known answers, and CI's small additions; with the user's open calls (where
  stored references live, CI's time budget, which reference). docs/visual-testing-plan.md.
- **Performance**: the shadow maps of static casters cached (virtual shadow maps longer term), and step 5's rich
  worlds (model instances, streaming and level of detail, async compute, recording on several threads, quality presets
  aimed at a frame time). docs/performance-plan.md.
- **Volumetric effects**: clouds and fog banks (the user's wish, 2026-11-20). Their light in the air from the clipmap's
  volumes (the probes' light and the cache's, trilinear in hardware), which is why the world cache leaves the air to
  them (docs/world-cache-plan.md, step 4); clouds mostly the sun's and the sky's (docs/notes/sky.md).
