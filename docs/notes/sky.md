# The sky

The default environment is a physically based model of Earth's atmosphere lit by the sun, with the moon and stars at night, plus aerial perspective and fog applied to the scene. The sun, moon and stars are placed from the time of day, latitude and date. Sky tables are computed by gpu kernels (`gfx::EnvironmentFilter`) and sampled by the backdrop, reflections and lighting. Shared definitions are in `sky.h` (used by `environment.slang` and the main layout) and `capi.h` (`SHADER_TYPES_ATMOSPHERE_*`).

## The sky: Earth's atmosphere and the time of day

The sky is the default environment (View > Environment > Physical Sky...; used when `SPEEDO_AUTOLOAD_ENVIRONMENT` is unset). It follows Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique": air (Rayleigh), aerosols (Mie, Cornette and Shanks phase function) and ozone absorption, with Bruneton's coefficients.

Tables and passes, all storage images made by `gfx::EnvironmentFilter`:

- **Transmittance** to the top of the atmosphere (Bruneton's mapping), and **multiple scattering** (Hillaire's isotropic approximation, serial loops because the tests run the kernels on the cpu). Both are made once.
- **Sky view** (`SHADER_TYPES_SKY_VIEW_TEXTURE`): the sky by angle from the zenith (rows concentrated near the horizon) and azimuth from the sun's, folded to 0 to 180 degrees since the sky is symmetric about the sun's vertical plane. The ground below the horizon is lit by the sun. Made at every redraw.
- **Panorama**: `EnvironmentSky` draws it from the sky view without the sun, and the usual prefilter runs on it as on a loaded file.
- **Backdrop and mirrors** sample the sky view (`SkyRadiance`) and add the sun's disk at its real angular radius, attenuated by the atmosphere (`EnvironmentData::skySunRadiance`).
- **Sky from the eye's altitude** (`ComputeSkyEyeView`, `SHADER_TYPES_SKY_EYE_VIEW_TEXTURE`): a sky view per split view at the eye's radius (`SkyViewUvFrom`, `SkyViewParametersFrom`), so the horizon dips correctly from high up. It is redrawn only when the altitude, sun or air changes.

### Settings and redraws

View > Environment > Physical Sky... holds Time of Day, Advance Time, Time Speed, Latitude, Day of Year, Year, Visibility, Aerial Perspective, Light Shafts and the fog settings. Variables: `SPEEDO_TIME_OF_DAY`, `SPEEDO_TIME_SPEED`, `SPEEDO_LATITUDE`, `SPEEDO_DAY_OF_YEAR`, `SPEEDO_YEAR`, `SPEEDO_VISIBILITY`, `SPEEDO_AERIAL_PERSPECTIVE`, `SPEEDO_LIGHT_SHAFTS`, `SPEEDO_FOG`, `SPEEDO_FOG_VISIBILITY`, `SPEEDO_FOG_HEIGHT`; Copy Scene writes them.

`Scene::UpdateSky` advances the time by real time and redraws the sky (fields, sky views, panorama, prefiltering) when the sun or moon has moved more than `kSkyRedrawAngle`. Between redraws only the sky fields are uploaded, every `kSkyUpdateAngle`, so disks and stars move smoothly.

### How clear the air is

Visibility (`SPEEDO_VISIBILITY=<km>`) is the meteorological visibility at the ground: Koschmieder's relation (a dark object keeps 2% contrast at the horizon, 3.912 over the extinction at 550 nm). It sets the aerosol density (`environment::SkyAerosols`, relative to Hillaire's at `kSkyDefaultVisibility`; none at `kSkyClearestVisibility`), carried in `EnvironmentData::skyAtmosphere` and applied by `SkyMediumAt` in the tables, sky view, sun transmittance and aerial perspective. A change redraws the sky and its lights.

### The sun

A directional light (`environment::SkySun`) of `SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE`, attenuated by `environment::SunTransmittance` (the same optical depth integrated in doubles on the cpu; `assettest` checks it against the table), zero below the horizon. `environment::SunDirection` places it: declination by day of year, hour angle by local solar time (15 degrees an hour from noon), +x east and -z north. Defaults: Paris, 21 June, 10:30.

### The moon, the stars and the night

- **Moon**: `environment::MoonAt` is a low precision ephemeris (leading terms of Schlyter and Meeus, from the days since J2000), with the hour angle from `LocalSiderealTime`; accurate to a degree or two. The year matters (`environment::CurrentYear` by default).
- **Light**: illuminance follows phase and elongation (`SHADER_TYPES_ATMOSPHERE_MOON_ILLUMINANCE`; `SHADER_TYPES_ATMOSPHERE_LUX` is a real lux). It is a directional light through the atmosphere (`environment::SkyMoon`) while the sun is below the horizon (`SkyLights`), with traced shadows. The air is lit by whichever is up (`FrameInputs::airLight`).
- **Sky**: `EnvironmentSkyView` draws a second sky view for the moon (`SHADER_TYPES_SKY_MOON_VIEW_TEXTURE`), added to the sun's in the panorama with a uniform night glow (`SHADER_TYPES_ATMOSPHERE_NIGHT_RADIANCE`: airglow and starlight).
- **Disk** (`SkyMoonDisk`): a Lambertian sphere lit by the sun, its mean radiance scaled by the lit share `(sin e - e cos e) / pi` for elongation e, so it reproduces the moon's illuminance.
- **Stars** (`SkyStars`): a hashed star field on the celestial sphere in an octahedral grid (`SHADER_TYPES_SKY_STAR_CELL_COUNT` cells a side), magnitudes -1.5 to 6.5 with more at fainter magnitudes, each a pixel wide Gaussian carrying its magnitude's illuminance. They are in the equatorial frame by latitude and local sidereal time, so the sky turns.
- **Exposure and vision**: auto exposure follows moonlight down (`kMoonlightStops`), and night vision is part of the look (`ViewData::scotopic`, `Scotopic` in `tonemap.slang`: a blue cast through the mesopic range, applied with the exposure before the tonemapper).

## Aerial perspective

`aerialperspective.slang` and `AerialPerspectiveData` (on by default with the sky; Light Shafts separately) compute the light the air scatters toward the eye and what it lets through, as in Hillaire's aerial perspective volume.

- **Volume**: per view, two RGBA16F 3D images (`gAerialPerspective`: in-scattering and colored transmittance) over the viewport, split views stacked along z. The first slice is at the eye, the second 1 m away, each later one a constant factor farther, to 200 km. `ComputeAerialPerspective` marches each cell's column through the sky's medium (`SkyScatteredLight`) with the eye at its altitude above the surface (the scene's lowest point, by the scene's unit), stopping where rays meet the surface (`groundReach`). A step's medium is the mean of the exponential layers over it (`SkyMediumAlong`).
- **Sun and shafts**: single scattering through the transmittance table; with shafts on, the sun is shadowed by its cascades, with steps jittered per cell by interleaved gradient noise that turns every frame. The nearer slices keep a history over frames (`gAerialPerspectiveHistory`, reprojected by `ViewData::previousViewProjection`, `kAerialPerspectiveHistoryBlend`) to average out the noise.
- **Multiple scattering**: from the indirect lighting's probes where they hold the air (`AerialPerspectiveAmbient`), else the sky's multiple scattering table.
- **Application**: `ComputeMain` applies the volume by depth to everything drawn (opaque color times transmittance plus light; blended layers through the air in front of them), except the backdrop (which has its own air), debug views and the voxel world. Glass (`ComputeTransmissionCopy`, `TransmittedLight`) and traced hits (`AerialPerspectiveBetween`) see the air too.

## Fog near the ground

Fog (on by default; `SPEEDO_FOG=0`, `SPEEDO_FOG_VISIBILITY=<km>`, `SPEEDO_FOG_HEIGHT=<m>`; `AerialPerspectiveData::fog`, `environment::kFogDefault*`) is a layer of haze and droplets above the sky's air, densest at the ground and falling off exponentially with height, defined by its ground visibility (Koschmieder's) and scale height.

- It is marched with the air in `ComputeAerialPerspective`: added to extinction and scattering, with its own phase (`SHADER_TYPES_FOG_G`). Sunlight reaches a point through the layer above it (`FogDepthToSky`, Chapman's function at the horizon).
- Where probes do not hold the air, it is lit by the environment's prefiltered radiance along the ray mixed with the sky's and ground's mean (`skyAround`), standing in for multiple scattering.
- The sky's tables do not include it: the backdrop and environment reflections take it in closed form per pixel (`FogToSky`, `FogDepthToGround`, `FogAround`).
- Lights' light comes through it too (`LightFog` in `LightFalloff`: the main pass, traced hits, scattering and the voxels' kept light, which is remade when the fog changes).
- The pass runs before Reflection Trace and the main passes. `FogHall.gltf` is a test scene for shafts in dense fog.
- Not yet: the environment's irradiance and emitters' light through fog, fog's own multiple scattering, fog in the sky's tables.

## Not yet

Clouds, spreading the time of day's redraw cost over frames, the moon's bounce in indirect lighting, the moon's sky from the eye's altitude, earthshine and the maria, a star catalogue and the Milky Way.
