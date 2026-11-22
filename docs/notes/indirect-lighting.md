# Indirect lighting

A note of CLAUDE.md (its index names each): the probes, the voxel clipmap, the surface cache, the near field, the rays and the emitters.

## Indirect lighting: irradiance probes over a voxel clipmap

View > Rendering Features > Indirect Lighting (on by default since 2026-10-22, the user's call once the artifacts that followed the camera
were gone; `SPEEDO_GI=0` to start without it, and per frame:
`FrameInputs::indirectLighting`, off drops the volumes, on voxelizes again) gives the diffuse ambient term from world
space irradiance probes (`gfx/indirectlight.h`, `gi.slang`; `GiData` in `capi.h`), as DDGI (Majercik et al. 2019, RTXGI)
has them, over a voxel clipmap the probes' rays march: each probe traces a set of rays a frame through the voxels'
radiance and blends what they see into what it holds (hysteresis), its irradiance along the axes (an ambient cube) and a depth map
for a visibility test; the shading interpolates the 8 probes around a point, each by whether it sees the point. Lighting
lags by the blend (about a second to settle, nothing else is temporal: no reprojection, so no ghosting), and a still
scene flickers by the frame's rays' share of the blend (measured below). It replaced world space radiance cascades
(2026-10-21): a fixed set of 36 directions per probe sampling a scene's small bright parts (CornellBox's lamp, Sponza's
arches) landed differently from probe to probe, which showed as splotches a fifth of a room wide that no amount of
tuning took below a third of the error (CornellBox-Sphere against a 2048 ray reference, `mean |log2|` at 48 pixels of
blur: 0.13 against the reference's own 0.046 with 512 rays; the merge's parallax 0.03 of it, the levels 0.01, the rest
the direction grid), and their cost grew with the clips (every clip's cascades reach across the last: CornellBox-Sphere
29.9 ms a frame on the day it was dropped, 9.3 with the probes). What it does, per frame:

- **The clipmap** (`gi::MakeClipmap`, `SHADER_TYPES_GI_CLIP_COUNT`): `SPEEDO_GI_CLIP_COUNT` levels of volumes (3 by default,
  at most `SHADER_TYPES_GI_LEVEL_COUNT`, 4), finest first; each of split screen's views has its own clips of every level
  but the last, which they share, centered ahead of its own eye by its own look distance (`gi::Eye`,
  `FrameInputs::lookDistances`), ordered by level (every view's finest, then every view's next, then the last), so that
  the clips a point is in, finest first, are those of a lower index: the lookups, the rays' walk and the marking of what
  is seen take them in that order as they did one chain (a point in two views' clips of a level takes both, by their
  weights, so its light doesn't depend on which view sees it), and what starts from a coarser clip (a probe, a cache
  entry new to a clip) takes its view's, `GiData::coarser`. With the first view's alone, the other views were lit by the
  last clip (8.9 m probes in Bistro): at Bistro's noon with four views far apart, each view's quadrant against its own
  single view render 16.1, 18.3 and 13.3 of 255 before, 5.2, 7.1 and 5.0 after (the half resolution's control 6.9). It
  costs the clips' work again per view: Bistro's evening frame 38.4 ms with one view, 73.2 with four (the indirect
  lighting 21.5 and 49.0 of them; four views at one pose 76.9), the probes' ray budget spread over three times the
  probes (40 rays each rather than 128). The resolve's volumes stack the clips' slabs along z, in columns along x where a
  column would pass the device's largest 3D image (`DeviceLimits::maxImageDimension3d`, 2048 on KosmicKrisp;
  `GiData::volumeImageX`). The last is over the scene's bounds (`SPEEDO_GI_RESOLUTION`, 128 by default, voxels along its
  longest axis; View > Rendering Features > Indirect Lighting... > Resolution offers `gi::kResolutions`, 64 to 384, and a change makes the clipmap
  again, `FrameInputs::giResolution`. Finer resolves light through narrower openings: the sky under Sponza's curtains,
  a gap of a few tens of centimetres, which 128 drew as spots at the corridor's end that faded into longer, dimmer
  patches as the eye neared them (the coarser clips' probes, 1.8 m apart, giving way to the finer's; the surface
  reference has a band along the curtains' lower edges), and 256 as the reference does; at the user's pose
  `7.7949,0.8419,-3.7077,0.531,-91.763` the frame takes (without the surface cache, below) 2.1 ms at 64, 2.6 at 96, 3.5 at 128, 5.4 at 192, 8.2 at 256, 11.9 at 320 and 17.8 at
  384 (RTX 4070 Ti SUPER, 1920x1080; Bistro's street 18.6 at 384); a margin of a probe spacing, so that a layer of probes stands outside its outermost surfaces; a whole
  spacing puts the bounds halfway between two planes of probes in every clip, the probes being half a spacing from a
  cell's corner: at one and a half, a plane of the last clip's probes lay in the outermost surfaces, CornellBox's floor,
  back and left walls, half of each one's rays leaving through the surface it was on and the relocation unable to tell
  which way was off it, the surface's voxels being on both sides of it, and they lit the floor along the back wall
  with the sky; Sponza's error against its surface reference went from 0.385 to 0.341); each before it is a cube of about its voxel count (a multiple of 4 voxels a side, the flags' blocks; no
  bigger than the last along an axis: Bistro's and Sponza's 92) of voxels half the size of the next, centered ahead of
  the eye toward what the camera looks at (below) on a grid of its probes and kept inside the last. As slabs of the last clip's shape (Bistro's 122x53x128
  voxels made the finest 14.7 m tall at 27.7 cm, Sponza's 4.9 m), the ground fell out of them from a high viewpoint
  (the user's over Bistro's square, 13 m up), and the square was lit by the last clip's probes, 8.9 m apart, of a cell
  half under the ground and one probe inside the building by its entrance. A clip's center is ahead of the eye, toward
  what the camera looks at, by `SPEEDO_GI_LOOK_AHEAD` of its half size at most (0.75 by default, 0.5 before 2026-11-21; 0 centers it on the
  eye): how far the camera looks is the median distance of the frame's drawn pixels from the eye (`ComputeMain`
  unprojects the main depth into 64 bins of log2 distance after the exposure histogram's, `SHADER_TYPES_DISTANCE_BINS`,
  read back with it and smoothed as the exposure is: `FrameInputs::lookDistance`), and the centers lie along the view's
  forward, so each clip stays inside the next, and the eye stays in the finer clips, two thirds of the way to their
  fade. Centered on the eye, half of each clip was behind it, and from the roof over Bistro's square (27 m from the
  hotel's entrance) the square was the last clip's; with the look ahead the ground is clip 1's (4.4 m probes), the
  entrance's facade still mostly the last's at 0.5 (its weight in clip 1 is 0.2 there). 0.75 since (2026-11-21): what
  a clip takes in as the eye moves forward comes in further ahead, and has longer to settle before the eye is near:
  walking Sponza's corridor (`-4.5893,0.8216,2.8666,-2.892,-268.574`, 56 frames at 5 cm), the floor's band along the
  arcade's bases formed behind the clips' front, the walk's last frame 2.65 of 255 from its pose's settled frame (6.1
  in the band's strip, the indirect view); 2.05 (3.4) at 0.75 and 1.99 (3.3) at 1, where the eye is at the clips'
  back. What else was tried: every clip updated every frame (`SPEEDO_GI_STAGGER=0`) 2.50, 2 cache rays 2.67, the cache
  never settled 2.71, a fourth level 3.61; `SPEEDO_GI_RESOLUTION` 192 2.22 (3.5) and 256 2.25 (2.8), but 192 took
  Sponza's frame 4.72 to 7.34 ms and Bistro at night's 7.70 to 10.53, the indirect lighting's memory 336 to 955 MB.
  A 60 degree turn 3.08 to 3.28 from settled; the regression set within 0.4 of 255 (a strip within 0.92). The buffers a new clipmap gets
  are cleared first (the GI Clear pass: every GI buffer and the volumes filled with 0 when they were replaced,
  `myGiClearBuffers`): the resolve writes only the blocks near a surface, and a lookup's filter reaches into the ones
  beside them, which held whatever their memory had (cyan and red over Bistro's square, in one run of two). Each has its own voxels, radiance, probes
  and flags, at its offsets in the shared buffers (`GiData::voxelFirst`, the radiance buffers' too, `probeFirst`,
  `lightCellFirst`, `flagFirst`; the resolve's volumes stack the clips' slabs along z, `GiData::volumeImage`: the
  images' texels per axis and the clip's first along z;
  `gGi[frame][clip]`), and the kernels take theirs from `PushConstants::giClip`. The flags' offset is in words
  (`SHADER_TYPES_GI_FLAG_WORD_COUNT` a block): in blocks, each clip's second half of flags was the next clip's first,
  whose kernels rewrote them (its bits, and other blocks' stamps), so in the finer clips' far halves blocks near a
  surface went unresolved and probes untraced, at random (found 2026-10-22 reading the flags from the rays' walk). A
  voxel's radiance is a uint2 in each of three buffers: the frame's (`gGiRadiance`), the static voxels' direct
  (`gGiDirect`) and its part the lights don't change (`gGiKept`). They were one buffer of three parts
  (`GiData::radianceFirst`, `directFirst`, `keptFirst`) until 2026-11-12: 144 MiB at the default resolution, past a
  storage buffer binding's range where the device's `maxStorageBufferRange` is 128 MiB (lavapipe; the validation
  layer's VUID-VkWriteDescriptorSet-descriptorType-00333 in `visualtest.ps1 -Validate`'s furnace check). A
  clip's rays (`GiMarch`) and shadow rays (`GiOccluderDistance`) that leave it go on through the coarser ones (a box is
  never entered again). All are made again when the model, its bounds or its draws change; a clip that moves scrolls
  (`GiData::scroll`, in voxels, whole probe cells): GI Shift (`ComputeGiShift`) gives each voxel the static one kept at
  its place before, or empty where it scrolled in, and only that is voxelized (its jobs are the static draws whose
  bounds reach the slabs that scrolled in, `GiClipJobs`; a triangle whose voxels in the clip were all kept skips, and a
  row of its samples only covers the slab); the probes shift the same way (below); a move of its size or more, or a
  change of the scene, voxelizes all of it (a scroll of its whole size). Voxelizing (the GI Shift and GI Voxelize
  passes, only then): a thread per triangle of the main list's static draws (`gGiJobs`: per draw its first triangle of
  all, first index, `DrawRecord` and instances; `gIndexBuffer`, so index buffers are storage buffers too) samples its
  surface at under half a voxel apart into the voxel half a voxel behind each of its faces (`GiVoxel`: albedo and
  occupancy, normal facing away from the face, emission; `InterlockedMax`, so that the order doesn't matter), in rows
  of samples, at most 64 (`kGiVoxelizeSampledSteps`); a longer triangle (over 32 voxels) is rasterized along its
  normal's dominant axis instead (`GiVoxelizeRasterize`: each voxel column whose square its projection overlaps, the
  voxels its plane crosses there). Sampled at most 512 rows apart, as it was, a long triangle was sampled a little more
  than a voxel apart and skipped voxel columns: at resolution 384 Sponza's corridor floor (triangles some 10 m long,
  voxels of 2 cm in the finest clip) had lines of holes across it, which the surface cache drew as bright lines along
  the floor; sampled half a voxel apart however long, its voxelizing took 13 ms rather than 4.4 when a clip scrolled
  (a triangle is a thread), rasterized 3.4 (Bistro's street 3.5 to 3.0). Rasterized only from 256 voxels long, a
  triangle just under was a hundred thousand samples a side (about four a voxel, five atomics each) on one thread:
  CommercialRefrigerator's glass door, two triangles, took GI Voxelize Moving to 18 ms a frame and the whole model's
  voxelizing to 168 ms; from 32 voxels, 0.5 and 17, the renders as they were to their noise (Sponza's corridor,
  salle_de_bain, Bistro's street and the CornellBox set 0.11 to 0.25 of 255). A long triangle's columns, and a sampled
  one's rows from 32 of them, are shared by its wave's lanes (`GiVoxelizeRasterizeTogether`, `GiVoxelizeSampleTogether`:
  the lanes with one take turns broadcasting it, and every lane takes its columns or rows in turn; the kernel's lanes go
  round together for it, a lane past the triangles or an instance past its count staying to help): the same voxels,
  where one lane took them one after another (the refrigerator's frame 6.1 ms a lane a triangle, 5.9 shared). Both
  faces, always: a double sided material's either face can be the one seen, and a triangle's winding, its vertex
  normals and the side it is drawn from needn't agree (CornellBox's generated normals point out of the box, whose walls
  are seen from inside). Which faces wrote a voxel is kept (`GiVoxelFaces`: 1 a front face, behind which it stands for the
back side, 2 a back face, 3 both), and whether a double sided triangle did (`kGiVoxelDoubleSided`; a job's bit 30, from
its draw's cull mode), and a drawn side's normal wins the voxel's (`GiPackVoxelNormal`: the front side's, either of a
double sided triangle's, by the normal's top bit in the atomic maximum). A voxel a single sided triangle's front face
wrote holds a side never drawn, and the resolve gives it no light (see the BVH's paragraph below). Each side's voxel is
a shell for the lookups and the relocation; for the rays (`GiMarch`,
  `GiOccluderHit`: the probes' rays and depth, the shadow rays, the Voxel World view) the voxel a back face alone wrote
  (`GiVoxel::emissionB`'s faces 2: outside a solid, by the importers' outward winding) is passed, and is the surface
  met if its twin behind is hit within two voxels (its radiance and normal), so a surface is a voxel thick for them
  rather than two, and an opening a voxel wider a side. At 0.29 m voxels Sponza's first floor arcade lost its
  openings' lower 0.6 m to the shells: no sun reached its floor in the last clip and its probes held no light (their
  rays' mean 2e-5 against the next clip's 4e-3), so the corridor was black beyond the finer clips' boxes, the third
  artifact that followed the camera (the user's poses `-7.7534,4.8126,-3.5046,0.426,448.765` and
  `-2.3323,4.8044,-3.5225,0.779,449.768`), and the surface reference agreed, reading the same voxels. With the shells
  passed its far corridor at exposure 6 reads 0.045 (the reference 0.033; before 0.017 and 0.011), the mid floor 0.18
  (0.09; 0.07 and 0.04) and the near 0.42 (0.39): it still darkens with the distance, the last clip's openings being
  what they are (two clips at 256 voxels light it, 0.13, for Sponza's 13 to 24 ms a frame, 29 with three, and three at
  192 0.16 for 21: too much; TODO.md). A probe's rays walk from the finest clip whose box holds it (`GiFinestClipHolding`, in `ComputeGiTrace`),
  not from its own, so the clips' probes agree where they overlap: by their own voxels, the finest clip's sun patches
  lit its probes and not the coarser clip's beside them, so its box's edge was a step of 12% on the vault (the user's
  "sharp discontinuity", in the surface reference as much as the probes' render, whose fade band is narrow that far
  out), moving with the eye; the step is 8% now, over a probe spacing, and the far corridor reads 0.075 (the coarser
  clip's probes nearest the box walk the fine voxels too). What is left of it is the coarser voxels' own lighting
  beyond the finest box. The one voxel shell has corner gaps only where a plane passes voxel corners exactly, which the second shell
  covered: the samples' half voxel offset keeps it face connected elsewhere. The voxels' albedo is the base color times 1 minus the metallic factor times its texture's
  (by the factor alone, Sponza's voxels were all black). Those voxels are kept (GI Keep, into a buffer of their own:
  the clips voxelized, the others' voxels holding what moved last frame), lit by the lights (GI Light Static, below),
  and every frame with something that moves (skinned, morphed, or following an animated node: not cullable) they are
  copied back (GI Restore) and what moves is voxelized over them (GI Voxelize Moving: its jobs after the static ones',
  by `PushConstants::cullJobBase`), placed by `WorldPosition` (`vertex.slang`: morphed, skinned, then by the instance,
  as `VertexMain` does): it occludes, is lit and bounces light, in the frame.
  A frame what moves stands where it did (the animation paused: `FrameInputs::modelStill`) and no clip is voxelized,
  neither runs: its voxels stay as the last voxelizing left them and its blocks stop being stamped as moving, so that
  the light about it settles (the probes freeze, the cache's entries there aren't capped, the resolve isn't every
  voxel's there); the first update it moves again restores the blocks of the last it was voxelized in
  (`GiData::restoreStamp`, the clip's `movingStamp` before; it was the frame before's alone). Before (the user's
  report, 2026-11-18: MovingInterior paused, splotches on the sphere and the cube that kept changing), frame 1500 to
  1504 of the paused pose changed by up to 25 of 255 (0.18 on average), where the room without its animation changed
  by 7 (0.14); since, 7 (0.13). Playing, frame 700 is as it was (0.11 of 255 from step 3's capture); resumed after 500
  frames paused, no voxel is left where it stood (`gi-voxel-world`).
  What moves isn't lit by the surface cache (the user's report, 2026-11-18: rings, seams and patches over
  MovingInterior's sphere and turning cube; with the cache off they were smooth): a cache entry is its voxel's, which
  stays where it is while a moving surface passes through it, its samples of what was there before, the voxels' layers
  drawn over the sphere as rings. So the filter reads no entry of a voxel something moving holds this update (not the
  static voxel at its place, in a block voxelized as moving: `GiCacheReadTap`), which keeps it out of the voxel's own
  light, its neighbours' on the surface and the air's beside it; and a surface that is something moving's
  (`GiMovingSurface`: in the finest clip holding it, one of the voxels half a voxel out, at it and half a voxel in; one
  alone drew a seam across the turning cube, its layer in one voxel or the next over a face) is looked up as without the
  cache (`GiSurfaceBias`: 0.75 voxels out and 1.5 toward the eye), where the air's light is the probes', smooth: its
  own voxels' probe light, resolved at each voxel it passes through, came out in patches. Tried and dropped: the probes'
  visibility traced again in moving blocks every update (no change). And an entry's window stays short (32 samples) for
  `kGiCacheMovedFrameCount` (32) updates after something last moved in its block, not that update alone, so that what
  it gathered while something stood over it (the floor under the runner) falls out within as many updates where the
  whole window back at once kept it for seconds; measured at the user's pose against the moment held still, the floor
  around the runner is within the noise either way (5.15 and 4.82 of 255 with upscaling off), and what trails there
  most is the probes' (with the cache off a dark silhouette on the far wall where the sphere had been: their light
  moves a few percent an update).
  Playing, a frame in four or five darkened the sphere in patches (the user's report, 2026-11-18, pose
  `-4.2534,1.1361,-0.0074,0.532,-266.575` at 2.316 s: "ringing & darkening on single animation frames"), whenever it
  crossed a row of the finest clip's probes (4.5 cm a frame at 1/100 s against a spacing of about 19 cm). The near
  field's: off, it was gone, the cache on or off. Its history took nothing of what moves, so on the sphere it was the
  frame's rays alone at a sixteenth of the pixels, blotchy; the shading takes it by the lookup's trust, which on the
  sphere stays near 1 and dipped to about 0.9 on those frames, a tenth of the blotches let through. Its history now
  follows what moves by the pre-pass's motion (see the near field below): with `SPEEDO_REPEATABLE` and
  `SPEEDO_CAPTURE_COUNT` (the same frames every run), the sphere coming at the camera (frames 741 to 800 at 1/100 s) had
  3 frames with more than 0.3% of the pixels darker than both neighbours by 8 of 255 (up to 1.16%), now none (up to
  0.27%); after the loop's wrap (801 to 860) 12, every fifth, now 1 (the sphere 20 cm from the lens, which moves more
  than a tenth of its distance a frame and takes no history). GI Near Temporal 0.045 ms to 0.046. Its own copy of
`PixelHistory` (`GiNearHistory`): the mark added to `PixelHistory` itself (-1 from the emitters' and the traced
reflections' calls) failed the validated quick tier's D3D12 clients three runs in three (3 or 4 of the 61, the emitters'
and transmission's models: DispersionTest, IORTestGrid, the lamps, AnimationPointerUVs, the draw thread silent for 20 s,
`02295a514e8db0f7`; each passed alone), and passed 61 of 61 with that file as it was; why wasn't found then (most likely the tier's own stall when its
clients start together, found later the same day: see "Hangs" in docs/notes/rhi.md). Ruled out on the way:
  the probes' irradiance (gi-probes steady), the probes' inside flag (ignored, no change), the voxels' visibility of
  probes the sphere hides (traced against what moves, no change), rays starting inside the traced sphere (none), the
  volume beside the sphere (steady along the normal).
  That was at 1/100 s a frame with the indirect lighting's blend by the real frames (2 ms): not what the user watched,
  who saw no difference. Stepped as a 60 Hz session is (the animation and `SPEEDO_TIME_FRAME_SECONDS` both 1/60), three
  causes showed, each fixed:
  - **A moving surface's lookup was half distrusted, by a share that changed every frame.** The shading takes the near
    field's light for the lookup's by the lookup's trust (`GiLookupTrust`: its voxels' weight over 0.2), a line drawn
    for still surfaces, whose voxels hold the cache's light at its confidence (1 once settled). Something moving's
    voxels and the air beside it hold the probes' light alone, at the probes' weight (their facing's 4th power: about a
    tenth), so over the sphere's outer third the image was about half the near field's light (flat, 96 of 255) and half
    the lookup's (shaded along the normal, 38 to 60), the share following where the sphere stood among the voxels: its
    rim 59, 57 and 45 in three frames (upscaling off), the lookup alone 38, 37, 38. Temporal upscaling smeared the
    one pixel rings into crescents: the user's "ringing & darkening on single animation frames". Now a moving surface's
    lookup (`GiSurfaceBias`'s moving) is trusted from a tenth of that weight (`kGiNearTrustedMovingWeight`, 0.02; a
    hidden probe counts a thousandth): the rim 39, 38, 38.
  - **What something passing hid showed its cache entries unfiltered when it came back.** The filter ran in the blocks a
    lookup read in the last 2 updates (the tracing's line too): the ring of wall the receding sphere's outline uncovered
    each frame, the floor behind the runner, and the whole room when the sphere had passed over the camera (a third of a
    second) came back as each voxel's own samples' mean, blotchy, for a frame. The filter now runs for about a second
    after the last read (`GiData::cacheSeenFrameCount`, `SPEEDO_GI_CACHE_SEEN_SECONDS` by the frames' length); the voxels
    trace as read for 2 updates as before (`kGiCacheReadFrameCount`). A still camera's cost is the same (the same
    blocks); one that turns keeps the filter in what it saw within the second: turning in place 90 degrees a second
    (1800 frames at 1920x1080, stepped at 60 a second, `temp/gi-disocclusion/turncost.ps1`, the second against 2
    frames in turn, twice) Sponza's GI pass 2.00 to 2.12 ms (the frame 4.70 to 4.79) and Bistro's 3.17 to 3.30 (8.26 to
    8.35); walking 3 m a second while turning 12 degrees a second 1.39 to 1.42 and 2.47 to 2.52.
  - **The near field's history for what moves gave out near the camera**: within a tenth of its distance the sphere lost
    it under 75 cm at 7.5 cm a frame (grainy grey patches as it passed); within half now.
  Measured on the same frames (`SPEEDO_REPEATABLE`, `SPEEDO_CAPTURE_COUNT`), 1280x720, frames 466 to 510 (the sphere over
  the camera and off again): 3 frames with more than 0.3% of the pixels darker than both neighbours by 8 of 255 (0.50%
  at most), now none (0.15%). Ruled out before these: the air's light refreshed every update
  (`SPEEDO_GI_PROBE_AIR_INTERVAL=1`, no change), the lookup's offset (along the normal alone, 1.5 or 2.25 voxels: the
  same; the sphere's darker rim is its shading, lit from the room's middle), temporal upscaling (off, the rings were
  there, a pixel wide).
  And spots that came and went slowly around the runner (the user's report, 2026-11-18, pose
  `-0.9729,0.7820,-1.4863,9.854,-234.218`: "brightened spots appearing gradually and slowly fading around the small
  moving green box"): an entry's window was cut to 32 samples for 32 updates after something moved in its block,
  whatever its samples said, and the runner crosses its track every half second, so the floor along it was a mean of 32
  to 60 samples for good, a tenth brighter or darker in patches that wandered over tens of frames (three floor pixels
  between its passes 58 to 70 of 255; with no cut at all 62, 64 and 66 to 67 on every frame). The cut is now as far as
  the entry's light has changed (`GiCacheUpdate`'s cap): by how far its fast mean (see the dark walls, below) is from
  its whole one, in deviations of a fast mean; within 2 none, beyond that the window over the deviations less one,
  squared, down to 32. The same pixels 61, 61 to 62 and 62 to 66. What really changes still follows: the playing frame
  against the same moment held still, MovingInterior's floor around its cube 3.96 of 255 (about 5 before), and
  CommercialRefrigerator's door mid swing 0.428 (0.427).
  Left: a patch of the far wall's lower right shows unfiltered for one frame as the room comes back from behind the
  sphere at the camera (frame 471 of that loop), before and after these.
  And a block read for the very first time (the user's report, 2026-11-18, walking forward from
  `3.4680,1.2528,0.5256,13.720,-249.950` towards the corner: "unwanted bright artifacts are popping up sharply during a
  few frames"): the stamps the filter goes by were the main pass's (the composite's, at the frame's end), so the filter
  ran in a block from the frame after a lookup first read it, and that first frame showed its entries as they were,
  each voxel its own few samples' mean. Walking at 1.8 m a second (stepped at 60 a second,
  `temp/gi-disocclusion/forward.ps1`), the wall ahead was specked with bright voxels for one frame, the 51st, when the
  finest clip's lookups reached it (voxels scrolled in 14 frames before, read only once a spacing inside the clip:
  `GiClipWeight`); gone with every block read (`SPEEDO_GI_CACHE_EVERYWHERE=1`) and with the cache off, there with the
  near field off, upscaling off and the fast mean off. Now GI Seen stamps the blocks the frame's pixels will read
  before the GI pass (`ComputeGiSeen`: `GiMarkSeen` at every pixel of the pre-pass's depth, at the blocks' places
  before the clips' scroll of the update, where the resolve reads them), for which the pre-pass is recorded before the
  indirect lighting's passes; the composite's stamps stay, for what the late pre-pass draws. The bright frame's sum of
  pixels brighter than both neighbours 630 a million pixels to 0, GI Seen 0.03 ms at 1920x1080, the frame the same
  within the runs' noise (Sponza 4.56 ms, Bistro 6.21). Left there: for about 15 frames after, the wall is faintly
  patchy where the finest clip's entries, begun from the middle clip's coarser ones, take their own samples.
  - **A surface hidden inside a solid held light of the solid's outside** (the user's report, 2026-11-19, at
    `5.1480,0.7274,1.5497,16.221,-251.868`: "this bright while static line here, that changes in size as you move
    farther&closer to it"): a line along the +x wall a few centimetres before its corner with the +z wall, 64 of 255
    where the wall is 50 (`temp/gi-disocclusion/row.ps1`), gone with the cache off, there with the near field, the
    upscaler or `SPEEDO_GI_CACHE_THIN` off, and from the finest clip alone (`SPEEDO_GI_LOOKUP_LEVEL=1` hides it). The
    walls are boxes 0.3 m thick that overlap at the corners, so the +x wall's inner face goes on 0.3 m inside the +z
    wall, coplanar with that wall's end. The finest clip's voxels along that hidden part held valid entries of 20 times
    the room's light (`SPEEDO_GI_DUMP=5.977,0.51,2.045`: -x 0.088 where the wall's are 0.0035): their rays started inside
    the +z wall and met its outer shell, lit by the sun, from inside, beyond the inside test's reach (a voxel and a
    half), and only a fifth of them were caught (those toward the coplanar end face, met at once). They face -x as the
    visible wall's voxels do, so the filter's passes 2 and 4 voxels apart took them as the same surface: the wall's last
    two voxels before the corner came out 0.56 and 0.61 (scaled) where the rest of it is 0.46 (their own entries no
    brighter). An anchored entry's rays start on the surface itself, so a back face met at any distance means it is
    inside something (or sees a single sided surface from behind); counted inside at any distance there, those entries
    are no light, and the line is gone (the wall flat at 50, falling to 43 into the corner as the probes alone have it),
    from 0.6 and 1.6 m further back too. The regression set against HEAD (`temp/gi-disocclusion/ab.ps1`, settled 1500
    frames: salle_de_bain framed, at the user's two poses, Sponza's columns and corridor, Bistro by day and night,
    CornellBox) within 0.74 of 255 a strip everywhere, by eye the same.
  - **A clip's scroll and the bounce.** The specks weren't all of it (the user, 2026-11-18, after GI Seen: "It does
    not look correct... Moving forwards and backwards a couple of times makes bright lines along the edges pop in
    sharply"). Walking the same 1.2 m to and fro four times (`forward.ps1 -Move 40 -Cycles 4`; each step of the walk is
    then drawn 8 times, and `visits.ps1` sums what a frame is brighter by than the median of its step's visits, past
    2 of 255, a million pixels): a band along the wall's edges at the corner lit up 22 frames into every walk back,
    more each time (1433, 3613, 5787, 9868), fading over 15 frames; with the near field off it stayed instead, a step
    brighter each time (to 4344); as much with the cache off or the fast mean off, a thirtieth with the bounce off
    (147), and the last clip's light alone (`SPEEDO_GI_LOOKUP_LEVEL=2`), which never scrolls, brightened with it. The
    frame was the middle clip's scroll back (`SPEEDO_GI_EYE_LOG`). The voxels' lighting (`ComputeGiLight`) bounces the
    volumes' light at each voxel, which on the frame its clip scrolls is still the last update's, written at the
    clip's place before the scroll (the resolve writes it at the new one later in the frame): every voxel of the clip
    bounced, for that frame, the light of the point 8 voxels along the scroll from it. Walking forward that is a
    point in the room, much the same; walking back, the far wall's voxels took the light 0.45 m beyond the wall
    (the finest clip's 0.22 m beyond is still in the wall). One frame of it, but every ray that met those voxels took
    it: the near field's for that frame, shown through its history's 15, and the cache's entries and the probes for
    as long as their windows, so that each walk back added to what the last had left. Now the lighting reads the
    volumes where they were written (the clip's origin less its scroll), and a voxel that scrolled in, which they
    hold nothing of, takes the coarser clip's light there (where that was written, likewise). The same walk: the
    frames' mean 1163 to 92, the worst frame 9868 to 609, and the walks back no brighter each time (417, 149, 157,
    198 after them); the cache off 18696 to 3087 at worst.
    And `gGiAhead` (how far a voxel's entry's fast mean is ahead, which the cache's rays add to what it bounces) stayed
    where it was when its clip scrolled, as the bounce's read did: a voxel bounced what the voxel that had been at its
    index was ahead by until its own entry's next ray, and one without an entry for good. Carried with the voxels now
    (`ComputeGiShift`, through `gGiDirect`, which `ComputeGiLightStatic` makes anew after reading it), as `gGiKept` is.
    Whatever is kept per voxel index has to move with a scroll or be read at its place before it: the voxels, their
    kept light, their visibility bits, the cache's page tables and the seen stamps did; these two didn't.
    Left: the frame a wall enters a clip whole. A clip is clamped to the last clip's edges, so at the scene's edge it
    doesn't fade in (`GiClipWeight`): on the first walk forward the middle clip's second scroll (the frame 17 steps
    in) took the far wall in, all of it at once and at full weight, its entries begun from the last clip's as they
    are, unfiltered, where the frame before showed the last clip's filtered over four times the reach: the wall goes
    blotchy in one frame (patches up to 10 of 255 either way, 0.5 m across; its mean 62.3 to 59.8 with the fast mean
    on, the same with it off) and smooths over the next 15 or so. The same with GI Seen or without, before this fix
    and after; not there with every block read (`SPEEDO_GI_CACHE_EVERYWHERE=1`). With the cache off the probes do
    the same there (a layer of probes new to the clip, begun from the last clip's: 66.8 to 63.0), not where the rays
    walk the voxels (64.9 to 64.0). Looked at again (2026-11-19, with the corner fix below in): 62.4 to 60.1. Not the
    new entries' seeds: the voxels that pop were in the middle clip already, their entries seeded 18 frames before
    and holding about 70 rays of their own besides (`SPEEDO_GI_DUMP=pixel:300,250` at steps 16 and 17), they only come
    into its weight at the scroll; begun from what the coarser clip showed (its volumes read by hand where it isn't
    updated that frame, which the middle clip's scroll always is not, counted as a settled entry's 256 samples:
    tried, not kept) the step was the same (61.8 to 59.8, the same patches). The same with the probes off
    (`SPEEDO_GI_PROBES=0`: 59.1 to 56.9, the same patches). So it is the two clips' caches disagreeing: each holds its
    own residual noise, which changes over tens of seconds (see the far wall's below), and the switch shows one for
    the other. What would take it out: less of that noise, or the clip's weight eased in over some frames after a
    scroll rather than at once (`GiClipWeight` by the clip's place a few frames back: a change to what Renderer
    hands the shaders).
    Why the weight switched at once at all (2026-11-19): `GiClipWeight` was the smaller of the box's fade (the box
    around the clip's center, which moves with the eye smoothly: half the clip's size less half a snap, faded over a
    third of its half size) and the edges' (0 at a probe spacing inside the clip's edges, 1 at 8 voxels more). The box
    is 0 to a snap (8 voxels) inside the edges, wherever the clip snapped to, the edges' fade 16 voxels deep: within
    the box's fade band the edges' was mostly the smaller, and it moves with the clip, a snap at once, on the side it
    moves toward (what comes in, at full weight) and the side it leaves (a weight of up to 0.8 to none). An ease in
    time couldn't help the side it leaves (those voxels are gone). The weight is now the box's alone, less a probe
    spacing and faded over 8 voxels (`GiData::fadeExtent.w`) inside that, which a clip that isn't clamped holds
    wherever it snapped to, so a snap changes no point's weight; the edges' fade only on the sides a clip is clamped
    to the last clip's (found in the shader by the clip's place against the last's, `SHADER_TYPES_GI_SNAP_PROBE_COUNT`
    shared with `gi::MakeClipmap`), which stay put while it is. The rays still leave a clip at the whole box
    (`GiFadeBoxExit`). The region a clip lights in full is the same as the edges left it at worst before (MovingInterior's
    finest clip, 68 voxels a side: 14 voxels around its center). Walking to and fro (`forward.ps1 -Move 40 -Cycles 4`,
    `temp/gi-disocclusion/pops.ps1`: per frame the change of the blurred frame from the one before, over the median of
    the 4 around it, per million pixels): the scrolls' steps (8 of them, 40000 to 103000) gone, the worst frame 8800,
    the walk's total 615000 to 130000; the far wall at the step 63.34 to 59.98 before, 60.01 to 59.85 now
    (`temp/review/clip-entry-pop-before-after.png`: the frames and their change, red darker, green brighter). The
    regression set against HEAD (`ab.ps1`) within 0.66 of 255 a strip.
- **The dark walls converge slowly, not only noisily** (MovingInterior without its animation, the user's first pose, the
  far wall against 12000 frames with a window of 16384: SPEEDO_GI_CACHE_SAMPLES): -22 of 255 at frame 300, -11 at
  1500, -3.5 at 6000, darker all the way. Each bounce reaches the wall through the cache's mean, which a window of half
  the updates (up to 1024 samples) takes long to follow: a dim interior lit by bounces alone brightens for thousands of
  frames (as salle_de_bain's front did). A longer window (4096, the user's first choice) smoothed the blotches at 6000
  frames but converged slower (7.3 from the reference against 5.2), and counting the warm-up's 4 rays a voxel in the
  window changed nothing at 300 (20.7 either way): both not done. What would: a window that follows the light while it
  still changes (the recent samples' mean against the whole one's, beyond the noise) and grows once it settles.
  Measured again from a cold start in one run (`SPEEDO_REPEATABLE` with `SPEEDO_CAPTURE_COUNT` and `_STRIDE`, stepped as
  a 60 Hz session, `temp/gi-disocclusion/curve.ps1`; the reference above was itself still rising), the wall's mean of
  255 every 500 frames: 55.7, 58.0, 58.9 at 1500, 60.5 at 4000, 62.7 at 12000, its level about 65.5. What sets the pace:
  - what an entry's rays read at their hits is the voxels' light as shown, entries' means themselves, so each bounce
    waits a whole window for the one before it, and the window grows with the frames (half the updates, to 1024);
  - 8 times as long through what no lookup reads, which traces a ray in 8 frames into the same window: with every
    block read (`SPEEDO_GI_CACHE_EVERYWHERE=1`) 61.7, 63.5, 63.9, and 65.6 at 12000;
  - a short window is fast and blotchy (64 samples: there by 2000 with every block read, the mean |pixel - the 33
    pixels' mean around it| 5.2 against 3.1), and 16 samples leave half the light the probes' (71, another level).
  Fixed by bouncing on a fast mean while showing the whole one (2026-11-18): an entry also keeps the mean of its last
  `SPEEDO_GI_CACHE_FAST_SAMPLES` samples' light along its open side (16; an eighth of them where its block isn't read; a
  half in its fourth words' spare bits), and writes how far that is ahead of its whole mean per voxel (`gGiAhead`, a
  float a voxel, 4 bytes more of about 100; a half since 2026-11-22, beside where the entry's rays start: see "Light
  gathered under a double sided surface"), which the cache's rays add to the bounced part of what a hit's voxel
  reflects (`GiHitLit`; the probes', the near field's and the reflections' rays read the light as shown). The wall:
  62.7 at 500, 65.9 at 1000, 65.3 at 1500, 65.2 at 12000, as smooth once settled (3.2 either way; 5.0 against 4.5 at
  frame 500) and as still (a frame from the next by 0.023 of 255 against 0.019). The bathroom scene's frontal pose
  (`-0.8,17.0,-6.5,0,1.2`, exposure -1), which kept brightening for 14000 frames: 148.3, 153.9, 155.7 and 157.8 at frames
  300, 1000, 3000 and 9000 before, 150.8, 161.7, 161.9 and 162.0 now. The regression set at 900 frames within 0.7 of 255
  on average (the bathroom +0.65, the others under 0.15); the GI pass the same within the runs' noise (Sponza 1.93 to
  1.96 ms, Bistro 2.39 to 2.40, at night 2.27 to 2.28, off and on interleaved). Tried first: the ratio of the two means,
  clamped to 1/4 and 4, on the bounced part: the wall settled 2 of 255 low (the fast mean is mostly a little under and
  rarely far over, so a clamped ratio averages under 1); their difference averages none. A fast mean of 32 samples was
  half as fast (56.5 at 500, 62.3 at 1000), of 64 slower again.
  The wall's mottling is the other half (the user: "general noise levels in the dark areas"), and with the bounces
  no longer waiting on the window, what smooths it costs no convergence:
  - a longer window doesn't work, and why isn't found: 4096 samples (`SPEEDO_GI_CACHE_SAMPLES`) were 14% smoother by
    frame 10000 (2.73 against 3.15), then the wall rose without end (66.9 at 12000, 88.3 at 24000, its level 65), in
    pink and yellow patches (the colors each their own way); 2048 fell instead (65.3 at 6000, 60.7 at 24000 and
    falling), 1024 holds (65.1 to 65.6 over 24000 frames). The same with the fast mean off. A wall voxel's entry at
    frame 16000 held 1.8 times the light in every face with 4096 (`SPEEDO_GI_DUMP=5.95,1.5,0.3`: 0.0101 against
    0.00567 along -x, where a half's step is under a thousandth). First taken for the faces' halves rounding a mean
    of thousands' small steps away, but the faces rounded up or down at random by where the value lies between two
    halves (tried in `GiCacheStore`, 2026-11-18, not kept) changed neither the drift nor anything at 1024 (the wall
    the same to 0.13 of 255), nor did rounding up whenever between two, though a step up at every store ran away at
    once: what drifts past 1024 samples is still to find before a longer window can be had. Tried again
    (2026-11-19), the rounding shader-only and checked to be on (the captures differ in 8.6% of their bytes): at 4
    rays a voxel without the window's time limit (`SPEEDO_GI_TIME_BASED=0`, `SPEEDO_GI_CACHE_RAYS=4`, frames 2000 to
    8000 of the static room) the wall the same to 0.1 of 255 with and without it, 64.0 to 64.2 with 1024 and 62.7 to
    62.6 with 4096 either way, and with the bounce off (where 4096 creeps up, 21.2 to 22.6, and 1024 holds at 21.8)
    the creep the same to the digit: not the halves. Nor the filter's edge test (`SPEEDO_GI_CACHE_SIGMA=1000`: the
    same to 0.1). With the fast mean off both windows are far slower (1024: 54.2 at 2000, 62.0 at 8000; 4096: 49.0
    and 55.4), so the gap between 64.3 and 62.8 with it on may be where each settles, not a bias; which of them is
    right is open. (Without the time limit the window is the setting; with it, at most half the updates since the
    cache started, and the last clip, which lights the far wall, updates every other frame: 4096 and 2048 were 1500
    at frame 6000, which is why they first measured the same as 1024.)
    Past 8000 frames it isn't a level but a climb (2026-11-19, the static room to frame 16000, a capture every 1000,
    clients in parallel, `curve.ps1`): with a window of 4096 (`SPEEDO_GI_TIME_BASED=0`) the far wall rises 2.5 of 255
    every 1000 frames from about frame 4000, without end (53 at 1000, 61.8 at 5000, 91.8 at 16000), where 1024 levels
    at 65.5 by frame 6000 and the default (the time limit) holds 65.5 throughout; 2048 holds, 8192 climbs as 4096 does
    from the same frame. The same with the bounce off and the fast mean off (no light of the cache's reaches what the
    rays read, `GiHitLit`: 11.7 to 27.3, where 1024 holds 13.5), with the probes, the near field, the thin test or
    two of the filter's passes off, settled early or never (`SPEEDO_GI_CACHE_SETTLED_FRAMES`); faster per frame
    without the stagger (per update, then). By the rays an update (`SPEEDO_GI_CACHE_RAYS`): 1, the climb; 4, 13.7 to 14.5; 8, flat at 12.5. The entry itself
    climbs (`SPEEDO_GI_DUMP=5.92,1.92,-0.36`, the wall's voxel from the first pose: its face along the open side
    0.00135 at frame 3000, 0.00314 at 9000, 0.0047 at 15000 with a ray an update; 0.00096 and 0.0012 with 8), where
    512 rays more a frame for the dumped voxel alone, into a copy of its entry, put their mean at 0.0015 and 0.0009:
    its samples didn't rise, its mean drifted off them. It was the halves after all: that face kept in 32 bit floats
    (in the fourth words, which hold the emitters' light where there are emitters) stayed at its samples' mean (0.0013,
    0.0011, 0.0008 at frames 3000, 9000, 16000) and the wall flat (12.2 to 13.4). Past 2048 samples a sample moves the
    mean by less than half a step of a half unless it is far brighter than the mean (the face is 0.002, a step of a
    half there 1.9e-6, a dark sample's step down 4.9e-7): rounded to the nearest, the dark samples' steps were lost and
    the bright ones' kept, a few rays an update adding their steps in floats before the store. Rounding at random
    (`GiHalfDithered`) should have taken it out, and twice changed nothing, to the digit: it compared floats (the half
    back as a float against the value, `f16tof32(f32tof16(v)) > v`), which the compiler folds, so it rounded to the
    nearest still. From the float's bits alone (the half's bits are its exponent less 112 and its mantissa's top 10;
    the 13 bits dropped, more for a subnormal, decide by a random number of as many bits) it is the mean: the climb
    flat (12.2 to 12.6 to frame 16000), 4096 with the bounce settled at 64.0 where it climbed past 90, 1024 at 64.5,
    the default 64.8 where it was 65.5 (rounded, the means were off below 2048 too). The regression set against the
    halves rounded to the nearest (`ab.ps1`, settled 1500 frames) 0.15 to 1.7 of 255 brighter a scene (Sponza's
    corridor at exposure 10 the most), by eye the same. With the window working past 2048 the default is 4096 (see the
    far wall below).
    And a bias found on the way: where the bounce is off, the fast mean's lead (`gGiAhead`) is all a hit's bounced
    light, which `GiHitLit` clamps to none below 0, so only its positive half arrives (1024 without the bounce: 23.4
    with the fast mean, 13.5 without).
  - what the mottling is, measured (`temp/gi-disocclusion/wallpattern.ps1`: two captures' blotches, each pixel less
    the mean of the 33x33 around it, and their correlation): settled, the wall's blotches are 0.95 to 0.99 the same
    12000 frames apart, from run to run, and between resolutions 96, 128 and 160, so most of that measure is the
    wall's own light (brighter in the middle, darker to the corners and by the boxes); what differs between
    independent frames is 0.44 to 0.49 of 255, the noise proper (5.7 with a window of 64). It shows more at a finer
    resolution (160) and less at a coarser (96), as the filter's reach in the world goes.
    Measured again (2026-11-19, the user: the pattern "stays put"), on the wall alone this time (the crop above held
    the boxes' and the wall's edges, which made those correlations; now its inside, 0.333 to 0.542 across and 0.370 to
    0.574 down, at the 120 pixel scale the blotches have, `wallpattern.ps1 -Reach 60`, `detail.ps1` to see them):
    - it isn't the cache rays' fixed sequences (each voxel's offset of its R2 sequence, a hash of its place, which
      would leave each voxel its own fixed error): with another seed for the hash the pattern at frame 3000 is 0.68
      the same, at 6000 0.90, and at one filter pass the entries' own noise 0.14 the same (it is the seed's);
    - the pattern both seeds share is the wall's light: with 4 times the samples and 4 rays a voxel (window 4096
      without the time limit, frame 4000) it is the same at resolutions 96, 128 and 160 (0.93 to 0.95), brighter at
      the top and darker toward the floor between the boxes, about 1.8 of 255 off the 120 pixels' mean; the probes
      alone (`SPEEDO_GI_CACHE=0`) draw another, bright upper corners and a ring low in the middle (0.64);
    - on top, the cache's own noise: two seeds differ by 1.0 to 1.2 of 255 at that scale with the window of 1024 (each
      about 0.75 off), 0.55 to 0.63 with 4096; it changes over the window's span (a ray every other frame once settled:
      2048 frames, half a minute), so it looks fixed. Most of it is the sun's patch on the floor under the skylight,
      which lights the room: one ray of the wall's in about 40 meets it, at 30 times the mean
      (`SPEEDO_GI_DUMP=5.977,0.51,1.989` with `SPEEDO_GI_CACHE_RAYS=8`). Less of it wants more samples (the window, see
      above) or rays sent toward the patch. Sent toward where the entry's own faces say its light comes from (their
      first moment: each face's luminance less the opposite one's; a quarter of the rays in a 26 degree cone around it,
      each sample weighed by the mixture's density, where the moment is more than 0.4 of the faces' sum: tried
      2026-11-19, not kept), nothing changed, to the digit: the wall's entries see their light nearly evenly (the faces
      0.0033 to 0.0053, the brightest along the open side, the moment 0.07 of their sum: `SPEEDO_GI_DUMP=5.92,1.92,-0.36`
      from the first pose), so the patch is no direction the faces can tell. Guiding toward it would want a direction
      kept per entry (its fifth uint4: a quarter more of the cache's pool);
    - the near field, the upscaler and the animation add none (the animated room between the sphere's passes the same
      shape as the static one, 0.82, and no noisier).
    `temp/review/far-wall-window-seeds.png` (window 1024 two seeds, window 4096 two seeds, the wall's detail times 12)
    and `far-wall-resolutions-4096.png` (96, 128, 160, the probes alone).
    With the faces rounded at random (see the window above), two seeds at frame 20000 of the static room (the time
    limit on, as by default) differ on the wall by 0.88 of 255 with a window of 1024, 0.44 with 4096 and 0.57 with 8192
    (which the time limit still held to 5000 there), the level the same (64.6 to 65.2): the default is 4096 since, the
    cache settled after 2048 updates still (`kGiCacheSettledUpdateCount`; it was twice the window), so that it costs
    what it did, its window filling at the settled rate after. Half the noise takes the time limit's 8192 updates of
    the last clip to come (4.5 minutes at 60 frames a second), and what both seeds share, 1.7 of 255, is the wall's
    light.
  - one more pass of the filter at the coarser levels does (`Renderer`: two for the last level and one more per level
    finer, at most `SPEEDO_GI_CACHE_FILTER_PASSES`'s 3; it was one for the last): 5.0 to 3.2 at frame 500, as smooth
    as it used to be settled, and 3.2 to 2.7 settled (two more: 2.8 and 2.7). The reach at the last level is 3 voxels
    rather than 1 (0.7 m in Sponza, where 7 voxels' 1.6 m spread the light under the curtains over the corridor's
    floor): Sponza's columns pose, the floor in front of its curtains (`-3.0027,0.2878,2.2588,9.549,338.778`), the
    bathroom scene's shelves from the front and Bistro at night are as they were by eye and within 0.6 of 255 a strip
    (1.1 in one of the bathroom's, where a white speck by the window's handle went). The GI pass 0.58 to 0.62 ms in the
    room, Sponza's 1.96 to 1.99, Bistro's 2.40 to 2.42, at night 2.28 to 2.32.
  - every level's three passes since (2026-11-20, the user's poses with the world cache off): MovingInterior's far wall
    from `1.1616,1.5031,-0.5107,0.798,-256.876` is at its last level from 3 m, and with that level's two passes stayed
    blotchy with a pattern 4000 frames don't take away (the mean |pixel - the 33 pixels' mean around it| 0.49 of 255,
    1.83 over 121 pixels, settled, repeatable; 1.26 after 4000 frames; the world cache's 0.29 and 0.66). Not the
    cache's noise alone (the probes alone are as blotchy over 121 pixels) nor the lookups' level (through the coarser
    levels alone 1.45 and 1.59): the bounce makes a pattern the voxels hold to, and only the filter's reach takes it
    out; four passes or a sigma of 16 changed nothing while the last level kept two. Three at every level: 0.31 and
    0.79. The curtains' light, the reason for two, no longer spreads, since the taps past something crossing their way
    are left out (`GiCacheFilterBlocked`, below): the regression set within 0.5 of 255 a strip against two.
  - but a reach in the world, not a count, since (2026-11-21): Sponza's last level's three passes (7 of its voxels of
    0.27 m, 1.9 m) smoothed away the corridor floor's darker, bluish band along the arcade's bases (the sky's light
    between them, the bases' shade), which the finer levels hold, and the hand-over between them was a line on the
    floor that moved with the eye (the user's report, from `-4.5893,0.8216,2.8666,-2.892,-268.574` walking to
    `-1.8188,0.9616,2.9356,-2.892,-268.574`; through the last level alone, `SPEEDO_GI_LOOKUP_LEVEL=2`, no band with
    three, a band with two). The two for the last level and one more per level finer again, and as many more as keep
    the reach (2^passes - 1 voxels) within a metre, `kGiCacheFilterReachMeters` by the scene's metres per unit: Sponza's
    and Bistro's levels 3, 3 and 2 as before, MovingInterior's 3, 3 and 3 (its last level's voxels 0.11 m; the far
    wall 0.30 of 255, as with three).
  - and walking that way, the band still forms behind the eye's front: in the indirect view (upscaling off) the walk's
    last frame is 2.65 of 255 from the settled frame of its pose, 6.1 in the band's strip, where the band ahead stops
    short. Tried and not kept: an entry not settled yet blending from the next coarser clip's light rather than its
    probes' (the voxels a finer clip takes in showing the coarser level's detail until they settle): 2.73, and settled
    frames 0.9 apart. The floor ahead where the band is missing is the coarser clips' (1 and 2, the clip view), not the
    finest's: their own light lags there as their boxes scroll, and blending from them gives what they lack.
  - and no tap two voxels away or more past something crossing its way (`GiCacheFilterBlocked`: one of the voxels a
    voxel out along the open side, between the voxel and the tap, holds a surface facing along the way to the tap).
    Sponza's arcade walls go on up behind the vaults into the sunlit gallery, the same plane facing the same way, and
    the middle level's top voxels of each bay (0.13 m) took the gallery's light through the taps 4 apart: a bright
    bar at the top of each bay under the vault (the user's report, 2026-11-20, `-10.0017,0.7484,1.3894,-19.073,-200.834`;
    the entries' own light 3.5e-5 along the wall's normal, the volumes 5.7e-4, the corridor's below 2.6e-4: found by
    the lookup's 8 voxels dumped at the pixel). By anything there rather than what crosses the way, a vault's taps along
    the wall it springs from were left out too, and its few samples showed (an orange blot over a corbel). The
    corridor's floor along the columns' bases is darker (20 of 255 against 27; the world cache's 23): its filter took the
    courtyard floor's sky light through the taps under the bases. What it doesn't fix: the specks at the vaults'
    springings (a wall's lookup reads a corbel's voxel, whose light is measured over its side facing the open arcade and
    the sky; leaving such voxels out, or weighing them by how far their side faces the lookup's way, framed
    MovingInterior's walls bright at every edge, or didn't move the specks), and the levels' disagreement at their
    hand-over (the gallery floor's sky light by its balustrade, `1.1663,5.5013,2.8972,35.570,-988.359`: through the last
    level alone it is far weaker; a fade twice as wide only spreads it).
- **Moving clips**: a clip that follows the eye moves by whole snaps (`gi::kSnapProbes`, one probe: 8 voxels; clamped
  to whole snaps at the last clip's sides too), so everything a point inside it is lit by stays where it is in the
  world as it snaps: its probes are laid out from its corner (`GiData::probeOrigin`), a whole number of snaps from the
  clipmap's, and keep what they hold across a snap (the update reads each probe's history from where it was, by the
  scroll); the flags' blocks are rounded up (`GiLevelVoxels`, `gi::LevelVoxels`); and each clip has a box around its center that it always holds well
  inside (`GiData::fadeCenter`, `fadeExtent`: half its size less half a snap; with a snap of 4 probes and a spacing and
  a voxel more of margin, Sponza's thin clips, 68 voxels tall, kept a box a third of their height, and the finest clips
  lit nothing on its ground floor: everything there was the last clip's, 23 cm voxels and 1.9 m probes), which
  the shading fades it out toward (over 8 voxels, a probe spacing inside it, `GiClipWeight`; over a third of its half
  size until 2026-11-19, see "A clip's scroll and the bounce") and rays leave it at (`GiFadeBoxExit`), so what changes
  as a clip snaps changes with the eye, smoothly. The shading takes each clip by its weight of what the finer ones leave
  (`GiSampleAt`). Under a still camera with the clips moving 6 m through Sponza's arcade (`SPEEDO_GI_EYE_PATH`, 120 frames),
  the frames' means differ from the still run's by 0.1% at most.
- **The coarser clips in turn** (`GiData::updated`, `Renderer::InternalPlanGi`; `SPEEDO_GI_STAGGER=0` updates every clip
  every frame): the finest level's clips are updated every frame, the coarser levels' a level a frame (with 3 levels the
  second's every other frame and the last's in the frames between), since a clip costs about as much as another (its
  voxels near a surface: Bistro's evening 38.4 ms with 3 clips, 33.9 with 2, 30.2 with 1; Sponza 22.3, 19.2 and 16.5);
  every clip in a frame that resets, starts the surface cache or dumps, and a clip voxelized (moved) whenever; and every
  clip while something moves (`myGiMovingJobCount`): a coarser clip's light then changed in steps on the frames it was
  updated, a flicker at half the frame rate (CommercialRefrigerator's interior as its door swung, the user's pose
  `-0.0138,1.0920,2.3710,0.000,0.000`: a region's change alternating 3.3, 2.2, 1.7, 2.5 ... between frames, now as
  smooth as with `SPEEDO_GI_STAGGER=0`), for BrainStem's frame 4.6 to 5.2 ms (2560x1440, RTX 4070 Ti SUPER). A clip
  not updated keeps all it holds (its voxels, light, probes, volumes, cache) as its last update left it, which the
  shading, the rays walking on into it and the finer clips read. Each clip has a clock (`Renderer::GiClipClock`) that
  advances only when it is updated: its frame stamp, ray phase, halves (probes, visibility, the cache's tables) and the
  cache's window and warm-up count its updates, so that it takes every ray phase (by the global frame, a clip updated
  every other frame would only ever trace half of them), its blend is the frame's raised to the frames since its last
  update (in sixtieths of a second where by time), it is frozen only after two laps of its own phases, and what changed
  while it waited (the lights, the emitters, the cache started over) is taken in at its next update (`relight`). A finer
  clip's probe or cache entry new to it starts from the coarser clip's current half where that isn't updated in the
  frame, the previous one where it is (`GiCoarserHalf`). The ray budget and the cache's pool go by every clip's count
  as it last counted (`GI_STATISTIC_CLIP_FIRST`): by the frame's, the rays per probe rose and the pool shrank. What
  the coarser clips light follows a change over twice the frames: walking along Sponza's corridor, the far corridor
  differs from every clip every frame by 0.16 of 255 (still: 0.09, the captures' noise 0.07), smooth, with no seams.
  At the benchmark poses (balanced, interleaved): Sponza 21.93 to 19.93 ms, Bistro 38.44 to 37.36, San Miguel 44.17 to
  43.85; less than a clip's cost from the clip counts (those change what the remaining clips cover), the finest clips,
  updated every frame, holding most of the surface cache's work.
- **Lighting the voxels** (`GiDirectRadiance`): each voxel's emission plus its albedo over pi times the punctual lights
  of its cell and the emissive surfaces where they are lights (`EmitterVoxelIrradiance`, see "Emissive surfaces as
  lights": the rays that read a voxel's radiance then take its emission out) (`gi::BinLights`: a 16^3 grid of cells over the volume, as the clusters bin lights), from its front only
  (a wall's voxel isn't lit by the sun outside it), each shadowed by a ray through the voxels (the shadow atlas covers
  only what the camera sees; `GiOccluderDistance` visits each voxel the ray crosses once, Amanatides and Woo). The
  shadow rays start a voxel out along the voxel's normal, the bounce's lookup 1.5 voxels and the point its probes are
  tested from half a voxel, and the surface cache's rays a voxel along its open side (`GiCacheOrigin`), each no
  further than halfway to a surface facing back at it there (`GiOpenAlong`: a ray through the static BVH, 3 voxels at
  most, once a voxelizing in GI Light Static, kept in the direct light's alpha, in voxels; the cache tracing a ray of its
  own each update cost Sponza 0.3 ms a frame): in the slot between salle_de_bain's bench and its wall (0.66 units, a last clip's voxel and a half), the bench
  back's voxel faces the wall 0.26 units off, and its shadow rays started past the wall, outdoors, where the sun lit it,
  and its bounce read the volume outdoors; whatever read it (the cache's rays in the slot, the probes', the near field's)
  drew the slot as a band brighter than the room (488 of 765 against the wall's 330 at the user's pose
  `13.6266,14.4950,35.7340,16.977,4.625`, the slot's wall seen from above), now 66 to 133, fading out toward its opening.
  A voxelizing's GI Light Static took 0.14 to 0.24 ms in Sponza, 0.92 to 1.41 in Bistro's street. The
  static voxels' is kept (GI Light Static, `ComputeGiLightStatic`, into `gGiDirect`) and
  made again when a clip is voxelized or the lights change (`Renderer::myGiLights`, compared bytewise: the environment's
  rotation turns its dominant lights, which are among them, and the sky's sun moves with the time of day). The part the
  lights don't change, the emitters' light and how far each voxel is open (`GiOpenAlong`), is kept apart
  (`gGiKept`) and taken as it is where the lights changed alone (`GiData::lightsOnly`): remade with them, the
  sun moving every frame cost Bistro's street 72 ms a frame (its emitters' samples and shadow rays per voxel), 10 now. A
  change of the same lights turned by under 2 degrees and dimmed by under a quarter is gradual
  (`Renderer::myGiLightsGradual`): the surface cache keeps what it has rather than starting over, sampling as where
  something moves (32 samples at most) for `kGiLightsFollowFrameCount` (64) frames after, so that it follows the sun
  rather than restarting every frame; and the static voxels are lit again a share at a time, over
  `kGiRelightPhaseCount` (4) of the clip's updates (`GiClipClock::relightPhase`, `GiData::relightPhase`: every fourth
  voxel from the update's phase, a thread each; `SPEEDO_GI_RELIGHT_PHASE_COUNT`, 1 for all at once; a change in the
  middle of a cycle starts its count over, the shares still to come taking the newer light; a change that isn't
  gradual, a voxelizing or the emitters toggling light every voxel at once, as before), since the sun moving every
  frame (600x) lit every voxel of every clip every frame, the shadow rays of GI Light Static (4.8 ms at Bistro on
  NVIDIA; about a millisecond on KosmicKrisp, whose rays are queries: Bistro's street at 600x 64.0 to 63.0 ms). Each
  frame GI Light (`ComputeGiLight`) gives every voxel
  that (a moving voxel's, which differs from the static one at its place, made then: a moving voxel doesn't shadow the
  static ones' kept light), plus what its albedo reflects of the probes' light at it as the last frame resolved it
  (`GiData::bounce`, `SPEEDO_GI_BOUNCE=0` for the lights' light alone): the light bounces on, a bounce a frame, settling
  with the blend; not for the first 32 frames after a reset (`kGiBounceDelayFrameCount`): the probes' depth maps are
  provisional then, and what leaked through them into the voxels fed the probes, which fed the voxels, with the loop's
  time constant, `1 / ((1 - hysteresis) (1 - albedo))` frames: Sponza's arcade started 8 times too bright and darkened
  for 200 frames; it rises to its level in 100 now. The shadow rays every frame cost CornellBox-Sphere 2.9 ms of its 3 clips' lighting, the kept light 1.3.
  What moves does shadow the kept light: the moving voxelizer stamps the frame into the blocks it writes (the flags'
  second word, `SHADER_TYPES_GI_FLAG_WORD_COUNT`), and a static voxel whose line to a light that lights it crosses a
  stamped block (`GiMovingShadows`: a walk over the blocks, `GiMovingBetween`) traces its shadow rays that frame; only
  with something moving in the frame (`GiData::moving`).
  The **flags** (`gGiFlags`, `ComputeGiFlags`): per block of 4x4x4 voxels
  (`SHADER_TYPES_GI_FLAG_LEVEL`) the lighting stamps the frame into the blocks holding something (a plain store: an
  atomic maximum kept whatever a new buffer held, and such a block never counted again, which blacked out Sponza; the
  store keeps bits 0 to 3, which the same dispatch reads to skip the blocks nothing was near: storing the stamp alone,
  the voxels of a block that ran after one of them had stored skipped their lighting, their radiance that of the frames
  before the bounce. lavapipe, running the groups one after another, lit one row of each block, the probes' light no
  further than the emission, and KosmicKrisp a few blocks less: found 2026-11-11 by the furnace interior, see
  docs/visual-testing-plan.md's step 4), then
  bit 0 says whether anything is in the 3x3x3 blocks around it and bit 3 whether that is within 4 blocks along each axis
  (three separable steps): the resolve skips the blocks without bit 0 (the lookups are within a few voxels of a surface)
  and the probes without bit 3 (a probe is only ever looked up from within its cell of a surface's lookup), which is
  what makes a clip's cost that of its surfaces rather than its volume. A block's third word is the last frame a pixel's
  lookup read it, and bit 4 of its first whether the surface cache works on it this frame (see the surface cache, below).
- **The probes** (`GiData::probe*`): a lattice every `SPEEDO_GI_PROBE_SPACING` voxels (8 by default) from the clip's
  corner (a multiple of 8, so that the resolve's blocks of 4 voxels never span two cells), centered on voxel boundaries
  (so every point of a voxel interpolates the same 8), of which only those near a surface trace (the flags, below):
  CornellBox-Sphere 6400, Sponza's inside 4200, Bistro's street 4500, DamagedHelmet 10400 (its surface fills its last
  clip). The rays per probe are `SPEEDO_GI_RAY_COUNT` (128) at most, and `SPEEDO_GI_RAY_BUDGET` (500000) over the
  probes traced last time (in eights, at least 16: a ray costs 4 to 8 ns here, so DamagedHelmet's 10400 probes at 128
  cost 10 ms; at the budget's 46 they cost 6.5, and Sponza keeps 112, Bistro 104, CornellBox 72). Each probe is at its
  lattice point plus an offset it keeps (coefficients 2 to 4's w; DDGI's relocation, `GiProbeOffset`,
  `GiProbePosition`: the rays trace from where it was, `GiProbeHistory`, and the update moves it): out of the voxels at
  its corner along their normals' mean by 0.45 of a spacing at once (`GiCornerAway`; a probe on a wall's voxel shell
  saw nothing and was skipped, and the points nearest it took the mean of the others: Sponza's inside against its
  surface reference from 1.24 to 0.95 mean |log2|; a step of one and a half voxels, stopping as soon as the corner was
  clear, left a probe's harmonics the surface's and drew dots on CornellBox's ceiling; the move is taken only where its
  target is clear of voxels, else the probe stays where it is: taken wherever the normals pointed, a probe behind
  Bistro's hotel facade, whose two targets were each in the other's voxels, the facade's and the window frames' at
  28 cm, jumped between them every frame, one of them inside the hotel, whose emissive interior is 30 times brighter
  than the street, so the wall by the window lit yellow on alternate frames, the user's "heavy flicker": found by
  dumping the volumes along a line into the wall, 1 to 4 and back, with every probe's light, depth and position of the
  cell in front steady, and then the probes of the cell behind), else, where more than a quarter
  of its rays met a surface's back (`GiOccluderHit`: the voxel's normal along the ray, unless both of a triangle's faces
  wrote the voxel, a thin wall's, whose normal is either's: the voxelizer keeps which faces wrote in
  `GiVoxel::emissionB`'s bits 16 and 17), away from the nearest such by its distance and a tenth of a spacing, a frame at
  a time, never more than 0.45 of a spacing from the lattice point, and only while the probe settles (its first two
  cycles of ray phases, again whenever it starts over); a probe still inside after that (Sponza's outer walls are 0.6 m
  thick and empty between their shells) is marked (DDGI's classification: face 5's w holds the share of its rays that
  met a back, blended over its frames, inside above a quarter, `kGiInsideShare`) and the resolve skips it. Both used to
  go by the frame's rays alone, and each frame's are another set: a probe near a quarter of its rays on backs moved
  every frame within its reach and was skipped on alternate frames, which with the BVH's rays (they meet single sided
  walls from behind, where the voxels' walk passed them) flickered salle_de_bain's niches by up to 100 of 255 (mean
  change 2.64 a frame, 0.11 after; Sponza's error against its reference the same, 0.377). It costs: a wall on a lattice plane makes a plane of probes trace that returned dead before
  (CornellBox +3 ms, Sponza +1). A probe that hugs a surface sees a bright emitter in it at a steep angle: as
  spherical harmonics (bands 0 to 2, until 2026-10-22) its light rang, a yellow halo and a dark ring on CornellBox's
  ceiling around its lamp, and a bright compact source at a grazing angle kept 3/32 of its strength along the surface's
  normal (the bands' 1/4 + cos/2 + 5/16 P2 at 90 degrees; a Hanning window gained 5% on CornellBox and lost 8% on
  Sponza, and can't help: the leak is band 0's quarter, which only the higher bands cancel): Bistro's lit interiors
  and lamps, a hundred times the sky, lit the square before the hotel's entrance yellow. The probes hold an ambient
  cube instead (`GiIrradianceBasis`: 6 faces, the irradiance over pi along +-x, +-y and +-z, each ray weighed by its
  cosine to the axis where it faces it, times 4 over the ray count; `GiEvaluate`: the faces a direction's components
  point at, by the components squared): a grazing source gives a face nothing, at some accuracy between the axes (a
  uniform sky gives a 45 degree slope 0.75 of the zenith's irradiance where 0.854 is right). CornellBox against its
  surface reference reads 0.21 mean |log2| either way; Bistro's entrance from the roof 0.62 to 0.60 (the rest of its
  yellow was a lamp beside a probe, below). Each frame (the GI pass, GI
  Trace then GI Probe Update) each probe near a surface traces `SPEEDO_GI_RAY_COUNT` rays (128), a Fibonacci sphere of
  directions turned by the frame's phase (`GiRayDirection`: a quaternion from Roberts' R3 sequence over
  `SPEEDO_GI_RAY_PHASE_COUNT` phases, 32, then the same sets again, so what a probe holds converges to a fixed value
  rather than wandering, as it would under DDGI's random rotation a frame; every 32nd point of one set 32 times as
  big, tried first, was a coarse spiral of its own, and the phases' light differed by a quarter: a 32 frame pulse of
  5%), the same directions for every probe (each probe turning them its own way made each probe's error its own, which
  showed as dots where the resolve's weights single a probe out) but each probe at its own phase of the cycle
  (`GiProbePhase`: the frame's plus a hash of its place in the clipmap's lattice, the same wherever its clip has
  scrolled to): the blend's ripple (the latest subset weighs 3% of what a probe holds, the oldest of the 32 1.2%, so it
  pulses with the cycle by the subsets' differences) was the same across the probes of a region, which pulsed together,
  a 32 frame flicker of Sponza's walls that the user saw under auto exposure; at their own phases the ripples of a
  cell's 8 probes cancel in the lookup (frames 16 apart at exposure 9 differed by 94 of 255 at most, 17 after). A ray
  walks the voxels to the first one something is in (`GiMarch`: each voxel it crosses visited once, Amanatides and Woo,
  through its clip to its box around the eye and on through the coarser ones; a block of 4x4x4 voxels nothing is in
  this frame, by the flags' stamps, is crossed in one step: DamagedHelmet's frame 12.6 ms walking every voxel, 9.7
  skipping, Bistro's street 17.5 and 16.0), the environment past the last clip (`EnvironmentRadiance` at
  roughness 0.6, without the dominant lights, which light the voxels themselves), and to the first occupied voxel as
  far as the depth map reaches (`SHADER_TYPES_GI_DEPTH_REACH`, 3 spacings: the points a probe is tested for are within
  its cell), into `gGiRays` (rgba16f: radiance, distance in spacings). Where emissive surfaces aren't lights (View > Rendering Features > Emissive Lights off), a probe's ray that meets an emitter nearer than `SPEEDO_GI_EMISSION_REACH` spacings (0.5; 0 for none:
  `GiData::emissionReach`, `GiMarch`'s `emissionReach`) takes its emission as from that far away (scaled by the square
  of the distance over it, as a small source's irradiance falls), since what the probe holds lights the points of its
  cell, mostly about half a spacing from it, where a source beside the probe is that far (instant radiosity clamps a
  virtual light's contribution the same way; the references, from the points themselves, don't): a street lamp's head
  beside one of Bistro's last clip's probes (8.9 m apart, at 4 m over the square before the hotel's entrance) lit the
  square and the entrance's facade yellow, the probe's faces 2.4 where its neighbors' were 0.06, where the surface
  reference has a pool under the lamp and the interior's light at the windows beside the arch; with it the entrance
  region reads 0.48 mean |log2| against the reference (0.60 before; its mean 111 of the reference's 115, 137 before),
  and CornellBox's lamp, 8 voxels over its probes, changes nothing (0.218 against 0.210). The rays used to march a mip chain of the
  radiance past two probe spacings (anisotropic, 6 faces a voxel composited front to back, a level per four times the
  distance: a cone of a constant angle, half voxel steps each taking half a voxel's share of its opacity), through which
  a wall a voxel thick is half empty (a quarter at the level above), so every ray that met one kept a share of its
  transmittance and brought the sky in behind it: Sponza's arcades were lit from outside by a thousandth of the sky
  (their voxels hold nothing: the probes' rays read 7e-4 in the finest clip, 1.3e-4 in the next and 4e-5 in the last,
  whose rays go coarse later), and the finest clip's box edge was a dark band that followed the camera along them (the
  user's pose `0.7013,1.0767,-4.0779,0.008,90.314`; its corridor at exposure 10 against the surface reference's 0.000:
  0.16 to 0.31 before, 0.007 to 0.026 now, and no band; walking the corridor after the probes settled, the far end
  brightens evenly as it nears). Walking every voxel costs nothing in the interiors (Sponza's inside 13.1 ms against
  13.7 with the levels, CornellBox-Sphere 9.1 against 11.8; their kernel is gone), and the open scenes paid for rays
  walking to the sky until the block skip. A still frame at the lion wall steps 13 of 255 at most between frames at
  exposure 9 (17 before). The references walk without it (`skipEmptyBlocks`):
  with the blocks' reads the surface reference's fragment shader ran past Metal's command queue timeout
  (`MTL_COMMAND_QUEUE_ERROR_TIMEOUT`, `VK_ERROR_DEVICE_LOST` within seconds, with three clips; under validation too,
  which reported nothing else). The walk's loop has no `continue`: with one, KosmicKrisp didn't keep the block last
  read across it, so a probe's ray starting on a block's face (the probes are at voxel corners, and a spacing of 8 is
  a multiple of the blocks' 4) with a hair of its direction into it (`-0.000874, 0.9999, 0.013`: the push of a
  thousandth of a voxel lost to float rounding) found the same empty block anew every iteration, its exit at 0, and
  hung the gpu: the draw thread in `Fence::Wait` for good, no device loss (and once the machine, 2026-10-22, at the
  lion wall pose `-6.9604,1.3018,-0.3295,7.269,-86.774`). A step cap (4096 a clip) ends such a walk, counts it in
  `gGiStatistics` (`GI_STATISTIC_CAPPED_RAYS`, with the ray and where it was) and the renderer prints it after the
  frame: a hang mustn't take the machine down again, and a capped ray must be seen. The cap's writes are only where the
  walk skips: in the main pass's fragment shader (which holds the surface reference's walk) a storage write, even on a
  path never taken, turned its early depth test off, and Sponza's Main took 10 ms rather than 4.7. The update (a group per probe, a thread per texel
  of its depth map, the rays through shared memory; it counts the probes traced into `gGiStatistics[frame]`, host
  visible, read back when the frame index comes round, which `SPEEDO_GPU_TIMINGS=1` averages at exit) blends them into
  the probe: its irradiance faces (`SHADER_TYPES_GI_IRRADIANCE_COUNT`, 6 a color, `gGiIrradiance`; face 0's w counts the
  frames it has), and its depth map (`gGiProbeDepth`: `SHADER_TYPES_GI_DEPTH_SIDE` 16 by 16 octahedral texels of the
  mean distance and the mean of its square, half2, DDGI's, and the nearest the rays within about 20 degrees met,
  blended as the mean is (packed in the one word with the mean: the mean a half, the spread and the nearest over the
  mean 8 bits each, `GiPackDepth`; a second word doubled what the resolve streams per block, every probe's 256
  texels, for 2 to 3 ms a frame) (a running minimum relaxing by a percent a frame stepped whenever a nearer
  ray came, and the weights with it: the lion wall's still frames stepped 28 of 255 rather than 14; see below); a texel takes the rays near its direction by `cos^50` of
  their angle, `SHADER_TYPES_GI_DEPTH_SHARPNESS`, four texels a thread, and keeps what it had where none is near: at
  8x8 and `cos^10` a texel toward a wall mixed the wall's distance with what is beyond it, and a sunlit probe outside
  the wall lit the points at its base inside, and Sponza's walls showed the probe cells as blotches; 16x16 cost 2 ms on
  Sponza. A texel's first rays count whole, not by the probe's age: blended in by it, a texel first hit at age 10 held
  a tenth of its distance, and the probe failed the test that way for a second, dark patches that followed the camera
  as the clips scrolled; and a texel no ray is near yet takes a wide cone's rays, `cos^4`, as a provisional value, its
  mean negated, until rays near it replace it whole: a hole was a direction the probe saw nothing in, which the test
  took as seeing nothing at all, and with every probe of a cell new after a scroll a point took their mean unweighted,
  the sunlit probes beyond the walls too, the leaks that followed the camera. The depth rays walk the probe's own clip
  to its edge rather than to its box around the eye (`GiOccluderHit`'s `box`, which the shadow rays keep: what they
  see of the coarser voxels should change with the eye), and in a coarser clip the voxel they start in doesn't count,
  since a point outside a surface's shell in the finer clip is inside the thicker shell of the coarser: a probe over
  the floor below the finest clip's box traced its depth in the next clip from the first step, whose shell around the
  floor it stood in, so its depth read 0 that way and it was dead), by `SPEEDO_GI_HYSTERESIS` (0.97 at 60 frames a
  second: the frame's rays are 3% of it; raised to the frames' length in sixtieths otherwise, smoothed over about ten,
  so that a probe follows a change in the same time at any frame rate: `Renderer::myGiTimeBased`, `SPEEDO_GI_TIME_BASED=0`
  per frame; by frames, five times as long at San Miguel's 12 frames a second, and with the cache off Sponza at 27 came
  within 5% of settled after 220 frames, 170 by time), or by the
  frames it has while fewer (so a probe starts from its first frame's rays and settles in 30), or, with
  `SPEEDO_GI_ADAPT=1`, a quarter at a time through a change of the light (DDGI's adaptive hysteresis: each probe keeps
  how much its frames' mean radiance usually differs from what it holds, coefficient 1's w, and a difference of four
  times that and half of what it holds is a change rather than noise; a light stepping from 0 to 200 cd, eight frames
  later the probes are at 139 of the instant blend's 167, 105 with the hysteresis alone). It is off by default: on a
  still scene the same ray set returns every `rayPhaseCount` frames, and a ray that hits a sunlit patch tripped the
  test each time, a jump of a quarter every 64 frames in Sponza's arcade (70 of 255 at exposure 9, where the plain
  hysteresis steps 10, in a 32 frame ripple of the phases). A slow change (AnimatedCameraLight's 12% over 100 frames)
  lags 7% either way, by design. What the irradiance is blended towards is not the frame's rays but the mean of
  the probe's last whole cycle of ray phases (`GiData::cycleOffset`: past both halves of the irradiance, the cycle's
  running sum and the last cycle's mean; the running mean until the first whole cycle, and a change found by
  `SPEEDO_GI_ADAPT` starts the cycle again from the frame): a bright source smaller than a ray set's spacing (the sky
  through the gaps between Sponza's curtains) is met by one set of the 32, and blending each frame's rays in jumped the
  probe once a cycle and decayed it for the other 31 frames, a 32 frame sawtooth on the floor it lit (the user's pose
  `-9.1511,1.4025,-3.5391,26.261,106.832`, pixels from 134 to 84 of 255 and back; 3% of the pixels of the indirect
  diffuse view stepped by 32 or more); a still scene's cycles are the same directions, so not one pixel changes from
  frame to frame there now. A change of the light reaches the target once the probe's cycle ends, up to 32 frames
  later, then the blend's; no measurable cost (the frame 3.51 ms either way). So, where the scene is still (nothing
  reset, the lights unchanged and not being followed, nothing moving, for two laps of the phases: `GiData::probeFreeze`,
  `SPEEDO_GI_PROBE_FREEZE=0` for never), a probe that has settled (4 laps of frames) holds what it has every second lap
  rather than tracing (`GiProbeFrozen`: `rayPhase` runs over two laps, a probe's second its own by the phase's hash;
  copied over from its place last frame, its light, place, depth and cycle; decided as the lap starts and kept for it,
  the last mean's face 1 w, so that the laps it holds are whole and each cycle it traces still takes every phase once;
  counted for the ray budget as tracing): its next cycle's mean would be its last's. Bistro 41.36 to 38.31 ms, San Miguel
  45.70 to 44.49, Sponza 23.47 to 22.05 at the benchmark poses (balanced); still, the captures as before to their run to
  run noise; walking along Sponza's corridor, without upscaling 0.040 of 255 from before against a noise of 0.028 (a
  probe's light follows the clips' boxes around the eye, which move with it, a lap later), with it 0.35 at the edges
  alone (the upscaler's locks; 0.105 at an eighth of the size against 0.04), the same to the eye. The probes' buffers hold
  two halves, the frame's and the last's (`GiData::probeFirst`, `probePreviousFirst`, swapped each frame), and a
  probe's history is the probe at its place last frame (by the scroll), a probe new to the clip taking the coarser
  clip's light where it is (its previous half, at its place then, trilinear) as one frame's worth; on a reset (the
  scene changed, or the buffers were replaced: `GiData::reset`) every probe starts over and the voxels take no bounce.
  Memory per probe: 2 x (3 x 96 B of faces + 256 B of depth) + 128 rays x 8 B; Sponza's 3 clips of 16x8x10 probes
  take 5 MB.
- **The resolve and the shading**: after the probes, GI Resolve (`ComputeGiResolve`, a group per block of 4x4x4
  voxels, with its 8 probes, or 27 where it spans two cells an axis, in shared memory) gives each voxel of each clip
  the irradiance faces of the 8 probes around its center, each weighed as DDGI weighs them: by how much it faces the voxel's
  open side (its occupancy's gradient over the 3 voxels each way, the nearer counting more, plus a surface's voxel's
  own normal: by the normal alone a floor's voxel at a wall's base was tested from a point straight up, as close to the
  wall as before; over the 6 neighbors alone, the voxels a lookup lands in, a voxel or two off a surface, had no side,
  so there every probe counted by its visibility alone, and a probe behind Bistro's hotel facade, whose depth toward
  the street mixes the facade's with the window's beside it, which Chebyshev lets through, lit the wall by the window
  with the hotel's interior: the air voxels in front of the wall resolved to 1.0 against the wall's own 0.26, 0.4 with
  the reach, for 0.3 ms of Sponza's frame; wrapped,
  `facing^2 + 0.02`: DDGI's 0.2 for the probes behind let the probes below Sponza's floors, in the clipmap's margin and
  lit by the environment through its bottom, in where every probe of the corridor failed), by whether it sees the
  voxel's point (out that way by a fifth of a spacing, at least a voxel: DDGI's normal bias; a probe's depth map along
  the direction to the point, bilinear across the octahedral map's folds, and Chebyshev's inequality where the point is
  farther than the mean, cubed, at least a thousandth; and a point well behind the nearest thing the texel's rays
  met, past one and a half times its distance and a tenth of a spacing, is behind the nearer of two things the texel
  sees, the wall with the window beside it, which the mean and the variance can't tell: nearer than the mean the test
  let the point through, and beyond it the mix's variance did; a probe inside the building by Bistro's square, its
  texel toward the ground before the entrance at a mean of 0.39 spacings with the wall at 0.066, saw the ground at
  0.54 at 0.55 and, alone of its cell's probes to pass, lit the square and the entrance with the interior (the user's
  "yellow lights that don't make sense"); such a probe takes a tenth of the floor below too, since a point every probe
  fails takes their mean, which one behind a wall, lit by what is there, pulled its way; by a tenth of a spacing
  alone, a texel toward a floor at a grazing angle failed its probe too, meeting the floor nearer than the point all
  the way, a continuum, where the point is within half again of the nearest. DDGI's twentieth of a sunlit probe outside a wall lit a band
  along the floors inside, and the floor only needs to be more than nothing, since the lookup normalizes; a probe whose
  depth that way is nothing sees nothing at all, being inside a surface's voxels, and is skipped, as is one marked
  inside, above), crushed below a fifth (cubed, so that a point few see takes the mean of them rather than nothing),
  then a floor of a thousandth (after the crush: a floor before it was crushed to 1e-11, which the volumes' half floats
  hold as 0, so a point no probe saw had no light at all, black patches wherever every probe of a cell failed), and
  only then by where the point is between them (trilinear: crushing the product crushed every probe at a cell's center,
  where each is an eighth, and the facing ones least, which drew the probes' cells as a checkerboard across every
  wall), premultiplied, and the sum of the weights. A surface's voxel is resolved as the voxel out along its open side
  would be (its center moved a voxel that way for the trilinear weights and the point, within the block's cell: past
  it, the probes read were not the ones in shared memory, black and colored garbage on the floor behind Sponza's
  fountains): the lookups land in it where their offset is small (in a finer clip's voxels, below), and its own cell's
  weights admitted a probe the voxel above rejected, a bright band along Sponza's floors at their walls that survived
  everything else (found by sampling each clip's own voxels, which has none; one or two clips have none either). The
  Probe State debug view (`GiProbeStateDebug`, a data view) shows, of the 8 probes around a point in the clip that
  lights it, the share marked inside (red), the share whose depth toward the point is empty (green) and the share
  failing the test (blue): every camera following artifact so far showed in it as green or blue where the lit view
  showed nothing yet. Where the rays trace the triangles the probes keep no depth maps (step 3), and the view read the
  ones left from before: there it traces the resolve's ray itself, from each probe to the point half a voxel out along
  its normal, green the share behind a surface's back (`kGiBehind`) and blue the share a front hides (`kGiHidden`): a
  ray a probe a pixel, what the resolve would find there now rather than the bits it keeps per voxel (fixed 2026-11-18,
  the user's ask; the dump's depth map is left out there too). `SPEEDO_GI_DUMP=<x,y,z>` (or `pixel:<x>,<y>`: the point under a pixel, which `ComputeMain` unprojects into
  `GI_STATISTIC_DUMP_POINT` for the next frame's dump, `GiData::dumpPixel`: what found the entrance's probes) prints, at exit, per clip the probe nearest the point: its position,
  offset, age, backface count, its lattice point's voxel coordinates and the column of voxels through it, its depth
  map, the first rays' depths, its light along the axes, a walk straight down from it (`GiOccluderHitDebug`), the
  volumes at its voxel and along a line from the point into -z (`GiData::dumpPoint`: occupied, red along +x and
  the weight per voxel, which is what found the alternating cell): what found the dead floor probes and the probe
  beyond the end wall; and the surface cache entry of the point's voxel (`GI_DUMP_CACHE_FIRST`: its samples and inside
  rays, its rays' origin, the voxel's open side and normal, its light per face, and up to 8 of the frame's rays, their
  distance and light: what found the slot behind salle_de_bain's bench lit by the sun, see `GiOpenAlong`). Edit > Copy Scene (ctrl/cmd+shift+c)
  copies the view as the variables that reproduce it (`SPEEDO_CAMERA`, with split screen `SPEEDO_LAYOUT` and the other
  views' cameras, `SPEEDO_CAMERA_VIEWS='<pose>;...'` for views 1 and on, placed after `SPEEDO_CAMERA` places them all, or
  `SPEEDO_CAMERA_VIEWS=around` for views turned apart about the model, 360 degrees over the views, after the framing or
  `SPEEDO_CAMERA`: split screen for any model, as the regression runs it, `scripts/test-assets/splitscreen.txt`, `SPEEDO_AUTOLOAD_MODEL` (and `SPEEDO_GROUND_PLANE=0` where it stands on no ground plane), the environment's path,
  and the settings that aren't their defaults, `ui::ApplyPoseSettings`'s: the exposure, the tonemapper, the shadows,
  `SPEEDO_GI` and the indirect lighting's, the upscaling, the sky's time, `SPEEDO_DEBUG_VIEW`; then `# window <w>x<h>`,
  the framebuffer), for reports: an artifact is a pose first. Edit > Paste Scene (ctrl/cmd+shift+v;
  `SPEEDO_PASTE_SCENE_FRAME=<n>` once n frames are drawn, for tests) applies such a text from the clipboard
  (`ui::ParsePose`; a shell's `export`/`env` words are skipped) to the running client: the settings by the same function
  the startup takes them from the environment with, those it leaves out back to their defaults, the model and the
  environment loaded where they aren't the ones installed (by the scene's file path, scene and variant, and
  `Scene::GetEnvironmentPath`), the window's framebuffer resized to the comment's size unless fullscreen, and the camera
  placed as `SPEEDO_CAMERA`'s once the pasted model is installed: Bistro pasted over DamagedHelmet came out as a run
  started with the same variables (a mean of 1.7 of 255, the lighting's frames). They go in 5 `R16G16B16A16Sfloat` 3D images
  (`gGiVolumes`: per color its faces +x, -x, +y and -y, then red's and green's +z and -z, then blue's and the weight, so the
  shading reads vectors, see `GiSample`; the clips stacked along z, a lookup clamped inside its clip's slab), written
  as storage images (`gGiVolumeImages`) and sampled with the clamping sampler, always in the general layout (the
  voxels' lighting samples them in the pass that writes them; Main declares a storage read). rhi's 3D images: a depth
  above 1 in the image's extent (`ImageCreateDesc::extent`, an `rhi::Extent3d`, which an `Extent2d` converts to a slice
  deep; `Slice()` back), whose views are 3D without asking (a view's desc is made from its image, `ImageViewSource`).
  The desc holds level 0's extent alone and a `levelCount`: Vulkan sizes every level from level 0's, halving each
  dimension down to 1 (`ImageCreateDesc::LevelExtent`), so a desc can't size them otherwise, and the readers (bloom,
  the depth pyramids, the environment filter) take them from there. What an upload needs of each level is where its
  bytes start in the source: packed by default (level 0 at 0, each level right after the one above, its bytes
  `rhi::PackedByteCount` of the format's `BlockOf` and the level's extent), as the importer stages them (`Upload` fails
  hard where it doesn't), or `levelOffsets` for another layout; a level past the source's bytes fails hard. The levels'
  places in the image's own memory are the driver's. Until 2026-11-10 the desc listed every level's extent (and an
  unused byte size), the environment's storage textures listing levels at least 2 or 8 wide where the image's were
  narrower; the depth was a field of its own (`ImageCreateDesc::depth`) and views had a flag to set
  (`ImageViewCreateDesc::volume`), and only creating the image knew of it: its placement in memory, uploads, blits and
  mips were 2D (a placed or uploaded volume came out 2D, a blit one slice deep). `Shade`
  takes `GiSampleSurface` in place of the environment's irradiance where a clip covers the point (the environment's
  elsewhere): the finest clip holding it more than a probe spacing from its edges, blended into the next over 8
  voxels; there the 8 voxels around the point offset 0.75 voxels along its normal and 1.5 toward the eye (the line of
  sight is clear; the normal alone led into the other surface's voxels at a concave edge), in each clip's own voxels
  (in the finest clip's for every clip, a coarse clip's lookup at a thin wall landed within the filter's reach of the
  wall's outer shell voxel, resolved from a point outside, where a probe outside the building passes: the end wall of
  Sponza's corridor lit white by the sky beyond it, the leak that followed the camera as the coarse clip took over;
  with one clip it was dark, which is what found it), twice as far where that finds no probes (but where the near
  field's light stands in, see the near field), filtered, their
  premultiplied faces over their weights (`GiEvaluate`). The specular takes the same
  lookup: the environment by the indirect light's share of the environment's irradiance (luminance, at most 1; all of
  it in the open), and, by roughness (a smoothstep from 0.2 to 0.9), the faces along the reflection. Kept as arrays
  of the values (28 as harmonics) in the main pass's lookup, they spilled: Bistro's main pass took 31 ms. The lookups cost Sponza's
  main pass 1.5 ms (3.5 to 5.0) and Bistro's street 2.4 (3.8 to 6.2); the volumes' reads were 0.6 of Sponza's as harmonics (bands 0
  and 1 alone, 4 volumes: 4.45; the cube's 5 volumes hold 19 values where the harmonics' 7 held 28), the rest the lookup
  itself (the clips' weights, two attempts, the specular's second evaluation).
  Its debug views (View > Debug View > Rendering Features > Indirect Lighting, see "Debug views"; `SPEEDO_GI_DEBUG=<n>` is the n-th,
  `GiData::debug`): 1 the indirect diffuse alone, 2 the resolved weight (how much a point sees of the probes around it),
  3 and 4 the radiance of the voxel behind or in front of a point, 5 its albedo; 6 a reference: each probe's light
  from 512 rays through the voxels straight to the environment, nothing blended (seconds a frame); 13 the surface
  reference (`GiPixelReference`: `SPEEDO_GI_REFERENCE_RAYS` cosine weighted rays from the point itself, 256, fewer
  where a frame of it runs past Metal's queue timeout, Bistro's long walks to the sky at 64; its spiral turned per point by a hash,
  else its fixed directions made radial streaks; the rays shared out among the clips that hold the point by the
  weights the shading blends them with, each ray one clip's, so a pixel costs the same however many hold it: from the
  finest alone, the edge of its box cut the vault in two shades, and every ray in every clip ran past Metal's queue
  timeout; an origin in an occupied voxel, an adjacent surface's shell at a concave edge, moves out of it along the
  voxel's normal turned toward the eye, a voxel and a half at a time, `GiReferenceOrigin`: along the normal as
  stored, an inner shell voxel's led further into its solid, and the vault's ribs drew black lines; seconds a frame:
  capture it after a few frames); 7 which clip lights a point, 8 the indirect diffuse
  with the finest clip's voxel and probe planes drawn over it (`GiClipDebug`); and the Voxel World and Probes views (the
  8 probes around a point as they hold them, trilinear, without the resolve's weights). The kernels dispatch rows of
  `SHADER_TYPES_GI_ROW` threads (`GiThread`), so a volume's size isn't capped by a dispatch.

**Tracing the triangles** (the default since 2026-10-21, the user's choice; View > Rendering Features > Indirect Lighting... > Rays switches between
the triangles and the voxels at run time, `Scene::Settings::traceTriangles`, `FrameInputs::traceTriangles`;
`SPEEDO_GI_TRACE=voxels` starts with the voxels, and Edit > Copy Scene writes it): a model loaded gets a BVH of what the voxels hold of it (`gfx::bvh::Build`, `gfx/bvh.h`: its static triangle lists that aren't
blended or transmissive, at each instance, in world space), tinybvh's compressed wide BVH (CWBVH; the `ports/tinybvh`
overlay, not in vcpkg's registry, pinned to a commit of its main branch), built with spatial splits (tinybvh's `BuildHQ`, an
SBVH: a triangle spanning several nodes' space is referenced from each, so they overlap less and rays visit fewer;
`SPEEDO_BVH_BUILD=fast` binned SAH alone, `Build`, as it was, a cache entry of its own: Bistro's frame 66.0 to 57.6 ms,
San Miguel's 88.7 to 87.1, Sponza's 31.8 to 30.4, at 214.6 MB of BVH rather than 193 and 0.58 s of build rather than
0.4) on the executor (`TINYBVH_NO_BUILTIN_POOL`,
its spawn, barrier and parallel for hooks over the `TaskExecutor`) when the model is imported and kept in its asset cache
like the rest of it (after the staging buffers; `bvh::Version`, tinybvh's version and a `bvh-vN` tag, is part of the
cache key: bump it when `Build` changes), a merged set's built at upload from the staged triangles (its files are placed
when merged), and uploaded with the model whichever rays are chosen, so switching is immediate (`gBvhNodes`, `gBvhTriangles`; a default
node's worth of zeros while a model has none). The rays traverse
it in `bvh.slang` (tinybvh's OpenCL kernel, `kernels/traverse_cwbvh.cl`, ported to slang, with its dev branch's stack of a
uint per entry, the parent node and the hit bits left, dropping groups past 24 rather than writing past the stack): the
probes' rays (one closest hit answers the radiance and the depth, `GiTraceBvh`), the voxels' shadow rays (any hit,
`GiVisibility`) and both references. Where a ray hits, the radiance is the voxels' (`GiRadianceAtHit`: the shell voxel half
a voxel off the surface on the ray's side, in the finest clip whose box around the eye holds the point), so the lighting,
the bounce and the probes are as before; what changes is what a ray meets: the triangles, with no surface a voxel thick
and no opening narrowed by its shells. With the triangles the resolve doesn't test the probes by their depth maps either: whether each of a voxel's 8 probes
sees it is a ray between them through the static BVH (to the voxel's center, a surface's voxel's half a voxel out along
its normal, stopping a twentieth of a voxel short of it: a quarter left a surface crossed within it untested, and a
probe over CornellBox's ceiling saw a corner voxel's point 4 mm under it, a streak along the ceiling's edge; a back face
met means the probe is behind a wall, and it is skipped, a front face hides it), kept per voxel
(`gGiVisibility`: 2 bits a probe and a valid bit, in two halves as the probes, by the scroll) and traced again only for
the blocks where a probe moved or is new (`gGiSharedProbeMoved`). The resolve is then a kernel of its own (`ComputeGiResolveTraced`,
`GiResolve<false>`), which neither reads the depth maps nor holds them in shared memory: the generic one, a group
loading its 8 probes' 256 texels each, 8 KB of shared memory, whatever the rays traced, cost Bistro's frame 2.4 ms, San
Miguel's 1.5 and Sponza's 2.5 at the benchmark poses (43.65 to 41.26, 47.10 to 45.57 and 26.02 to 23.52 ms, balanced),
the renders the same. Every probe is traced on a retrace, the ones the
resolve then skips as inside too: skipped before the trace, their bits stayed visible, and a probe whose inside share
hovered at the line lit what it couldn't see once it fell below, in some runs and not others. A probe weighs its
facing's 4th power (nothing behind the voxel's open side), a thousandth of that if hidden; and the hidden ones bring
the least light of theirs (per face and color), not their mean: a voxel no probe sees can confirm none of it, and a
pocket no probe sees into is a shadowed one. salle_de_bain's niches, 67 units out in the last clip (3.5 m probes, 0.44 m
voxels), took all of that: by their depth texels, half a metre wide there, the probes behind the back wall (in the
closed space the window's emitter lights, 40 times the room's light) passed for the niches' points, and with the
traced test no probe sees into the niches at all (the room's hug the wall plane), so their voxels took the hidden
probes' light, and those behind the wall are straight along the niches' open side; neither the face a hiding ray meets
nor how far from the voxel it meets it tells a wall between from a niche's lip. The niches went from white blotches
that flickered (the largest pixel step a frame 24 of 255) to as dim as the walls around (the statue's base 0.21 to
0.055 against the walls' 0.068, then 0.077 against 0.058 once the last clip's margin was a whole spacing, as in the
surface reference, whose niches are as bright as its walls; steps of 2 at most). A test in the shading's lookup of which voxels are of the point's side (a
voxel's open side, kept per voxel, read for its 8 voxels, the filter by hand where any is not) cost Sponza's main pass
6.3 ms and wasn't needed once the voxels were right. Traced every frame it cost Bistro's street 12.6 ms; refreshed a
voxel in 8 a frame as much (a few threads of every simd group trace), a block in 32 a frame 2.6 ms (the dispatch waits
for its slowest group, whose threads trace 8 rays each in turn); settled, none are traced (4,700 blocks a frame while
Sponza's probes settle, none from frame 80). All of it, with the back sides, the margin below and the shorter rays,
against before (interleaved builds, the `Frame` scope): salle_de_bain 11.4 to 11.55 ms, Sponza 22.4 to 23.6, Bistro's
street 25.15 to 26.5, CornellBox-Sphere 9.1 to 9.9; of that the whole spacing of margin is Sponza's 0.5 and
CornellBox's 0.27 (its probes trace now, where they lay dead in the outer surfaces). Sponza against its
reference 0.386 to 0.390. The voxels the back sides of single sided triangles wrote (outside the room, inside a solid)
are given no light: resolved for that side, the row over salle_de_bain's ceiling (a single sided triangle 0.08 m under
the row's centers) was lit from the sky through the space over it, and a wall's top, whose lookup's filter reaches
that row, drew a bright line along every wall (0.56 against the wall's 0.13); at a wall's top the wall's room side and
the space over the ceiling share a voxel, whose open side the occupied voxels below it turned up, so they go too
(CornellBox the same in both ray modes; Sponza 0.385). Drawn sides' normals winning the voxel's took the room's level
down a tenth (the light from outside it). A hit carries its normal toward the ray, its side and its flags (`BvhHit`). A
backface is a single sided triangle seen from behind (the compressed triangles' transform divides the normal by its
largest component, whose sign `Build` keeps in their last float4's x, with `BVH_TRIANGLE_FLAG_*` and the material's index
in y and the first index in z). That sign is read off tinybvh's own transform (its third row, the normal over that
component, times the normal), not by picking the largest component again: where two tie (geodesic.zip's symmetric faces)
rounding picked one axis in tinybvh and the other in `Build`, by the build's flags, and the triangle faced the wrong way
(the sanitizer presets failed 2 of geodesic.zip, 110 of a mesh's 1656 hits on the wrong side; 2026-11-09, `bvh-v4`).
Alpha masked triangles (`BVH_TRIANGLE_FLAG_ALPHA_MASKED`) are cut out as the main pass's
alpha test does (`GiAlphaMask`, an `IBvhFilter` the traversal asks of each hit: the base color's alpha or the mask's at the
hit's texcoords, `BVH_ALPHA_LEVEL`), so light passes through foliage's gaps, which the voxels hold whole (0.2 ms on Sponza,
1 ms on Bistro's trees). What moves has a BVH of its own (`bvh::Moving`: what the voxels hold of it, placed each frame on
the cpu in `Model::Animate` as `WorldPosition` places it, morphed, skinned and by the instance, and refit; per frame,
host visible, `gBvhMovingNodes`, `gBvhMovingTriangles`; placed only while the rays trace triangles, `Animate`'s
`placeBvh`), traced after the static one (`GiTraceTriangles`), except by the
static voxels' kept light (`ComputeGiLightStatic`), which nothing moving may shadow for good (`GiVisibility`'s `moving`).
Its leaves are split to at most 16 triangles before the conversion (`BVH_GPU`'s leaf references count 7 bits, and
tinybvh clamps a bigger leaf, whose rest is never tested). One BVH for all of it, not one per submesh: BrainStem's dozens of overlapping parts each took a traversal (its frame 14.6
ms, 11.6 with one). It is tinybvh's binary layout in Aila and Laine's form (`BVH_GPU`, `BvhTraverse2`, a port of
`kernels/traverse_bvh2.cl`): refitting the binary tree and copying it costs BrainStem's 61000 triangles 1 ms a frame,
where encoding a CWBVH again took 3 (the refit is of the wide tree's: converting the binary tree again collapses it by the
new bounds, into another node count). For the static BVH the CWBVH stays: the binary layout took Bistro's frame from 25.2
to 28.3 ms (Sponza the same). Animated models against the voxels' walk (the `Frame` scope): CesiumMan 7.3 to 7.1 ms, Fox
the same, BrainStem (all of it moving, so every voxel's light traces the moving BVH each frame) 9.5 to 12.1.
tinybvh's dev branch stores CWBVH triangles uncompressed (3 float4: edges and a vertex, whose cross product faces the right
way), so moving the port to it means `BvhTraverse`'s triangle test follows (`bvh.cpp` doesn't compile without
`CWBVH_COMPRESSED_TRIS`); they would also fix the compressed triangles' precision on tiny ones (under a millimetre in a room:
living_room, salle_de_bain, San Miguel), whose barycentrics come out wrong by tens of percent and which a ray may miss for
one behind, where the binary layout's test is right (`CheckBvh` counts those apart and warns). Where on a triangle a hit
is, `CheckBvh` compares by the points its barycentrics give, within what the distance's test allows (1e-4 of the scene's
size and the distance): by the barycentrics, within a twentieth, the glTF research scenes' triangles of a few
millimetres (the bathroom's and the staircase's, in rooms of 14 and 7 m, a little over the tiny ones' bound) failed it,
off by a few tenths of a millimetre, the compressed triangles' precision (2026-11-21). A hit's normal, side and flags are read once the traversal is done (`BvhFinish`): read at
every accepted hit, the loop carried them all and Bistro's frame took 2 ms more. Bistro's cost tried and kept: the dev branch's stack of a uint an entry (no change); ordering the
probes' threads by ray (no help: each probe turns its ray set by its own phase, so neighbors' rays don't share
directions). Measured (Sponza's inside, Bistro's street, CornellBox-Sphere, as below, interleaved): the
indirect lighting's share of the frame (the `Frame` scope less a run without it) 9.9 ms walking the voxels, 10.5 tracing;
7.6 and 11.2 (Bistro's 2.8 M triangles, 193 MB of BVH, built in 0.4 s at import, without spatial splits; Sponza's in 40 ms); 4.9 and 3.6.
`assettest` checks each model's BVHs in both layouts (`Build`, `BuildBinary`) as `bvh.slang` traverses them (compiled to
c++, `src/tools/bvhkernels.slang`) against testing every triangle: nearest hits, distances, sides, barycentrics and any
hits (`CheckBvh`; a triangle tied with a coplanar one, e.g. back to back, isn't a disagreement). Each method's
probes against its own surface reference (Sponza's inside at exposure 3, mean |log2| at 48 pixels): voxels 0.45, the BVH
0.38; the two references differ by 0.78, the BVH's 48% brighter (the voxels' shells take that much of the light through
the arcades and onto the curtains). The quick tier passes with it, profile and debug (validation).

**Hardware ray queries** (where the device has `VK_KHR_ray_query` and its acceleration structures, `DeviceLimits::rayQuery`,
decided in rhi: `WantsRayQueries` enables the extensions and chains their features; `SPEEDO_RAY_QUERY=0` keeps the BVHs, as
an upstream KosmicKrisp does, having neither; on macOS they are opt in, `SPEEDO_RAY_QUERY=1`, see below): the main layout is compiled with `BVH_RAY_QUERY` (the scene's
preprocessor definitions, which are part of the shader cache's key: they weren't at first, and a profile that had compiled
one way handed those shaders to the other, ray queries with no structure bound, which traced no shadow at all), and every ray the BVHs answered (`GiTraceTriangles` and `ShadowOccluder`:
the indirect lighting's, the traced shadows', the emitters' and the near field's) is a ray query (`BvhTraceScene` in
`bvh.slang`) of `gBvhScene[frame]`: a top level structure per frame index of three instances (`gfx::AccelerationStructures`,
`gfx/accelerationstructures.h`, the model's), what doesn't move, built once when the model is installed of the model's own
indices and vertices (no copy of its triangles: a geometry per triangle list and instance of those `bvh::Triangles` takes,
`bvh::RayQueryGeometries`, each placed by its instance's transform, `ModelBuffers::bvhTransforms`), and what moves, refit
each frame from where `Model::Animate` placed it (`bvh::Moving` without its tree, `WriteRayQueryCorners`, a float4 a corner
into the frame index's host visible buffer; `AccelerationStructures::RecordUpdate` at
the start of `Scene::Record`, then the frame index's top level structure built again), and the transmissive surfaces,
those that don't move built once and those that move refit each frame (a second `bvh::Moving` of them, `glass`, its
corners after what moves' in the frame index's buffer; placed wherever the traced reflections, refractions or caustics
are on too, `Scene`'s `myMovingPlaced`), which only the paths through the glass query (see "Shadows"; IridescentDishWithOlives'
cover, 3584 triangles, 0.2 MB a frame). The instances' masks tell them apart
(`BVH_RAY_QUERY_MASK_*`: the static voxels' kept light queries what doesn't move alone). The alpha masked geometries
aren't opaque: their hits are the query's candidates, which `GiAlphaMask` accepts or not (`CommitNonOpaqueTriangleHit`). A
hit's flags and first index come from its geometry's entry in `gBvhTriangles` (a uint4 a geometry: the flags, the first
index, the instance or, of what moves, the first corner, and the instance its vertices' normals are placed by,
`BvhHit::normalInstance`, what moves rigidly's too, none where skinned or morphed; what doesn't move's, then what moves',
then the glass's, then the glass's that moves, each instance's first geometry its custom index: what moves' entries were
missing, so a ray meeting what moves read past the table), and its normal and side from its corners: the model's vertices
placed by `gModelInstances`, or the frame's corners of what moves.
No CWBVH is uploaded nor a set's built, nor the moving tree built or refit on the cpu (BrainStem's 1 ms a frame), and no
copy of the triangles is kept: they were 3 float4 a triangle at first (Bistro's 2.8 M, 134 MB), a uint4 and a transform a
geometry now, renders and the traced rays identical (lavapipe). A model
without any (the file browser's preview) has the scene's default bound, top level structures of no instances. rhi:
`rhi::AccelerationStructure` (a structure in a buffer of its own; `GetSizes`), `CommandEncoder::BuildAccelerationStructure`
(builds and updates), `Buffer::GetDeviceAddress`, `BufferUsage::kDeviceAddress`, `kAccelerationStructureStorage` and
`kAccelerationStructureInput`, the builds' stage and accesses, and `DescriptorType::kAccelerationStructure` bindings
(reflected from slang's `RaytracingAccelerationStructure`, bound by their handle; a pool without ray queries leaves the
type out); VMA allocates with device addresses only where there are ray queries.
Checked on lavapipe (mesa's cpu driver, which has ray queries: `VK_DRIVER_FILES=<mesa>/share/vulkan/icd.d/lvp_icd.aarch64.json`,
from Homebrew's `mesa`, which setup no longer installs: the `mesa` port builds KosmicKrisp alone;
a frame in seconds at `SPEEDO_WINDOW_SIZE=320x180`, with `SPEEDO_GPU_HANG_TIMEOUT=0` and a user profile of its own: its
pipeline cache isn't the mac's), against the BVHs on the same driver (`SPEEDO_RAY_QUERY=0`), frame by frame
(`SPEEDO_GI_TIME_BASED=0`, a fixed exposure, upscaling off, `SPEEDO_ANIMATION_TIME`: an animated model's pose by the
clock differed between runs): ShadowScene's traced rays (`traced-shadow-raw`) differ in 45 pixels, all where the BVH let
light through the edge between two triangles (the device's test is watertight); CesiumMan's (skinned, refit each frame)
are identical, and its indirect diffuse within 0.004 of 255; DiffuseTransmissionPlant's (alpha masked leaves, every pixel
traced) differ on its thin stems, which the CWBVH's compressed triangles miss (their slivers: see tinybvh's dev branch, above)
and light through themselves, where the shadow atlas has them in shadow as the ray queries do; its indirect diffuse is
darker by 2.7 of 255 on average, for the same reason. Clean under the validation layer. Compute work over many materials'
textures (the voxelizer, the rays' alpha masks and emission, the emitters) indexes them as non-uniform (`SampleViewLevel`):
Vulkan requires it of an index that varies across a wave's threads, and without it a driver may sample one thread's
texture for all. On the mac, the mesa port's KosmicKrisp has them behind `MESA_KK_EXPERIMENTAL=ray_query` (the
djohansson/mesa fork, see "vcpkg": Metal 4's acceleration structures and intersection queries), which the client sets
as it makes its instance (the driver reads it then) where ray queries are asked for and it isn't set: on by default from
2026-11-04 (the user's call, once each kernel traced from one call site, see "Traced reflections and refractions", its
pipelines then made in seconds), opt in since 2026-11-08 (`SPEEDO_RAY_QUERY=1`, `RayQueriesEnabled` in rhi, the user's
call): a single client still restarted the gpu now and then (Apple's, below: ABeautifulGame 2 runs in 6 on 2026-11-04;
the release builds most runs, until the mesa port's workaround of 2026-11-08), and with the workaround the packaged app
lost its device 55 s into the user's session (its window in front, the browser making previews of the test assets just
downloaded, the kernel's GPURestart type 1 at that moment), where the BVHs never restarted it and perform about the same
(below). Elsewhere ray queries are on where the device has them, unless `SPEEDO_RAY_QUERY=0`. Against the BVHs on it,
frame by frame as above: ShadowScene's traced rays identical, its lit frame within 0.012 of 255; CesiumMan 0.004,
DiffuseTransmissionPlant 0.002, Sponza 0.002 and Bistro 0.010 at their benchmark poses; San Miguel 0.066, its leaves'
dappled sunlight, where the CWBVH's compressed triangles lose precision; clean under the validation layer and Metal's
(`MTL_DEBUG_LAYER=1`). Settled frame times at the benchmark poses (2560x1440, GI on, interleaved runs), ray queries
against the BVHs: Bistro 40.6 against 43.6 ms, Sponza 26.3 against 26.0, San Miguel 48.7 against 47.1; warming up 47.9
against 54.8, 28.0 against 28.8, 53.1 against 55.4. Metal compacts: Bistro's static structure 258 to 145 MB, San Miguel's
906 to 572. On NVIDIA (RTX 4070 Ti SUPER, driver 610.88, Windows; 2026-11-01, the benchmark poses at 2560x1440, GI on,
balanced, immediate presentation, two interleaved runs of each): settled frame times Bistro 5.9 and 6.1 ms against 10.2
and 10.4, San Miguel 7.1 against 10.1 and 10.7, Sponza 4.4 and 4.5 against 5.6; by the passes' gpu timings, the GI pass
(the probes' rays and the resolve's visibility) 5.80 to 1.83 ms at Bistro, 4.48 to 1.40 at San Miguel and 2.62 to 1.34 at
Sponza, the near field's rays, the emitters' shadow rays and the traced shadows a few tenths each, and GI Light Static (the
voxels' shadow rays, when the lights or the voxels change) 18.5 to 4.8 at Bistro. BrainStem (all of it moving) 4.9 ms
against 14.5: the BVHs' draw thread waits on the moving tree's refit and copy on the cpu (the gpu's frame 3.4 against
4.6). The captures differ from the BVHs' as much as each path's runs from each other (Bistro 0.39 of 255 against 0.37 and
0.41; Sponza 0.13 to 0.20 against 0.10 to 0.17), San Miguel's a little more (0.57 and 0.61 against 0.47 and 0.50: its
leaves' dappled light, the CWBVH's precision). NVIDIA compacts Bistro's structure 189.5 to 82.9 MB (its CWBVH is 214.6
MB), San Miguel's 666.8 to 212.9. Clean under the validation layer (Sponza, BrainStem, DiffuseTransmissionPlant, logged
to a file, see the presentation test's paragraph), and the quick tier passes, profile. Never run several ray query clients at once on KosmicKrisp (`assettest.ps1` runs them one at a time where the
driver is KosmicKrisp and ray queries are asked for, `SPEEDO_RAY_QUERY=1`; the BVHs, the default, go four at a time): four, or a small model beside a big one, made the gpu restart ("CDM Kill
timeout" in `/Library/Logs/DiagnosticReports/gpuEvent-client-*.ips`, the clients losing the device with
`MTL4CommandQueueErrorTimeout`), clients stuck exiting, and an hour later the machine panicked (IOKit's termination queue
backed up behind them); one at a time never did, nor the BVHs in parallel, and not with the structures built but no ray
traced. Not understood yet: a standalone Metal program tracing with a texture sampled in its candidate loop had
"Impacting Interactivity" kills under four processes, but no gpu restart, and speedo's simple models (no alpha masks,
so no candidates) restarted it too. Nor is it two clients tracing: one ray query client beside three of the BVHs
restarted it once in three runs ("progress timeout"), the ray query client's GI pass (BoxTextured's, milliseconds alone)
in flight for 10 s at frame 20 by its breadcrumbs (KosmicKrisp has buffer markers), the BVH client's Prepass with it.
The driver's synchronization shows no race that contention would expose (a producer barrier of every stage, the
acceleration structures' included, where each encoder ends; every pipeline barrier over dispatches, blits and the
builds; every structure in a heap of the device's residency set), and its Metal code keeps `intersection_query`'s rules.
`SPEEDO_RAY_QUERY_WATCH=1` (`gfx/rayquerywatch.h`, the shaders' `BVH_RAY_QUERY_WATCH`) watches the queries of compute
shaders: counted as they begin and end in a host visible buffer, each recorded in a slot while in flight (its ray, its
dispatch's clip and step, how far it got), which a crash report reads (`rhi::SetCrashReportSection`), and each compute
dispatch a breadcrumb of its own (by its entry point's index). Written with atomics, it survives the restart: in the next
one ("CDM Kill timeout", a restart of the channel), GI Resolve (entry point 16) of clip 0 was in flight at frame 27, and
1296 of its queries never ended, every one inside the query (670 after `TraceRayInline`, waiting in `Proceed`, 126
before it had returned; no candidates, BoxTextured is opaque), from 26 probes over one face of the box, short visibility
rays, while 181650 queries begun after the last of them ended: a set of threads (a core's?) stopped in the ray queries,
not the gpu. Every restart so far caught the GI pass early in a run (frames 20 to 28, while GI Resolve's visibility rays
are traced: only where probes moved or are new). The rays aren't it: `SPEEDO_RAY_QUERY_REPLAY=<file>` (a ray a line, as
the report lists them; the Ray Query Replay pass, `ComputeRayQueryReplay`, traces each `SPEEDO_RAY_QUERY_REPLAY_REPEAT`
times a frame) traced those 796 alone, 1024 times a frame for 300 frames (245 M queries), and every one ended, for 0.5 ms
a frame. A standalone Metal 4 program (no Vulkan: an MTL4 queue, argument tables, a residency set, three cubes'
structures, the instance structure rebuilt every frame, a dispatch of those rays through `intersection_query` a frame)
restarted the gpu beside three BVH clients ("CDM Kill timeout", blamed on a BVH client, its GI in flight), but not beside
three Metal programs of plain compute (short dispatches, 58 ms ones, or 65 ms ones whose every thread runs that long),
nor four of itself: so far every restart had a ray tracing process and speedo on KosmicKrisp, the stuck work either's
(`src/tools/macos`: `rtrepro.mm`, its rays and `contend.ps1`, which runs processes at once and stops them at the first
restart). With the clients' indirect lighting, traced shadows, emitters, upscaling and ui off, it restarted all the same,
and took the machine down: nine more restarts 5 s apart, then WindowServer missed its watchdog and the machine froze (a
forced restart, as once before). It isn't memory (three programs wiring 2.5 GB each and making it resident every
frame), nor presentation (three windows presenting every refresh), nor timestamps (counter heaps around 16 dispatches a
frame), and it isn't ray tracing: it is pipelines made and dispatched at once through an `MTL4CommandQueue`
(`rtrepro compile`: a kernel of unique source after another, each dispatched once). Three of them alone restart the gpu
within 2 or 3 s ("CDM Kill timeout"), every run, once the shader cache holds their kernels (about 1700 pipelines a
second each; compiling for real, 50 a second, they survived), one alone sometimes, whichever compiler made them
(`MTL4Compiler` or `MTLDevice`'s); through an `MTLCommandQueue` they never did (4000 to 6000 a second), nor made but
not dispatched. speedo's restarts came in its clients' first frames, where KosmicKrisp (Metal 4 throughout) makes their
pipelines, from a warm pipeline cache, and the ray queries' processes with them. Reported to Apple as FB25381275 (2026-10-31), with `rtrepro`.
A second bug of Apple's (found 2026-11-08) is what restarted single release clients (about half of the cold helmet
runs on 2026-11-07, still 6 of 20 once the pipelines were made up front, and every run on a warm profile; the profile
builds' timestamps, each resolved after its pass, mostly hid it): a compute kernel compiled with Metal ray tracing, beside other gpu work (another queue of the
process, another process, WindowServer), hangs the gpu until its firmware restarts it ("CDM Kill timeout"), whether its
queries run or not. `rtrepro frame` (a pre-pass, a kernel tracing a ray a pixel from it, a pass into the drawable, every
refresh; its header lists the flags) reproduces it in pure Metal: `rtrepro frame 60 rtpudxb` (with an ALU load on a
second queue of the process) restarted the gpu in 3 runs of 3, within 6 to 30 s, and never without ray tracing in the
kernel (0 of 2 runs of 60 s); the classic `MTLCommandQueue` hangs too. In speedo it took a ray tracing kernel after the
frame's first render pass (Shadow Trace, GI Near or Main Visibility alone, the last with no query run; the GI kernels,
before the prepass, never did, until moved after it), the hang showing in the next frame's first compute kernel with every
query ended. Not KosmicKrisp's barriers, fences or command buffer splits, the structures' builds or compaction, nor
pipelines made during the frame. Reported to Apple on 2026-11-08 (the draft: claude.ai/artifact/Se7CGaP9B3RtfkuSBwXPyt).
Until it is fixed, the mesa port patches KosmicKrisp (`KK_WORKAROUND_63`, see "vcpkg"): after every encoder, a counter
heap resolve of one entry never written, on devices with ray queries. That makes the restart rare, not impossible: the
protection grew with the number of resolves (after the ray tracing encoders only, Sponza lost the device in 1 of 2 runs
of 2000 frames; after every render encoder, the helmet 0 of 12), so it narrows a race whose mechanism is Apple's. A blit
encoder, an empty debug group or the command buffer's timestamp in its place didn't help. With it the release client lost
no run (Sponza 0 of 4 and, before, 0 of 3, at 2000 frames; the helmet 0 of 6 and 0 of 4 at 600), where without it
(`MESA_KK_DISABLE_WORKAROUNDS=63`) every run lost the device within 1 to 10 s. It costs 0.21 ms a frame at Sponza's
default camera (2560x1440, GI on, unthrottled: 9.93 against 9.72 ms, the BVHs with the resolves forced on in a build of
the driver, 4 interleaved runs each, every run within 0.02 ms of its variant's others); ray queries with it took 10.31 to
10.36 ms there, the BVHs 9.68 to 9.73. Check each macOS update with `rtrepro frame 60 rtpudxb` (a gpu restart within a
minute: still there), and drop the patch once it no longer restarts.
Don't run such experiments without asking. Metal's shader validation (`MTL_SHADER_VALIDATION=1`) crashes in
MetalTools at the first commit with KosmicKrisp, with or without ray queries; Metal 4 has no encoder execution status,
and a restart's kernel log says only its type. What doesn't move is built
to be compacted: once its build is done (`CanCompact`, its compacted size queried and read without waiting at each
`Scene::Update`), it is copied into a structure of that size (`RecordCompaction`; `rhi::QueryType`,
`CommandEncoder::WriteAccelerationStructureCompactedSize` and `CopyAccelerationStructure`), the instances naming it
written in order with the frames and the top level structures built again (lavapipe doesn't compact: 6.8 MB either way
for DiffuseTransmissionPlant). Not yet: measured on a desktop gpu (NVIDIA: Bistro, San Miguel and BrainStem against
`SPEEDO_RAY_QUERY=0`, and the structures' memory against the BVHs'), a structure per model instanced into a scene.

**The surface cache** (View > Rendering Features > Indirect Lighting... > Surface Cache, on by default since 2026-10-23, the user's call; `SPEEDO_GI_CACHE=0`
to start without it, `FrameInputs::giSurfaceCache`; `gGiCache`, `GiData::cache*`, the cache section of `gi.slang`): the
probes, 8 voxels apart (1.8 m in Sponza's last clip), spread the light through a gap of a few tens of centimetres (the sky
under Sponza's curtains, at the user's pose `7.7949,0.8419,-3.7077,0.531,-91.763`) into spots a probe cell wide, which
changed as the finer clips took the floor over; per voxel near a surface, the cache measures it at the voxel's size, a
band along the curtains' lower edges down the whole corridor (`docs/reference/sponza-corridor-surface-cache.png`: the
probes alone against the cache, lit, and the floor, as the user approved it). An entry is 3 uint4: the 6 faces' rgb as
half floats, the samples and the rays traced 16 bits each, the mean of the samples' squares (`GiCacheSample`, the light
along the voxel's open side), and the open side (octahedral, 8 bits a coordinate) and the rays that started inside
something. Only the blocks of 4x4x4 voxels that hold entries and were read lately have them, a page of 64 each from a
pool (`gGiCache`, `GiCachePopPage`): each clip has a page table a frame (`gGiCacheBlocks`, two halves swapped each frame,
so that a clip that scrolled finds its blocks' pages where they were; two words a block: the page, with a bit its voxels
holding an entry set in the resolve, and the last frame a lookup read it, `GiCacheTableWord`). A block keeps its page
while it is near a surface, held an entry last frame and was read within `SPEEDO_GI_CACHE_KEEP_FRAMES` (1024;
`GiCacheKeeps`): the release (`ComputeGiCacheRelease`) frees the others' pages onto a free list (`gGiCacheFree`), and
the resolve takes pages for the blocks that wanted one last frame and are read now (a frame late: a block's first
frame near a surface has no page), never both in one dispatch; the pool starts at a thirty-second of the clipmap's
blocks and grows to a quarter more than the resolve counted (`GI_STATISTIC_CACHE_BLOCKS`), its pages copied over (the
GI Cache Grow pass, the new pages pushed by `ComputeGiCachePrepare`). The resolve (`GiCacheUpdate`) carries the voxel's
entry over in place (a voxel new to its clip from the coarser clip's, as 8 samples, where the coarser block keeps its
page, so that it is still its own) and traces `SPEEDO_GI_CACHE_RAYS` (1) rays from the voxel (Roberts' R2, offset per
voxel by its place in the world, three quarters of them over the hemisphere its open side faces and the rest over the
other, each sample weighed by its side's density, `kGiCacheForwardShare`, `GiCacheEstimate`: over the sphere evenly,
half the rays added little to the faces the lookups read; over the open side's hemisphere alone, Sponza's interior came
out 6% darker, the lookups reading the other side's faces at edges and corners; split, Sponza came within 5% of settled
after 620 frames rather than 960, its noise 0.0052 rather than 0.006, and settled as before to 0.025 mean |log2|; a surface's voxel's out along its open side by a voxel, but no
further than halfway to a surface facing back at it there, `GiCacheOrigin`: CornellBox's sphere a voxel off its wall had
the voxel out along the wall's open side inside it, the entries were inside, and the wall drew a black crescent along
the sphere, stepped at the voxels, at the user's pose `0.1488,-1.1952,0.7514,12.998,14.697` at resolution 192), as the
probes' rays are traced, into a running mean of at
most `SPEEDO_GI_CACHE_SAMPLES` (4096 since 2026-11-19, 1024 before; 32 where something moves), and at most half the frames since it started over
(at least 32; `SPEEDO_GI_TIME_BASED=0` without), tracing 4 rays a voxel for its first 256 frames
(`kGiCacheWarmFrameCount`, then `SPEEDO_GI_CACHE_RAYS`): its first frames' samples, read while the probes and the
bounce build up and overshoot, then fall out sooner (Sponza from a cold start within 10% of settled after 280 frames
rather than 840, 460 with one ray; 4 rays throughout, 240 but 4.8 ms a frame and noisier;
the tail, a mean of a light still settling, fades as 1 over the frames either way: Sponza's still changed 2% 2000 frames
in, see docs/performance-plan.md); a ray meeting a back face within a voxel and a half started inside something (at any
distance where the rays start on the surface itself, anchored, see below), and a voxel with more than a quarter of
those is no light (the probes' alone). The samples and those rays are kept over one
window, scaled down together past the cap (the inside rays in sixteenths): clamped apart, the samples stopped at the cap
while the inside rays went on to it, so a voxel with a tenth of its rays inside crossed a quarter after 3400 frames. Lights changing
or a new buffer start it over. `ComputeGiCacheFilter` then filters it as SVGF filters a frame (Schied et al. 2017): a pass
estimating each voxel's noise (its samples' variance pooled over the 3x3x3 on its surface, over its own samples: by its
own alone, a voxel that missed a source smaller than its rays' spacing measured no noise and refused its lucky
neighbours' light, and the curtains' light broke into dim blobs), then `SPEEDO_GI_CACHE_FILTER_PASSES` (3) passes of an
a-trous filter (taps 1, 2 and 4 voxels apart) weighing each tap by the kernel, the open sides' agreement (to the 8th), its
distance off the voxel's plane, its confidence, and how its light differs against `SPEEDO_GI_CACHE_SIGMA` (8) deviations
of both voxels' noise, the noise of a mean of at most 64 of their samples (`kGiCacheFilterCount`: as the variance of the
mean of all of a voxel's samples, it shrank as they grew to 1024, the filter smoothed less and less, and the light it had
spread from bright voxels into darker ones withdrew for as long: Sponza's interior darkened 2000 frames after a cold
start, its probes settled by 300; held, it came within 5% of settled after 960 frames rather than 1480) (by the voxel's alone, noisy bright voxels took their quiet neighbours' light and not the other way,
and the floor's band lost a quarter of its light; a sigma of 4 left blotches in it). The last pass moves the volumes'
light toward the filtered light by the voxel's samples over them and 16 (`SPEEDO_GI_CACHE_SHOW=1`: all of it), and weighs
the voxel in the lookups by that where the probes see it less (premultiplied by a probe weight of a ten thousandth, at
resolution 64, where the last clip's probes don't see into the corridor, the cache's light went to half floats'
subnormals and came out in pink, yellow and blue blotches). The passes ping-pong between the halves of the records
(`gGiCacheRecords`), which only the blocks a lookup read take (`GiCacheTakeRecord`, grown by
`GI_STATISTIC_CACHE_ACTIVE_BLOCKS`). Only what is looked up is traced and
filtered: ComputeMain stamps the blocks each pixel's lookup reads (`GiMarkSeen`, the flags' third word, in each clip that
lights the pixel), and the resolve marks a block active (`SHADER_TYPES_GI_FLAG_CACHE_ACTIVE`) where it or a block around
it was read in the last 2 frames (at its place then, by the scroll; a thread each of the 27 blocks: by the first alone,
one after another, the frame took 0.16 to 0.34 ms more); the others trace a ray every 8th frame and take their
own light unfiltered, so that the bounce doesn't change with where the camera looks (`SPEEDO_GI_CACHE_EVERYWHERE=1` does
them all: a turn in place differs from it by a mean of 1 to 2 of 765). Once its window has all its samples (twice
`SPEEDO_GI_CACHE_SAMPLES` frames after it started over, `SPEEDO_GI_CACHE_SETTLED_FRAMES` to say when) and the sun isn't
being followed, a voxel where nothing moves traces every other frame (`GiData::cacheSettled`): the same samples over
twice the frames, of a light that doesn't change (Bistro 57.5 to 56.3 ms, San Miguel 68.3 to 67.4, in runs of thousands
of frames). Its rays are most of its cost, and the blocks around those read (traced every frame and filtered, for the
filter's taps and the lookups' filter at a block's edge) most of those: at the benchmark poses (balanced, 2026-10-31)
no rays at all took Bistro's frame from 41.2 to 34.4 ms and Sponza's from 23.2 to 20.1, and leaving the blocks around
out 3.9 and 2.25 ms (patches across Sponza's vault at 0.33 of 255 from before: kept), tracing them every 4th frame
0.84 and 0.26 (faint patches still converging at frame 600: dropped), the entries left unread and unwritten where no
ray adds to them nothing. Costs (RTX 4070 Ti SUPER, 1920x1080, the `Frame`
scope): Sponza's corridor 3.6 ms without it, 5.7 with it (8.2 everywhere), Bistro's street 6.5, 9.4 (12.4),
CornellBox-Sphere 2.6, 4.7 (4.9); everywhere, a ray a voxel was 1.9 ms of Sponza's and a filter pass 0.4. Its storage exists only while it is on, and holds the blocks that hold entries and were
read lately: at Sponza's corridor, near nothing at resolution 128 and 0.3 GB at 384. An entry for every voxel of every
clip, two halves and the filter's scratch, as it was, took 0.3 and 6.4 GB (Sponza then used 14.9 GB of the GPU's 16);
paged for every block near a surface (25,000 of 32,500 at 128, 290,000 of 730,000 at 384, half of their voxels holding
an entry, a tenth of them read in a frame), 0.1 and 1.1 GB. Paged, the renders are the same as the dense cache's (to a
mean of 0.2 to 0.8 of 765, still, walking and turning); taking pages only for what is read, a block nobody looks at
carries the probes' light into the bounce rather than its cache's (a mean of 1.3 of 765 still), and an area seen for
the first time starts from the probes' light, which showed nothing visible in a turn in place (a mean of 1.4 to 3.7 of
765 against the dense cache's). The frame's time is the same. A still camera changes by 0.026 of 255 a pixel a frame (the probes alone, 0). Interiors come out darker than the
probes alone (light measured at the surface, where corners and overhangs shade it, rather than in the open cells around
it), and the CornellBox's and salle_de_bain's probe blotches go. The surface reference (debug view 13) is no use as a
reference (the user, 2026-10-24: too slow, and it doesn't look like one): seconds a frame, one bounce on its first frame
(the voxels' starts after 32 frames), noisier than the cache; judge by A/B captures instead, until a real one exists
(settled, accumulated over frames, or path traced).

**The near field** (View > Rendering Features > Indirect Lighting... > Near Field, on by default since 2026-10-24; `SPEEDO_GI_NEAR=0` to start without
it, `FrameInputs::giNearField`; `ginear.slang`, `GiData::near*`; where the rays trace the BVHs): where a solid rests on a
surface, the surface's voxels at the contact are also the solid's never drawn inside, which gets no light (see the
resolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the gap's own unlit voxels, so the
volume's lookups there found no light, in blocks a voxel wide (the user's CornellBox poses, the 11 models side by side:
`0.0062,-2.2486,1.2961,23.077,13.130`, black rectangles under the spheres; `0.1488,-1.1952,0.7514,12.998,14.697` at
resolution 192, red and black blocks where the sphere touches the green wall). Every 4th pixel each way of the
pre-pass's depth (View > Rendering Features > Indirect Lighting... > Near Field Rays, `FrameInputs::giNearStride`, `SPEEDO_GI_NEAR_STRIDE`, `gi::kDefaultNearStride`:
a sixteenth of the pixels, the default since 2026-10-31, the user's call; 2 traces a quarter, finer at thin features'
contacts and 1.3 to 3.2 ms more at the benchmark poses (Sponza 19.90 to 18.62, Bistro 37.06 to 33.86,
San Miguel 43.79 to 40.92, balanced; 4 to 11 ms before the filter took the traced pixels alone), docs/performance-plan.md, where moving the traced pixels within
the square a frame at a time didn't take 4 closer to 2; a change starts the frames' history over) traces a ray from its surface
through the BVHs (GI Near, `ComputeGiNear`), as far as
`SPEEDO_GI_NEAR_REACH` (8) voxels of the clips that light the point, its direction one of 16 over a 4x4 tile of traced
pixels (the traced shadows' disk points lifted onto the hemisphere, cosine distributed), and takes the light what it
meets reflects (the voxels' there less their emission, `GiHitReflected`, read a voxel and a half off the pixel's tangent
plane along the hit's surface: within it the voxels at the hit are the pixel's surface's too, and the floor under the
left sphere drew its own lit light as a tan crescent; moved as far along it as reaching that height takes, and a
surface facing the pixel's that doesn't get there within 3 voxels, a sphere's underside near the floor it rests on, is
in a gap thinner than the voxels, which hold neither side's light there, and gives none: moved a voxel and a half
whatever the slope, the underside rose a fraction of that, the reads along its normal crossed the floor into its
underside's voxels, lit by the gap below, and each sphere had a bright crescent around its contact, the user's pose
`0.8869,-1.0542,1.2337,17.751,7.611`; reading the floor's own voxel there, the sun's patch beside a sphere lit its
contact; and its lights' light taken at the point itself by a shadow ray, `GiHitLit` as the surface cache's rays'
hits: a voxel takes it from a voxel out along its open side, which the sun coming down through MovingInterior's
window lit higher up the walls than their sunlit band reaches, and the rays from either wall by a corner, meeting the
other at once, read those voxels: a bright line up the corner over the band, 30 of 255 above the walls beside it, the
user's report of 2026-11-20 at `-3.1333,1.2172,-0.5528,6.899,-68.277`, found by the near field's views,
`SPEEDO_GI_NEAR_SHOW` 1 and 3, and its contact switched off, `SPEEDO_GI_NEAR_CONTACT=0`; the near field 0.049 to 0.077
ms in Sponza's corridor, Bistro's shaded nooks beside sunlit surfaces up to 2.6 of 255 darker; without the voxel's
lead, `gGiAhead`, which the cache's rays take to speed their entries' convergence, and with the shadow ray from the
hit rather than the point moved off the corner, `GiHitLitAt`: the near field shows what it reads, over its frames,
and each voxel's lead held still, white speckles a few voxels apart along the ceiling's edges of MovingInterior's room
that never settled, the user's report of 2026-11-21 at `-3.9566,1.1530,-1.0380,-24.667,-150.194` and
`2.3231,1.5046,-0.8240,-0.263,-261.055`, there with the cache alone and the near field's hits' light,
`SPEEDO_GI_NEAR_SHOW=3`, gone with the probes alone, and without the near field's change before; the regression set
within 0.6 of 255 a strip but Bistro at night's lit windows' panes) or, where it meets nothing, the volume's light along it from its
end. Emitters' light is the volume's, which averages it over its probes' rays and frames: with the voxels' emission, a
ray meeting anything in an emitter's voxel took all of it, and Bistro's lanterns (a bulb of a few centimetres, 3.5, in
voxels of 30 cm with their frames and glass) lit the walls around them yellow in blotches, a hundred times what the bulb
gives (the user's pose `3002.7041,404.1237,1410.4180,10.076,5.141`); with the bulb's own, rays rarely met it and each
that did was a white speck. The emitters light the scene themselves now (see "Emissive surfaces as lights"); where they
don't, the probes' rays take a hit's own emission (`GiHitRadiance`: the voxel's reflected light and the triangle's
emission, its material's times its emissive texture at the hit), not its voxel's. (Only the alpha masked triangles
carried their material then, so every other emitter a ray met gave nothing: EmissiveStrengthTest's cubes lit nothing
around them. Every BVH triangle carries it now, `bvh-v3`.) GI Near X
and Y filter it at the traced pixels alone (at every pixel, as they were, the filter and the frames cost Bistro 5.4 ms,
San Miguel 8.5 and Sponza 9.3 at the benchmark poses; at the traced pixels the frames take 2.1, 1.8 and 3.2 ms less, the
main pass's upsampling taking some back, and differ from before by about twice the frames' noise, a contact a few pixels
wide a little lighter), over the traced pixels within 16 along rows, then columns, on the pixel's surface (by the plane and the
normal, as the traced shadows' filter), by a tent of the distance (a box of the 17 traced pixels around a traced one and
the 16 around the others drew stripes a pixel wide and a bright ray as a square). GI Near Temporal
(`ComputeGiNearTemporal`; `SPEEDO_GI_NEAR_TEMPORAL=0` without it, View > Rendering Features > Indirect Lighting... > Near Field Over Frames),
at every pixel of the pre-pass's depth, upsamples that from the traced pixels (`GiNearUpsample`: bilinear over the 2x2
around, each by how much it is on the pixel's surface, as the emitters' light, with their fallbacks) and blends it into a
history reprojected by the pixel's point through the view's last view projection (`ViewData::previousViewProjection`,
`previousEyePosition`, which `Views::Upload` keeps from its last upload, w 0 where the view moved in the target, 2 where
the view didn't move; both frames' jitters taken out, `PixelHistoryAt`, see the emitters'), bilinear over the 4 pixels
around where it was, each only if its point was as far from the last eye as this one's and its normal within 25 degrees
(by the pixels' surfaces, `SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE`: RGBA32F, distance, packed normal and the frames
the history holds; within a
fifth of a percent and two pixels' depth along the surface), else the 3x3 around that are (`PixelHistory`); what moves (the
main depth's stencil) by its own motion (the pre-pass's, as temporal upscaling reprojects it), from the pixels of what
moved alone (the surfaces' fourth channel marks them; the rest take none of theirs), within half its distance
(where its point was isn't known, only where it showed), never by the still blend (until 2026-11-18 it took none, and a moving sphere flickered: see the clipmap's
paragraphs on what moves); by 1 over `SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT` (16), a frame far brighter than what the history
holds taken as at most 8 times that, or that and a quarter of the frame's white more (`HistoryFireflyClamp`, the
emitters': a traced pixel's ray that meets what is bright, the voxels by a lit window's interior from a window frame's side
across the opening, is a sixteenth of the pixels around at a stride of 4, and faded over the 16 frames, white fireflies
along Bistro's window frames at night, the user's pose `724.7479,659.6074,-2775.4233,-21.034,93.844` at 02:14: pixels
that jumped over 80 frames 2291 to 582, 393 with the near field off; in its light alone 1568 to 244, its mean the same),
into two images each, the frame's by its parity (`GiData::nearFrame`, fixed slots: a descriptor written every frame takes a set
from the pool); the rays turn every frame (`GiNearDirection`), each pixel taking every one of its tile's 16 directions
over 16 frames. Fixed, the rays' grain stayed on the screen as the camera moved (the user: "fixed to the camera"; their
pose under Sponza's curtains, `2.8260,0.3324,-2.7619,27.122,-513.942`); over frames it is smooth, and a slow turn
smears nothing. It costs 0.9 ms a frame on KosmicKrisp by the `Frame` scope (Sponza 30.1 to 31.0, Bistro's street 57.3
to 58.3), which misses compute work there: by the draw thread's frame time, 18.9 ms at Bistro's evening benchmark pose
and 16.4 at San Miguel's, its rays 10, its filter 4 to 8 (every pixel) and its frames 1 (docs/performance-plan.md). The frames were
the traced pixels', which the main pass upsampled, until temporal upscaling's jitter made the pixels upsampled from a
traced pixel at an edge change as a block, as the emitters' did (see "Emissive surfaces as lights"): per pixel, the
near field's light alone (the indirect diffuse debug view with `SPEEDO_GI_NEAR_SHOW=1`) at the user's San Miguel pose
`7.6715,4.3834,1.7963,-22.087,-88.462` (performance, a stride of 4, resolution 64) went from 870 pixels a frame changing
by more than 32 of 255 to 447, and by more than 64 from 200 to 41, and its leaves and branches came out crisper. Main's
opaque fragments (`PushConstants::prepassImages`' `PREPASS_IMAGE_GI_NEAR`) take what the rays gathered at their pixel
(the frames' pass runs whether or not over frames, blending by 1 without), by how many met another surface nearer than a
contact (`FrameInputs::giNearContact`: View > Rendering Features > Indirect Lighting... > Near Field Contact, `SPEEDO_GI_NEAR_CONTACT`, 2.5 voxels), the
volume's lookup by the square of the share that met none (`SPEEDO_GI_NEAR_POWER`) and by how far it is trusted
(`GiLookupTrust`: its weight, how much its voxels see of their probes, over a fifth, `kGiNearTrustedWeight`; none where
it sees no probe), so the rays' light alone where the lookup saw no probe (`GiNearIrradiance`), whose second try, twice
as far off the surface, isn't taken where the rays' light is (`GiSampleSurface`'s `retry`). Each traced pixel's share is
of its rays that met no contact times its own lookup's trust, which the filter and the frames spread as they spread the
light, so the shading hands over smoothly across where the lookups fail and only around it. The floor in front of
Sponza's curtains, which hang a few centimetres over it (the user's pose `-3.0027,0.2878,2.2588,9.549,338.778`), isn't at
a contact (a tenth of its rays met the curtain), and kept the lookup, whose voxels beside the hem see no probe or barely
the courtyard's beyond it (normalized by its weight, a lookup that barely sees a probe takes all of its light), and the
second try found the courtyard's: jagged white patches along every hem, which the gold strip at the curtains' foot
reflected in orange flames. By the pixel's own trust, the lookups' failing region kept its outline, a staircase of
voxels (a lookup's weight jumps from 0 to a tenth at it); ramped up to a weight of 1 it was smooth, but most pixels took
some of the rays' light (92 to 97% weigh 0.5 to 0.99) and Sponza's columns between the curtains came out flat and light;
a power of 8 changed every scene (Bistro 0.64 of 255, CornellBox 0.50, salle_de_bain 0.43); 4 times the rays (a stride
of 2) changed nothing (the blocks were the volume's, not the rays' noise). Against before: the hem pose 2.0 of 255,
Sponza's corridor 0.52 (along its hems; the columns as they were), salle_de_bain 0.20, Bistro's street and CornellBox
0.13 and 0.10; GI Near 0.001 ms more. Blended by anything met within the rays' reach, the rays' light took every
junction over: Sponza's columns between the curtains lit from the courtyard through gaps the voxels close, and a broad
darkening along every wall. At a contact of 1.5 voxels the rectangles' edges stayed faintly; at 2.5 (the user's choice)
faint straight edges a few voxels out are what is left, and Sponza's columns are a little lighter than at 1.5. Against
the near field off, by mean difference of 765: Sponza 8.4 (6.4 at 1.5), Bistro's street 3.6 lit and 10.4 indirect (contact
shading at every moulding and frame), salle_de_bain and CornellBox 1 to 2. Costs (RTX 4070 Ti SUPER, 1920x1080): Bistro's
street 0.9 ms of rays and 0.4 of filter, Sponza's corridor 0.24 and 0.4; a ray every pixel took Bistro's 3.4 ms, and rays
2 voxels long 1.6 (the rays' count costs, not their length). `SPEEDO_GI_NEAR_SHOW`: 1 the rays' light alone, 2 the share
that met no contact, 3 and 4 the light of the rays that met something and of those that didn't.

Measured on KosmicKrisp by the frame's gpu time (2560x1440, 3 clips, 128 voxels, probes every 8, 128 rays, hysteresis
0.97; the gpu's state drifts between runs by up to a third, so compare runs interleaved): before relocation
CornellBox-Sphere 9.3 ms (4.5 without the indirect lighting; 7.5 with 32 rays, where its 3.0 ms are the voxels'
lighting 1.3, the resolve 0.9, the rays 0.6, the probes' update 0.2, the mips and flags little), Sponza's inside
(`0,2,0,0,90`) 12.0 (6.5), Bistro's street (`564,170,-2436,0,30`) 14.9 (9.9); with it CornellBox-Sphere 12.4 (4.5;
8.2 with 8 rays), Sponza's inside 13.0 (6.5); with the 16x16 depth maps and the rest of the leak fixes CornellBox-Sphere
11.8 (4.7), Sponza's inside 14.3 (6.5); with the ambient cube, the clips centered ahead and the emission clamp
(2026-10-22) CornellBox-Sphere 9.8, Sponza's inside 14.5 (7.4 without, that session), Bistro's street 16.6,
DamagedHelmet 10.4. Probes every 4 voxels cost 8 times the
probes (CornellBox 12.8 ms with 32 rays), with no visible gain at these voxel sizes. A still camera in Sponza's arcade
over 80 frames after settling: the frame's mean steps by 1.2% at most between frames and stays within 1.3% of its
average (64 rays 2.1%; hysteresis 0.9 with 128 rays 3.9% and 4%, with 64 rays 7% and 7%: the arches' light is a noisy
thing to sample with 64 rays). Renders: CornellBox's walls and ceiling smooth, with the lamp lighting the ceiling around
it, colour bleeding on the ceiling's edges and the spheres' contact shadows; Sponza's inside within a shade of its
surface reference (its arcades a little brighter), its sunlit upper floors lit by the sky; Bistro's street lit between
its walls. Against the old cascades: no splotches, no dark blobs at the walls' bases, no blocks behind the spheres, and
the clips' moves change nothing measurable. The frame's lighting skips the blocks nothing is near (their flags, from the
last frame; the static lighting clears the frame's radiance so the skipped voxels hold 0) and the restore copies only the blocks
something moving was in (`ComputeGiRestore`): CesiumMan 9.65 to 9.3 ms, CornellBox 12.4 to 12.0. What a small scene
still pays is its clips' probes and voxels near its surfaces, which fill them: DamagedHelmet 4.7 to 11.2 ms with the
ray budget (the resolve and lighting of a last clip that is all surface, the finer clips mostly empty). What's left
(TODO.md): that, the lighting's lag through slow changes, glossy reflections, hardware ray queries, and a path traced
reference.

## Emissive surfaces as lights

View > Rendering Features > Emissive Lights (on by default; `SPEEDO_EMISSIVE_LIGHTS=0` to start without, `FrameInputs::emissiveLights`,
Copy Scene writes it): the model's emissive triangles light the scene as area lights, at the first hit and in the
indirect lighting's voxels, with soft shadows traced through the BVHs (`emitters.slang`, `gfx/emitters.h`). Before, an
emitter's light reached anything only through the indirect lighting's rays meeting it by chance: spread over its voxels
it lit as if it were a voxel big (Bistro's lanterns lit the walls around them yellow, a hundred times the bulbs' light),
met as a triangle by a ray in four pixels it was a white speck (the near field took no emission at all), and the
triangle's emission was read through a material index only the alpha masked BVH triangles carried, so EmissiveStrengthTest's
cubes lit nothing around them. The rays take only the light what they meet reflects now (`GiHitRadiance`; `GiMarch`
takes a voxel's emission out), and what the emitters give arrives through these:

- **The emitters** (`emitters::Build`, at upload, from the staged triangles: Bistro's 20972, 56 ms): the triangle lists'
  triangles that don't move whose material emits (its emissive factor times its strength, or an animation sets it),
  at each instance, in world space (`EmitterTriangle`: a corner and two edges whose cross product faces the front, a
  mirroring instance's swapped and flagged, `EMITTER_FLAG_MIRRORED`, so its texture coordinates' corners follow; its
  material and first index), and a tree over them (`EmitterNode`, `gEmitterTree`: a light BVH, pbrt's BVHLightSampler,
  built by its surface area orientation heuristic: box, cone of normals, power, the area times the luminance of the
  emission factor; the texture's isn't known there, and each point is weighed by its own). `EmitterPick` walks it from
  the root, each child by its share of `EmitterImportance` (pbrt's `LightBounds::Importance`: power over the distance
  to the box, by the least angles the emitters and the surface may face each other at; never 0 where something lights
  the point), its probability the product. A global alias table by power came first: Bistro's lanterns, a small share of
  its emitters' power, were rarely a candidate of a wall beside them, and the light was mottled in blotches
  of the filter's size (`mean |log2|` against the reference, blurred 4 pixels: 0.150 to 0.129, and smooth to the eye).
- **Per traced pixel** (every `SPEEDO_EMITTER_STRIDE`-th pixel each way, 4; a texel each of the emitters' images,
  `SHADER_TYPES_EMITTER_RAW_TEXTURE` on): Emitter Sample resamples `SPEEDO_EMITTER_CANDIDATE_COUNT` (8) candidates
  (RIS: a point of each picked triangle, uniformly, by the luminance of the irradiance it would give unshadowed, nearer
  than two pixels taken as two pixels away), and Emitter Shade traces the chosen point's shadow ray (`ShadowOccluder`:
  the static and moving BVHs, alpha masks cut out) and writes the irradiance over pi. Each frame's sample is its own:
  ReSTIR's temporal reuse (`SPEEDO_EMITTER_REUSE=1`: the reservoir of the traced pixel where its point was last frame,
  by the view's last view projection, where that point was the same surface, `EmitterTolerance` and normals within 25
  degrees, not where the stencil says something moves; its count capped at `SPEEDO_EMITTER_HISTORY_CAP`, 20, times the
  candidates; kept in `gEmitterReservoirs`: three parts, the frame's before the spatial reuse and after it the frame's
  and the last frame's, by parity; triangle, barycentrics as unorm16, W, and M and the visibility's age, 16 bits each)
  keeps a point for many frames, so Emitter Temporal's frames are of one sample, not independent ones, and gain little.
  It was the default until Bistro's string lights at night (hundreds of colored bulbs across its streets and its lit
  windows, the user's pose `-906.5073,218.3646,-40.5114,-7.916,60.803` at `SPEEDO_ENVIRONMENT_INTENSITY=0.002`,
  exposure 5) came out a grainy colored wash: against the reference (mean |log2|, still) 0.235 with it and 16 frames
  (cap 4 0.186, cap 1 0.164: the more history, the worse), 0.154 without it, 0.140 with 8 candidates (4 candidates were
  as good as 8 with it), 0.132 with 16; at the end of a turn there 0.334 against 0.239; Bistro's lantern pose 0.181 to
  0.089, EmissiveStrengthTest 0.064 to 0.033, CornellBox-Original 0.029 to 0.015, the means the same. With the reuse,
  a point its ray finds hidden is kept as the candidates it stood for, of no light (RTXDI's discard of invisible
  samples: kept as it was, a back wall's reservoir held the brighter cube behind the partition as often as its own, and
  half of EmissiveStrengthTest's pixels were black), and under a still camera a point found visible stays so for
  `SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT` (8) frames without a ray (a quarter of that where something moves;
  `EmitterViewStill`). Below `SPEEDO_EMITTER_ROULETTE` (0.02) of the frame's white (the unshadowed estimate times the
  exposure) the ray is traced by chance, weighed up by it: Bistro's street by day under auto exposure, 2.5 ms (65.4 to
  67.9 with every ray).
- **What comes into view** (`EmitterNew`, the Emitter New pass, `ComputeEmitterNew`; the user's report, 2026-11-16:
  moving quickly in a dark scene, the image is noisy where it comes into view). Measured in a turn of 1.5 degrees a frame,
  flying 10 cm a frame, over Bistro's facades at night (the user's pose `724.7479,659.6074,-2775.4233,-21.034,93.844` at
  02:14, 1080p, balanced, exposure fixed at 8.6, grain and motion blur off; D3D12, RTX 4070 Ti SUPER): each moving
  frame against a settled capture of the same pose, the mean difference of 255 in eight vertical strips, over five poses
  of the turn. The leading strip was 30.0 in the emitters' view against 8.3 over the rest (the lit image 14.8 against
  5.1), as bright on average: variance, and all of it the emitters' (the near field's is flat across the screen). A
  newly seen pixel had the frame's one sample, filtered over the traced pixels around, which hold one each: blotches a
  filter's reach wide until its frames added up. What the settings could do, as bounds (without upscaling, the strip 28):
  stride 2 and a reach of 48, 15; stride 1, that reach and 32 candidates, 10; the reuse (`SPEEDO_EMITTER_REUSE=1`) 31.8,
  worse. So a newly seen traced pixel's light is the mean of `SPEEDO_EMITTER_NEW_SAMPLE_COUNT` (16) samples in its first
  frame, each its own `SPEEDO_EMITTER_NEW_CANDIDATE_COUNT` (8) candidates and ray, as the reference's, and the count goes
  with the light (the raw image's alpha): the filter weighs each traced pixel by its samples (its alpha their mean),
  the upsampling carries it, and the history counts in samples (the frame's weight its samples over them). The strip:
  21.7 with 8, 18.1 with 16, 15.6 with 32, 13.8 with 64; the lit image's 9.4 with 16 and 8.3 with 32; the strip beside
  it 16.3 to 13.7; the rest and the settled image the same (0.83 of 255 against 0.76 between two runs of one setting);
  on Vulkan 30.4 to 18.0. Newly seen is by where the point was, and of a region:
  - By the history's count (new: fewer than 16 samples where the point was, `PixelHistory`'s), a fifth of Bistro's
    traced pixels took all of the samples every frame, a still view's too (Emitter Shade 0.10 ms to 1.22): along thin
    geometry and edges (a window's frame, a ledge, a railing) a pixel's history starts over most frames, whichever
    surface its jittered sample met and whatever its normal map gave.
  - By where it was (`EmitterSurfaceNew`: the last frame's 3x3 pixels there off the screen, or all of something nearer;
    not where one saw this depth, whatever its normal, or something farther or the sky, a thin thing its sample missed
    then), a still view still cost 0.87 ms and the turn 1.44: single pixels (a gap its jittered sample looked through, a
    sliver beside an edge), few, but a thread waits for the slowest of its wave. So only where at least three of the four
    traced pixels beside it are new too (off the screen and sky count), and never in a still view: those pixels alone
    gain little (their light is the filter's over the traced pixels around, which hold one sample each). Then a still
    view and flying straight cost nothing (what a building uncovers there is slivers), and a frame of the turn marks its
    leading strip, 3% of the traced pixels. Not what moves (the stencil): it has no history at all, and takes one
    sample as before.
  - Taken in a row by the pixel's thread of Emitter Shade, that strip alone held the pass 1.45 ms (0.71 with 8
    samples) while most of the gpu had nothing to do: a sample is a chain of dependent loads, 0.011 ms of the pass a
    candidate (its walk down the tree) and 0.005 its ray, however few pixels take it. Fewer candidates a sample was no
    better a trade (the strip, and the pass's time: 16 samples of 8 candidates 18.0 and 1.44 ms; 8 of 8 20.9 and 0.67;
    16 of 4 20.4 and 0.76; 32 of 2 20.5 and 0.87; 16 of 2 23.3; 48 of 1 22.4 and 0.79; 16 of 1 27.8). So the samples are a
    pass of their own, a group of `SHADER_TYPES_EMITTER_NEW_THREAD_COUNT` (16) threads per traced pixel, each a sample
    (in turns where there are more), summed through the group's shared memory, where Emitter Shade marked the pixel
    (its alpha a half, `kEmitterNewPending`): 0.18 ms in the turn with 16 samples (0.17 with 8, 0.29 with 32; Vulkan
    0.16), 0.034 where nothing is new (a still view; the groups' launch), on a frame of 5.1 ms.
  Where the view jumped every pixel is new: at most `kEmitterJumpSampleCount` (4) samples each.
- **Filter and frames**: Emitter X and Y at the traced pixels (`EmitterFilter`: the traced pixels within
  `SPEEDO_EMITTER_FILTER_REACH`, 16, pixels along rows and columns, on the same surface, by a tent, as the near field's);
  then Emitter Temporal at every pixel of the pre-pass's depth: the frame's filtered light upsampled (`EmitterUpsample`:
  bilinear over the 2x2 traced pixels around, each by how much it is on the pixel's surface, by the traced pixels'
  surfaces as Emitter Shade wrote them, `SHADER_TYPES_EMITTER_SURFACE_TEXTURE`, RGBA32F: the point and the normal packed
  exactly, `EmitterPackNormal`; else the 3x3 by that alone, else, thin geometry between the traced pixels, a frame's side
  or a ledge's edge, the 3x3 by how their normals agree, never 0: left without light, those pixels were dark dashes along
  every edge at night, magenta in the debug views, which the tonemapper made white streaks at exposure 5), blended by 1
  over the frames it holds, at least 1 over `SPEEDO_EMITTER_HISTORY_FRAME_COUNT` (32), into the history the frame's parity
  says (`SHADER_TYPES_EMITTER_HISTORY_TEXTURE`), bilinear from where the point was (the last view projection, both frames'
  jitters taken out, `PixelHistoryAt`), each pixel only where its last surface (`SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE`,
  RG32F: the distance from the eye and the packed normal) was the same (within a fifth of a percent and two pixels' depth
  along the surface, normals within 25 degrees; 16 frames, 0.154 against 0.188 at the string lights' pose), else the 3x3
  around that was (`PixelHistory`). It was accumulated at the
  traced pixels, which the main pass upsampled, until temporal upscaling's jitter: a traced pixel at an edge saw one
  surface in some frames and the other in others, the pixels upsampled from it changed as a block, and the upscaler's
  clamp to the 3x3 around passed that on as the frame's light: speckles on every edge, ledge and leaf the emitters light
  (the user's evening pose `-994.4661,340.8679,-53.6291,-8.370,78.594` at 19.817, balanced, the emitters' debug view at
  exposure 5: 6066 pixels a frame changed by more than 32 of 255, against 356 without upscaling; 738 per pixel, most of
  them the bulbs themselves, which the view draws and the jitter samples or not; at no measurable cost with the near
  field's frames per pixel too: 50.9 ms either way, 60.1 without upscaling). Checked by the distance alone, within 1%,
  the history blurred across window frames and railings (227). A frame far brighter than what a settled history (8
  frames or more) holds is taken as at most 8 times that, or that and a quarter of the frame's white more
  (`kFireflyRise`, `kFireflyFloor`: `HistoryFireflyClamp`, which the near field's history takes too): an emitter point the candidates favour (unshadowed and near) that
  seldom reaches the pixel, a lit window's interior beside its curtain, is the whole unshadowed light the once in tens
  of frames its ray gets through, a 32nd of which was a blob that faded over 32 frames (the user's night time lapse,
  `-827.3380,155.1481,33.8635,-9.897,72.297` from 00:19; the frames its random numbers pick, 63, 103 and 146, whatever
  the time). The curtain's pixels that jumped went from about 420 to 76 over 200 frames; the reference is 11% brighter
  there than the frames (8% before), the light those rare frames carry, and the frame as bright as it to 0.1% (4 times
  took 19% and 1%). While the view is still and the model has no animation (`EmitterData::stillBlend`;
  `ViewData::previousEyePosition`'s w 2, which `Views::Upload` sets by the view projections without the jitter), the
  history holds up to `SPEEDO_EMITTER_STILL_FRAME_COUNT` (256) frames, back to 32 once the view moves: a mean of the last
  32 frames' samples never settles, and what was left of their noise wandered at the pace the frames replaced it, light
  creeping over the walls (the user's night pose in Bistro's street, `-752.4570,186.6341,97.5802,-1.211,63.522` at 00:57,
  a near field stride of 4, performance: where the user pasted, a mean change of 1.74 of 255 over 20 frames, 1.65 of it
  the emitters'; theirs at the render size without upscaling 1.81, now 0.27; 1024 frames gain little). With the 3x3's
  history at edges (`PixelHistory`) and the jitters taken out (`PixelHistoryAt`), the pose's 1.74 is 0.83. What is left
  is FSR's: on a still frame with nothing traced it repeats with its jitter's cycle (32 frames at performance: 0.10
  between frames 32 apart, 0.53 between 16), and what noise is left in the emitters' light flips its locks and shading
  change tests at thin features, window frames' and pilasters' edges (0.77 with FSR at the render size, 0.27 without).
  `EmitterViewStill` compared the jittered view projections, which never found a view still under upscaling (it gates
  the visibility reuse, `SPEEDO_EMITTER_REUSE=1`). The near field's history takes the same 3x3, reprojection and still
  frames (`GiData::nearStillBlend`, `SPEEDO_GI_NEAR_STILL_FRAME_COUNT`, 256, where the model doesn't move and the lights
  haven't changed, nor are being followed: the volume it reads is lit again as the time of day advances), its count in
  its surfaces' third channel (RGBA32F; its alpha holds the contact share): the light in a lit window's opening, the
  user's pose over Bistro's facades `724.7479,659.6074,-2775.4233,-21.034,93.844` at 02:14, changed by 1.36 of 255 over
  20 frames, 0.22 with the near field off, now 0.28. Main's opaque fragments (`PREPASS_IMAGE_EMITTERS`) take
  it at their pixel (`EmitterIrradiance`) and add the base color times it to their direct light (Direct Light's debug
  view has it; blended and scattering fragments don't).
- **The voxels** (`EmitterVoxelIrradiance`, in `GiDirectRadiance`): `SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT` (4) resampled
  points of `SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT` (8) candidates each, shadowed as the punctual lights' are, in the static
  voxels' kept light (made again when emitters start or stop lighting) and the moving ones' each frame. An animated
  emission is the frame's at the pixels, and the kept light's until the next voxelizing. Where the surface cache is
  anchored, a static voxel's kept light is its entry's from the entry's first sample on (see "The emitters' light
  the voxels keep is their cache entries'").
- **Debug views**: Emissive Light (white), its rays (unfiltered) and a reference (`DEBUG_VIEW_EMITTERS_REFERENCE`, 64
  independent estimates a traced pixel, accumulated while the camera is still: compare at stride 2, and only converged:
  the tonemapper compresses a noisy reference: a single frame of EmissiveStrengthTest's was 2% darker than 40 frames').

What was measured against the references (EmissiveStrengthTest at its frame, 40 frames; Bistro's lantern pose,
`3002.7041,404.1237,1410.4180,10.076,5.141` at exposure 6, 96 frames; mean, and `mean |log2|` blurred 4 and 24 pixels),
with the temporal reuse, before it was turned off (above): within 0.3% (EmissiveStrengthTest 0.089, Bistro 0.189 and
0.093; stride 2: 0.073, 0.138 and 0.069, a rod's shadow and thin trims a stride 4 loses). The reference uses the same
tree and candidates as the frames, so it checks what they converge to, not the tree's own densities. Spatial reuse (`SPEEDO_EMITTER_SPATIAL_COUNT`, 0; up to 8 traced pixels
within `spatialRadius`) by counts (1/M) came out 2.6% and 6% darker (a neighbor that couldn't have picked a point counted
as if it had tried); by the generalized balance heuristic (as merged now) 0.2% and still 6% with the discard (a
neighbor's point hidden there but not here), as bright as the reference without the discard but noisier than no spatial
reuse (Bistro, every other pixel traced: 0.180 against 0.129), so the discard applies only without it. Dropping a hidden point's count with it made
the light 40% too bright. Offsetting the traced pixels in turn over the stride's square made each frame's temporal reuse
a point beside the last, with the spatial reuse's bias (6% darker at stride 4). Costs (KosmicKrisp, 2560x1440, GI on, the
frame's gpu time), with the reuse: Bistro's street 58.4 to 63.8 ms, its lantern pose 70.2 to 75.4, EmissiveStrengthTest
15.3 to 16.3, CornellBox-Original 29.3 to 30.4; without it and 8 candidates, 4.1 ms more at the string lights' pose
(80.9 to 85.0, interleaved: the rays every frame 2.3, the candidates 1.8), the lantern pose 76.8 to 81.2, the others the
same; the first version (every other pixel, 8
candidates by power, the filter and frames at every pixel) took Bistro's street 49 ms more (GI off), 25 of them its shadow
rays, 1.7 a candidate, 4.4 the filter. Not yet: the emitters' specular (a glossy floor reflects no lantern), what moves emitting,
a textured emitter's power by its texture (DamagedHelmet's triangles all candidates by the factor), and the light on blended
surfaces.


## The levels disagree, and what a neural cache would cost

Two reports of the user's (2026-11-16) are of light where it is newly needed: what comes into view in a fast turn in a
dark scene (the emitters' light, see "Emissive surfaces as lights"), and dark patches on the floor of Sponza's lower
corridor that travel with the camera walking forward (the user's pose `3.9845,1.0230,-3.4321,-2.892,-91.017` at 10.5,
`SPEEDO_AUTO_EXPOSURE=0`, the indirect diffuse view at exposure 7), which the user took for the clipmap's structure.

- **Sponza's patches are the levels disagreeing.** Walking 5 cm a frame from `6.984,1.0230,-3.485,-2.892,-91.017`
  through that pose, after a hold of 2000 frames (the surface cache's sample count is capped at half the frames since
  the start: a hold of 240 measures its warm-up, not the walk, and with that the cache off seemed to remove the patch),
  a walking frame is 2.0 to 5.7 of 255 from the settled frame of the same pose (a dark blob on the floor beside the
  curtains, the walls too bright 4.8 m in), with the surface cache or without (2.2 and 5.2): the probes' too. And the
  settled frames of one view with 1, 2 and 3 levels (`SPEEDO_GI_CLIP_COUNT`) are 4.7 and 4.1 apart on average, up to 12
  in a strip, not in order (2 levels' walls the brightest), the walking frame like the 2 levels': the finer level's
  light isn't there yet where the camera arrives, and what stands in for it is another answer, not a blurrier one. Each
  level lights its own voxels with its own rays, so a place's light depends on which level the camera's distance gives
  it. A fade by how far the finer level has converged would smooth the hand-over, not remove it, and wasn't built;
  seeding a new cache entry with more of the coarser level's samples (`kGiCacheSeedCount` 8 to 64 and 512) changed
  nothing. What would remove it is light kept once per place, wherever the camera is, at several resolutions made of
  the same samples.
- **What disagrees is where each level measures its light, not what its rays meet** (step 0 of the plan to remove it;
  `SPEEDO_GI_LOOKUP_LEVEL=<n>`, `GiData::lookupLevel`: the shading takes no light from the levels finer than n, so one
  settled state is seen through each level's light, the clips, their rays and the bounce the same; the near field
  off, 2600 frames at the user's pose, the mean difference of 255 between the lookups from level 0, 1 and 2, and the
  most in an eighth of the width, the floor along the curtains):

  | | 0 against 1 | 1 against 2 | 0 against 2 | the floor's strip, 0 against 2 |
  |---|---|---|---|---|
  | as shaded (the near field on) | 1.64 | 2.23 | 3.39 | 8.6 |
  | the near field off | 1.85 | 2.68 | 3.73 | 9.9 |
  | the rays' hits lit by the last level's voxels, whatever the ray's | 1.64 | 2.94 | 3.92 | 11.4 |
  | no bounce (`SPEEDO_GI_BOUNCE=0`) | 0.79 | 2.07 | 2.07 | 6.3 |
  | the probes alone (`SPEEDO_GI_CACHE=0`) | 2.09 | 5.71 | 5.85 | 11.1 |
  | the surface cache alone (`SPEEDO_GI_CACHE_SHOW=1`) | 1.84 | 2.72 | 3.73 | 10.0 |

  So not the near field, and not the light at the rays' hits (which level's voxels a hit reads changes nothing): the
  probes of each level, and the cache's entries of each level, gather differently. Two things, both of the voxel's
  size. A level's light is measured off the surface by its own voxels: an entry's rays start at its voxel's center (an
  air voxel's, up to 3 voxels off a surface) or a voxel out along the open side (a surface's), and a lookup reads 0.75
  to 2.25 voxels off the surface: 4 to 13 cm at Sponza's finest level, 17 to 52 at its last, where the light changes
  with the height over the floor (the sky under the curtains): the band of sky light along the curtains is the wider
  the coarser the level. And a coarse voxel at a thin sheet holds both of its sides:
  the corridor's side of the curtains is lit from the atrium at levels 1 and 2 (the last level's probe nearest a
  point on it stands on the atrium's side, 0.003 to 0.009, where the finest level's entry beside it, 648 samples,
  holds what its rays meet in the corridor, voxels of 0.00002 to 0.00003: the curtain is near black at level 0 and
  dim red at the others, by leak). So the finer level is the truer, and what the levels need in order to agree is
  to measure the same thing: the light arriving at the surface itself, per side it faces, averaged over the voxel's
  footprint, whatever the voxel's size (the plan's step 1).
- **The cache anchored at the surface** (step 1; `GiCacheAnchored`; measured below against the cache
  as it was, `SPEEDO_GI_CACHE_ANCHOR=0`, `GiData::cacheAnchor`, a switch step 4 removed, see "The old cache path's
  switches removed"; only where the rays trace the triangles, which is how the surface is found). An
  entry is the light arriving at the surface itself, in the surface's voxels alone:
  - Its rays start on the surface (`GiCacheAnchor`): a ray from the voxel's open side (its center half a voxel out
    along its normal, no further than halfway to a surface facing back) toward a point of the voxel's footprint a
    voxel under it, jittered by the entry's sequence, finds the triangle, and the entry's ray leaves a sixty-fourth of
    the last level's voxel off it (`GiData::cacheAnchorBias`, 4 mm in Sponza, the same at every level); what it finds
    is seen from the open side, so beside a wall it is the wall, not the floor inside it; a back found is a ray that
    started in something. The footprint's mean, so a coarse voxel's entry is that of the finer ones in it.
  - The voxels both of a triangle's faces wrote hold entries too, where their normal is a drawn side's (the resolve
    gave those none: they stand for the hidden side as well): a slanted surface's voxels are all such (the two passes'
    layers overlap), and without them Sponza's vaults and arches had no light of their own and bounced none (the
    corridor a fifth darker, a dark smear on the vault).
  - The air voxels beside a surface's take its light in the filter's last pass (the 3x3x3 whose open side they are
    on), since the lookups now read half a voxel off the surface along its normal (`GiSurfaceBias`: the surface's own
    voxel and the 7 around), and a voxel's bounce is its own entry (the light a voxel and a half out where it has
    none).
  - A double sided triangle's rays behind it started on its other side at first, so that a thin sheet's voxel held
    each side's light in the faces toward it: the faces along the sheet then held both sides', and the floor beside
    the curtains read the atrium's sky a voxel or two out. Dropped: a voxel stands for the side its normal says.
  - The filter takes one pass at the last level and one more per finer level (3, 2, 1), so that it reaches about as
    far in the world at each: with 3 at every level it reached 1.6 m at Sponza's last, and the light under the
    curtains spread over half the corridor's floor there (levels 0 against 2: 9.8 of 255, 31.8 over the floor's strip;
    3.7 and 9.4 with the passes by level).
  - Every block that holds a surface's voxels has a page, read or not, and keeps it (`cacheKeepFrameCount` unset):
    what no lookup reads traces a ray every 8 frames, unfiltered. With pages only for what was read, what bounced off
    everything the camera hadn't looked at was the probes' light, another light than the cache's.
  - A voxel new to its clip traces 4 rays a frame for its first 256 (`kGiCacheYoungRayCount`), settled or not: at the
    settled rate, a ray every other frame, a walking camera reached it before it had 30. It started from the coarser
    clip's voxel of the same surface as 256 samples (`kGiCacheAnchoredSeedCount`, "the same light over a wider
    footprint"), until 2026-11-22: it starts from nothing now, the probes' light standing in by its samples (see "A
    voxel's light is taken only where it was gathered", the seed).
- **The cache's rays' hits take the lights' light at the hit** (step 2; `GiHitLit`, `GiHitIrradiance`,
  the measurements against `GiData::cacheLit`, `SPEEDO_GI_CACHE_LIT=0`, removed in step 4). What a ray's hit reflects was its voxel's radiance, in the
  finest clip holding the hit: a voxel takes the lights' light by a ray from its center, a voxel out along its normal,
  so a coarse voxel at the curtains or the columns, half in the atrium's sun, lit the rays that met the corridor's side
  of it. The corridor's light so depended on where the finer clips were, which is what the walking frame showed: with
  every hit lit by the last level's voxels instead (an experiment), the walking frame's difference from the settled
  frame of its pose fell from 3 to 5 of 255 over the walls to about 1. Now the lights' part of the voxel's radiance
  (`gGiDirect` less the emitters' kept light and the emission) is replaced by the voxel's albedo times the lights'
  irradiance at the hit: one light of the hit's cell of the last clip's light grid, chosen by what each gives
  unshadowed, and a shadow ray from the hit. The rest stays the voxel's (its bounce, the emitters' light, the
  emission), as do a moving voxel and a single sided triangle's back. On ShadowScene, open, where the voxels' light is
  as good, the two agree (0.12 of 255, of a mean of 55; the anchored cache against the cache as it was 0.45). The first version lit all of the hit there (the material's color from its
  texture, the bounced light by the lookup a pixel makes, `GiSampleAt`): the resolve this runs in is writing the
  volumes, five images a voxel, and salle_de_bain's walls came out pink and yellow.
- **What they give, and what they don't** (Sponza's corridor, RTX 4070 Ti SUPER, D3D12, 1080p; Vulkan the same within
  noise). The light itself changed, which is the user's to judge: the corridor's walls take about half the indirect
  light they did (the indirect diffuse view at the user's pose 12 of 255 to 4 or 5 over the walls; lit, at the
  columns' pose `7.7949,0.8419,-3.7077,0.531,-91.763`, 15 to 11.5), the leak having lit them more than what comes
  through the arches does, and the floor along the curtains is lit by the sky under their hems (17 to 25) at every
  level. The levels, one settled state through each (as step 0's table): 1.56, 2.58 and 2.78, 7.4 over the floor's
  strip (1.85, 2.68, 3.73 and 9.9), of a mean of 7.2 where it was 9.7: the last level's band along the curtains is
  still the widest, its voxels 27 cm. The walk (2000 frames held, then 5 cm a frame; each moving frame against a
  capture settled at its pose for as many frames, since a settled image still darkens by 0.5 of 255 between frames
  2700 and 5000, before and now), at 31, 47 and 63 frames: 1.18, 1.68 and 1.76 where it was 1.99, 2.41 and 2.50; over
  the walls 0.3 to 1.2 where it was 0.5 to 3.7. But of a darker image (its mean 6.8 where it was 10.3), so about a
  quarter of the light either way: what is left is beside the curtains (the band, wider in the coarser levels the
  walk comes from) and the young entries' noise, not the leak. As shaded, with auto exposure: 2.77 to 2.04. The
  regression set against the cache as it was: salle_de_bain 0.15 of 220, CornellBox-Sphere 0.07, Bistro's street 0.5
  of 66 (4 in an eighth). The indirect lighting's pass takes 1.07 ms still and 1.27 walking as it was, 1.31 and 1.62
  anchored, 1.38 and 1.74 with the hits lit (a frame 3.83 to 4.11 ms, walking 4.22 to 4.68): the search ray and the
  shadow ray, against fewer entries (no air voxels'). Not measured on Metal, where the rays are the BVH's.
  Not built: the samples of a finer entry added to the coarser ones' (the seed and the idle rays stand in), and the
  probes are as they were (they light the air, what moves, and a voxel until its entry has samples).
- **While the lights change, the cache's memory is by their pace** (`myGiLightsFollowSampleCount`; the user's report
  after trying the two steps, 2026-11-17: standing in Sponza's darkest corridor with the time advancing 60 times real
  time, large noise patterns changing over the darkest parts). Following lights that change gradually, a voxel's mean
  was of 32 samples at most, whatever the pace, and with the hits' light taken at the hit, sharper than a voxel's, 32
  are few where the light is a few rays through the arches: frames a second apart differed by 0.75 of 255, 0.7 to 0.9
  over walls of 4.6 to 6.4 (a seventh), in patches half a metre wide (0.61 before the two steps, 0.3 to 0.6 over walls
  of 9 to 12). Now as many samples as the lights take frames to turn 2 degrees or to dim by a tenth, by each change and
  the frames since the last (the sun is drawn again every 0.05 degrees: 480 samples at 60 times, 48 at 600), at least
  32, which is also what a change after the lights stood still takes; a voxel no lookup reads, a ray every 8 frames,
  an eighth of that (`GiData::cacheIdleSampleCount`: the same frames). Frames a second apart: 0.16 (0.08 to 0.15 over
  the walls, 2 to 3%); two seconds apart 0.22 (0.94). It still follows: after an hour at 60 times, the frame against a
  settled one at the hour reached is 0.87 (the cache as it was, 1.03), and 6 minutes of the sun are 0.06 of 255 there.
- **A thin thing's two sides, apart** (`GiFarSides`, `GiSampleClipWithout`, `GiVoxelSided`, `GiData::cacheThin`,
  `SPEEDO_GI_CACHE_THIN`; the user's report after the two steps, 2026-11-17: long bright lines up the edges of
  salle_de_bain's shelves, frontal pose `-0.8,17.0,-6.5,0,1.2`, where the last level lights them, voxels of 0.41 for
  boards of 0.2). A voxel holds one side's light, a lookup reads the 8 around its point whatever is between them,
  and where a board is thinner than a voxel the 8 are of both its sides: the side of a panel set a third of a voxel
  into a niche read the voxel in front of the panel's hidden face (0.68 along the side's normal, the niche 0.2 to
  0.3). The cache as it was gave such voxels no light (it lit both sides by the air voxels' own entries, the lookups
  1.5 voxels toward the eye); anchored, they hold their side's entry, which a slanted surface's must. Now the
  voxelizer keeps which way the sides that wrote a voxel face (`GiVoxelFacings`, the drawn ones' and the undrawn
  ones': a voxel in front of a drawn side and behind the front of a surface facing the other way is between a thin
  thing's sides, `GiVoxelBetweenSides`; a double sided triangle's is a sheet's) and where the drawn side's surface is
  (`GiVoxelPlane`, with the normal, so the atomic maximum keeps one side's both); the cache's filter marks such a
  voxel in the volumes' spare channel where the other side's entry holds another light (15% of the faces' sums; all
  of them marked, Bistro's main pass and reflections took 0.4 ms for looking); and a lookup that reads a marked
  voxel loads its 8 voxels and leaves out those whose surface its point is behind, blending the others by hand.
  Tried: such voxels showing nothing, the air voxels beside them showing their light (the lines gone, but a
  surface standing on the board read nothing within a voxel of it, black without the near field, and Bistro's
  panes, all of whose voxels are such, came out in blocks); the lookup moved off the far side's voxels by their
  share (blocks again in the clutter of Bistro's window frames, where it jumped a voxel sideways); what is left out
  decided by the view vector (wrong for what is seen past a board's edge, the floor under a table). Level 2 (the
  default) counts double sided sheets, 1 single sided boards alone.
- **A corner's voxel keeps the same surface's normal in the corner mirrored** (`GiPackVoxelNormal`: the axis the
  normal is most along first, z over y over x, whatever its sign). By the octahedral bits alone +z was kept over -y
  and -x, and +y and +x over +z, so the voxels along the edges of salle_de_bain's window's recess (shallower than a
  last level's voxel) stood for the wall along its top and right and for the recess's faces along its bottom and
  left, whose entries differ by two to three times (the wall's sees the recess's lit faces a fraction of a voxel
  off): the recess glowed on two of its four sides (the user's report, 2026-11-17, of a capture without the near
  field). Sponza's levels agree better for it, with the far sides left out: 1.44, 1.66 and 2.17 of 255 (1.56, 2.58,
  2.78), the band under the curtains narrower at the last level, and the image 2 of 255 darker there.
- **The emitters' light the voxels keep is their cache entries'** (`GiPackKept`, `GiCacheUpdate`,
  `EmitterLightSamples`, `ComputeGiShift`, `GiData::keptCarried`; the user's report, 2026-11-17: light splotches
  along the edges beside salle_de_bain's window). A static voxel kept the radiance its own normal reflects of the
  emitters, of 4 samples from a point of its own at its voxelizing, the same until the next: the near field's rays,
  which read the voxels they meet, drew each voxel's sample noise as a splotch. Now it keeps what arrives and the way
  it arrives from (a shared exponent color, the samples' mean direction), which a ray's hit takes by its own normal
  (`GiKeptIrradiance`, in `GiHitLit`); the voxel's entry of the surface cache takes a sample every
  `kGiCacheEmitterRayCount` (8) of its rays, of `kGiCacheEmitterCandidateCount` (2) candidates, where the ray starts
  (on the surface, over the voxel's footprint), keeps their mean as it keeps its rays' (a fourth group of words an
  entry), and writes it as the voxel's kept light; and what a voxel kept goes with it when its clip scrolls (through
  the frame's radiance, which the static lighting makes anew), so a scroll samples only what scrolled in. Tried: the
  emitters sampled at every hit (the rejected 36f0ecca: 2 candidates a hit were noise, and the nearer-than-a-voxel
  clamp at the hit's distance lit Bistro's shutters); the kept light facing-independent at the voxelizing alone (the
  same 4 samples, the same splotches); a sample every ray of 8 candidates (Bistro's indirect lighting 3.5 ms for
  1.85). Costs, with the two above (RTX 4070 Ti SUPER, D3D12, 1080p, a frame): Bistro 6.28 to 6.59 ms, at night 5.1
  to 5.73, Sponza 4.30 to 4.53, salle_de_bain 3.36 to 3.50; the indirect lighting's pass 1.85 to 2.10, 1.67 to 1.98,
  1.41 to 1.52, 1.10 to 1.24, the rest the lookups' (Main Visibility and Reflection Trace).
- **What is behind a single sided surface stays behind it** (`GiHitVoxel`, `kGiRadianceInside`, `GiPackKeptInside`,
  `GiFacingAxis`, `kGiBehindReach`, `kGiKeptNeighbourShare`; the user's report after the three above, 2026-11-17, pose
  `-0.9714,12.3124,-0.0642,-17.043,0.568` at 10.5 h: light blobs up the wall beside salle_de_bain's shelves, along the
  recess, under the ceiling and in the niche under the window, then a bright line up the left shelf's side where it
  meets the wall and a bright corner at the niche's right end). The model's wall is a sheet (z -21.322), its light a
  plane as big as the wall 1.9 behind it (the window is a hole in the sheet), the shelves boxes that reach 2.6 cm
  through the sheet (their backs at -21.348, drawn), the niche's back wall 0.3 in front of the light and its end a
  single sheet: behind the wall everything is lit 8 to 25 times as bright as the room, within a last level's voxel
  (0.41) of what the room shows. Four ways that light came through, each closed:
  what a ray met took the light of the voxel half a voxel off the hit on the ray's side, whatever that voxel stood
  for: the foot of a shelf's side (its last 10 cm before the wall are in the voxel under the wall's own) took the
  shelf's back's, or the wall's back's (9 where the room's is 0.6), and the wall's voxels along the shelf, an eighth
  of whose rays meet the side that low, held 0.5 to 1.23 where they hold 0.69 to 0.75 now. `GiHitVoxel` refuses a
  voxel whose entry of the surface cache is inside something (more of its rays than `kGiInsideShare` start behind a
  surface: the entry marks what the voxel keeps, `GiPackKeptInside`, and `ComputeGiLight` carries the mark in the
  radiance's fourth component, `kGiRadianceInside`, which the hit reads anyway) and one that only sides never drawn
  wrote with another normal than the triangle's, then looks at the six voxels beside for the triangle's own (a drawn
  side's, the triangle's normal, the hit within a quarter voxel of its plane), else takes the first not refused;
  the emitters' light a voxel keeps is shared with the voxels beside it on its surface (`kGiKeptNeighbourShare`, 0.8
  theirs, each time the entry writes it): from the shelves' sides a few percent of the light's plane is seen, a
  sample is all or nothing of it, and a voxel's hundred samples left each half off or twice over;
  the filter's taps leave out a voxel behind a surface the voxel is in front of (by the ways the drawn sides that
  wrote it face, less its own open side's; the tap's open side not that way): the niche's back wall's last voxel took
  a share of the voxels past the niche's end, which face the same way a voxel's height apart (its light 0.79 for
  0.40); and a tap further back than a quarter voxel behind the voxel's own surface (`kGiFilterBehind`, by the
  voxel's plane), whatever its open side: the gaussian along the open side gives a tap two voxels back 0.14, and the
  pass that steps two voxels took the voxels in front of the light's plane, 0.3 behind the niche's back wall and the
  same way up (21 to the wall's 0.42), where the level's voxels are 0.2. The last level's one pass doesn't step that
  far, so the patch came with the finer levels as the camera neared (the user's report after the first fixes,
  2026-11-17, poses `-2.8617,10.4162,-13.6560,-2.513,1.407`, a bright patch on the niche's back wall and a bright
  corner behind the statues, and `-3.5675,15.3023,-5.2263,-5.696,0.256`, lighter patches on the window frame's lower
  bar). The rule never leaves out the voxel itself: where its surface is more than a quarter voxel in front of its
  center (its plane runs from -1.5 to 0.5 voxels), it and the taps in its plane are that far behind the surface, and
  in a pass that steps two voxels or more, whose taps along the open side are in the air, every tap was left out and
  the voxel's light was 0 over 0. Once the last level took a second pass (716a67d5, 2026-11-18), the niche's wall beside
  its sculpture showed black squares of that level's voxels, three rows by the sculpture's hook (within 0.19 of the
  wall), in a darker band where the lookups and the near field took them in, and the sculpture's edges black marks
  (the user's report, 2026-11-20: walking forward from `-11.8867,18.0784,-4.1059,-0.239,0.298`; found by bisecting
  in a worktree whose `install/` was a junction to the checkout's, vcpkg's manifest install off in its user presets;
  the walk's frames as the 11-16 build's since);
  the emitters' light the voxels share (above) isn't shared across a surface either (`GiVoxelAcross`, the filter's
  rule, and not from behind the voxel's own): the voxel past the niche's end, under the plane of its top and facing
  the same way, keeps the light of the window's plane (94), and the niche's corner voxel took it across the end's
  sheet (29 where it is 0), its neighbours from it: the corner behind the statues, the middle level's wall there 0.82
  for 0.37;
  and a drawn side's voxel whose own point has none of its surface behind it (`GiLightPointApart`: a triangle's
  voxels reach half a voxel past its edges, that one past the niche's end) keeps no light and is marked as an inside
  entry's, so that what a ray met in it takes another's: 2 of the corner voxel's 48 rays met the top just inside the
  end and took 16 (the corner 0.56 with the sharing cut alone, 0.23 to 0.29 along it with this). Tried for the
  corner: every hit on a corner's other face looking for that face's own voxel beside (it found that same voxel past
  the end: the corner 1.8);
  and the lookups leave out a voxel behind a single sided surface they are in front of (`GiFarSides`: a side never
  drawn wrote it, the point more than `kGiBehindReach` of a voxel back that way), where the filter marked it (the
  voxel in front holds another light); a thin thing's far side is marked too where the voxel across holds no light
  of its own (the shelf's back's voxel, across the inside of its board: the line up the shelf's side).
  Tried: the kept light sampled at every ray of 8 candidates (the same blobs: the leak, not the noise, was most of
  them); an entry's rays started on the voxel's own surface alone (its normal within 60 degrees: the same picture
  with and without, not kept); every voxel refused whose normal is another and whose surface the hit is behind, and
  the voxels beside looked at for every such hit (the same picture and time, not kept); an entry's first 64 samples
  of the emitters' light one a ray (Bistro's indirect lighting 0.18 ms more, settled: not kept). Without the near
  field (`SPEEDO_GI_NEAR=0`) thin dark lines show inside the shelves' niches where every voxel a lookup reads is
  left out; the near field covers them.
  Measured (RTX 4070 Ti SUPER, 1080p): at a fixed exposure the room is 10 of 255 darker at the pose (the light that
  leaked lit it), with auto exposure the same to 0.4 at the two other poses (1 to 2 over an eighth of the image);
  Sponza's columns and corridor 0.04 and 0.09, CornellBox-Sphere 0.01, Bistro's street 0.16 (0.6 over an eighth), at
  night 0.01 (D3D12). A
  frame on Vulkan, each run beside one of the shaders before: Bistro 6.10 to 6.45 ms, at night 5.29 to 5.57, Sponza
  4.14 to 4.26 (the indirect lighting's pass 2.01 to 2.18, 1.89 to 2.04, 1.47 to 1.52; Reflection Trace 0.66 to 0.78,
  0.49 to 0.56, 0.27 to 0.31; Main Visibility 0.45 to 0.50, 0.46 to 0.49, 0.43 to 0.45): not found which of the
  parts the lookups' share is (without the marks of the single sided surfaces Bistro's two passes took 0.02 less).
  The filter's cut behind the voxel's own surface, the sharing's and the marked voxels together (the second report's
  fixes): salle_de_bain 0.5 of 255 darker at a fixed exposure, the rest of the regression set within 0.03 (Vulkan);
  Bistro's frame the same (6.36 and 6.34 ms), Sponza's 4.25 to 4.32.
- **A voxel's light is taken only where it was gathered** (`GiVoxelApex`, `GiApexSees`, `GiClipSees`, the GI Sides
  pass and `ComputeGiSides`, `GiSidesMask`, `GiSidesHidden`, `kGiApartMark`, `GiHitVoxel`; the user's reports,
  2026-11-21: light leaking along the edges of bathroom2's room, glTF research scenes, pose
  `4.4431,16.9345,49.9102,-1.189,-8.257` at 10.5 h, focal length 36.95, then the top of its pillar, the statuettes in
  the niche under the window and a line along the ceiling's right edge). The room is of double sided sheets, a hollow
  pillar in its back left corner a box of them open at the back to the bright space behind the back wall (lit by an
  emitter of radiance 125), the niche a box of them standing out into that space, and the camera sees it all at the
  last level (0.68 units). An anchored entry's rays find its surface from its apex, half a voxel out from the voxel's
  center along its normal (`GiCacheAnchor`), so its light is the light of where the apex is: the back wall's voxel at
  the pillar's side has its center on the side's sheet (x -15.704, the sheet -15.701), its apex 0.003 inside the
  pillar; the voxels of the niche's sheets hold their outer side's light. The walls beside the pillar read it (a column
  of blue light up the corner, 101 of 255 against the wall's 72; a line down the right corner, 75 for 58; lines down
  the shelves' edges), and the rays that met the niche's insides took it (the statuettes and the niche blue, 61 where
  voxels a quarter the size give 35; the pillar's top 74 to 82 for 53 to 58).
  Now a voxel's light is taken only where its apex sees the point taking it, by a short any-hit ray against the
  triangles: a lookup, and a ray's hit (`GiHitVoxel`) on a voxel marked apart. The filter marks a voxel apart where,
  at a junction (a voxel beside it of a surface more than 60 degrees from its own), its apex doesn't see a neighbour's
  (every occupied neighbour but a thin sheet's other side, which `GiFarSides` sees to: a voxel at the pillar's face
  keeps the face's normal and its apex is outside the left wall), as `kGiApartMark` times its weight in the volumes'
  spare channel, which `ComputeGiLight` copies into the voxel's radiance for the hits (`kGiRadianceApart`). A lookup's
  rays are traced before the main pass, in a pass of their own (GI Sides): at each pixel near a marked voxel
  (any of the 27 around its point) the pass tests the 8 voxels its lookup reads and keeps which of the 27 its point
  sees (RG32F, a clip's mask as a float's bits); the main pass's opaque fragments leave out the others, by hand, with
  the point and the clips of the pre-pass's depth (`ShadowSurfaceAt`), so that the 27 are the same ones.
  At the pose now: the corner 51 to 57 (the wall beside 55 to 60), the right corner 58, the shelves' lines gone, the
  pillar's top 55 to 65, the statuettes 47 to 56 (42 to 48 with every hit tested, below); D3D12's frame within 0.01 of
  255 of Vulkan's; the regression set within 0.15 of 255 of the build before (Vulkan, settled, repeatable). The
  line along the ceiling's right edge is the space above the ceiling seen through a slot (the ceiling stops 0.09 short
  of the right wall, the wall 0.3 under the ceiling): open, 0.54 high, lined with the outer shell's white (albedo 1),
  it is lit at the last level through the bounce alone (dark with `SPEEDO_GI_BOUNCE=0`, as at resolution 256 and 512),
  at 1.6 times the ceiling's light; open.
  Tried: the rays in the lookup itself (`GiSampleClipWithout`): a ray query in the main pass's shading, even where none
  was traced, made it slower (bathroom2's 0.66 ms for 0.48, Bistro's 0.90 for 0.61, and the near field and the
  reflections, whose lookups are the same code), and an opaque only query no less; every junction marked, nearly every
  lookup went by hand (Bistro's frame 11.3 ms for 9.1); the far sides' marks counted as apart (bathroom2's walls are
  double sided sheets, all of them: the pass 1.4 ms, 2.4 at Bistro); a ray for the marked voxels alone (the air voxels
  inside the pillar are never marked: the corner as it was); only a surface's own voxels beside it as the neighbours
  marked against (a band on the left wall by the corner, 106 for 86); all 27 tested (the pass 1.0 ms, 2.1 at Bistro);
  every candidate tested at the hits (the apexes of a corner of three surfaces' voxels, pushed along their normals, are
  behind the surface beside: hits refused all of a corner's voxels, a dark speck in each of MovingInterior's top corners
  and the room 2.8 of 255 darker; Bistro's GI pass 0.45 ms slower); a march through the voxels from the point to the
  voxel's surface point, as the world grid's lookups reach theirs (`GiWorldReach`): nothing changed, since the first
  apex is a twentieth of a voxel from the point across the sheet and the other's march starts in the pillar face's own
  voxel.
  Cost (RTX 4070 Ti SUPER, Vulkan, 1080p, settled, interleaved with the build before): bathroom2's frame 4.24 to 4.26 ms
  for 3.84 (GI Sides 0.29, the GI pass +0.05 to 0.12, the main pass +0.08), Sponza's 5.28 to 5.29 for 4.63 (GI Sides
  0.29, the GI pass +0.29, the main pass +0.08), Bistro's passes 1.3 ms more (GI Sides 0.62, the GI pass +0.44 to 0.54,
  the main pass +0.09, the reflections +0.13). Next: the marks made once a voxelizing (they are of the static voxels),
  and fewer rays where a mark is of a corner.
  - **The masks by the clip's level, the point the fragment's own** (`GiSidesHidden`; the user's report, 2026-11-21:
    "black darkened areas in the sink and non smooth blocky patterns", bathroom2 at
    `-2.2144,14.1125,-5.1979,43.639,1.292`, the bath seen from above). The main pass reconstructed the point and the
    clips from the pre-pass's depth (`ShadowSurfaceAt`), to have the GI Sides pass's 27 voxels, but read there it took
    another clip (1 where the pass had 2) and applied clip 2's mask to clip 1's voxels: patches the size of the last
    level's voxels, stair stepped. Now a mask keeps its clip's level (bits 27 and 28: a view's clips are one a level)
    and is found by it, and the voxel is the fragment's own point's, the mask left out within a thousandth of a voxel of
    a voxel's face (`kGiSidesEdge`), where the pass's point may be in the voxel beside. The main pass 0.07 ms faster
    for not reconstructing (bathroom2 0.49 ms for 0.56, Bistro 0.63 for 0.70).
  - **A surface stacked over another within a voxel is a junction too**: the bath's bottom is a sheet 0.35 over the
    floor, and under it the floor's voxels' apexes are in the dark between; the lookups on the bottom read them (a dark
    ring around it, the voxels a quarter the size even), and only the floor's voxels beside the bath's sides were
    marked apart (their neighbours outside see past the side), so the masks cut the ring in patches there alone. Now a
    voxel with an occupied neighbour along its own normal is at a junction (`ComputeGiCacheFilter`): the ring is gone
    with the patches. The GI pass the same (+0.01 to 0.04 ms).
  - **A voxel new to its clip isn't seeded from the coarser clip** (`GiCacheUpdate`; the user's report, 2026-11-21,
    walking at the client's speed between `17.2606,17.5270,1.5210,29.050,-38.099` and
    `5.8027,7.2424,-12.9784,27.193,-39.740`: a dark patch on the bath's inner wall under the counter "fading into view
    when moving ... fading over time", and streaks across the wall, the surface cache's, gone with it off). An anchored
    entry new to its clip started from the coarser clip's voxel its surface was in, as 256 samples, as settled as it
    arrived: under the counter's lip that voxel gathered its light under the counter (the patch has the shape of the
    taps' plate), and elsewhere the coarser entries were darker than this level's (the streaks); as the entry's own
    rays outweighed the seed (4 a frame for its first 256, then one every other frame) the patch faded over tens of
    seconds. Measured on the irradiance view against the frame settled at the second pose (exposure 3 fixed, frames
    counted as 6.95 ms, the user's 144 a second, the walk 260 frames, the user's 12 units a second): seeded 1.86 of
    255 darker over the frame on arriving, 1.82 three seconds later, 1.35 after 35; now 0.17 lighter, 0.12 after a
    second. The pair at `1.2450,14.7488,4.4895,23.745,-4.228` and `0.7593,11.8509,-2.0801,23.745,-4.228`: 0.80 darker
    on arriving for 0.12 lighter, 0.16 for 0.10 backing out. Tried: the coarser entries around the new one's apex
    whose own apexes see it, by their trilinear shares (as a lookup takes them): the patch gone, the streaks not (1.92
    darker); a seed of 32 samples: fainter. Unseeded, the probes' light stands in for the entry by its samples until
    it has its own (that the seed was for, when the light along Sponza's curtains faded in and out as the finer clips
    scrolled): Sponza's corridor walk within 0.06 to 0.08 of 255 of the seeded one's frame for frame, and
    salle_de_bain's, MovingInterior's and Bistro's walks within 0.06; the regression set settled within 0.04 of the
    build before. The frame 0.04 to 0.4 ms faster (the main pass, above).
  - **Light gathered under a double sided surface** (`GiOpenAlong`, `GiCacheFilterStacked`, `gGiAhead`'s word and
    `GiAheadPlane`, `GiFarSides`; the user's report, 2026-11-22: at `0.2820,17.9171,-9.4363,69.900,-15.270`
    "darkening, that changes in size in steps when moving backwards and forwards"). The bath is a shell of double
    sided ceramic with the room's floor running on inside it, and at the middle level (0.34 units) the voxels under
    the bath's bottom stand for that floor (its exact +y is kept over the bottom's slightly tilted one); where the
    bottom crosses their row, a ring, their centers are just under it. How far a voxel is open (`GiOpenAlong`) was
    measured to the bottom's back, 0.035 of a voxel, so the apex stayed in the shell and the entry's rays started on
    the floor inside it: a thirtieth of the bottom's light (0.0004 against 0.01). The lookups on the bottom read those
    voxels at about half, and the cache's filter took them into the lit voxels beside at the same height, whose planes
    (the floor's as well) passed its behind test: along the ring 35 to 49 of 255 (exposure 3, the irradiance view, the
    bottom 47 to 49) where the voxels' own light, unfiltered (`SPEEDO_GI_CACHE_SIGMA=0`), gave 51 to 56 and resolution
    256 gives 54 to 57; as the clips' levels change with the camera, the ring changed in steps.
    Now an entry keeps where its rays start (the anchored point along its open side, a running mean of 32, in
    `gGiAhead`'s spare half: the fast mean's lead fits a half), and the filter leaves out a tap whose rays start more
    than a quarter voxel behind the voxel's while the voxel's are as far in front of the tap's (on a convex surface each
    is behind the other's plane, and stays); and the open side is measured past a double sided triangle facing the
    voxel's way, met from behind within half a voxel of its center (the center is inside what it bounds). The ring row
    58 across; the bottom smooth at five poses from 8 units back to 12 forward, and in walks both ways at the user's
    speed (mid walk within a level of 255 along a row, the build before dipping to 38); the rim's top 3 of 255 brighter
    (its voxels' centers were inside its double sided wooden board) and the opening under its lip no longer black. The
    blob at the second pose of the 2026-11-21 report, low on the bath's left (41 to 45 where the voxels a quarter the
    size give 55, taken for the levels' disagreement), 55 to 58: the floor's voxels under the bath too.
    With the rim's top lit, a line 6 of 255 darker showed along it where the voxel under the top stood for the lip's
    rounded underside, tilted, and the lookups on the top were in front of its plane: now a sided voxel facing against
    the lookup's surface with its center behind that surface is a far side too (`GiFarSides`).
    Tried: keeping the frontmost of a facing's surfaces in the voxelizer (the plane's bits before the normal's): the
    ring gone as well, but beside the taps it changed which surface the voxels under the deck stood for, and dark
    blocks came up on the rim; the open side measured from past the voxel's own plane (the plane is the floor's there:
    nothing); the filter's test alone (2 to 4 of 255 left along the ring, the voxels the lookups read directly); the
    open side past a double sided triangle met from either side (the same here, but a pane facing back at a voxel would
    have been passed); the far side test for every voxel behind (Bistro's lookups 0.02 to 0.04 ms a pass more).
    Measured: the regression set within 0.4 of 255 of the build before (its run to run spread), bathroom2 0.9 brighter
    (its surfaces are double sided); Bistro at night's lit window interior comes out 53 or 59 of 255 from run to run,
    before as after (3 runs of 13, 4 of 16). D3D12 within a level of Vulkan. Cost (RTX 4070 Ti SUPER, Vulkan, 1080p,
    settled, interleaved): the GI pass 0.02 to 0.03 ms more, Bistro's lookups 0.02 a pass (its frame +0.06 to 0.09),
    Sponza's frame +0.04, bathroom2's the same.
  - **The niche behind the statuettes** (`kGiSidesPush`, `GiCacheFilterBlocked`, `GiCacheAnchor`'s facing,
    `kGiAnchorFacing`, the rays behind an entry's surface in `GiCacheUpdate`; the user's report, 2026-11-22: at
    `-1.8947,17.6481,6.2699,12.074,1.442` light leaking into the niche behind the statuettes, its look changing with
    the distance). The niche under the window is a box of double sided sheets standing into the space behind the back
    wall that the emitter lights; its back wall lies on a face of the voxels (the scene's bounds, which the grid is
    aligned to), and the camera sees it from the middle and the last level. Four things, measured on the irradiance
    view at exposure 3 (the last level's voxels 0.68 units):
    - The lookups on the back wall never left out what their point doesn't see: the GI Sides mask is skipped within a
      thousandth of a voxel of a voxel's face (`kGiSidesEdge`, the pass's point may be in the voxel beside), and the
      wall is on one. The voxels under the niche's floor and beyond its sides, whose apexes are in the lit space, lit
      a band along the floor, the left end and the right edge, 87 to 93 of 255 where the wall above is 51. Now the
      pass and the lookup take the 27 voxels about the point pushed a tenth of a voxel off the surface, as the apexes
      are tested from: the band 48 to 54, continuous with the wall.
    - Walking in, at 8 units forward, a blue smear behind the statuettes: the filter's pass that steps two voxels took
      a statuette's back's voxel under the floor (its center under it, 0.0335 against the voxel's 0.0071), a quarter
      of the weight, across the floor's voxel between, whose own normal is the statuette's. Now a wide tap past a
      drawn side on the voxel's own layer between that faces against the way (but the ways the voxel's own drawn
      sides face) is left out. Sponza's corridor 1% darker for it (the column tops by the sunlit atrium 29.7 of 255
      for 31.8, the voxels a quarter the size 26.0 to 26.4). Tried: a ray between the two apexes where either is
      marked apart, as well, but in the filter's loop 0.1 to 0.2 ms a frame more, whatever it was gated by (the ray
      query in the loop itself).
    - A lighter strip beside the leftmost statuette, darker behind them (the user's report): the last level's voxels
      behind the statuettes stand for their backs, 0.59 in front of the back wall, which has no voxel of its own
      there, and the wall's lookups read theirs. The strip's voxel has a sliver of a statuette in its footprint (4.5
      centimetres), and its anchor's rays, aimed back across the footprint, found the niche's side wall a voxel or two
      forward, near the opening (0.0075): a surface more than 60 degrees from the voxel's normal is taken as none now,
      the ray starting in the open toward it, within half a voxel of the apex (`kGiAnchorFacing`). And the quarter of
      an entry's rays that go into the hemisphere behind its surface started on it and met it at once (behind a
      statuette its own light, 0.0009): they start in the open in front of it now, halfway from where the ray was
      anchored to the apex. Both in a gap alone: a voxel with a surface facing back at it within a voxel in front, which
      wrote it too (the two surfaces' entry is one, and the one the voxel doesn't stand for has none of its own).
      Everywhere, the anchors took the air in every corner (salle_de_bain 0.25 of 255 brighter) and the rays behind saw
      past the surfaces' edges at grazing angles (Sponza's columns 3% brighter, by the sunlit atrium); in every voxel
      written by sides facing both ways (the two sides of Bistro's frames and shutters) its facades 0.25 brighter, away
      from the voxels a quarter the size. The strip at the room pose
      (`4.4431,16.9345,49.9102,-1.189,-8.257`) 28 to 31 for 40 (the voxels a quarter the size 28), the statuettes' box
      37.7 for 37.8 and the leak's 64.7 along the niche's floor 51.8 (36.6 and 52.1); at 8 forward the strip 28 to 29
      for 36 to 41 (27 to 28), between the statuettes 26 to 35 for 9 to 18 (29 to 33). Tried: weighing a clip by the
      share of its voxels around the point that face its way (the middle clip took over in its fade: 30 to 31 and 25
      to 29 at 8 forward; no better once the entries held the wall's light); trusting the lookup by that share, so the
      near field stood in (the user's call: the cache to hold the light); refusing the anchor's other surfaces (the
      strip's voxel had no entry and the probes' light, which leaks there, stood in: 52).
    - Open: a voxel stands for one surface, and at the last level the statuettes take voxels that are mostly wall (the
      opposite facing kept by the order of the normal's bits), and the niche's side wall loses its voxels beside them
      to their fronts (the z axis over x): a dark line in its corner (13 for 25) and a few levels in the strip. Next,
      a voxel's surface chosen by how much of it each facing covers (the user's call, 2026-11-22). And walking in, the
      niche is 4 to 5 of 255 too bright for some 30 frames: the young entries show the probes' light, which leaks
      there (63 to 87 with the cache off).
    - Measured: the regression set within 0.2 of 255 of the build before (Sponza's corridor 0.18 darker, the filter's
      test above; MovingInterior's within its animation's run to run spread).
- **The probes, resolved where they show** (step 3: the probes demoted or kept; `GiResolve`, `GiData::probes`,
  `SPEEDO_GI_PROBES`, `SPEEDO_GI_PROBE_AIR_INTERVAL`, `kGiCacheSettledCount`, `kGiVisibilityCacheAlone`,
  `kGiVisibilityBlended`). Kept, and resolved into fewer voxels. With the cache anchored, a settled entry's voxel shows
  the cache's light and a sixty-fifth of the probes'; what the probes are still for was measured by leaving
  them out altogether (`SPEEDO_GI_PROBES=0`, against every voxel as it was, `2`; settled, RTX 4070 Ti SUPER, Vulkan,
  1080p, of 255): salle_de_bain 0.02 at its framed pose, CornellBox 0.08, BrainStem, CesiumMan and Fox on their ground
  0.02 to 0.16, Sponza's columns 0.56 and its corridor 1.33 of 7.7 (the floor along the curtains), Bistro 0.8 (4 in
  parts); the levels' agreement in the corridor the same (1.45, 1.57 and 2.07 for 1.46, 1.62 and 2.09) and the walk
  (1.07, 1.58 and 1.73 for 1.08, 1.57 and 1.86): they are no longer what makes the levels disagree. But:
  - the fog reads them itself (`AerialPerspectiveAmbient`), and without them takes the sky's light indoors: Sponza's
    corridor blue with haze at the default 30 km;
  - what moves is theirs: an entry in a block something moves through is capped at 32 samples (a third the probes'),
    and a moving thing's voxels are new every frame (most of its light);
  - and the air's light is theirs, which more reads than the note of step 1 has it: the near field's rays that meet
    nothing (`GiNearTrace`: the volumes where the ray ends, 8 voxels out), what lets light through from behind
    (`GiIrradiance` along the normal's opposite, a voxel and a half back), and every lookup beside a surface's voxel
    that holds few samples or none (below).
  What they cost is the resolve, not the rays (the GI pass, before: salle_de_bain 1.53 ms, Sponza 2.00, Bistro 2.19;
  without any 1.04, 1.26, 1.66): the rays 0.06, 0.02 and 0.17 ms (`SPEEDO_SKIP_PASSES=GI Trace`), the probes' update
  0.12 to 0.14, and their light weighed into every voxel within the flags' reach of a surface, 8 probes each, the rest
  (0.25 to 0.55). So fewer rays save nothing (64, 32 and 16 a probe: salle_de_bain's pass 1.513, 1.512 and 1.500 for
  1.526) and aren't taken: a moving thing's light is as it was.
  - **Tried first: the probes left out of the blocks whose entries are settled** (a bit a block in the cache's table,
    set by its voxels' entries of fewer than 256 samples and read by the 27 blocks around the frame after; a block no
    lookup reads settled at 32; an entry settled too once it had traced 256 rays, since 40 to 60% of the blocks hold a
    voxel past its triangle's edge, whose rays find no surface under it to start from, and never were; the voxels of
    what lets light through marked, their blocks never settled). The probes then lit a twentieth of the blocks after
    500 frames and none after 5000, the pass 1.56 to 1.23 ms in salle_de_bain, 1.99 to 1.52 in Sponza, 2.17 to 1.98 in
    Bistro. And the picture changed where no test had looked: salle_de_bain's window's recess 10 to 27 of 255 brighter
    from the front (pose `-0.8,17.0,-6.5,0,1.2`, exposure -1), and Bistro's shadowed facade (`564,170,-2436,0,30`) 10 to
    20 darker and blotchy, through the finer levels alone (through the last level, `SPEEDO_GI_LOOKUP_LEVEL=2`, the two
    were 0.04 apart). A lookup reads the surface's voxel and the 7 around it; beside a surface's voxel of few samples
    (thinner than a voxel: Bistro's window frames and shutters, a recess shallower than a voxel) those are air voxels
    that take no entry's light, and what they held, the probes', was what kept the lookup smooth (the recess: the
    entries 0.65 to 0.75 along +x, the air beside them 0.016 to 0.037 at weights of 0.2 to 0.4). Not a truer picture
    either way; dropped for one that keeps it.
  - **As built**: the probes' light stays in every voxel it was in, and is resolved into a voxel only when that can
    change what it shows. The cache's filter leaves in the voxel's word of its probes' visibility what its light was
    made of: the cache's alone (a settled entry's, 256 samples, or an air voxel's beside settled ones), or a blend. The
    next frame's resolve, per voxel: none into the first (the probes' share, the prior's 16 over the samples and it, is
    scaled to reach none at 256, so nothing steps); every frame into the second (few samples: just made, after the
    lights changed, where something moves); and into the rest, the air and a surface's voxel without an entry's light,
    every `SPEEDO_GI_PROBE_AIR_INTERVAL`-th update (8), a block at a time in turn, the voxel keeping what it holds in
    between (the volumes aren't written), at once where a probe of the block moved. Their light changes by the
    hysteresis's few percent a frame, so 8 frames late is nothing while the lights are still; while they change,
    gradually or within the last 64 updates, the interval is 1 (Renderer). Into every voxel as it was after a reset, in
    a clip that scrolled this frame, in a block something moves in or beside (this frame or the last), and where the
    cache is off or isn't anchored.
  - **The probes' depth maps aren't made where the rays trace the triangles** (`ComputeGiProbeUpdate`): the resolve
    tests each probe by a ray there, and the maps were most of the update's work, a texel against every ray (0.01 to
    0.05 ms). The indirect lighting starts over when what the rays meet is switched. The Probe State debug view, which
    read the maps, traces the resolve's ray there instead (see `GiProbeStateDebug`), and `SPEEDO_GI_DUMP` leaves the
    probe's map out.
  Measured, each run beside one of every voxel (`SPEEDO_GI_PROBES=2`, with the depth maps left out too), 5000 frames:

  | | the GI pass | a frame | voxels the probes are resolved into |
  |---|---|---|---|
  | salle_de_bain | 1.84 to 1.21 ms | 3.09 to 2.95 | 793000 to 92000 |
  | Sponza's corridor | 1.98 to 1.62 | 4.36 to 4.01 | 1320000 to 155000 |
  | Bistro | 2.18 to 2.08 | 6.05 to 5.94 | 1116000 to 144000 |
  | Bistro at night | 2.09 to 1.99 | 5.32 to 5.18 | 1080000 to 142000 |

  (the air every 4th update: 1.27, 1.66, 2.10 and 2.01; every update, the settled voxels alone left out: 1.82, 1.92,
  2.18 and 2.08, so the air is the saving; after 600 frames into 232000, 409000, 344000 and 344000.) The
  picture is the same to the noise between two runs: the regression set, the user's four poses of salle_de_bain's
  shelves, niche and window, and the frontal one, 0.00 to 0.18 of 255 on average and 0.12 to 0.60 in absolute
  difference (CornellBox, where nothing changes, 0.12), on D3D12 the same; the walk 1.02, 1.54 and 1.84 for 1.05, 1.58
  and 1.86. What Bistro still pays for its probes (0.46 ms of the pass) is their rays and update.
  - **What moves, checked** (`resources/test-models/gltf/MovingInterior.gltf`, `scripts/test-assets/movinginterior.ps1`:
    a room lit through a skylight at one end, a sphere flying its length at 4.5 m/s, a cube turning, a small box darting
    across by the floor; `SPEEDO_REPEATABLE=1` starts the animation over where its count starts, so that with
    `SPEEDO_ANIMATION_FRAME_SECONDS=0.015625` frame 700 is the same pose in every run, and `SPEEDO_ANIMATION_TIME=10.9375`
    holds that pose still, which the client prints at exit): the frame against every voxel's 0.49 of 255 (0.30 on
    D3D12), and against the pose held still, settled, 1.94 where every voxel's is 2.34. With no probes at all, after
    2816 frames, 1.12 from every voxel's.
  Seen on the way, as it was before and not looked into: salle_de_bain's frontal frame brightens by 5 of 120 between
  frames 2600 and 14000, with the probes resolved either way (at 600 it is 9 to 12 under).
- **The old cache path's switches removed** (step 4, the user's go-ahead 2026-11-18): `SPEEDO_GI_CACHE_ANCHOR=0` and
  `SPEEDO_GI_CACHE_LIT=0`, `GiData::cacheAnchor` and `cacheLit`, kept through steps 1 to 3 to compare against. Where the
  rays trace the triangles the cache's entries are the light at the surface (`GiCacheAnchored` is `GiTracesBvh`) and
  their rays' hits take the lights' light at the hit, always. The code of a voxel's light at the voxel is not gone
  with them: it is what runs where the rays walk the voxels (View > Rendering Features > Indirect Lighting... > Rays: Voxels,
  `SPEEDO_GI_TRACE=voxels`, a model without a BVH), which finds no surface to anchor at, with the probes' depth maps
  and the resolve by them (`ComputeGiResolve`). Removing that is removing the voxel walk, a decision of its own.
- **A neural radiance cache in plain compute is affordable, and learns too slowly for this** (`tools/neuralbench.cpp`,
  `neuralbench.slang`: the gate of the plan to try one; the user asked for Slang's neural features to be looked at, and
  for hardware such as tensor cores to stay an opt in beside a portable default that is practically useful). A
  multiresolution hash grid of learned features (8 levels of 2^17 entries of 2, 16 to 2048 cells a side) decoded by a
  small network, trained by Adam on a made up scene's noisy estimates (walls an eighth apart, surfaces of a colour each,
  a shadow's edge, four small bright lights; each estimate the radiance times a uniform 0 to 2; the error at 16384 places
  no step sampled, the difference's share of the light), 16384 samples a step, on an RTX 4070 Ti SUPER, Vulkan's times
  (D3D12's 10 to 35% more; Metal's not measured):

  | | 129600 queries | 2073600 queries | a step | error after 1, 16, 64, 256, 1024 steps |
  |---|---|---|---|---|
  | the grid alone (the features' weighted sum) | 0.13 ms | 2.1 ms | 0.12 ms | 90, 63, 45, 26, 15% |
  | 2 layers of 32 | 0.12 | 1.8 | 0.14 | 99, 68, 35, 16, 9.7% |
  | 2 layers of 64 | 0.30 | 4.9 | 0.23 | 98, 55, 31, 15, 9.6% |
  | 4 layers of 64 | 0.71 | 11.3 | 0.42 | 98, 54, 36, 15, 9.7% |
  | 2 of 32 over 12 levels of 2^19 | 0.16 | 2.6 | 0.63 | 97, 59, 31, 16, 8.9% |
  | cells of running means, 4 levels (16 to 128 a side) | 0.012 | 0.18 | 0.010 | 38, 20, 13, 9.6, 7.6% |

  The cost is no obstacle: every sixteenth pixel queried and four steps a frame is under a millisecond for the small
  network. A step's time is its samples' pass (their answers, gradients back through the layers, and the grid's
  gradients added with integer atomics, fixed point, the one kind every backend has: 0.09 ms of 2 of 32's 0.14), then the
  weights' gradients as a matrix product of the samples' gradients and inputs, a thread per 4 by 4 weights and chunk of
  256 samples (0.025), not per sample atomics on every weight. But the size of the network hardly changes how fast it
  learns, nor do the rates (the grid's 0.01 to 0.1, the weights' 0.003 to 0.03: 26 to 35% after 64 steps) or four
  times the samples a step (30% after 64, 7.6% after 1024, at 0.52 ms a step): it takes tens of steps whatever it is
  given, as Mueller et al. 2021 say of theirs (usable after 8 frames of 4 steps, right after 64). Cells of running
  means given the same samples (each sample added to its cell at every level, a query the finest level's cell that
  holds 16) are at the network's 64 steps after 4, at its 256 after 64, and ahead of it still at 1024, for a tenth of
  the time: a mean is as good as its samples from the first, and its coarser levels are the same samples' means, so
  they agree. The made up scene flatters neither much: its cells are dense and stop at 128 a side, where a real scene
  needs them hashed and finer, and the network's answers are smooth where a cell's are flat.
- **Slang's neural module** (`slang/neural.slang`, 2026.22, experimental: "do not use in production"): its layers'
  portable vectors (`InlineVector`) sum the weights' gradients with an atomic add per sample and weight (a compare and
  exchange loop on floats on HLSL), which is the pattern the benchmark's matrix product avoids, and its accelerated ones
  (`WaveTangledVector`) are cooperative matrices, on CUDA, Metal and SPIR-V (`VK_KHR_cooperative_matrix`), none on
  D3D12 (cooperative vectors left shader model 6.9 for 6.10); `CoopVec` is NVIDIA's Vulkan extension. So an opt in for
  such hardware would be per backend, behind an rhi capability, and nothing to build on yet.

So the replacement for the levels' light, if one is made, is world-anchored cells of means at several resolutions (as
the hashed radiance caches keep theirs), not a network: the network stays an option for what cells can't hold (a glossy
lobe's radiance by direction), where its tens of steps are affordable.
