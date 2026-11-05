# The sky

A note of CLAUDE.md (its index names each): Earth's atmosphere, the sun, the moon and the stars, the air and the fog.

## The sky: Earth's atmosphere and the time of day

The default environment (View > Environment > Physical sky; without `SPEEDO_AUTOLOAD_ENVIRONMENT`) is Earth's atmosphere
lit by the sun, after Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique") and his
reference implementation: air (Rayleigh), aerosols (Mie, Cornette and Shanks' phase) and ozone, with Bruneton's
coefficients in km (`SHADER_TYPES_ATMOSPHERE_*` in `capi.h`), in `sky.h`, which `environment.slang` and the main layout
share. It replaced a stylized gradient whose sun was fixed.

- **Tables** (`gfx::EnvironmentFilter`'s, storage images in the general layout): the transmittance to the top of the
  atmosphere (256x64, Bruneton's mapping by the distance to the top and to the horizon, 40 steps) and the light scattered
  twice or more (32x32, Hillaire's isotropic approximation: 64 directions of 20 steps, serially, since the tests run the
  kernels on the cpu without shared memory), made once; the sky's view of itself (256x128, `SHADER_TYPES_SKY_VIEW_TEXTURE`,
  slot 37: by the angle from the zenith, the rows closer together near the horizon, and the azimuth from the sun's, 0 to
  180 degrees, since the sky is symmetric about the sun's vertical plane; 32 steps, the ground below the horizon at
  albedo 0.3 lit by the sun), made at every redraw. `EnvironmentSky` draws the panorama (256 wide, without the sun) from
  it, and the prefilter runs on that as on a file's. The backdrop and mirror reflections sample the view (`SkyRadiance`)
  and add the sun's disk at the real radius (0.27 degrees) through the atmosphere (`EnvironmentData::skySunRadiance`, from
  the cpu).
- **The sun** is a directional light (`environment::SkySun`): `SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE` (2.6, white, in
  the renderer's lux, so that the default sun gives the 2.2 of the sky before) through the atmosphere
  (`environment::SunTransmittance`: the same optical depth in doubles, 256 steps, within half a percent of the table, which
  `assettest` checks), none below the horizon. Where the time of day puts it (`environment::SunDirection`): the
  declination by the day of the year, the hour angle by local solar time (15 degrees an hour from noon), +x east and -z
  north; the defaults Paris (Bistro's), the 21st of June, 10:30: 59 degrees up in the south east, where the fixed sun was.
  View > Environment (with the sky installed): Time of Day (shown as hours and minutes, typed as hours), Advance Time and
  Time Speed (a multiple of real time, 60 an hour a minute), Latitude, Day of Year and Year (the moon's place and phase go
  by it; this year's by default, `environment::CurrentYear`, the user's call); `SPEEDO_TIME_OF_DAY` (hours or
  hours:minutes), `SPEEDO_TIME_SPEED` (nonzero advances), `SPEEDO_LATITUDE`, `SPEEDO_DAY_OF_YEAR`, `SPEEDO_YEAR`; Copy
  Pose writes them (the year always: its default moves).
- **The moon, the stars and the night** (2026-11-05): the moon where a low precision ephemeris puts it
  (`environment::MoonAt`: Schlyter's and Meeus's leading terms, the moon's mean longitude, anomaly and node and the sun's
  longitude by the days since J2000 through the calendar's leap days, the hour angle by the local sidereal time
  (`LocalSiderealTime`: the sun's hour angle plus its right ascension), within a degree or two: the full moon of
  2026-11-24 comes out 3 hours late), its illuminance at the top of the atmosphere by its phase
  (`SHADER_TYPES_ATMOSPHERE_MOON_ILLUMINANCE`, 0.26 lux full, in the renderer's units, `SHADER_TYPES_ATMOSPHERE_LUX` a
  real lux; Allen's magnitudes by the phase angle), its elongation from the sun. It is a directional light through the
  atmosphere (`environment::SkyMoon`, as the sun) while the sun is below the horizon (`SkyLights`: by day its light is a
  millionth of the sun's, not worth its shadow views and traced rays), so a moonlit scene has the moon's shadows, traced,
  and the voxels' kept light; the air is lit by it too (`FrameInputs::airLight`, `AerialPerspectiveData::sun`: the sun
  while above the horizon, else the moon, and the shafts follow). Its sky: `EnvironmentSkyView` draws the sky's view of
  itself a second time for the moon (`EnvironmentFilterConstants::light`, `gSkyMoonView`,
  `SHADER_TYPES_SKY_MOON_VIEW_TEXTURE`, the Sky Moon View table), which the panorama adds to the sun's
  (`EnvironmentSky`) with the night's glow (`SHADER_TYPES_ATMOSPHERE_NIGHT_RADIANCE`: airglow and starlight, a dark
  rural sky's quarter of a millicandela, uniform; `EnvironmentData::skyMoon`, `skyMoonRadiance`, `skyNight`), and the
  backdrop and mirrors (`SkyRadiance`) add the moon's disk on its phase (`SkyMoonDisk`: a Lambertian sphere lit by the
  sun, its disk's mean radiance over the share of a full sphere's light the phase shows, `(sin e - e cos e) / pi` of
  the elongation e, so the disk gives the moon's illuminance; the sun's size) and the stars (`SkyStars`: the celestial
  sphere's octahedral grid of `SHADER_TYPES_SKY_STAR_CELL_COUNT` cells a side, a cell in eighty holding a star by its
  hash, magnitudes drawn three times as many a magnitude from -1.5 to 6.5, some twelve thousand, each a Gaussian as wide
  as a pixel of the shading view carrying its magnitude's illuminance, 2.5 microlux at 0; in the equatorial frame by the
  latitude and the local sidereal time, so the sky turns through the night; the 3x3 cells about the direction, those past
  the map's edge folded back onto it). `UpdateSky` draws the sky again when the moon has moved by `kSkyRedrawAngle` too,
  and uploads the sky fields alone every `kSkyUpdateAngle` (0.005 degrees, a tenth of it) so the disks and the stars move
  smoothly between redraws (they stepped 3 pixels every 12 s at real time). Auto exposure follows the moonlight down
  (the user's call: see "Auto exposure" under "Asset import and testing", `kMoonlightStops`), and the eye's night vision
  is the scene's look there (`ViewData::scotopic`, `Scotopic` in `tonemap.slang`: the rods' luminance in a blue cast, by
  the scene's luminance through the mesopic range, 3 to 0.003 cd/m^2, whether or not the exposure follows; applied with
  the exposure before the tonemapper, in the composite and in Sharpen). Checked at the full moon of 2026-11-24, 1 am
  (`SPEEDO_DAY_OF_YEAR=328 SPEEDO_TIME_OF_DAY=1`): the moon's disk 52 degrees up in the south west, as the almanac has
  it, stars about it, DamagedHelmet's ground lit by it alone at 2^20 (97 to 173 of 255), Sponza's floor patches a dim
  blue gray at +20.8 EV, its interior black: the indirect lighting's half floats flush moonlight's bounce (TODO.md).
  The cpu test kernels draw the sun's sky alone (no moon, `skyMoonRadiance`'s w 0, which skips its table).
- **Redraws**: the draw thread advances the time (`Scene::UpdateSky`, by the real time between frames) and draws the sky
  again where the sun or the moon has moved more than `kSkyRedrawAngle` (0.05 degrees: every 12 seconds at real time,
  every frame at 600x): the sky fields of `gEnvironment` alone (the irradiance is the filter's; they alone go up every
  `kSkyUpdateAngle`, a tenth of it, see the moon's bullet), the view (the sun's and the moon's), the panorama and the
  prefiltering, into the installed environment, the filter's first barrier waiting for the frames still reading it, its
  resources' set pushed at each kernel (see "Descriptor sets": it took a set from the pool each, handed back once the
  gpu was done, until 2026-11-05; 3000 frames at 3600x, a redraw each, had run the pool out of storage images within
  minutes before that), in one command buffer with the fields' update. What a time lapse costs (2026-11-05, Bistro's street at 600x, 2560x1440, KosmicKrisp,
  the draw thread's frame time; the sun moves 0.16 degrees a frame, a redraw every frame): with the indirect lighting
  off 37.2 ms still against 45.9 moving (47.4 before), with it 52.3 against 62.0 (63.7 to 64.0 before). Of the 8.7 the
  sun's cascades are 5.6 (the shadow cache keeps them while the sun is still, and at 600x a far shadow moves texels a
  frame: real work), and the redraw itself about 2: its kernels' work is small (the sky's views, the panorama, the
  pyramid, the levels, the sheen levels, the irradiance: 2.1 ms with a barrier after each, 1.2 now), and on KosmicKrisp
  each barrier ends the compute encoder, about a tenth of a millisecond, so the kernels that read the pyramid alone (the
  levels, the sheen levels, the irradiance's rows) have no barrier between them, and the irradiance is projected a row
  at a time (`EnvironmentIrradianceRows`: one thread per coefficient over the mip's 8192 texels was 0.85 ms of it,
  measured by elimination, since KosmicKrisp's compute timestamps read 0 but the first; the filter's own gpu scope,
  "Environment Filter", times it where a backend can). The static voxels' relight is spread over frames (see "Lighting
  the voxels", `kGiRelightPhaseCount`). Left: the pyramid's five levels from level 0 in one go (four barriers), the
  sheen levels only where a material has sheen, and the cascades drawn at their texels' pace (a far cascade's texel is
  30 cm, which a 50 m building's shadow crosses in a frame at 600x: nothing to skip).
- **What it looks like**: a deep blue zenith at noon ((0.021, 0.043, 0.096)), the sky 8% of a horizontal surface's light
  at noon (a very clear day's: Hillaire's aerosols are few; hazier skies give 10 to 30%), so shade is about 3 stops below
  sunlight, where the gradient gave half of it; a red sun near the horizon ((1.08, 0.36, 0.05) at 3 degrees up), a pink
  sky after sunset and the blue hour purple (the ozone), and at midnight in June at Paris (the sun 17.7 degrees down) a
  sky 4e-8 of noon's: night is black, lit by the emissive surfaces (see "Emissive surfaces as lights").
- **Tests**: `assettest`'s sky check draws it with the kernels on the cpu, checks its panorama with the sun added as any
  environment (the sun the one dominant light, where and as bright as put), the table against the cpu's transmittance at
  90, 30, 10 and 3 degrees up, the zenith blue at noon, the sky's share of the noon light (5 to 40%), the sun red at 3
  degrees up, and midnight dark.
- **How clear the air is** (View > Environment > Visibility, `SPEEDO_VISIBILITY=<km>`, which Copy Pose writes): the
  meteorological visibility at the ground (where a dark object against the horizon keeps 2% of its contrast: Koschmieder's
  3.912 over the extinction at 550 nm, the green channel's), which sets the aerosols' density (`environment::SkyAerosols`,
  times Hillaire's: 1 at `kSkyDefaultVisibility`, 217 km, a very clear day; none from `kSkyClearestVisibility`, 288 km,
  the air alone; 41 at 20 km, a hazy day; 880 at 1 km, the least offered), in `EnvironmentData::skyAtmosphere`'s x, which
  `SkyMediumAt` scales the aerosols by everywhere: the tables (made again when it changes, `EnvironmentFilter::Record`'s
  `skyAerosols`), the sky's view of itself, the sun through it (`SunTransmittance`'s and `SkySun`'s, on the cpu) and the
  aerial perspective. A change draws the sky again (`UpdateSky`, as the sun's moving does), and its lights. Haze spread
  through the whole atmosphere (the aerosols' scale height is 1.2 km) blocks the sun as it thickens: at 1 km the sunlit
  patches on Sponza's floor are gone, lit by gray skylight alone.
- **Aerial perspective** (View > Environment > Aerial Perspective, on by default where the environment is the sky,
  `SPEEDO_AERIAL_PERSPECTIVE=0`; Light Shafts, `SPEEDO_LIGHT_SHAFTS=0`; both written by Copy Pose; `aerialperspective.slang`,
  `AerialPerspectiveData`): the light the air scatters toward the eye between it and what it sees, and what it lets through
  of that, as Hillaire's aerial perspective volume. Per view a volume over its viewport (`gAerialPerspective`: two RGBA16F 3D
  images, the in-scattering and the transmittance, colored, 160x90 cells by 64 slices, every split view's stacked along z,
  59 MB, and a history over frames of the first 40 slices' light, 37 MB, made each frame by the Aerial Perspective pass, `ComputeAerialPerspective`, a thread per cell's column): the first
  slice at the eye, the second 1 m away and each after a constant factor farther, to 200 km at the last (fine near the eye
  for the shafts, kilometres apart in the haze), each marched in 2 steps through the sky's medium as the sky's view is
  (`SkyScatteredLight`'s energy conserving step), the eye at its altitude over the planet's surface (the scene's lowest
  point, by the scene's unit in meters, View > Units, see "The scene's unit": 0.01 for Bistro's centimetres), as far as
  the surface where the ray meets it (`groundReach`, the step cut there, the slices
  past it holding what reached the eye from the surface: marched on through the planet, a cell's ray below the horizon
  crossed 200 km of air at the surface's density, denser by the exponential layers' negative altitudes, with no sun,
  the planet in the way of every point under its surface, a saturated dark fog that the sky's last rows took bilinearly
  from the cells below the horizon and the ground plane's last rows, farther than the surface, whole: a dark band over
  the plane's horizon, 2026-11-05). The sun's single scattering by each phase function through the transmittance table, shadowed
  by the sun's cascades (its light among `gLights`, the sky's own or a model's merged with it: `AerialPerspectiveData::
  shaftLight`; a bilinear comparison in the finest cascade holding the point, lit where none does, only as far along the
  ray as the scene goes, `sceneDepths`) where the shafts are on, the steps offset per cell by interleaved gradient noise,
  and turned every frame by the golden ratio: each of the first 40 slices (to 2 km, the scene's reach, where the
  shadows and the probes are) keeps its light per meter over frames (`gAerialPerspectiveHistory`, two of them by the
  frame's parity, `AerialPerspectiveData::history*`; per meter, since the last frame's slices there are of other
  lengths), the last frame's taken from where the slice's middle was in its view (`ViewData::previousViewProjection`
  and `previousEyePosition`, where the view was at this place then; none where it falls behind that eye, out of its
  view or past its 40 slices) and kept by 0.9 (`kAerialPerspectiveHistoryBlend`); the fog's share of each slice's light
  rides in its alpha, for the fog's slice toward the sky. The same offsets every frame and 4 steps a slice aliased against
  the shadows' edges in dense fog (dotted arcs across FogHall's sheets, 2 m steps at 38 m against sheets 1.7 m thick),
  which the frames' different samples average out; the history costs nothing measurable (Sponza at the user's shadow
  pose, 2560x1440, KosmicKrisp: 31.6 and 31.5 ms against 31.4 and 30.8 with the pass off, interleaved, where the pass
  cost 0.6 to 0.7 before it). Finer cells were tried with it, 240x135: about a millisecond more there, for cells of 10.7
  pixels rather than 16 (the probes' ambient, read every other slice since, the history's samples and the slices' stores
  all scale with them), and kept out; a shaft's edge stays as soft as a cell, and a cell's ray past a near silhouette
  still lends the pixels beside it its integral. The light scattered more than once: the indirect lighting's around the air where its probes hold
  it (`AerialPerspectiveAmbient`, a slice's middle: the 8 probes around in each clip, blended as `GiSampleAt` blends them,
  each the mean of its faces, those that traced and aren't inside something alone: the volumes are only resolved near
  surfaces), else the sky's multiple scattering table, as under an open sky: by the table alone the air in Sponza's roofed
  corridors glowed with skylight it has none of (the user: "a bit bright"; at 100 m a unit, a corridor's pixel 19,43,82
  of 255, now 14,6,5). `ComputeMain` applies it by the main depth's distance (the volume sampled trilinearly, each view
  within its own slices), to everything drawn: an opaque pixel's color times the transmittance plus the light, each blended
  layer's light through the air in front of it (`layer T + alpha S`, by its depth from the transparency's sort key); not
  the backdrop (the sky has its air), the debug views or the voxel world. At 1 m a unit it is what clear air does at eye
  height: the ground plane's last pixels below the horizon (36 km of air at 1000 m a unit takes the ground's red to 76%,
  its blue to a third, and adds the horizon's light), a faint warmth down Sponza's corridors; shafts at a building's scale
  want a denser layer near the ground (TODO.md). Cost by the draw thread's frame time on KosmicKrisp at Sponza (2560x1440,
  GI on, the user's shadow pose): 0.6 to 0.7 ms (30.54 and 31.63 ms against 30.00 and 30.91, interleaved; by the sky's
  term alone, before the probes' light, within the runs' noise).
- **Fog near the ground** (View > Environment > Fog, on by default, the user's call, 2026-11-05; `SPEEDO_FOG=0`; Fog
  Visibility and Fog Height, `SPEEDO_FOG_VISIBILITY=<km>` and `SPEEDO_FOG_HEIGHT=<m>`, all written by Copy Pose;
  `AerialPerspectiveData::fog`, `environment::kFogDefault*`): a layer of haze and droplets on top of the sky's air, densest
  at the ground and falling off exponentially with the height, by its visibility at the ground (Koschmieder's, as the
  sky's) and its scale height: a clear day's by default, 30 km in a layer of 200 m (an optical depth of 0.03 up through
  it, a clean day's aerosols: the far ground and the horizon's lowest degree or two haze over, the sky above and the sun
  stay clear; 50 m drew the horizon a line a pixel thick). It is marched with the air (`ComputeAerialPerspective`: added to
  the extinction and the scattering, by a phase of its own, `SHADER_TYPES_FOG_G`, 0.6, haze's, broader than the aerosols'
  0.8; the sun's light reaching a point comes through the layer above it, `FogDepthToSky`, Chapman's at the horizon),
  and lit, where the probes don't hold the air, by the environment's radiance along the ray, its second prefiltered
  level (roughness 0.2, which resolves the horizon's bright band), for three quarters and the sky's and the ground's
  mean for the rest (`skyAround`: by the clear air's multiple scattering table, or by the mean alone, a fog along the
  ground darkened the horizon a tenth to a third below the clear sky's, a gray line between the sky and the lit ground,
  the air's phase scattering a high sun sideways four times as much as the fog's; prefiltered to about the phase's
  width, roughness 0.5, the band was blurred into the darker sky above and the ground below, and the fog's horizon came
  out at half the clear sky's; a thin layer's light scattered twice or more, which the single scattering leaves out, is
  what makes a real haze's horizon as bright as the clear sky's or brighter). The sky's tables don't have it (it is far
  thinner than their rows): the backdrop takes it in closed form per pixel (`FogToSky`, below; until 2026-11-05 the
  volume's slice of the fog alone along the whole ray, whose cells, 16 pixels apart, straddle the horizon, so the sky's
  last rows took half of the cells below it, through the planet then: with the dark band above, the ground plane's
  horizon was a harsh edge, the user's words), so the sky seen through it hazes and the sun dims. Glass and the traced rays see
  through the air too (2026-11-05): the transmission copy has the air from the eye to the opaque scene at its depth
  (`ComputeTransmissionCopy`), of which the glass takes the eye's stretch to itself out again by its own distance
  (`TransmittedLight`: ComputeMain applies that stretch to the glass's fragment), leaving the air between the glass and
  what it shows; a traced hit's radiance comes through the air between the surface and the hit as if the hit lay along
  the eye's ray that much farther (`AerialPerspectiveBetween`: the volume's integrals from the eye, at the shading's
  pixel; the air is nearly the same within the scene's reach, the shadows and the probes aside); and what rays,
  refractions and the shading's environment reflection show of the sky comes through the fog toward it in closed form
  (`FogToSky`: the depth through the layer that way, Chapman's at the horizon, rising, and falling the layer below the
  eye along the way to the surface, no longer than the surface's horizon from the eye, `FogDepthToGround`: the
  environment's ground is the surface, and a ray toward it used to take the whole layer above the eye, as toward the
  sky, so the sky's own ground just below the horizon, seen where there is no ground plane, hazed as through 200 m of
  fog at 20 m off; and the fog's source over it, the sun's light by the phase and the light around, `FogAround`; the
  shafts in the fog against the sky, which the volume's slice had, are 0.03 deep at the default fog and went with it).
  The Aerial Perspective pass runs before Reflection Trace and the main passes for it, and the Transmission
  pass reads the depth. MetalRoughSpheresNoTextures' mirror spheres in dense fog reflect the fogged horizon as the sky
  behind them is (1.3% of the pixels off by more than 16 of 255 against before); the shading's fog toward the sky costs
  within the runs' noise (Sponza 32.9 and 33.2 ms against 32.3 with the pass off). The lights' light comes through
  it too (`LightFog`, in `LightFalloff`: a directional light's through the layer above the point, a point or spot light's
  along the way from it; the main pass's, the traced hits', the scattering's and the voxels' kept light, which is made
  again when the fog changes, as when the lights do, and not gradually, which the surface cache would follow), so a thick
  fog takes the sun away before it reaches the ground (0.2 km of visibility in a 100 m layer lets a tenth through).
  Light shafts: `FogHall.gltf` (see "Asset import and testing") in dense fog, `SPEEDO_FOG_VISIBILITY=0.3
  SPEEDO_FOG_HEIGHT=30`, the sheets of sunlight through its roof's slots seen from the side as bands at the sun's slope
  across the view, bright at their edges nearest the view's diagonal, where the view runs along a sheet for the hall's
  width, over the strips they make on the floor. Only from the side: looking at the sun through the slots, the sheets fell
  on the slots themselves (bright sky either way) and the roof's underside alone showed the haze, a glow about the sun;
  and a hall a third open to the sky had its air, lit by its sky and its sunlit floor, as bright as the sun's light
  scattered sideways (a tenth of what the fog scatters forward). The march's steps aliased against the sheets' edges (2 m
  steps at 38 m, sheets 1.7 m thick), dotted arcs across the bands, until the volume's history over frames (above:
  the steps' offsets turn every frame). The default fog costs
  nothing measurable (Sponza at the user's shadow pose, 2560x1440, KosmicKrisp: 31.57 and 31.64 ms a frame against 31.61
  and 31.66 without, interleaved). Not yet: the environment's irradiance on surfaces (the sky's light reaches them through
  no fog), the emitters' light through it, the fog's own multiple scattering (a thick bank should glow nearly as white as
  a sunlit wall), and the fog in the sky's tables.
- **The sky from the camera's altitude** (2026-11-05): the backdrop and the mirrors sample the sky's view of itself from
  each split view's eye at its altitude (`ComputeSkyEyeView`, the Sky Eye View pass: a table per view as the
  environment's, `SkyViewUvFrom` and `SkyViewParametersFrom` mapping at the eye's radius, the views' stacked along y in
  one image, `SHADER_TYPES_SKY_EYE_VIEW_TEXTURE`; `SkyRadiance` takes the shading view's, `gShadingViewId`, which
  ComputeMain's backdrop sets), made again for a view when its eye's altitude moved by a hundredth of itself or a metre, or
  the sun or the air changed (`AerialPerspectiveData::skyEyeViewRedraw`, `FrameInputs::sky` and `skyAerosols`), so a
  still camera costs nothing; the environment's own table, from half a metre up, lights the scene as before, and the
  sun's disk is its. From 5 km up the sky's horizon dips the 2.3 degrees the ground's does, with the air below it as the
  aerial perspective has it over the ground. Found there: the backdrop's ray (and the volume's cells') was the normalized
  difference of the near plane's point and one twice as far, each unprojected and divided, which the positions' float
  error over their few centimetres turned by a thousandth of a radian with the eye 500 m from the origin (Bistro's camera
  is 4000 cm out), stripes across the sky near the horizon by the row: `ViewRay` (common.slang) takes the homogeneous
  difference of the near plane's and the far's (at infinity, w 0), whose baseline is the whole frustum (the 500 m and
  5 km skies smooth, the ground's captures as before to 0.2 of 255). And the volume's march takes each step's medium as
  its exponential layers' mean between the step's ends' altitudes (`SkyMediumAlong`, `FogDepthAlong`), where a point's
  density missed the layers a far step kilometres long crosses.
- **Not yet** (TODO.md): clouds, the time of day's redraw cost spread over frames, the moon's bounce in the indirect
  lighting (its half floats), the moon's sky from the eye's altitude (the ground's table serves it), the earthshine and
  the maria, a star catalogue and the Milky Way.

