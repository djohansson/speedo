# Research scenes plan: what the glTF research set needs of the renderer

The glTF research set (`resources/test-assets/gltf-research-scenes`, 28 scenes: Bitterli's Tungsten scenes and others,
converted from pbrt-v4) comes with what most test assets don't: each scene's `conversion.yaml` says how every pbrt
material and light was mapped to glTF and what was approximated or lost, and a path tracer's render of the converted
glTF (Blender Cycles; cornell-caustic's pbrt's) with the conditions it was made in (`reference_render`: the camera, the
size, the environment and its strength, the exposure multiplier, Khronos PBR Neutral). So the renderer can be held
against a ground truth of the very file it loads. `scripts/researchcompare.ps1` renders every scene as its reference was
made and sets each beside its reference, with imagecompare's measures.

The user's call, 2026-11-22: support what these scenes use as far as a real-time renderer can, aimed at 60 Hz on a wide
range of modern hardware with hardware ray tracing; this plan is reviewed before anything of it is built.

## What the scenes use

From every `conversion.yaml` and every file's glTF (the extended files; the core files leave the material extensions
out, so their glass is opaque and their surfaces take glTF's default specular):

| Feature | Scenes | The renderer |
|---|---|---|
| `KHR_materials_specular` (factor 0 on the converted diffuse and coated bases) | 27 | read |
| `KHR_materials_transmission`, `KHR_materials_ior` (pbrt's dielectrics) | 20 | traced refraction, sharp; fails through several surfaces (B1) |
| `KHR_materials_clearcoat` (pbrt's coateddiffuse, coatedconductor) | 18 | traced, glossy since c0f0359a |
| emissive meshes, `KHR_materials_emissive_strength` (pbrt's area lights) | 16 | lights the diffuse; no light-sampled highlights (C1) |
| an equirectangular panorama at a strength, or a uniform color (pbrt's infinite lights) | 11 | loaded by hand (A1), turned 90 degrees from the set's convention (A2) |
| no environment at all | 16 | the physical sky lights them unless turned off (A1) |
| `KHR_lights_punctual` directional (pbrt's distant) | 5 | read; the sky's sun beside it unless they align (A3) |
| `KHR_materials_volume` | 3 | absorbs by the thickness factor (B2) |
| `KHR_materials_dispersion` | 1 | read |
| rough transmission (roughness 0.04 to 0.32) | 4 | sharp, faded to the transmission texture's mips (C2) |
| caustics from an emissive surface (cornell-caustic's water, glass-of-water's table) | 2 | the caustic photons are the lights' alone (C3) |
| emission up to 541127 (cornell-caustic), 12048, 3040, 1000 | 4 | past the half float's 65504 in the main color (B4) |
| a camera, the first one used on load; nearest filtered textures; masks; double sided | all | read |
| a mirrored camera node (bmw-m6: its matrix a reflection) | 1 | the reflection dropped: the image mirrored (A4) |
| units: pbrt's, not meters (crown 200 across, bathroom2 39 by 39 by 77; bathroom, kitchen, living room in meters) | most | read as meters (C5) |

Lost in the conversion, and so in the references too: displacement (3 scenes), diffuse transmission (3: the renderer
has the extension, the converter wrote only metadata), crown's depth of field, exact spectral IORs, pbrt's bump mapping.

## The comparison

`scripts/researchcompare.ps1 -Preset <preset>`: each scene headless at its reference's size from the file's camera,
1500 frames, repeatable; the reference's panorama at its strength (a uniform color written as a panorama), or the
environment's intensity at 0 where it had none, so neither the sky nor its sun lights the scene; the panorama turned -90
degrees (the set's convention, which its notes state: the panorama's center column +X, three-quarter column +Z; the
renderer's center is -Z); a manual exposure of the multiplier (the renderer's 0 stops show a unit of radiance as white);
PBR Neutral; the client's ground plane, glare, motion blur and grain off; the temporal upscaler at native size for its
antialiasing. It writes a sheet per scene (`temp/review/research/<scene>.png`: the reference, the render, their
differences amplified 16 times), an overview, and a table, the scenes most unlike their references first: the mean
|log2| of the two luminances blurred over 8 pixels (the lighting, not its noise or edges) and the ratio of their means
(the light overall). Nothing fails on a measure: a path tracer's reference maps what is approximated, it isn't a
regression check. `-Only`, `-Set name=value` (an experiment's variables), `-Units scene=unit`, `-Kind core`.

## The target

60 Hz, 16.7 ms a frame, on the machine this is measured on (RTX 4070 Ti SUPER) at 2560x1440 with balanced upscaling
(the user's call, 2026-11-22: hardware scaling, the settings for slower GPUs, is revisited later). Each step below
states what it adds at that size, at the research scenes' reference poses and the performance plan's benchmark poses
(docs/performance-plan.md); a step over its budget is made cheaper or set aside, as the user decides.

## How it is judged (every step)

- **The comparison**: each scene's blurred measure before and after, every scene, not only those a step targets; no scene
  worse by more than the runs' noise (step 0 measures it), and the user's eye on the sheets.
- **The renderer's own checks**: the validated quick tier; the regression sets the step's area has (the GI's, the glass
  sample models for transmission, the traced reflections' scenes); `visualtest.ps1`.
- **Performance**: as the performance plan measures, warm pipelines, runs interleaved, serial.

## The gaps

### A. The scene's conditions (no rendering changes)

- **A1. The conditions travel with the file.** The environment (a panorama and its strength and turn, or none), the
  exposure, the unit and the ground plane are the comparison script's alone today; opening a scene in the client shows
  it under the physical sky at the profile's time of day, on a ground plane, auto exposed: what the user saw as
  oddities, partly. The user's call, 2026-11-22: the client reads the set's `conversion.yaml` beside the model (its
  `reference_render`: the panorama, beside the file or in the set's `common`, at its strength, or a uniform color, or
  none; the exposure multiplier as a manual exposure, the user's call too), and the ground plane is off. A reader of
  that block's flat keys alone, not a YAML library.
- **A2. The panorama's turn**: the set's convention (its notes: the panorama's center column +X, its three-quarter
  column +Z) is a turn of -90 degrees from the renderer's (center -Z). The user's call, 2026-11-22: the renderer's
  default changes to the set's, for every panorama.
- **A3. Two suns**: a model's directional light and the sky's sun both light the scene unless within `kMergeAngle` of
  each other (dragon and lamp under the sky). The user's call, 2026-11-22: a model whose lights play the outdoors' part
  (a directional light) or that brings its own panorama (A1) turns the physical sky off and is lit by its own: its
  panorama, else no environment.
- **A4. A mirrored camera**: a camera node whose matrix is a reflection (pbrt's `Scale -1 1 1`; bmw-m6) is rendered
  mirrored, as Blender and pbrt do (the source's own render agrees): the final pass flips the image across, the view and
  its passes unchanged. bmw-m6 is 1.49 stops off its reference for this alone.

### B. Bugs the comparison found

- **B1. Glass went black when traced** (the largest error in the set): glass-of-water's glass, water and ice were black
  silhouettes (2.906 stops off; without traced refractions, `SPEEDO_TRACED_REFRACTIONS=0`, nearly invisible instead,
  2.41), coffee-maker's carafe (0.511), spaceship's dome, crown's gems. Not the glass: a diagnostic of the paths' ends (red
  where a surface left nothing, green where they ran out of surfaces, blue where they met glass as opaque, yellow where
  they met nothing) showed a few red specks and the ice green; the black paths reached an ordinary opaque surface, lit
  dark. A traced hit took no light from the emissive surfaces (the notes' "not yet": the emissive surfaces' light at a
  hit), and these scenes are lit by emissive meshes: glass-of-water's backdrop by an emissive quad. So a traced hit
  takes it as the main pass's pixels do (step 2). Two more of the set's ways with glass show then:
  - **Thin walls** (open question 6): glTF has transmission without `KHR_materials_volume` as an infinitely thin wall,
    which bends nothing, and the renderer so; the set's dielectrics are pbrt's solids (the converter adds a volume only
    for absorption), and Cycles' importer refracts them as solids, so the reference's water and glass bend what is
    behind them and the renderer's don't.
  - **The emitters' light passes through glass**: their shadow rays meet no glass, as the punctual lights' don't, which
    take the glass back through the shadow transmittance; the emitters have none, so cornell-caustic's floor under its
    water takes its light's direct light with the boxes' hard shadows, where the reference has the water's caustics
    (0.495 stops off before step 2, 0.732 after: the direct light is right, its caustics missing; C3).
- **B2. Volume absorption by the thickness factor where the refraction is traced**: modern-hall's green glass panel is an
  opaque slab (its thickness factor 8.84 at an attenuation distance of 2 absorbs all but 1%; Cycles absorbs over the
  path's length inside the thin panel, which the factor doesn't describe). Where the refraction is traced, the
  absorption over the traced path's length inside the volume; the factor for the screen space lookup alone.
- **B3. A small panorama's mips**: spaceship's uniform panorama (64 by 32, the script's) made the validation layer
  report `mipLevels` past the complete chain.
- **B4. Emission past the half float's range**: cornell-caustic's light is 541127, the main color RGBA16F before exposure
  (65504 at most). Pre-exposed color (the exposure applied before the main color and the HDR histories store it, as the
  upscaler's pre-exposure already allows), and the emitters' sums kept in 32 bit floats; what breaks first is measured
  on cornell-caustic.
- **B5. The brightest light's highlight dark in its center** (veach-mis's smallest sphere, strength 3040, on the smooth
  plate): the glossy samples' cap (`kReflectionMostWhite`, by the light a pixel takes since a99997f2). Once the emitters'
  highlights are light sampled (C1), the cap is back to catching what a BRDF ray met by chance, and this goes with it.

### C. What the renderer approximates

- **C1. The area lights' highlights** (16 scenes lit by emissive meshes; crown's gold and gems, veach-mis's plates, every
  coat and metal under them). The emitters' samples light the diffuse alone (Emitter Sample and Shade: RIS over the
  light tree's picks, a shadow ray, every 4th pixel each way, filtered: irradiance); their highlights come only from the
  traced reflections' BRDF sampled rays (up to roughness 0.6): noisy for small lights, dim on rough surfaces, absent past
  0.6. veach-mis is the test made for exactly this (four plates of rising roughness under four spheres of falling size):
  0.89 stops off. The plan, the standard real-time answer (RTXDI's, pbrt's MIS):
  - **Glossy surfaces** (the traced ones, up to 0.6): the Reflection Trace pass adds a light sample to the pixel's glossy
    ray, an emitter point picked by the light tree and resampled (RIS) by the GGX lobe times its emission and geometry,
    with its shadow ray, the two weighed by the balance heuristic into the one sample the Reflection passes filter and
    accumulate as now. A light pick and a shadow ray per glossy pixel.
  - **Rough surfaces** (past 0.6, untraced): the emitters' existing samples carry their direction and light (shadowed),
    and the main pass evaluates the specular lobe for the nearest traced pixels' samples, as the diffuse takes their
    irradiance. The risk: a sample every 16 pixels for a view dependent lobe; measured on veach-mis's roughest plate.
  - Analytic area lights (linearly transformed cosines) would be exact for a few large polygons but don't scale to
    Bistro's twenty thousand emissive triangles; not planned.
  Budget: 1 ms.
- **C2. Rough glass**: a transmissive surface (drawn forward, after the visibility buffer) traces one sharp ray each for
  its reflection and its refraction, all of it below roughness 0.15, none from 0.4, the rest the transmission texture's
  mips and the probes: the floor's coat's error, for glass (bathroom2's rough glass and lamp's bulb at 0.32, spaceship's
  at 0.1, classroom2's at 0.04). The plan: the nearest transmissive surface a pixel records what the visibility buffer
  records for the opaque (its depth, normal, roughness, IOR), and its reflection and refraction are each one ray a pixel
  sampled from their GGX lobes (reflection as now; transmission by Walter et al. 2007), filtered and accumulated by the
  Reflection passes run over that layer with textures of their own; farther layers keep today's. Budget: 1 ms where
  glass fills the view.
- **C3. Caustics from emissive surfaces**: cornell-caustic's water surface focuses its light into caustics over the box
  (pbrt's SPPM reference); the renderer's caustic photons are the lights' (the punctual lights' and the sun's) through
  still glass, and this scene's light is an emissive quad (strength 541127): no caustics at all, 0.495 stops off, and the
  water surface a dark sheet as B1's glass. glass-of-water's table caustics are its backdrop emitter's too. The photons
  emitted from the emitters as well (picked by the light tree by power, from a point of a triangle, cosine weighted), as
  many as the lights' are today; measured after B1 and B4, which this scene also shows.
- **C4. Gems** (crown: dispersion, volumes, many internal reflections): after B1, measured; the renderer follows
  `kTracedReflectionGlassSurfaceCount` (2) surfaces through glass for a reflection and
  `SHADER_TYPES_CAUSTIC_SURFACE_COUNT` for a refraction.
- **C5. Units**: the set's units are pbrt's (crown 200 across); read as meters, the parts that go by meters (the GI
  filter's reach, the air, the camera's depth of field and motion blur) see a scene the wrong size. A1's file carries a
  unit where one is known; nothing in the comparison shows it yet (no air with the panoramas or without an environment,
  the effects off).
- **Outside this plan**: the indirect lighting's own approximations (the rooms are 0.14 to 0.35 stops off, many bounces
  in the references: 17 to 65), which the GI plans and the backlog keep.

## The steps

### 0. The baseline

Done, 2026-11-22 (Vulkan, RTX 4070 Ti SUPER, the extended files, 1500 frames, validated; `researchcompare.ps1` as
committed with this plan). The mean |log2| luminance against the reference, blurred over 8 pixels and sharp, and the
ratio of the means, in stops; what shows, and the gaps above that explain it:

| scene | blurred | sharp | ratio | what shows |
|---|---|---|---|---|
| glass-of-water | 2.906 | 2.998 | -1.283 | glass, water and ice black (B1); the table's caustics (C3) |
| bmw-m6 | 1.560 | 1.605 | -0.229 | mirrored (A4) |
| veach-mis | 0.889 | 0.928 | -0.474 | the plates' highlights (C1); the smallest light's dark center (B5) |
| crown | 0.718 | 1.063 | -0.627 | the gold dark (C1); the gems dark (B1, C4) |
| coffee-maker | 0.511 | 0.564 | -0.169 | the carafe dark (B1); the floor's reflection |
| cornell-caustic | 0.495 | 0.570 | -0.182 | no caustics (C3); the water a dark sheet (B1) |
| modern-hall | 0.398 | 0.441 | -0.227 | the green glass panel opaque (B2) |
| spaceship | 0.365 | 0.393 | -0.343 | the dome dark (B1); the metal darker (C1); the panorama's mips (B3) |
| classroom2 | 0.356 | 0.521 | -0.011 | |
| breakfast-room | 0.323 | 0.538 | -0.031 | |
| pontiac-gto | 0.319 | 0.359 | -0.209 | |
| living-room | 0.313 | 0.376 | -0.010 | |
| kitchen | 0.308 | 0.375 | -0.074 | |
| veach-bidir | 0.290 | 0.308 | -0.145 | |
| classroom | 0.287 | 0.406 | -0.085 | |
| bathroom | 0.266 | 0.424 | -0.068 | |
| bathroom2 | 0.253 | 0.288 | -0.122 | the rough glass (C2) |
| teapot-full | 0.251 | 0.304 | -0.034 | the tea in its glass (B1?) |
| lamp | 0.216 | 0.273 | -0.032 | the bulb (C2) |
| living-room-2 | 0.202 | 0.244 | +0.104 | |
| dragon | 0.198 | 0.352 | +0.025 | |
| veach-ajar | 0.185 | 0.197 | +0.112 | |
| vintage-car | 0.171 | 0.202 | -0.179 | |
| staircase | 0.142 | 0.225 | -0.057 | |
| house | 0.129 | 0.201 | -0.037 | |
| material-testball | 0.086 | 0.099 | -0.049 | |
| teapot | 0.081 | 0.086 | -0.061 | |
| cornell-box | 0.077 | 0.085 | -0.048 | |

The runs' noise: two runs of the same build agree to the third decimal on most scenes, 0.02 stops at most
(glass-of-water). Before the ground plane was turned off, dragon was 0.849 (a lit floor to the horizon where the
reference is black) and living-room 0.455; the cars and the teapots came out 0.01 to 0.05 closer with it, as if their
floors gained what it bounces: to look at in step 1. The unmarked rooms are within what the indirect lighting's
approximations give (outside this plan), and what their sheets show beyond that is looked at again once B1 and C1 are
done: most of them hold glass and coats under area lights.

### 1. The conditions with the file, the mirrored camera, the small panorama (A1 to A4, B3)

The settings file and its loading, the research set's files written from the notes, the mirrored camera, the panorama's
mips. Gate: the comparison takes the conditions from the files (the script's own variables gone) with the same
measures, bmw-m6 near its reference, the validation layer quiet on spaceship.

### 2. The emitters' light at traced hits (B1)

A traced hit's emitter sample (`TracedHitRadiance`: `EmitterCandidates` from `kTracedEmitterCandidateCount` candidates,
`EmitterShade`'s shadow ray, by the hit's brdf). Gate: glass-of-water, coffee-maker, spaceship, teapot-full and crown
toward their references; every scene else as before (cornell-caustic's regression is C3's to take back); Bistro at
night's reflections; the cost in the Reflection Trace pass and Main Transmissive.

### 3. Absorption over the traced path (B2)

Gate: modern-hall's panel; DragonAttenuation and the glass sample models as before.

### 4. The emitters' highlights on glossy surfaces (C1, glossy)

Gate: veach-mis, crown, the rooms' taps and mirrors; Bistro at night and the regression sets; the cost within budget;
the cap's dark center gone (B5).

### 5. The emitters' highlights on rough surfaces (C1, rough)

Gate: veach-mis's rough plates; the cost.

### 6. Glossy glass (C2)

Gate: bathroom2, lamp, spaceship, classroom2; the glass sample models; the cost where glass fills the view.

### 7. Emission's range (B4)

Gate: cornell-caustic without overflow; every scene and the regression sets the same to the noise.

### 8. Caustics from the emitters (C3)

Gate: cornell-caustic's and glass-of-water's caustics; the caustic scenes of the notes (CausticSpheres) as before; the
photons' cost with the emitters' added.

### 9. Gems, measured (C4)

What is left of crown's difference after B1, C1 and C3, and whether a step is worth it.

### 10. A few comparisons in CI

Scenes small enough for lavapipe's software ray tracing within CI's budget (cornell-box, material-testball: measured
there first), at a smaller size and fewer frames, with thresholds a few times what lavapipe measures against the
reference, so that they fail on a regression, not on what the renderer approximates.

## The user's calls (2026-11-22)

1. A1: the client reads the set's `conversion.yaml` itself, its exposure included; where the renderer's own convention
   differs from the set's (A2's panorama turn), the renderer's default changes to the set's.
2. The target: 60 Hz on this machine at 1440p balanced; slower hardware later.
3. A3: a model with outdoor lights or its own panorama turns the physical sky off.
4. CI: a couple of the comparisons, if they fit its time budget, chosen knowing lavapipe traces in software (step 10).
5. The plan is worked through without stopping between steps; commits as each step passes its gates, nothing pushed
   before the user approves.

6. Thin walls (B1): the set's dielectrics carry no `KHR_materials_volume`, which glTF reads as infinitely thin walls (no
   bending, the renderer's way and the Khronos viewer's), while Cycles' importer refracts them as solids, as pbrt
   rendered them. The user's call, 2026-11-22: solids where the set's conditions apply (`SPEEDO_SOLID_TRANSMISSION`).
## Progress

- 2026-11-22: the plan written; `scripts/researchcompare.ps1` added and the baseline taken; nothing else built.
- 2026-11-22, **step 1**: the panorama convention the set's (A2: material-testball 0.086 stops unturned, as it was
  turned -90 before; the environment cache's version bumped, its dominant lights' directions turned); a model's own
  conditions read from its `conversion.yaml` (A1: `ReadModelConditions`, uniform environments as `uniform:<r>,<g>,<b>`,
  none at 0; the comparison script sets none of them now, and its five scenes measured the same: material-testball
  0.086, teapot 0.081, dragon 0.198, cornell-box 0.076, spaceship 0.367); a model's directional light replaces the sky
  (A3, `SPEEDO_SKY_WITH_MODEL_LIGHTS=1` keeps it); a mirrored camera rendered mirrored (A4: bmw-m6 1.560 to 0.388);
  panoramas narrower than 256 scaled up (B3: spaceship's validation message gone). docs/notes/assets.md, "A model's own
  conditions"; docs/environment-variables.md.
- 2026-11-23, **step 2**: a traced hit takes the emitters' light (`TracedHitRadiance`): the diffuse as the screen
  gathered it where the hit is on the screen (`TracedEmitterSeen`), else as its voxel keeps it (`TracedEmitterKept`);
  the specular from a light sample (2 candidates, a shadow ray) where the hit's specular takes at least a tenth of the
  light. A light sample at every hit (the diffuse too) came closer but cost Bistro at night 3.1 ms a frame at 1440p (2.2
  with one candidate); the split costs 0.6 there, 0.1 in bathroom2, 2.2 in glass-of-water (its metals' highlights all
  sampled), nothing without emitters (Sponza). The comparison against step 1, every scene (blurred, stops; the light
  sample at every hit's in brackets): glass-of-water 2.906 to 1.056 (1.055), veach-mis 0.889 to 0.726 (0.639), crown
  0.718 to 0.594 (0.591), coffee-maker 0.511 to 0.319 (0.279), modern-hall 0.398 to 0.284 (0.277), spaceship 0.365 to
  0.260 (0.262), kitchen 0.308 to 0.278 (0.262), bathroom 0.266 to 0.242 (0.224), bathroom2 0.253 to 0.234 (0.223),
  veach-bidir 0.290 to 0.271, staircase 0.142 to 0.121, lamp 0.216 to 0.193; cornell-caustic 0.495 to 0.672 (its floor
  under the water lit directly, its caustics missing: C3). Transmission without a volume as a solid where the set's
  conditions apply (question 6; `TransmissionSolid`, `SPEEDO_SOLID_TRANSMISSION`): glass-of-water's water, ice and
  glass refract as the reference's do (its measure held up by its darker table), teapot-full 0.251 to 0.186; the cars'
  single sheet windows refracted too, vintage-car 0.171 to 0.197, pontiac-gto 0.320 to 0.330. If the user takes
  quality over the cost here, the light sample at every hit is a constant's change away.
- 2026-11-23, **step 3**: the traced view's paths absorb along their length inside a volume (`GlassPathStep`'s
  `alongPath`; the caustics' photons by the thickness still, whose shadows the user judged by it), the untraced lookup by
  the thickness: modern-hall's green panel clear (0.284 to 0.234), teapot-full's tea a little lighter (0.186 to 0.208,
  its nested interface volumes), the glass sample models within 0.26 of 255, the cost a distance and a power a segment.
- 2026-11-23, **step 4** (C1, glossy): a glossy reflection's emitter highlights by the balance heuristic between its
  ray and a light sample beside it (`TracedGlossyEmitters`: a pick down the light tree, its light by the lobe's density,
  a shadow ray, Russian roulette below a quarter of white in what the pixel takes), the light density a walk of the tree
  toward the point (`EmitterDensity`: a function of the point alone, so that the weights sum to one, with no lookup from
  a hit to its emitter); the cap left to what the ray met besides the emitters, which takes B5 with it (the smallest
  sphere's highlight on the smooth plate whole). veach-mis 0.726 to 0.458 (light -0.441 to -0.209), glass-of-water 1.055
  to 0.739, bathroom2 0.234 to 0.225, kitchen 0.276 to 0.271, the rest within 0.006; D3D12 the same (veach-mis 0.464).
  The cost in Reflection Trace at 1440p balanced: Bistro at night 0.91 ms (the frame 1.06), glass-of-water 0.43,
  bathroom2 0.11, Sponza 0.02 (docs/notes/rendering.md; one candidate, the resampling from two measured the same for
  0.17 ms more). Left: the roughest plate (step 5),
  crown's gold (0.591; step 9 after 5 and 8).
- 2026-11-23, **step 5** (C1, rough): the untraced specular (its share past 0.45, all of it past 0.6, and rough
  surfaces where nothing is traced) takes the emitters' highlights from the points the four traced pixels around found
  reaching them (`EmitterSpecular`: the plan's way, each weighed by the pixel's lobe), over frames as the diffuse's
  (`EmitterSpecularOverFrames`: a frame's alone was a grain over rough metal). veach-mis 0.458 to 0.421 (its roughest
  plate's highlights, light -0.209 to -0.171), crown 0.591 to 0.581, living-room-2 0.212 to 0.204, classroom2 0.356 to
  0.351, the rest the same; frame to frame as still as before. The cost at 1440p balanced: Bistro at night 0.3 ms (Main
  Visibility 0.22, Emitter Temporal 0.07), bathroom2 0.21; with step 4's, C1 costs Bistro at night 1.2 ms, over the
  plan's 1 ms by a fifth (docs/notes/rendering.md for what each part cost).
- 2026-11-23, found on the way (veach-mis nearer its reference at 8 frames than at 1500): the histories that average
  256 frames while the view is still (the emitters' light's, the near field's, the highlights') sank at half precision,
  their small steps up rounded away and their rare large steps down kept; 32 bit since. Against step 5, most scenes
  nearer their references: veach-mis 0.421 to 0.290, crown 0.581 to 0.491, modern-hall 0.235 to 0.177, spaceship
  0.257 to 0.202, pontiac-gto 0.330 to 0.276, kitchen 0.273 to 0.242, bathroom2 0.225 to 0.199, cornell-box 0.076 to
  0.064; some past them, brighter, what the darkening had hidden: coffee-maker 0.319 to 0.411 (its backdrop),
  living-room-2 0.204 to 0.242, veach-ajar 0.179 to 0.197, staircase 0.121 to 0.131. 0.08 ms a frame in Bistro at night.
- 2026-11-23, **step 9** (C4, measured): crown 0.718 stops (blurred) at the baseline, 0.581 after steps 2 to 5, 0.491
  with the 32 bit histories; its light -0.44 stops still. Its gold is a mirror in pbrt as in the glTF (alpha 0, gold's
  measured eta and k), lit by six emissive panels (one behind it facing away, as pbrt has it, the rest toward it), and
  no environment: the reference's band shines where speedo's mirrors the velvet's red and the dark, which is the gold's
  reflections of itself and of the panels over many bounces (Cycles' 100), where a traced hit takes the probes' light
  and one emitter sample. Not a step of its own in real time; C3's caustics through its gems are next to measure.
  Steps 6 (C2) and 8 (C3) wait on the user's calls on their designs (see the report of 2026-11-23).
- 2026-11-23, **step 7** (B4): nothing to build: the main pass already clamps its color to 65000 (`MainColor`), short of
  the half float's 65504, and cornell-caustic's light (541127) drew without infinities or NaNs, glare and balanced
  upscaling on included.
- 2026-11-23, **step 10**: two research checks in `visualtest.ps1`, which CI runs on lavapipe (its software ray queries
  included): cornell-box (its emitter and the indirect lighting's bounces, 256x256, 256 frames) and material-testball (a
  panorama, the coat and the metals, 320x180, 64 frames), each against its reference scaled to the capture's size
  (`imagecompare --fit`: a png reference, area averaged in linear light). Their files come from the set's repository one
  by one (`research-files.txt`, 14 MB at the release's commit, GitHub's raw downloads serving the LFS files), which the
  quick tier names, so CI's `-Minimal` fetch has them without the 1.1 GB archive. Measured (blurred stops, light):
  cornell-box on lavapipe 0.108, -0.063, 103 s warm; on the gpu Vulkan 0.115, -0.069, D3D12 0.118, -0.074;
  material-testball on lavapipe 0.074, -0.036, 53 s; on the gpu 0.076, -0.039 both. The thresholds hold them about where
  they are: cornell-box 0.16 blurred and -0.07 ± 0.08 of light, material-testball 0.11 and -0.04 ± 0.06. Cornell-box
  needs its frames, the indirect lighting settling: 0.29 at 64 frames, 0.20 at 128, 0.10 at 512.
