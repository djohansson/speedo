# Indirect lighting

A note of CLAUDE.md (its index names each): the probes, the voxel clipmap, the surface cache, the near field, the rays and the emitters.

## Indirect lighting: irradiance probes over a voxel clipmap

View > Indirect Lighting (on by default since 2026-10-22, the user's call once the artifacts that followed the camera
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
  longest axis; View > Indirect Lighting Resolution offers `gi::kResolutions`, 64 to 384, and a change makes the clipmap
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
  what the camera looks at, by `SPEEDO_GI_LOOK_AHEAD` of its half size at most (0.5 by default; 0 centers it on the
  eye): how far the camera looks is the median distance of the frame's drawn pixels from the eye (`ComputeMain`
  unprojects the main depth into 64 bins of log2 distance after the exposure histogram's, `SHADER_TYPES_DISTANCE_BINS`,
  read back with it and smoothed as the exposure is: `FrameInputs::lookDistance`), and the centers lie along the view's
  forward, so each clip stays inside the next, and the eye stays in the finer clips, two thirds of the way to their
  fade. Centered on the eye, half of each clip was behind it, and from the roof over Bistro's square (27 m from the
  hotel's entrance) the square was the last clip's; with the look ahead the ground is clip 1's (4.4 m probes), the
  entrance's facade still mostly the last's at 0.5 (its weight in clip 1 is 0.2 there). The buffers a new clipmap gets
  are cleared first (the GI Clear pass: every GI buffer and the volumes filled with 0 when they were replaced,
  `myGiClearBuffers`): the resolve writes only the blocks near a surface, and a lookup's filter reaches into the ones
  beside them, which held whatever their memory had (cyan and red over Bistro's square, in one run of two). Each has its own voxels, radiance, probes
  and flags, at its offsets in the shared buffers (`GiData::voxelFirst`, `radianceFirst`, `probeFirst`,
  `lightCellFirst`, `flagFirst`; the resolve's volumes stack the clips' slabs along z, `GiData::volumeImage`: the
  images' texels per axis and the clip's first along z;
  `gGi[frame][clip]`), and the kernels take theirs from `PushConstants::giClip`. The flags' offset is in words
  (`SHADER_TYPES_GI_FLAG_WORD_COUNT` a block): in blocks, each clip's second half of flags was the next clip's first,
  whose kernels rewrote them (its bits, and other blocks' stamps), so in the finer clips' far halves blocks near a
  surface went unresolved and probes untraced, at random (found 2026-10-22 reading the flags from the rays' walk). A
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
- **Moving clips**: a clip that follows the eye moves by whole snaps (`gi::kSnapProbes`, one probe: 8 voxels; clamped
  to whole snaps at the last clip's sides too), so everything a point inside it is lit by stays where it is in the
  world as it snaps: its probes are laid out from its corner (`GiData::probeOrigin`), a whole number of snaps from the
  clipmap's, and keep what they hold across a snap (the update reads each probe's history from where it was, by the
  scroll); the flags' blocks are rounded up (`GiLevelVoxels`, `gi::LevelVoxels`); and each clip has a box around its center that it always holds well
  inside (`GiData::fadeCenter`, `fadeExtent`: half its size less half a snap; with a snap of 4 probes and a spacing and
  a voxel more of margin, Sponza's thin clips, 68 voxels tall, kept a box a third of their height, and the finest clips
  lit nothing on its ground floor: everything there was the last clip's, 23 cm voxels and 1.9 m probes), which
  the shading fades it out toward over a third of its half size (`GiClipWeight`) and rays leave it at
  (`GiFadeBoxExit`), so what changes as a clip snaps changes with the eye, smoothly. The shading takes each clip by its
  weight of what the finer ones leave (`GiSampleAt`). Under a still camera with the clips moving 6 m through Sponza's
  arcade (`SPEEDO_GI_EYE_PATH`, 120 frames), the frames' means differ from the still run's by 0.1% at most.
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
  static voxels' is kept (GI Light Static, `ComputeGiLightStatic`, into `GiData::directFirst` of `gGiRadiance`) and
  made again when a clip is voxelized or the lights change (`Renderer::myGiLights`, compared bytewise: the environment's
  rotation turns its dominant lights, which are among them, and the sky's sun moves with the time of day). The part the
  lights don't change, the emitters' light and how far each voxel is open (`GiOpenAlong`), is kept apart
  (`GiData::keptFirst`) and taken as it is where the lights changed alone (`GiData::lightsOnly`): remade with them, the
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
  its cell), into `gGiRays` (rgba16f: radiance, distance in spacings). Where emissive surfaces aren't lights (View > Emissive Lights off), a probe's ray that meets an emitter nearer than `SPEEDO_GI_EMISSION_REACH` spacings (0.5; 0 for none:
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
  showed nothing yet. `SPEEDO_GI_DUMP=<x,y,z>` (or `pixel:<x>,<y>`: the point under a pixel, which `ComputeMain` unprojects into
  `GI_STATISTIC_DUMP_POINT` for the next frame's dump, `GiData::dumpPixel`: what found the entrance's probes) prints, at exit, per clip the probe nearest the point: its position,
  offset, age, backface count, its lattice point's voxel coordinates and the column of voxels through it, its depth
  map, the first rays' depths, its light along the axes, a walk straight down from it (`GiOccluderHitDebug`), the
  volumes at its voxel and along a line from the point into -z (`GiData::dumpPoint`: occupied, red along +x and
  the weight per voxel, which is what found the alternating cell): what found the dead floor probes and the probe
  beyond the end wall; and the surface cache entry of the point's voxel (`GI_DUMP_CACHE_FIRST`: its samples and inside
  rays, its rays' origin, the voxel's open side and normal, its light per face, and up to 8 of the frame's rays, their
  distance and light: what found the slot behind salle_de_bain's bench lit by the sun, see `GiOpenAlong`). View > Camera > Copy Pose (ctrl/cmd+shift+c)
  copies the view as the variables that reproduce it (`SPEEDO_CAMERA`, with split screen `SPEEDO_LAYOUT` and the other
  views' cameras, `SPEEDO_CAMERA_VIEWS='<pose>;...'` for views 1 and on, placed after `SPEEDO_CAMERA` places them all, or
  `SPEEDO_CAMERA_VIEWS=around` for views turned apart about the model, 360 degrees over the views, after the framing or
  `SPEEDO_CAMERA`: split screen for any model, as the regression runs it, `scripts/test-assets/splitscreen.txt`, `SPEEDO_AUTOLOAD_MODEL` (and `SPEEDO_GROUND_PLANE=0` where it stands on no ground plane), the environment's path,
  and the settings that aren't their defaults, `ui::ApplyPoseSettings`'s: the exposure, the tonemapper, the shadows,
  `SPEEDO_GI` and the indirect lighting's, the upscaling, the sky's time, `SPEEDO_DEBUG_VIEW`; then `# window <w>x<h>`,
  the framebuffer), for reports: an artifact is a pose first. View > Camera > Paste Pose (ctrl/cmd+shift+v;
  `SPEEDO_PASTE_POSE_FRAME=<n>` once n frames are drawn, for tests) applies such a text from the clipboard
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
  Its debug views (View > Debug View > Indirect Lighting, see "Debug views"; `SPEEDO_GI_DEBUG=<n>` is the n-th,
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

**Tracing the triangles** (the default since 2026-10-21, the user's choice; View > Indirect Lighting Rays switches between
the triangles and the voxels at run time, `Scene::Settings::traceTriangles`, `FrameInputs::traceTriangles`;
`SPEEDO_GI_TRACE=voxels` starts with the voxels, and View > Camera > Copy Pose writes it): a model loaded gets a BVH of what the voxels hold of it (`gfx::bvh::Build`, `gfx/bvh.h`: its static triangle lists that aren't
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
one behind, where the binary layout's test is right (`CheckBvh` counts those apart and warns). A hit's normal, side and flags are read once the traversal is done (`BvhFinish`): read at
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

**The surface cache** (View > Indirect Lighting Surface Cache, on by default since 2026-10-23, the user's call; `SPEEDO_GI_CACHE=0`
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
most `SPEEDO_GI_CACHE_SAMPLES` (1024; 32 where something moves), and at most half the frames since it started over
(at least 32; `SPEEDO_GI_TIME_BASED=0` without), tracing 4 rays a voxel for its first 256 frames
(`kGiCacheWarmFrameCount`, then `SPEEDO_GI_CACHE_RAYS`): its first frames' samples, read while the probes and the
bounce build up and overshoot, then fall out sooner (Sponza from a cold start within 10% of settled after 280 frames
rather than 840, 460 with one ray; 4 rays throughout, 240 but 4.8 ms a frame and noisier;
the tail, a mean of a light still settling, fades as 1 over the frames either way: Sponza's still changed 2% 2000 frames
in, see docs/performance-plan.md); a ray meeting a back face within a voxel and a half started inside something, and a
voxel with more than a quarter of those is no light (the probes' alone). The samples and those rays are kept over one
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

**The near field** (View > Indirect Lighting Near Field, on by default since 2026-10-24; `SPEEDO_GI_NEAR=0` to start without
it, `FrameInputs::giNearField`; `ginear.slang`, `GiData::near*`; where the rays trace the BVHs): where a solid rests on a
surface, the surface's voxels at the contact are also the solid's never drawn inside, which gets no light (see the
resolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the gap's own unlit voxels, so the
volume's lookups there found no light, in blocks a voxel wide (the user's CornellBox poses, the 11 models side by side:
`0.0062,-2.2486,1.2961,23.077,13.130`, black rectangles under the spheres; `0.1488,-1.1952,0.7514,12.998,14.697` at
resolution 192, red and black blocks where the sphere touches the green wall). Every 4th pixel each way of the
pre-pass's depth (View > Near Field Rays, `FrameInputs::giNearStride`, `SPEEDO_GI_NEAR_STRIDE`, `gi::kDefaultNearStride`:
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
contact) or, where it meets nothing, the volume's light along it from its
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
(`ComputeGiNearTemporal`; `SPEEDO_GI_NEAR_TEMPORAL=0` without it, View > Indirect Lighting > Near Field Over Frames),
at every pixel of the pre-pass's depth, upsamples that from the traced pixels (`GiNearUpsample`: bilinear over the 2x2
around, each by how much it is on the pixel's surface, as the emitters' light, with their fallbacks) and blends it into a
history reprojected by the pixel's point through the view's last view projection (`ViewData::previousViewProjection`,
`previousEyePosition`, which `Views::Upload` keeps from its last upload, w 0 where the view moved in the target, 2 where
the view didn't move; both frames' jitters taken out, `PixelHistoryAt`, see the emitters'), bilinear over the 4 pixels
around where it was, each only if its point was as far from the last eye as this one's and its normal within 25 degrees
(by the pixels' surfaces, `SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE`: RGBA32F, distance, packed normal and the frames
the history holds; within a
fifth of a percent and two pixels' depth along the surface), else the 3x3 around that are (`PixelHistory`), none for what moves (the
main depth's stencil), by 1 over `SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT` (16), a frame far brighter than what the history
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
contact (`FrameInputs::giNearContact`: View > Indirect Lighting Near Field Contact, `SPEEDO_GI_NEAR_CONTACT`, 2.5 voxels), the
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

View > Emissive Lights (on by default; `SPEEDO_EMISSIVE_LIGHTS=0` to start without, `FrameInputs::emissiveLights`,
Copy Pose writes it): the model's emissive triangles light the scene as area lights, at the first hit and in the
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
  emission is the frame's at the pixels, and the kept light's until the next voxelizing.
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

