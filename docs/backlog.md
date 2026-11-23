# Backlog

What is left to do across the plans and the notes, in the order the user chose (2026-11-19): the smaller items first,
the larger ones after. Each item points at where its detail is kept; this file only tracks what is open. Remove an item
when it is done (its plan or note says how), and add one when a plan or a report leaves something open.

## Smaller

None open (2026-11-19: the preview's reload confirmed by the user; rhitest's NVIDIA device loss was fixed on 2026-11-15,
its draw recorded on a queue without graphics, and passes 10 of 10 and under validation on driver 610.88; a reload's
wait for the queues measured and left, docs/shader-compile-plan.md, step 6).

## Larger

- **The research scenes as the user saw them in the client** (the user's review, 2026-11-23, eleven reports): the
  emitters' caustic photons carry the light through glass that doesn't focus (speckles in lamp, modern-hall,
  spaceship) and mottle cornell-caustic's hidden places; the surface cache's finest entries show as blocks and teeth
  (classroom, veach-ajar, cornell-box, the bathroom's mirror); the indirect light takes 4 to 6 seconds to settle, with
  splotches moving meanwhile (kitchen, living-room, veach-bidir). Six steps, the photons first.
  docs/research-scenes-review-plan.md.
- **The thorough voxel test for every ray** (the user's call, 2026-11-22): the hits' voxel choice is thorough
  (`GiHitVoxel<kThorough>`: other surfaces' voxels tested too, not only those marked apart) for the main pass's near
  field alone; try it for the reflections' near field (`GiNearTrace<false>` in `traced.slang`), the surface cache's and
  the probes' rays (`GiHitLitAt<false>`, `GiHitRadiance<false>` in `GiTraceBvh`), each and together, and keep what gives
  the most visual quality for a reasonable frame time: by every ray, Bistro's frame took 0.26 to 0.30 ms more, which the
  user takes as acceptable where it clearly improves the picture. Measure what each shows (bathroom2's niches and
  mirrors, Sponza's curtains, Bistro's frames and shutters, MovingInterior's corners) and what it costs.
  docs/notes/indirect-lighting.md, "One rule for every lookup and hit".
- **A voxel's surface chosen by how much of it each facing covers** (deferred, the user's call, 2026-11-22): the
  voxelizer keeps one surface a voxel, by the order of its normal's bits (the axis, then the octahedral bits, then the
  plane), so at a coarse level a thing smaller than a voxel takes voxels that are mostly another surface's, and a wall
  meeting others in a corner can lose its voxels there. What shows now, in bathroom2's niche under the window at the
  room pose (`4.4431,16.9345,49.9102,-1.189,-8.257`): a dark line some 4 pixels wide where the side wall meets the back
  wall, 17 to 20 of 255 where the voxels a quarter the size give 29 to 32, and 3 to 5 lighter beside the leftmost
  statuette; the rest of the niche within 1 to 2. Choosing by coverage (each facing's samples counted a voxel, a second
  pass or more atomics) would cost the voxelizing, at every scroll and every frame for what moves, and change which
  surface every coarse voxel stands for in every scene; keeping the frontmost of a facing's surfaces instead brought up
  dark blocks beside the bath's taps. Revisit if such a corner shows. docs/notes/indirect-lighting.md, "The niche
  behind the statuettes".

- **bathroom2's remaining light where it isn't** (the user's report, 2026-11-21): the line along the ceiling's right
  edge, the space above the ceiling seen through a slot, which the last level lights through the bounce at 1.6 times the
  ceiling (dark at resolution 256); and walking into the niche under the window, the young entries show the probes'
  light, which leaks there (4 to 5 of 255 for some 30 frames). And what testing
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
  gives 15.6 in the emitters' view against 18.1); and Bistro at night's lit window interior at the regression set's
  pose, settled, 53 or 59 of 255 from run to run (3 runs of 13, 2026-11-22). docs/notes/indirect-lighting.md.
- **Visual testing, steps 4 to 7**: a reference for the indirect lighting (a progressive path tracer, likely), golden
  images on lavapipe in CI, scenes with known answers, and CI's small additions; with the user's open calls (where
  stored references live, CI's time budget, which reference). docs/visual-testing-plan.md.
- **Performance**: the shadow maps of static casters cached (virtual shadow maps longer term), and step 5's rich
  worlds (model instances, streaming and level of detail, async compute, recording on several threads, quality presets
  aimed at a frame time). docs/performance-plan.md.
- **The emitters' caustics beyond the views** (2026-11-23, the research scenes plan's step 8; in cells of the world
  since the review plan's step 2): the photons land in the cells of what the views hold, shown or hidden; what is out of
  every view takes the voxels' light straight through glass (a coarse level of cells for every landing would give it
  them, and what comes into view its light at once), and the indirect lighting's voxels take it so too (their light the
  photons' in their place would bounce the caustics: the cells could hold it); the highlights through solid glass are
  the straight light's (the photons bring irradiance alone: a direction kept with it would let the specular take them);
  and cornell-caustic's ceiling is darker than its reference's (what the waves reflect up, and the indirect light).
  docs/notes/rendering.md, "The emitters' caustics".
- **The traced hits' light sample out of the transmissive fragment shader** (2026-11-23, the review plan's step 1): a
  refraction's hit the screen hides takes a light sample (`TracedEmitterHidden`), and the rule that tells glass that
  gathers light from glass that doesn't made that shader slower by its size alone, whatever it traced: cornell-caustic's
  Main Transmissive 3.3 to 3.95 ms a frame at 1440p balanced, glass-of-water's 11.3 to 13.3. The direct light kept in
  cells of the world as the caustics' are, sampled in a compute pass for the cells the hits ask for, would take the
  sample and the rule out of it. docs/notes/rendering.md, "The emitters' caustics".
- **glass-of-water at 60 Hz** (2026-11-23, the research set timed at 2560x1440, balanced: every other scene within it):
  19.5 ms a frame (21.9 since the review plan's step 1, above), 11.3 of it Main Transmissive (13.2), its glass, water, ice and bubbles' refractions through up to
  `SHADER_TYPES_CAUSTIC_SURFACE_COUNT` surfaces each, a hit shaded at the end; the next slowest, crown 14.7 ms and bmw-m6
  14.5, Main Transmissive 3.3 and 3.7. docs/research-scenes-plan.md, "Where it stands".
- **Rough glass closer to its references** (the user's call, 2026-11-23: step 6's lobe ray an improvement, not yet the
  references' look): lamp's frosted bulb still shows a thin dark edge along its rim where the reference's glows to its
  silhouette, and past roughness 0.15 most of a rough surface's light is still the transmission texture's mips, a blur
  of the screen at the surface's own place by the roughness alone, not by how far behind it the scene is. The plan's
  own design is the next step: the nearest glass layer's reflection and refraction each a ray of its lobe a pixel,
  filtered and accumulated by Reflection passes of its own, as the visibility buffer's glossy reflections are, with the
  traced share then as far as theirs (tried without the filter: bathroom2's jar's sides went orange). Judge by lamp's
  bulb, bathroom2's jar and the glass sample models (TransmissionRoughnessTest). docs/research-scenes-plan.md, C2 and
  step 6; docs/notes/rendering.md, "Traced reflections and refractions".
- **Volumetric effects**: clouds and fog banks (the user's wish, 2026-11-20). Their light in the air from the clipmap's
  volumes (the probes' light and the cache's, trilinear in hardware), which is why the world cache leaves the air to
  them (docs/world-cache-plan.md, step 4); clouds mostly the sun's and the sky's (docs/notes/sky.md).
