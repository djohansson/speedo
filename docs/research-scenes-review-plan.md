# Research scenes review plan: what the user saw in the client

The user's review of the glTF research set's extended scenes in the client (2026-11-23, after the research scenes plan's
steps 1 to 10, 5e25adcf; a 3840x2160 window): eleven reports, each with its Edit > Copy Scene line below. This plan
says what each one is, measured, and how to fix them, the causes first. It was written for the user's review; nothing
of it is built yet.

## The reports

| # | Scene | What the user saw |
|---|---|---|
| 1 | cornell-box | Top of the leftmost box has regular, jagged artifacts. |
| 2 | bathroom | The left wall reflected in the mirror has bright speckled noise. |
| 3 | classroom | The indirect light settles slowly and in large square patches (voxels?) with sharp discontinuities, not interpolated with their neighbours. |
| 4 | cornell-caustic | The caustics settle slowly, and look unnatural behind objects after the camera moves. |
| 5 | kitchen | The indirect light settles slowly on the cupboards; noise patterns and splotches move around while it settles. |
| 6 | lamp | Bright single-pixel white speckles across the image; seen in other scenes too. |
| 7 | living-room | The indirect light settles slowly on the sofa and the bright areas in the windows; large noise patterns move around while it settles. |
| 8 | modern-hall | Bright high-frequency noise on the wall to the right of the first steps. |
| 9 | spaceship | Red and pink single-pixel speckles on the ground and the window around the pink reflection; some fade over time. |
| 10 | veach-ajar | The indirect light settles slowly; large square voxel artifacts, not interpolated. |
| 11 | veach-bidir | The indirect light settles slowly; bright splotches move around on the left wall. |

The poses (every line also has `SPEEDO_GROUND_PLANE=0 SPEEDO_AUTO_EXPOSURE=0 SPEEDO_SOLID_TRANSMISSION=1 SPEEDO_GI=1`;
`<set>` is `resources/test-assets/gltf-research-scenes/scenes/gltf_research_scenes_glb/gltf_research_scenes/scenes`):

```
1  SPEEDO_CAMERA=0.0000,1.0000,6.8000,-0.000,0.000 SPEEDO_AUTOLOAD_MODEL='resources/test-assets/gltf-research-scenes/files/scenes/cornell-box/gltf/cornell_box_extended.gltf' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_FOCAL_LENGTH=69.84
2  SPEEDO_CAMERA=0.0072,0.9124,-0.2276,-0.886,-48.761 SPEEDO_AUTOLOAD_MODEL='<set>/bathroom/gltf/bathroom_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_FOCAL_LENGTH=23.05
3  SPEEDO_CAMERA=-1.6905,1.2716,5.8865,-1.622,8.705 SPEEDO_AUTOLOAD_MODEL='<set>/classroom/gltf/classroom_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='<set>/classroom/gltf/classroom_environment.hdr' SPEEDO_FOCAL_LENGTH=36.95
4  SPEEDO_CAMERA=1.2370,0.8982,3.5203,9.417,-22.307 SPEEDO_AUTOLOAD_MODEL='<set>/cornell-caustic/gltf/cornell_caustic_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_FOCAL_LENGTH=69.84
5  SPEEDO_CAMERA=1.2110,1.8047,3.8524,2.388,-26.007 SPEEDO_AUTOLOAD_MODEL='<set>/kitchen/gltf/kitchen_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_FOCAL_LENGTH=36.95
6  SPEEDO_CAMERA=7.7560,5.0680,-6.6435,12.797,-136.904 SPEEDO_AUTOLOAD_MODEL='<set>/lamp/gltf/lamp_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_FOCAL_LENGTH=38.06
7  SPEEDO_CAMERA=5.1052,0.7311,-2.3179,-4.267,-105.320 SPEEDO_AUTOLOAD_MODEL='<set>/living-room/gltf/living_room_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='<set>/common/sunflowers_environment.hdr' SPEEDO_ENVIRONMENT_INTENSITY=13.004 SPEEDO_FOCAL_LENGTH=21.33
8  SPEEDO_CAMERA=6.9118,1.6516,2.5541,0.000,-64.181 SPEEDO_AUTOLOAD_MODEL='<set>/modern-hall/gltf/modern_hall_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_ENVIRONMENT_INTENSITY=1.181 SPEEDO_FOCAL_LENGTH=17.14
9  SPEEDO_CAMERA=-0.5197,0.8170,3.8244,2.962,7.824 SPEEDO_AUTOLOAD_MODEL='<set>/spaceship/gltf/spaceship_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0.3,0.3,0.3' SPEEDO_FOCAL_LENGTH=36.95
10 SPEEDO_CAMERA=4.0540,1.6165,-2.3065,1.851,-82.109 SPEEDO_AUTOLOAD_MODEL='<set>/veach-ajar/gltf/veach_ajar_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_ENVIRONMENT_INTENSITY=1.181 SPEEDO_FOCAL_LENGTH=36.95
11 SPEEDO_CAMERA=16.2155,4.0517,0.0115,0.268,-89.712 SPEEDO_AUTOLOAD_MODEL='<set>/veach-bidir/gltf/veach_bidir_extended.glb' SPEEDO_AUTOLOAD_ENVIRONMENT='uniform:0,0,0' SPEEDO_ENVIRONMENT_INTENSITY=1.181 SPEEDO_FOCAL_LENGTH=38.06
```

## How they were reproduced

Each pose captured headless at 3840x2160 with the user's variables, frames counted as 60 Hz
(`SPEEDO_TIME_FRAME_SECONDS` and `SPEEDO_ANIMATION_FRAME_SECONDS` 1/60), repeatable, the upscaler at its default
(balanced); then the same with one part of the light switched off or shown alone (`SPEEDO_EMITTER_CAUSTIC_PHOTON_COUNT=0`,
`SPEEDO_GI_CACHE=0`, `SPEEDO_GI_NEAR=0`, `SPEEDO_GI_LOOKUP_LEVEL`, the `direct`, `emitters`, `indirect-diffuse` and
`traced-reflections` debug views). Settling was measured as the mean |difference| from frame 600 over the reported region,
of 255, at frames 60 to 540; speckles as the pixels brighter than their 5x5 window's median by more than 20 of 255 in a
channel, per million pixels. cornell-caustic's camera move: 600 frames 0.67 m to the left of the user's pose, 30 frames
to it, 900 there.

## What they are: three causes

### A. The emitters' caustic photons (research scenes plan step 8): reports 4, 6, 8, 9

With the set's conditions (`SPEEDO_SOLID_TRANSMISSION=1`) every glass surface counts as solid, so the diffuse's shadow
rays stop at all of it and the photons carry all the light that passes through glass. They are 262144 a frame for
8.3 million pixels: a few photons a pixel even after the 256 still frames the history averages.

- lamp, modern-hall, spaceship: the speckles are photons. With `SPEEDO_EMITTER_CAUSTIC_PHOTON_COUNT=0` the lamp's floor
  grain, the hall's wall noise (5847 speckles a million pixels, 2088 without, the rest edges and texture) and the ship's
  pink dots are gone. None of these is a caustic: the lamp's emitter sits inside its bulb, the hall's wall lights shine
  through the flat glass rail, the ship's red lights sit inside its dome. That glass barely turns the light, and a
  shadow ray through it gave the right light without noise.
- cornell-caustic: the places the screen hides and a refraction shows are lit by the world cells
  (`gEmitterCausticCells`): the floor behind the small cube and the wall strips beside the tall box. On arriving after
  the move they are coarse grain with holes, and after 900 frames they are still mottled, as the still pose is after
  1500 frames. The cells are 2 pixels wide at the eye's distance when a photon lands. Each cell gets few photons, and
  after a move new cells are made at new sizes, so the mottle starts over.

### B. The surface cache's finest entries show: reports 1, 2, 3, 10

All four are gone with `SPEEDO_GI_CACHE=0` (the probes alone, smooth and slower):

- classroom: the wall above the windows is blocks a voxel of the finest level wide (about 7 cm, 70 pixels there), with
  sharp edges. They are the same through level 0 alone, softer through level 1 alone and absent through level 2 alone.
  With the near field off they are larger and sharper: the near field hides part of them. They differ between frames
  120 and 1800, so they are light, not texture.
- veach-ajar: square blocks around the door's handle and steps across the door, sharp edged.
- cornell-box: teeth along the tall box's top edge, in the indirect light alone (the direct and emitter views are clean).
  The box is turned against the voxel grid, so along the edge the voxels hold the top face or the side face in turn,
  and the side face's lookups beside the edge take the top face's light in a pattern that beats with the grid.
- bathroom: the wall in the mirror is mottled by the light the reflection's hits take from the voxels. A hit off the
  screen takes its emitters' light from the one voxel it is in (`TracedEmitterKept`, no interpolation), and that kept
  light comes from the cache entries' few samples. With the cache off the reflected wall is smooth.

Blocks with sharp edges mean the lookup does not blend across neighbouring entries: if it interpolated, entries that
differ would blur into each other. The likely place is a tap dropped rather than faded (the far-sides and inside rules,
or the anchored lookup choosing a voxel); a dump of the taps across an edge will tell. That neighbouring entries differ
at all is the entries' own noise (each its own rays).

### C. The indirect light settles slowly, and splotches move while it does: reports 3, 5, 7, 10, 11

The emitters' own light is within about 1 of 255 of its frame-600 value by frame 60. The indirect light is far from it:

| scene (region) | lit, frame 60 / 120 / 240 / 360 | indirect | indirect, cache off |
|---|---|---|---|
| living-room (sofa) | 20.4 / 12.4 / 4.9 / 3.2 | 22.0 / 13.4 / 5.1 / 3.4 | 37.5 / 30.1 / 11.3 / 5.1 |
| kitchen (cupboards) | 17.5 / 13.5 / 7.2 / 2.4 | 36.3 / 27.0 / 13.9 / 4.2 | 43.9 / 34.5 / 13.1 / 4.9 |
| veach-bidir (left wall) | 14.0 / 8.4 / 4.1 / 3.8 | 18.9 / 11.2 / 5.5 / 5.0 | 35.5 / 28.2 / 11.0 / 5.2 |

So the light takes 4 to 6 seconds to come within a few of 255, the cache speeds it up, and the probes alone are slower
still. The averaging over time (the probes' blend, the entries' windows, a bounce a frame, how often a still block
updates) is set for a still scene's noise, not for a cold start. veach-bidir's left wall is lit mostly by a small bright
spot, where its floor lamp lights the ceiling. Without the cache the wall shows lens-shaped blobs, the probes' pattern;
with it, vertical streaks that move between frames and are still visible at frame 600. Few of each probe's and entry's
rays meet that spot, which is a sampling problem, not a resolution one. The kitchen's cupboards have single white dots
for the first two or three seconds, which fade.

## The plan

In order: the photons first (from today's push, three scenes, the smallest change), then the cache's lookups (four
reports, one cause), then the settling (the largest). Every step is judged at the reports' poses at 3840x2160 with the
measures above, against the research comparison (`scripts/researchcompare.ps1`) and the GI regression scenes (Sponza's
columns, the bathroom scene's bench, Bistro, CornellBox), and by the user's eye; costs at the target (1440p balanced).

1. **Light through glass that doesn't focus goes straight** (A; reports 6, 8, 9). Decide per glass instance whether it
   focuses, from the photons already traced through it: an instance that turns nearly all of its photons by less than a
   couple of degrees (a pane, a bulb with its emitter inside, a dome) lets the diffuse's shadow rays through, with its
   Fresnel transmittance and absorption, and no photons are aimed at it. The other way, from the geometry at load
   (a closed shell with parallel faces), misses the emitter inside a single-sheet bulb. Gate: lamp, modern-hall and
   spaceship as speckle-free as with the photons off, at about the same light; cornell-caustic's water, glass-of-water
   and veach-bidir's egg keep their caustics.
2. **The caustics' density, estimated as photon mapping does** (A; report 4). On the screen, a gather radius set by
   how many photons are around a pixel, shrinking as they accumulate over still frames (progressive photon mapping):
   smooth where photons are few, detailed where they are many, which also answers the earlier wish for the caustics'
   lines behind the right cube. In the world, cells on a fixed ladder of sizes rather than the eye's distance, every photon
   into each, read at the finest with enough photons. They survive a camera move, and shown photons fill them too, so
   what a move reveals is lit already. Gate: cornell-caustic at the user's pose, still and after the walk (the mottle
   between seeds in the hidden places, the time to settle after the move), glass-of-water, the cost.
3. **The cache's lookups continuous** (B; reports 1, 3, 10). First the dump: a row of pixels across a block edge on the
   classroom's wall and across the cornell box's teeth, with each tap, its weight, its entry's light and sample count.
   Then the rules that drop a tap fade it instead (weights by how far the tap's surface is from the pixel's plane and
   how alike their normals are, then normalized), and at a convex edge a tap holding the other face weighs by its
   normal. If neighbouring entries still differ more than their noise, the finest level's filter along surfaces gets a
   wider reach. This may cover the backlog's "thorough voxel test for every ray" in part. Gate: the three poses, the
   regression scenes, and the bathroom scene's thin boards and niche poses (docs/notes/indirect-lighting.md), where
   these rules were made.
4. **The reflections' hits take the kept light interpolated** (B; report 2): the same continuous lookup over the eight
   voxels as step 3, in place of the one voxel, or the filtered entries' light. Gate: the bathroom's mirror; the cost in
   Bistro at night, where searching the voxels beside a hit cost 1.2 ms before.
5. **A cold start that settles in about a second** (C; reports 3, 5, 7, 10, 11). Measure each stage's time constant on
   a cold start first. Then young probes and entries take the mean of all their samples so far (not a blend at a fixed
   rate), get more rays while many are young (after a load, a jump of the camera, a large change of the lights), and are
   filtered wider along surfaces while young, narrower as they settle (as step 2's radius). Gate: the settling table
   above, frames 60 and 120 within a few of 255; the settled images unchanged; the frame time while settling against
   settled.
6. **Small bright places sampled by importance** (C; reports 11, 5): the probes' and the cache's rays aimed at what is
   bright in their view (the lit ceiling spot) and weighed by that density, each sample correct as it is. Gate:
   veach-bidir's wall, frame to frame and between seeds once settled; the kitchen's dots.

## Open for the user

Nothing now.

## Progress

- 2026-11-23, **step 1**: the rule is decided at each shadow ray, by the faces it meets, not for each glass instance
  by how far its photons are turned as the plan had it: lamp's bulb turns its photons up to 23 degrees and gathers
  nothing, and the set's glass measured (`temp/research-issues/glass.ps1`) showed what tells the cases apart. A ray lets
  the light through out of a body its emitter is inside, through a slab or a shell (the faces it goes in and out by
  within 2 degrees, a curved one's other face within a quarter of its radius), and by a flat face to a surface inside or
  out by another flat face; the photons follow the same rule and drop what a flat face reflects
  (`EmitterGatheredByGlass`, docs/notes/rendering.md, "The emitters' caustics"). At the reports' poses, 3840x2160, pixels
  a million brighter than their window's median by 8 of 255: lamp 3682 to 2385 (2396 with the photons off), modern-hall
  37687 to 13823 (13461), spaceship 4805 to 3121 (3107), each as bright as with the photons off; cornell-caustic's and
  veach-bidir's caustics as before. The research set: modern-hall 0.178 to 0.170 blurred stops off its reference, the
  rest within 0.003. The cost is the transmissive fragment shader's size: cornell-caustic's Main Transmissive 3.3 to
  3.95 ms a frame at 1440p balanced, glass-of-water's 11.3 to 13.3, whatever the rule traced (three ways of fewer or
  shorter queries measured the same); step 2 is to take the hits' light sample out of that shader.
- 2026-11-23, **step 2**: the caustics are kept in the world, not on the screen: every photon that lands in a view lands
  in cells of three sizes (2, 4 and 8 pixels where it lands, by the pixel's footprint on the surface), each the sum of
  its photons since it was made, up to 4096 frames; a lookup takes the coarsest level and each finer one over it by the
  photons its cells hold, so the picture is smooth from the start and sharpens as the photons come, and a move of the
  camera leaves it as it was. The plan's radius by the photons around a pixel is these levels. At the user's pose in
  cornell-caustic, 3840x2160: the grain 44591 to 6246 pixels a million (brighter than their window's median by 8 of 255)
  at the same light; from a cold start 8.0 of 255 off the settled frame after 1 second, 3.4 after 3, 2.1 after 7; on
  arriving after the move the places behind the cubes are as the wall around them, where they were holes and grain.
  docs/notes/rendering.md, "The emitters' caustics". Not done: the hits' light sample out of the transmissive fragment
  shader (step 1's cost stands, docs/backlog.md).
- 2026-11-23, **step 3**, looked into, nothing built: the blocks and teeth are where a lookup leaves voxels of its 8 out
  and takes the others by their weights' sum (`GiSampleClipWithout`). Where a left out voxel is the nearest, the others'
  weights are all small and their ratio jumps from one kept voxel's light to the next's across it: a square a voxel
  wide. Marked in the indirect diffuse view, most of the classroom's ceiling, walls and floor leave voxels out: the
  sight test (`GiSidesHidden`) leaves out surface voxels only (never the air's), and the far sides' rule
  (`GiFarSides`) leaves out more in patches of the floor, the windows and stripes down cornell-box's faces. With the far
  sides' rule off (`SPEEDO_GI_CACHE_THIN=0`) cornell-box's teeth are nearly gone and the classroom's wall is smoother;
  with the sight test off (`SPEEDO_SKIP_PASSES="GI Sides"`) the classroom leaks light in wedges (it is needed);
  veach-ajar's squares around the door's handle stay either way. Tried: what the left out voxels would have counted
  given to the next clip's light at the point (continuous where the clips are). The classroom's wall was then the next
  level's own blocks, larger, and a bright wedge where that level's lookup has no sight test of its own (the pass
  tests the clips that light the point by their weights, one near the eye). Open, for the user: how the left out
  voxels' share is filled so that the light stays continuous (below).
- 2026-11-23, the user confirmed steps 1 and 2 by eye. Left in spaceship: specks around its display seen through the
  dome (docs/backlog.md). Step 3, the user's call: (c) first, then (a), and (b) only if those two aren't enough.
- 2026-11-23, **step 3**, (c) and (a) tried, nothing kept (the experiment is `temp/research-issues/step3-across-fill.patch`):
  - (c): the voxels the sight test leaves out are never the surface's own. Marked by how they face the lookup's surface,
    they are surface voxels facing against it (a wall's or a ceiling's other side) or across it (the grooves between the
    classroom's ceiling tiles, cornell-box's top face beside its sides); none facing its way, and no air. They are left
    out for what they are: nothing to gain by leaving fewer out.
  - (a): each left out voxel filled with the mean of its grid neighbours that may stand for the surface (in front of its
    plane, air or facing its way, not left out themselves, no further off the surface). The classroom's wall a little
    smoother and 3 of 255 brighter; cornell-box's teeth and veach-ajar's squares as they were. Not the cause alone.
  - What does remove them: the voxels facing across or against the surface left out everywhere, not only where their
    apex doesn't see the point (their light is of the half of the directions their own surface faces; the sight test
    takes them in some columns of voxels and not in others, which is the teeth), in the sides' mask (`GiSidesMask`: in
    the lookup itself the shaders took 11 minutes to compile), with (a)'s fill. veach-ajar's squares gone, the
    classroom's ceiling and wall smooth, cornell-box's left face without teeth (the front face faintly). But the rooms
    come out brighter and living-room's ceiling takes a bright line along its cornice and its shutters more light: with
    the surfaces' voxels gone from a lookup among mouldings, what is left is the air's light, the probes', which leaks.
    The research set (blurred stops off the reference, the light's ratio): living-room 0.305 to 0.363 (+0.00 to +0.06),
    classroom 0.272 to 0.294 (-0.04 to +0.06), bathroom 0.224 to 0.238; pontiac-gto 0.276 to 0.252, veach-ajar 0.201 to
    0.194, spaceship 0.202 to 0.195, bathroom2 0.197 to 0.192; 9 scenes nearer and 7 further. Only where one of the
    lookup's 8 holds the surface's own voxel: living-room 0.358, classroom 0.290, the line still there.
  Open: what a lookup takes in place of the voxels that face another way, where the air's light isn't to be trusted.
- 2026-11-23, **step 3**, the user's call after seeing both: keep the facing rule and fix its leak. The leak was the
  fill's, not the rule's: with the rule alone the rooms are as near their references as before or nearer (living-room
  0.305 to 0.302, classroom 0.272 to 0.266, bathroom the same) and living-room's cornice is as it was. So the fill is
  gone, and what is kept is the rule, in the GI Sides pass's masks (`GiSidesMaskFacing`): a pixel's lookups leave out
  the voxels that hold a surface facing more than 84 degrees off its own, where one of their 8 holds the pixel's own
  surface. cornell-box's left face is without teeth (report 1; its front face faintly still), veach-ajar's squares gone
  but a small one by the door's handle (report 10); the research set 7 scenes nearer, 2 further by 0.005 at most
  (teapot, living-room-2); the indirect lighting's regression poses within 1.1 of 255, no leak new; the GI Sides pass
  cheaper (Bistro's 1.09 to 0.73 ms a frame at 1440p balanced), the frame with it. docs/notes/indirect-lighting.md, "The
  voxels that face another way are left out too", has what was tried on the way.
  Left of step 3: the classroom's patches (report 3) are not the lookups'. They are in the cache's own light
  (`SPEEDO_GI_CACHE_SHOW=1`), rectangles the size of its blocks of 4 voxels with straight edges; and the bathroom's
  mirror (report 2, step 4).
- 2026-11-23, **step 3**, the classroom's patches (report 3) looked into: they are single voxels of the finest level,
  0.5 m a side, and not the cache's blocks nor its filter (the same with `SPEEDO_GI_CACHE_SIGMA` 0 or 1000, one filter
  pass, `SPEEDO_GI_CACHE_EVERYWHERE=1`, and at 3600 frames as at 900: they are not still settling). Two causes:
  - **The model's floor is 222 m wide** (`35_Floor`, around a room of 9 by 12 m), and the last level covers the scene at
    128 voxels: 1.98 m a voxel, the finest of 3 levels 0.495 m. Twelve of the set's scenes have such a floor or backdrop
    (`temp/research-issues/glbbounds.ps1`: vintage-car 227 m, crown 200, teapot 160, pontiac-gto 118, lamp 114,
    spaceship 78, bathroom2 76, house 66, bmw-m6 55, veach-mis and glass-of-water 47), their finest voxels 0.09 to
    0.44 m where Sponza's are 0.06.
  - **At that size nearly every lookup in the room leaves out most of its 8 voxels**, and what is left is one column or
    two of the four along the surface, so the light is one voxel's, flat, with a step to the next
    (`temp/review/research-issues/s3-classroom-columns.png`: blue 4 columns kept, green 3, yellow 2, red 1, white
    none; `s3-classroom-columns-by-rule.png`: by the masks, by the sight test alone, by the far sides, by the voxels'
    weights). The far sides rule (`GiFarSides`) leaves out the most (the ceiling beside its beams and lights, the floor
    under the desks, the wall beside the blackboard: each voxel stands for one of several surfaces in it, and the
    neighbour's is another), the sight test at the junctions, the facing rule under the desks and by the windows.
  Tried, with the levels' limit raised to 8 (`temp/research-issues/levels-8.patch`: the constant, and the level in the
  sides' masks in three bits): `SPEEDO_GI_CLIP_COUNT` 4, 5 and 6, finest voxels 0.247, 0.124 and 0.062 m. With 6 the
  ceiling's patches are gone, the desks and chairs take their contacts' light, and what is left is a few squares on the
  far walls (`s3-classroom-3-vs-6-levels.png`), which a coarser level lights from there: a voxel is never smaller than
  a 32nd of the distance to its level's edge, so more levels don't change how large a far wall's voxels look. The cost
  at 1440p balanced: the frame 7.74 ms with 3 levels, 8.18 with 4, 8.36 with 5, 8.50 with 6 (the GI pass 0.52 to 0.72,
  GI Sides 0.47 to 0.67, the main pass 1.03 to 1.17), the indirect lighting's memory 146, 189, 231 and 278 MB.
  Not changed yet: how many levels a scene gets is the user's call ("Open for the user").
- 2026-11-23, **step 4**, the bathroom's mirror (report 2): three things were wrong with the wall it shows, none of
  them the one voxel's light the plan named first.
  - **The wall's blocks were never read.** It is beside the camera, off the screen, so no pixel's lookup stamps its
    blocks, and the cache traces them a ray every 8th update and doesn't filter them: bright blotches for the first
    seconds, a mottle after 15 (smooth with `SPEEDO_GI_CACHE_EVERYWHERE=1`). The reflections' hits stamp the blocks
    they read now (`GiMarkSeenHit`), where the ray shows the hit no wider than a voxel, every 8th update (traced 3
    updates of 8, filtered throughout), in the visibility buffer's reflections alone.
  - **The wall took no emitters' light off the screen.** The wallpaper is a double sided sheet, each side a voxel's,
    and the hit's voxel, a twentieth in, was the other side's: `TracedEmitterKept` found nothing facing the hit. It
    looks in the voxel beside it out along the normal too, and takes the 4 columns around the hit by their shares, in
    the clips that light it as their lookups blend them: the one voxel showed as squares of the last level's voxels
    while their light settled, the finest clip alone as a step where the next takes over.
  - **A patch of grain under the shelf**, where the wall is on the screen behind the bath: such a hit took a light
    sample of its own while the caustics are traced (`TracedEmitterHidden`), a sample a pixel a frame, which a sharp
    reflection doesn't filter. It takes the voxels' light now where the sample's emitter's light meets no glass that
    gathers it on the way (shadowed or not), the cells' caustics as before where it does.
  The wall in the mirror is smooth from 5 seconds on, soft blobs before that as the voxels' light settles (step 5's),
  where it was blotches, then a mottle and the patch (`temp/review/research-issues/s4-bathroom-mirror-final.png`: before
  at 900 frames, after at 60, 300 and 900). bathroom2's mirror the same way (`s4-regress-bathroom2-mirror.png`).
  cornell-caustic's cubes and walls as they were; the strip of its right wall at the picture's edge, seen off the
  screen through the water, is lit now as the wallpaper is (`s4-regress-cornell-caustic-strip.png`: the voxels' light,
  straight through the water, where it had none). The regression poses within 1.1 of 255 of before but the mirrors'
  (2.4). The cost at 1440p balanced, interleaved with HEAD's: Bistro's frame 21.7 to 22.2 ms by day (the GI pass 3.53
  to 4.10, the blocks its reflections show; Reflection Trace 6.14 to 6.36; Main Transmissive 5.67 to 5.79), about
  the same more at night; the bathroom's 11.1 to 11.5 (Reflection Trace 2.50 to 2.72, GI 1.32 to 1.46);
  cornell-caustic's 10.2 to 10.4; Sponza's 7.8 to 8.0 (GI 2.59 to 2.79). Stamped every update the GI pass took 1.3 ms
  more in Bistro, not 0.6. Left out for its cost: the stamps at the hits of the transmissive fragments' rays (the
  refractions, their sharp reflections), 1.7 ms of Bistro's Main Transmissive, the shader's size (docs/backlog.md).
  visualtest 8 of 8, the quick tier 142 of 142.
- 2026-11-23, **more levels, measured: not kept** (the user's call: "We need to prove that raising the number of
  levels is a clear improvement to motivate it"; the sheet of the classroom at 3 and 6 levels shows blocks either
  way). The 13 scenes of the set with a floor or a backdrop far larger than what they show, against their references
  at 3, 4, 5 and 6 levels (`scripts/researchcompare.ps1 -Set SPEEDO_GI_CLIP_COUNT=<n>`, from a worktree with the
  limit raised to 8, `temp/research-issues/levels-8.patch`; the sheets in `temp/review/research-levels/c<n>`; blurred
  stops off the reference):

  | scene | 3 | 4 | 5 | 6 | the light overall at 3, at 6 (stops) |
  |---|---|---|---|---|---|
  | classroom | 0.265 | 0.387 | 0.429 | 0.421 | -0.033, -0.211 |
  | house | 0.116 | 0.115 | 0.111 | 0.109 | -0.027, -0.022 |
  | lamp | 0.185 | 0.184 | 0.182 | 0.180 | +0.048, +0.042 |
  | pontiac-gto | 0.271 | 0.272 | 0.268 | 0.264 | -0.179, -0.170 |
  | bathroom2 | 0.182 | 0.185 | 0.185 | 0.186 | -0.078, -0.083 |
  | vintage-car | 0.187 | 0.191 | 0.190 | 0.189 | -0.170, -0.176 |
  | veach-mis | 0.243 | 0.244 | 0.245 | 0.246 | -0.087, -0.088 |
  | crown | 0.478 | 0.483 | 0.483 | 0.482 | -0.433, -0.438 |
  | bmw-m6, spaceship, teapot, teapot-full, glass-of-water | | | | | within 0.003 at every count |

  Twelve of the 13 are within 0.007 of where they were, either way: nothing to motivate the 0.25 ms and 43 MB a level.
  The classroom, the one scene whose picture changes, is further from its reference with every level added, by its
  light overall: 0.18 stops darker at 6 levels than at 3. Its blocks do go (the differences from the reference are
  smooth at 6 levels, patches at 3), so the finer voxels do what they were tried for, but the room they light is
  darker than the coarse ones': either the 0.5 m voxels leak light in that happens to make up for light lost
  elsewhere (the set's other large scenes are 0.17 to 0.22 stops dark at any count), or the finer levels lose some.
  Not looked into. So the levels stay 3, the limit 4, and the classroom's blocks are the lookups' to fix (what a lookup
  does where it keeps one column of four), not the voxels' size.
- 2026-11-23, **step 5**, a cold start. Measured first (`temp/research-issues/settle.ps1`: a pose's indirect diffuse
  view every 30 frames from a load, 1920x1080, its mean |difference| of 255 from its own frame 1800): what settles
  slowly is not noise but the light's level. The kitchen, lit by the bounces of its emitters' light alone, starts 40%
  dark (its mean 74 of 255 at frame 30, 121 settled) and takes 8 seconds to come within 4: 47.5, 33.1, 24.0, 12.3, 5.9,
  3.6 at frames 30, 60, 120, 240, 360, 480. With one bounce (`SPEEDO_GI_BOUNCE=0`) it is there at once (6.6 at frame
  30): the bounces build up slowly, not the first one. What made no difference: the probes' blend
  (`SPEEDO_GI_HYSTERESIS=0.9`), the cache's rays (4, 16), its fast mean (`SPEEDO_GI_CACHE_FAST_SAMPLES` 0, 4, 64),
  every voxel's probes, the probes' freeze, the near field, and a young entry's latest samples weighed more (the
  plan's own remedy, tried in a copy of the shaders: an entry's window is already the latest eighth of its updates).
  What did, each a part:
  - **The probes' share of a voxel's light.** It ended at 256 samples, and an entry's window is half the updates
    since the cache started over: 512 updates on. Until then 14% of a voxel's light (at 2 seconds) was the probes',
    which build up far slower, and every bounce lost that again. A settled entry is one whose window is full now
    (`ComputeGiCacheFilter`): 24.0 to 17.9 at 2 seconds.
  - **The blocks no lookup reads** (half the room, behind the camera): a ray in 8 updates, a mean of every sample
    since the start, those of before the light had bounced too. Every block is traced and filtered for a clip's first
    96 updates after its cache starts over (`kGiCacheColdFrameCount`): 17.9 to 9.9.
  - **The coarser levels' turns**: an update every other frame, a bounce an update. Every clip is updated every frame
    for those 96 updates: 9.9 to 6.8.
  - **No bounce for the first 32 frames** (for the probes' provisional depth maps, which the rays read only where
    they trace the voxels): 4 frames where they trace the triangles.

  | scene, frames 30 / 60 / 120 / 240 / 480 | before | after |
  |---|---|---|
  | kitchen | 47.5 / 33.1 / 24.0 / 12.3 / 3.6 | 11.5 / 6.7 / 4.4 / 2.8 / 1.9 |
  | living-room | 38.2 / 15.6 / 6.3 / 3.5 / 2.0 | 4.5 / 3.6 / 2.6 / 2.2 / 1.6 |
  | veach-bidir | 22.5 / 6.9 / 2.1 / 1.6 / 1.2 | 2.5 / 1.9 / 1.6 / 1.3 / 1.1 |
  | Sponza's columns | 24.7 / 9.0 / 5.2 / 3.3 / 2.8 | 6.6 / 4.9 / 4.0 / 3.3 / 2.7 |
  | Bistro's street | 21.2 / 7.5 / 5.1 / 3.1 / 2.0 | 6.0 / 4.9 / 3.4 / 2.4 / 1.7 |
  | classroom, veach-ajar (after alone) | | 18.3 / 10.2 / 5.4 / 3.0 / 1.9, 14.7 / 4.5 / 1.7 / 1.2 / 0.9 |

  No overshoot (the means rise to their level from under it). The settled pictures: the regression poses at 900
  frames within 1.5 of 255 of before but modern-hall's (3.0, its left wall) and bathroom2's mirror (2.2); the old
  build's wall there was still on its way after two minutes (the region's mean 73.6 at 5 seconds, 91.9 at 15, 90.8 at
  120) to where the new one is from 2 seconds on (88.5). The furnace check, whose exact answer is +1 stop: +0.930
  from 64 frames to 4000, where it was +0.898 at 256 (`scripts/visualtest.ps1`, its expected value moved).
  The cost: nothing once settled (Bistro's frame 22.5 ms, the kitchen's 10.0, Sponza's 7.8, as before). While a
  cache is cold, as those settings cost when they are on throughout: Bistro's frame 32.4 ms for the 23.7 of its
  warm up's 4 rays, Sponza's 13.9 for 8.8, the kitchen's 11.6 for 10.1, for 96 frames after a load or the lights
  changing at once: the user's call, 2026-11-23, "keep the cold start cost". 16 rays a voxel while cold bring the
  kitchen to 7.7 at frame 60 with the old delay, but Bistro's frame to 59 ms: not taken.
  Left of the reports' settling: the first half second (the kitchen 11.5 at frame 30), the voxels' kept emitters'
  light, whose blobs a mirror shows for 5 seconds (step 4), and veach-bidir's streaks and the kitchen's dots, which
  are step 6's (what few rays find). `SPEEDO_GI_CACHE_FAST_SAMPLES=2` blows up (175 of 255 off; docs/backlog.md).
  Against the references at 1500 frames (steps 4 and 5 together, from 3c3b13c1's run): 10 scenes nearer, 3 further,
  the mean blurred stops 0.258 to 0.254: veach-mis 0.290 to 0.242, coffee-maker 0.397 to 0.355, bathroom2 0.193 to
  0.168, modern-hall 0.170 to 0.160; living-room-2 0.245 to 0.269 (its light overall +0.164 to +0.183 stops, further
  over), veach-ajar 0.197 to 0.206, bathroom 0.224 to 0.230. visualtest 8 of 8, the quick tier 142 of 142.
- 2026-11-23, **the blocks while the light settles** (the user's question: could they be smoothed or interpolated;
  ideas first, nothing built). Checked with what there is, at the kitchen's, living-room's and the classroom's poses,
  frames 30 to 120 after a load (step 5 in): the filter's edge stop off (`SPEEDO_GI_CACHE_SIGMA=1000`), and 4 passes
  at every level with it, change nothing that shows; and the squares are where the lookups keep one or two of their
  four columns of voxels (the tint of step 3's classroom, `temp/research-issues/s2/dbg6.pl`, from a copy of the
  shaders): the band around living-room's arched windows, a row of teeth along each arch; the kitchen's wall behind
  its toaster and socket; nearly all of the classroom (`temp/review/research-issues/blocks-<scene>-<place>.png`: frame
  30, frame 120, frame 30 without the edge stop, the tint). On the open walls, floors and cupboards, where all four
  are kept, there are no blocks half a second after a load. So the blocks are not the young entries' noise, which
  the filter already spreads, but the lookups' steps, there once settled too and plainest while neighbours still
  differ. What would smooth them, by what it costs to try: a lookup's place jittered within a voxel a pixel a frame
  (the frames' accumulation makes a ramp of a step); the indirect light blurred on the screen along its surface, as
  far as the voxels it read are young or left out (the near field's passes); and which neighbours a voxel's light
  goes with decided between the two voxels once, in the filter's pass, not at each pixel (the same voxel is kept on
  one side of a cell's face and left out on the other while its share is still large: the step), which would mend
  the settled picture too.
- 2026-11-23, **the blocks, tried in a copy of the shaders** (`temp/shader-sandbox`, nothing in the repository's):
  - *The lookup's place jittered within a voxel* (`temp/research-issues/s2/jitter.pl`): the classroom's steps become
    ramps, but a dither shows on top and living-room's teeth stay. The user: "it just creates new artifacts, while the
    blocky pattern does not go away completely". Dropped.
  - *What a lookup keeps decided for the voxel the pixel is in, not the pixel* (`s2/pair.pl`: the sight test's and the
    far sides' point snapped to the voxel's center along the surface): nothing changes that shows. The pixels of a
    voxel already decide alike; what they leave out is left out for the whole voxel.
  - So what is left out, counted (`s2/dbg12.pl`: of a lookup's 8, the voxels that hold a surface facing the pixel's
    way within 60 degrees, and how many it keeps, by each rule alone; `temp/review/research-issues/own-<scene>-<place>.png`:
    the picture, every rule, the sight test, with the facing rule, the far sides): two causes, by scene.
    The classroom's wall has its four voxels and the sight test and the facing rule keep them; the far sides rule
    (`GiFarSides`) leaves out all but one or two, by its clause for a voxel behind a single sided surface (the ways its
    undrawn sides face): a voxel of 0.5 m holds several things, single sided ones among them, facing 5 ways of 6, and
    every neighbour half a voxel to the side counts as behind one. The kitchen's wall behind its toaster and the band
    around living-room's arches hold fewer than four such voxels to begin with, whatever the rules: each voxel there
    stands for something else (the toaster, the socket, the arch's mouldings).
  - *That clause not taken for a voxel that holds a drawn side facing the lookup's surface's way* (`s2/farown.pl`): the
    classroom's squares are gone, at half a second and at two (`farown-classroom-wall.png`,
    `farown-classroom-indirect.png`), and the poses the rule was made for are as they were: salle_de_bain framed 0.05
    of 255 from before and at its shelves 0.25, bathroom2's niche 0.23 and mirror 0.14, Sponza's columns 0.28, Bistro's
    street 0.39, the kitchen 0.33, living-room 0.11 (900 frames, 1920x1080). Not in the repository yet: the user's
    call. The kitchen's and living-room's blocks it doesn't touch: those want the surface's light from its voxels
    beyond the 8, or smoothing on the screen.
- 2026-11-24, **the classroom's squares** (report 3; the user: "bring the classroom fix in"): `GiFarSides`' clause for
  a voxel behind a single sided surface isn't taken for a voxel that holds a drawn side facing the lookup's surface's
  way within 60 degrees. The classroom's walls and ceiling interpolate again
  (`temp/review/research-issues/s6-classroom-indirect.png`, `farown-classroom-wall.png`). The regression poses at 900
  frames within 0.54 of 255 of step 5's (salle_de_bain framed 0.05, at its shelves 0.25, bathroom2's niche 0.24 and
  mirror 0.14, Bistro's street 0.54). Against the references nothing moves but veach-ajar (0.206 to 0.201) and
  living-room-2 (0.269 to 0.277): the classroom's number is its light's level, not its squares (0.262 either way).
  The cost: none to measure as it is (Bistro's frame 22.54 ms against 22.53 at 1440p balanced, the classroom's 7.92
  against 7.82); with the voxel's normal looked at for every voxel of the 8, as first written, Bistro's Reflection
  Trace took 0.75 ms more and its Main Transmissive 0.76. visualtest 8 of 8, the quick tier 142 of 142.
- 2026-11-24, **the wider fetch, tried in the copy of the shaders** (`temp/research-issues/s2/wide.pl`; the user: "bring
  the classroom fix in, then try the wider fetch"): where a lookup leaves out some of its 8, their share taken from
  the pixel's own surface's voxels among the 27 around its voxel (a drawn side facing its way within 60 degrees, on
  its plane within 3/4 of a voxel), a tent 1.5 voxels wide, not given to the ones kept. Alone it changes little
  (living-room's arches as they were, the kitchen's squares rearranged). With the 8's voxels that hold another surface
  left out too where the 27 hold any of its own, a good part of the arches' teeth is gone by two seconds and the
  kitchen's wall behind its toaster has fewer, fainter squares, but blocks are left in both, and the classroom's
  ceiling and upper walls are brighter (its lit picture 2.3 of 255 from before) with new light squares along its
  beams: a leak or not, not looked into (`temp/review/research-issues/wide-<scene>-<place>-f<frame>.png`: off, the
  fill, the fill with the others left out; `wide-classroom-indirect.png`). The regression poses at 900 frames within
  0.8 of 255 (Bistro's street 0.76, Sponza's columns 0.58, the rest under 0.4); the cost not measured. It patches a
  lookup that still starts from the 8 around the point, so the light still steps from cell to cell where few are kept.
  Not in the repository. Next, the user's to choose: the whole lookup from the surface's own voxels where it has
  any, smoothing on the screen, or step 6.
- 2026-11-24, **the blocks where a surface has no voxel of its own** (the kitchen's wall behind its toaster,
  living-room's arches; the user: "try 1 next", and to think of alternatives before smoothing on the screen), all in
  the copy of the shaders first.
  - *The whole lookup from the surface's own voxels among the 27* (option 1, `temp/research-issues/s2/own.pl`: a tent 1.5
    voxels wide, continuous from cell to cell, the own voxels tested for sight where one around is marked apart):
    the kitchen's squares mostly gone, with a dark bar or two of its own; living-room's teeth as they were. Painted by
    path (`ownfetch-living-room-path.png`, `ownall-living-room-arch-zoom.png`), the teeth are a band a voxel or two
    wide beside each arch's moulding where the 27 hold no voxel of the wall at all: every voxel there is the
    moulding's, and the lookup, having none of its own among its 8, kept the moulding's (the facing rule's own
    exception), lit by the window. Not a seam between two lookups either: the same with the own lookup at every pixel.
  - *At the source instead* (the alternative that came of it, `s2/own2.pl`'s 1024): an air voxel takes its light from
    the surfaces around whose open side it is on, every face from all of them; a surface's entry's rays leave its own
    side, so the air beside a wall and a toaster held a mix of both in every face, which the wall's lookups read with
    the wall's own. Each face is now from the surfaces among them that face its way (`ComputeGiCacheFilter`,
    `kGiSpreadFacedLeast`), so the air in front of a wall holds the wall's light along the wall's normal whatever
    stands against it; and a lookup leaves the other surfaces' voxels out of its 8 whether one of the 8 is its own
    surface's or not (`GiSidesMaskFacing`), since the air's are its own surface's light where that has none.
    The kitchen's wall is smooth at half a second and at two; living-room's arches keep a few soft notches where they
    had rows of teeth, its panels' frames and shutters are cleaner; the classroom's window walls too
    (`temp/review/research-issues/s7-<scene>-<place>.png`: before and after at frame 30, before and after at frame
    120; `s7-<scene>-indirect.png`, `s7-<scene>-lit.png`). Better than the own lookup, and than both together
    (`combo3-*.png`: off, this, this with the own lookup), for 8 voxels read where that reads up to 27.
    The regression poses at 900 frames within 0.7 of 255 of the commit before (the bathroom 0.68, living-room 0.58,
    the kitchen 0.47, Sponza's columns 0.33, bathroom2's niche 0.30 and mirror 0.28, salle_de_bain's shelves 0.26;
    living-room's cornice without its bright line). The cost: none (Bistro's frame 22.47 ms against 22.51 at 1440p
    balanced, the classroom's 7.80 against 7.91: GI Sides is cheaper, 0.73 to 0.64 and 0.47 to 0.35, a voxel left out
    by its facing needing no ray; the filter's pass within its noise).
  - Thought of and not built: smoothing the indirect light on the screen (hides steps in the view, not in what a
    mirror shows, and blurs the contacts the near field draws; kept for what is left, if it shows); the near field's
    rays standing in where a lookup is incomplete (the rays that meet nothing fall back on the same lookup); the
    coarser level's light for the left out share (tried in step 3: its own blocks, and a leak); the world cache's
    cells, which are per surface (docs/world-cache-plan.md, off for its cost).
  Left: the arches' soft notches (an air voxel with no surface facing the wall's way among its 26 keeps the mix), and
  cornell-box's front face's faint teeth. Against the references at 1500 frames, about the same: 3 scenes nearer, 5
  further, none by more than 0.006 blurred stops (the classroom 0.263 to 0.269, classroom2 0.333 to 0.337, veach-ajar
  0.201 to 0.205; the bathroom 0.229 to 0.226), the mean 0.2536 to 0.2540: what it mends is the blocks, which that
  measure blurs. visualtest 8 of 8, the quick tier 142 of 142.
- 2026-11-24, **step 6, looked into, and the cache's window** (report 11, veach-bidir's left wall). As it is since
  step 5 the wall's streaks no longer move (0.03 of 255 from frame to frame at 10 seconds) and are gone after 50
  seconds (`temp/review/research-issues/s6-veach-bidir-wall-grid.png`: 10 seconds, 50 seconds, resolution 256, 64,
  the emitters off): noise that takes most of a minute to average out, each voxel its own (finer at resolution 256,
  coarser at 64). And the same at 10 seconds with 8 rays a voxel, with the filter's edge stop off, with 4 passes
  (`s6-veach-bidir-wall-variants.png`): more rays made no difference because the cache's window, in samples, was half
  the updates since it started over, so with the warm up's 4 rays a voxel (or `SPEEDO_GI_CACHE_RAYS`) it held the
  last eighth of the updates, a mean of no more samples than with one. It is half the rays since now
  (`GiClipClock::raysSinceCacheReset`): the same share of the time, of 4 times the samples while 4 are traced. The
  wall at 10 seconds is 2.3 of 255 from its 50 seconds where it was 3.4 (`s6-veach-bidir-wall-window.png`: before and
  after at 10 seconds, before and after at 50), and every start is smoother: mean |difference| from settled at
  frames 60, 120, 240, 480, the kitchen 6.7, 4.4, 2.8, 1.9 to 7.0, 4.3, 2.6, 1.3; living-room 3.6, 2.6, 2.2, 1.6 to
  2.8, 1.9, 1.4, 0.9; Sponza's columns 4.9, 4.0, 3.3, 2.7 to 3.8, 2.4, 2.1, 1.5. No cost (Bistro's frame 22.4 ms,
  the kitchen's 9.9, Sponza's 7.8). The regression poses at 900 frames within 0.5 of 255 but modern-hall's (1.3, its
  left wall, still on its way). The emitters' visual check is 0.072 stops from its reference where it was 0.290.
  Step 6 proper, the rays aimed at what is bright, is not started: what is left of the wall's streaks at 10 seconds
  is what it would mend, and the kitchen's first seconds' dots. Against the references at 1500 frames the mean is the
  same (0.2540 to 0.2538): living-room-2 0.279 to 0.251, 7 scenes further by 0.008 at most (bathroom2 0.166 to 0.174,
  modern-hall 0.160 to 0.166). visualtest 8 of 8, the quick tier 142 of 142.
