// the fragment stage: ShadeFragment, and the main, scatter, shadow, transparent and pre-pass entry points. a part of shaders.slang, which includes it after capi.h and sky.h

// the main pass's color attachment, which opaque draws write (blended ones go in the per pixel lists instead, see
// FragmentTransparent)
struct FragmentOutput
{
	float4 color : SV_Target;
};

// a cut out fragment's end, where there are fragments: the visibility buffer's shading, in compute, only shades what the
// pre-pass kept (see ComputeMainVisibility)
void CutOutFragment()
{
	__stage_switch
	{
	case fragment:
		discard;
	case compute:
		return;
	}
}

// a fragment's color and alpha, lit and with emissive added. for the scatter pass (scatterPass): the light entering a
// scattering material (ScatterLight), and its material + 1 in alpha
float4 ShadeFragment(VertexOutput input, bool isFrontFace, bool scatterPass = false)
{
	VertexP3fN3fTa4fT014fC4f v = input.vertex;

	uint materialId = input.materialId;

	MaterialData material = gMaterialData[materialId];

	// linear: the vertex colors hold the materials' diffuse colors, and textures are sampled through srgb formats
	float4 color = v.color * material.color;
	if (material.checker != 0.0)
		color.rgb *= GroundChecker(v.texCoord01.xy, material.checker);

	// cutouts (foliage, fences) keep their mask in the diffuse texture's alpha channel, or in a separate texture
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		float4 sample = SampleView(material.baseColorView, v.texCoord01);
		if (sample.a < material.alphaCutoff)
			CutOutFragment();

		color *= sample;
	}

	if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u && SampleView(material.alphaView, v.texCoord01).r < material.alphaCutoff)
		CutOutFragment();

	// a back face (only drawn for double sided materials) is lit as seen from behind, as gltf defines it: its normal,
	// normal mapped or not, is reversed
	float faceSign = isFrontFace ? 1.0 : -1.0;
	// lines and points without normals (zero ones, see mesh::Submesh::topology) are unlit: gltf draws them in their base
	// color plus emissive
	bool unlit = dot(v.normal, v.normal) == 0.0 || (material.flags & MATERIAL_FLAG_UNLIT) != 0u;
	float3 normal = unlit ? float3(0.0, 0.0, 1.0) : normalize(v.normal);
	float3 geometryNormal = faceSign * normal;
	if (!unlit && (material.flags & MATERIAL_FLAG_NORMAL_TEXTURE) != 0u)
		normal = NormalFromMap(material.normalView, material.normalScale, normal, v.tangent, v.position, v.texCoord01, faceSign);
	else
		normal = geometryNormal;

	// light the surface gives off, unaffected by the lighting
	float3 emissive = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emissive *= EmissiveTexel(SampleView(material.emissiveView, v.texCoord01).rgb);

	// the debug views (not the scatter pass's: what it draws is only seen lit)
	uint debugView = scatterPass ? DEBUG_VIEW_NONE : DebugView();

	// KHR_materials_unlit: the base color alone. lines and points without normals: plus emissive
	if (unlit)
	{
		if (scatterPass)
			return float4(0.0, 0.0, 0.0, float(materialId + 1u));
		bool unlitMaterial = (material.flags & MATERIAL_FLAG_UNLIT) != 0u;
		float3 unlitEmissive = unlitMaterial ? float3(0.0) : emissive;
		float3 value;
		if (DebugSurfaceValue(debugView, color.rgb, normal, geometryNormal, 1.0, 0.0, 1.0, materialId, v.texCoord01.xy, value))
			return float4(value, color.a);
		// the views of the light: what it shows is unaffected by the lighting, as emission is
		if (debugView != DEBUG_VIEW_NONE)
			return float4(debugView == DEBUG_VIEW_EMISSIVE ? color.rgb + unlitEmissive : float3(0.0), color.a);
		return float4(color.rgb + unlitEmissive, color.a);
	}

	float metallic = material.metallic;
	float roughness = material.roughness;
	if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
	{
		float2 sample = SampleView(material.metallicRoughnessView, v.texCoord01).rg;
		roughness *= sample.r;
		metallic *= sample.g;
	}

	// ambient occlusion only darkens the ambient light, the stand-in for indirect light, which is what gltf applies it to
	float occlusion = 1.0;
	if ((material.flags & MATERIAL_FLAG_OCCLUSION_TEXTURE) != 0u)
		occlusion = lerp(1.0, SampleView(material.occlusionView, v.texCoord01).r, material.emissive.w);

	// the specular (KHR_materials_specular and KHR_materials_ior, or specular-glossiness)
	float specular = material.specular;
	if ((material.flags & MATERIAL_FLAG_SPECULAR_TEXTURE) != 0u)
		specular *= SampleView(material.specularView, v.texCoord01).r;
	float4 specularColor = float4(material.specularColor.rgb, 1.0);
	if ((material.flags & MATERIAL_FLAG_SPECULAR_COLOR_TEXTURE) != 0u)
		specularColor *= SampleView(material.specularColorView, v.texCoord01);

	Surface surface;
	surface.occlusion = occlusion;
	surface.normal = normal;
	surface.geometryNormal = geometryNormal;

	// KHR_materials_clearcoat: its own normal map, else the geometry's normal (not the base's normal map)
	surface.clearcoat = 0.0;
	surface.clearcoatRoughness = 0.0;
	surface.clearcoatNormal = geometryNormal;
	if (Layer(material, MATERIAL_FLAG_CLEARCOAT))
	{
		surface.clearcoat = material.clearcoat.x;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_TEXTURE) != 0u)
			surface.clearcoat *= SampleView(material.clearcoatView, v.texCoord01).r;
		surface.clearcoatRoughness = material.clearcoat.y;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_ROUGHNESS_TEXTURE) != 0u)
			surface.clearcoatRoughness *= SampleView(material.clearcoatRoughnessView, v.texCoord01).r;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_NORMAL_TEXTURE) != 0u)
			surface.clearcoatNormal = NormalFromMap(
				material.clearcoatNormalView, material.clearcoat.z, normalize(v.normal), v.tangent, v.position, v.texCoord01, faceSign);
	}

	// KHR_materials_sheen
	surface.sheenColor = 0.0;
	surface.sheenRoughness = 0.0;
	if (Layer(material, MATERIAL_FLAG_SHEEN))
	{
		surface.sheenColor = material.sheen.rgb;
		if ((material.flags & MATERIAL_FLAG_SHEEN_COLOR_TEXTURE) != 0u)
			surface.sheenColor *= SampleView(material.sheenColorView, v.texCoord01).rgb;
		surface.sheenRoughness = material.sheen.a;
		if ((material.flags & MATERIAL_FLAG_SHEEN_ROUGHNESS_TEXTURE) != 0u)
			surface.sheenRoughness *= SampleView(material.sheenRoughnessView, v.texCoord01).r;
	}
	if ((material.flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS) != 0u)
	{
		// the specular color is f0, and the diffuse keeps what it leaves
		surface.f0 = specularColor.rgb;
		surface.f90 = 1.0;
		surface.diffuseColor = color.rgb * (1.0 - max(surface.f0.r, max(surface.f0.g, surface.f0.b)));
		surface.roughness = 1.0 - saturate(material.roughness * specularColor.a);
	}
	else
	{
		float ior = material.specularColor.a;
		float reflectance = (ior - 1.0) / (ior + 1.0);
		float3 dielectricF0 = min(reflectance * reflectance * specularColor.rgb, 1.0) * specular;
		surface.f0 = lerp(dielectricF0, color.rgb, metallic);
		surface.f90 = lerp(specular, 1.0, metallic);
		surface.diffuseColor = lerp(color.rgb, float3(0.0), metallic);
		surface.roughness = roughness;
	}

	uint viewId = gShadingViewId;
	float3 eye = gViewData[gPushConstants.frameIndex][viewId].eyePosition.xyz;
	float3 view = normalize(eye - v.position);

	// KHR_materials_transmission: that much of the dielectric's diffuse is the light through it instead (TransmittedLight,
	// once the rays are traced: see below)
	surface.transmitted = 0.0;
	float transmission = 0.0;
	float transmissionThickness = 0.0;
	float transmissionIor = 1.0;
	float transmissionSpread = 0.0; // KHR_materials_dispersion: each color refracted by its own ior, spread around it
	gShadingTransmissive = false;
	if (Layer(material, MATERIAL_FLAG_TRANSMISSION))
	{
		transmission = material.transmission.x;
		gShadingTransmissive = transmission > 0.0;
		if ((material.flags & MATERIAL_FLAG_TRANSMISSION_TEXTURE) != 0u)
			transmission *= SampleView(material.transmissionView, v.texCoord01).r;
		transmissionThickness = material.transmission.y;
		if ((material.flags & MATERIAL_FLAG_THICKNESS_TEXTURE) != 0u)
			transmissionThickness *= SampleView(material.thicknessView, v.texCoord01).r;
		transmissionThickness *= input.modelScale;
		transmissionIor = material.specularColor.a;
		transmissionSpread = (transmissionIor - 1.0) * 0.025 * material.transmission.w;
		surface.diffuseColor *= 1.0 - transmission;
	}

	// KHR_materials_anisotropy: the direction in the tangent frame (of the vertex tangents, else derived from the
	// texture's texcoords), along +u and up the image (glTF's bitangent, -v as sampled), turned by the rotation
	surface.anisotropy = 0.0;
	surface.anisotropyT = 0.0;
	surface.anisotropyB = 0.0;
	if (Layer(material, MATERIAL_FLAG_ANISOTROPY))
	{
		TextureView frameView;
		frameView.uTransform = float4(1.0, 0.0, 0.0, 0.0);
		frameView.vTransform = float4(0.0, 1.0, 0.0, 0.0);
		frameView.textureId = 0;
		frameView.samplerId = 0;
		frameView.texCoordSet = 0;
		frameView.flags = 0;
		float2 direction = float2(1.0, 0.0);
		float strength = material.anisotropy.x;
		if ((material.flags & MATERIAL_FLAG_ANISOTROPY_TEXTURE) != 0u)
		{
			frameView = gTextureViews[material.anisotropyView];
			float3 sample = SampleView(frameView, ViewTexCoord(frameView, v.texCoord01)).rgb;
			direction = sample.rg * 2.0 - 1.0;
			strength *= sample.b;
		}
		direction = float2(
			material.anisotropy.y * direction.x - material.anisotropy.z * direction.y,
			material.anisotropy.z * direction.x + material.anisotropy.y * direction.y);
		float3x3 frame = SurfaceFrame(normalize(v.normal), v.tangent, v.position, v.texCoord01, frameView, faceSign);
		float3 t = direction.x * frame[0] - direction.y * frame[1];
		t -= normal * dot(normal, t);
		if (dot(t, t) > 1e-8)
		{
			surface.anisotropy = saturate(strength);
			surface.anisotropyT = normalize(t);
			surface.anisotropyB = cross(normal, surface.anisotropyT);
		}
	}

	// KHR_materials_iridescence: the film's fresnel over the surface's f0, at the view's angle
	surface.iridescence = 0.0;
	surface.iridescenceFresnel = 0.0;
	if (Layer(material, MATERIAL_FLAG_IRIDESCENCE))
	{
		surface.iridescence = material.iridescence.x;
		if ((material.flags & MATERIAL_FLAG_IRIDESCENCE_TEXTURE) != 0u)
			surface.iridescence *= SampleView(material.iridescenceView, v.texCoord01).r;
		float thickness = material.iridescence.w;
		if ((material.flags & MATERIAL_FLAG_IRIDESCENCE_THICKNESS_TEXTURE) != 0u)
			thickness = lerp(material.iridescence.z, material.iridescence.w, SampleView(material.iridescenceThicknessView, v.texCoord01).r);
		surface.iridescenceFresnel =
			IridescenceFresnel(1.0, material.iridescence.y, max(dot(normal, view), 1e-4), thickness, surface.f0);
	}

	// KHR_materials_diffuse_transmission
	surface.diffuseTransmission = 0.0;
	surface.diffuseTransmissionColor = 0.0;
	if (Layer(material, MATERIAL_FLAG_DIFFUSE_TRANSMISSION))
	{
		surface.diffuseTransmission = material.diffuseTransmission.a;
		if ((material.flags & MATERIAL_FLAG_DIFFUSE_TRANSMISSION_TEXTURE) != 0u)
			surface.diffuseTransmission *= SampleView(material.diffuseTransmissionView, v.texCoord01).r;
		surface.diffuseTransmissionColor = material.diffuseTransmission.rgb;
		if ((material.flags & MATERIAL_FLAG_DIFFUSE_TRANSMISSION_COLOR_TEXTURE) != 0u)
			surface.diffuseTransmissionColor *= SampleView(material.diffuseTransmissionColorView, v.texCoord01).rgb;
	}
	// the light through the volume (KHR_materials_volume) from behind, attenuated over its thickness
	float3 throughVolume = 1.0;
	if (surface.diffuseTransmission > 0.0)
	{
		float thickness = material.transmission.y;
		if ((material.flags & MATERIAL_FLAG_THICKNESS_TEXTURE) != 0u)
			thickness *= SampleView(material.thicknessView, v.texCoord01).r;
		throughVolume = VolumeAttenuation(material.attenuationColor.rgb, material.transmission.z, thickness * input.modelScale);
	}

	// KHR_materials_retroreflection
	surface.retroreflection = 0.0;
	if (Layer(material, MATERIAL_FLAG_RETROREFLECTION))
	{
		surface.retroreflection = material.retroreflection;
		if ((material.flags & MATERIAL_FLAG_RETROREFLECTION_TEXTURE) != 0u)
			surface.retroreflection *= SampleView(material.retroreflectionView, v.texCoord01).r;
	}

	// the debug views of the surface's values
	{
		float3 value;
		if (DebugSurfaceValue(
				debugView, color.rgb, normal, geometryNormal, surface.roughness, metallic, occlusion, materialId, v.texCoord01.xy, value))
			return float4(value, color.a);
	}

	// KHR_materials_volume_scatter: what enters the surface comes out blurred (see SubsurfaceScattering), and of what
	// comes through it from behind, only what isn't scattered
	float3 scattered = 0.0;
	if (Layer(material, MATERIAL_FLAG_VOLUME_SCATTER) && surface.diffuseTransmission > 0.0)
	{
		float3 multiscatter = MultiscatterColor(material, v.texCoord01);
		if (scatterPass)
		{
			// the thickness along the view, measured where the back faces were drawn, else the material's
			float measured = ScatterThickness(input.hPos.xy, viewId, v.position);
			if (measured >= 0.0)
				throughVolume = VolumeAttenuation(material.attenuationColor.rgb, material.transmission.z, measured);
			return float4(ScatterLight(surface, view, v.position, multiscatter, throughVolume), float(materialId + 1u));
		}
		if (!ScatteredLight(input.hPos.xy, materialId, scattered))
			scattered = ScatterLight(surface, view, v.position, multiscatter, throughVolume);
		scattered *= (1.0 - metallic) * (1.0 - surface.iridescence) * (1.0 - transmission) * (1.0 - ClearcoatFresnel(surface, view));
		// all of the diffuse transmission's light is the scattering's
		throughVolume = 0.0;
	}
	else if (scatterPass)
	{
		return float4(0.0, 0.0, 0.0, float(materialId + 1u));
	}
	// the light through the surface is the dielectric's, of the diffuse that transmission leaves
	surface.diffuseTransmissionColor *= throughVolume * (1.0 - metallic) * (1.0 - transmission);

	// the clearcoat covers the emission too
	emissive *= 1.0 - ClearcoatFresnel(surface, view);
	// the debug views of the light: what it gives off, or Shade's (its light, its debug views: see Shade), without the
	// scattering's (which is lights' and ambient alike)
	if (debugView == DEBUG_VIEW_EMISSIVE)
		return float4(emissive, color.a);

	// a fragment's sharp rays, all traced in one loop (TracedFragmentRays): its reflections (see Shade) and its refraction,
	// of each color where dispersion spreads them (see TransmittedLight)
	bool dispersion = transmissionSpread > 0.0;
	float3 transmissionIors = dispersion ? transmissionIor + float3(-transmissionSpread, 0.0, transmissionSpread) : float3(transmissionIor);
	float4 traced[kTracedFragmentRayCount];
	for (uint rayIt = 0; rayIt < kTracedFragmentRayCount; rayIt++)
		traced[rayIt] = 0.0;
#if BVH_RAY_QUERY
	if (!TracedGlossyHere() && (TracedReflectionsOn() || TracedRefractionsOn()))
	{
		float3 directions[kTracedFragmentRayCount];
		for (uint rayIt = 0; rayIt < kTracedFragmentRayCount; rayIt++)
			directions[rayIt] = 0.0;
		float shares[2];
		SharpReflections(surface, view, directions[kTracedFragmentBase], shares[0], directions[kTracedFragmentCoat], shares[1]);
		bool tracing[kTracedFragmentRayCount];
		tracing[kTracedFragmentBase] = shares[0] > 0.0;
		tracing[kTracedFragmentCoat] = shares[1] > 0.0;
		for (uint channel = 0; channel < 3u; channel++)
			tracing[kTracedFragmentRefraction + channel] = transmission > 0.0 && (dispersion || channel == 0u) &&
				TransmittedTraced(normal, view, v.position, surface.roughness, transmissionIors[channel], transmissionThickness, viewId,
					directions[kTracedFragmentRefraction + channel]);
		TracedFragmentRays(v.position, geometryNormal, directions, tracing, transmissionThickness > 0.0, traced);
	}
#endif
	// the light through the surface (KHR_materials_transmission), the dielectric's
	if (transmission > 0.0)
	{
		float3 light;
		for (uint channel = 0; channel < (dispersion ? 3u : 1u); channel++)
		{
			float3 channelLight = TransmittedLight(
				normal, view, v.position, surface.roughness, transmissionIors[channel], transmissionThickness,
				material.attenuationColor.rgb, material.transmission.z, viewId, traced[kTracedFragmentRefraction + channel]);
			if (dispersion)
				light[channel] = channelLight[channel];
			else
				light = channelLight;
		}
		surface.transmitted = transmission * (1.0 - metallic) * color.rgb * light;
	}

	float3 shaded = Shade(surface, view, v.position, traced[kTracedFragmentBase], traced[kTracedFragmentCoat]);
	return float4(debugView != DEBUG_VIEW_NONE ? shaded : shaded + scattered + emissive, color.a);
}

// the debug views of a surface's values (DEBUG_VIEW_BASE_COLOR to DEBUG_VIEW_TEXCOORD): true and the value if debugView
// is one of them
bool DebugSurfaceValue(
	uint debugView,
	float3 baseColor,
	float3 normal,
	float3 geometryNormal,
	float roughness,
	float metallic,
	float occlusion,
	uint materialId,
	float2 texCoord,
	out float3 value)
{
	value = 0.0;
	switch (debugView)
	{
	case DEBUG_VIEW_BASE_COLOR: value = baseColor; return true;
	// shown as they are: the srgb curve then makes 0.5 + 0.5 n come out as the usual normal colors
	case DEBUG_VIEW_NORMAL: value = pow(normal * 0.5 + 0.5, 2.2); return true;
	case DEBUG_VIEW_GEOMETRY_NORMAL: value = pow(geometryNormal * 0.5 + 0.5, 2.2); return true;
	case DEBUG_VIEW_ROUGHNESS: value = pow(saturate(roughness), 2.2); return true;
	case DEBUG_VIEW_METALLIC: value = pow(saturate(metallic), 2.2); return true;
	case DEBUG_VIEW_OCCLUSION: value = pow(saturate(occlusion), 2.2); return true;
	case DEBUG_VIEW_MATERIAL: value = DebugIndexColor(materialId); return true;
	case DEBUG_VIEW_TEXCOORD: value = pow(float3(frac(texCoord), 0.0), 2.2); return true;
	default: return false;
	}
}

// the ground plane's horizon haze (see MaterialData::checker): the plane runs to the horizon, which from eye height is a
// line, as a plain's is: the air hazes the far ground within the pixel at the line (a pixel below it the ground is 500 m
// off, through a tenth of the fog's depth along the horizon), and the sky's own ground is as sharp. so the plane is hazed
// toward what the backdrop shows at the horizon in the pixel's azimuth (BackdropHorizon, which the sky's last row has
// too, so the line goes) as the fog near the ground would haze it from the top of its layer: the layer above the eye
// laid over the ground, exp of its vertical optical depth from the eye's altitude (FogDepthToSky straight up: the whole
// layer from eye height, none from above it, where the air's own haze is in view) over the sine of the angle below the
// horizon, a haze that thickens with the fog's (Fog Visibility and Height) and goes with it; a presentation of the plane
// alone, which nothing of the scene takes (the user's call, 2026-11-05: a smooth transition rather than a line). at the
// default fog, 0.026 deep, the plane is three quarters hazed a degree below the horizon, two fifths at 3 degrees, a
// seventh at 10 and a twentieth at 30, where a model's patch is: its shadows lighten by a few thousandths
float3 GroundHorizonHaze(float3 color, float3 position, uint viewId)
{
	uint frameIndex = gPushConstants.frameIndex;
	AerialPerspectiveData aerial = gAerialPerspectiveData[frameIndex][0];
	if (aerial.enabled == 0u || aerial.fog.x <= 0.0)
		return color;
	float3 eye = gViewData[frameIndex][viewId].eyePosition.xyz;
	float3 ray = position - eye;
	float distance = length(ray);
	if (distance <= 0.0 || ray.y >= 0.0)
		return color;
	float below = -ray.y / distance; // the sine of the angle below the horizon
	float haze = 1.0 - exp(-FogDepthToSky(aerial, AerialPerspectiveAltitude(aerial, eye.y), 1.0) / max(below, 1e-4));
	return lerp(color, BackdropHorizon(gEnvironment[0], ray, eye.y), haze);
}

// the main pass's color of a surface at a pixel of a view (FragmentMain's, and the visibility buffer's shading's, see
// ComputeMainVisibility): the traced shadows' debug views, else shaded, with alpha 1; the ground plane's hazed toward
// the horizon (GroundHorizonHaze)
float4 MainColor(VertexOutput input, bool isFrontFace, uint2 pixel, uint viewId)
{
	gShadingViewId = viewId;
	gFragmentPixel = pixel;
	gFragmentShadowMask = (gPushConstants.prepassImages & PREPASS_IMAGE_SHADOW_MASK) != 0u;
	gFragmentGiNear = (gPushConstants.prepassImages & PREPASS_IMAGE_GI_NEAR) != 0u;
	gFragmentEmitters = (gPushConstants.prepassImages & PREPASS_IMAGE_EMITTERS) != 0u;
	// the traced shadows' views: the first traced light's, as filtered or as traced
	uint debugView = gPushConstants.debugView;
	if (debugView == DEBUG_VIEW_TRACED_SHADOW || debugView == DEBUG_VIEW_TRACED_SHADOW_RAW ||
		debugView == DEBUG_VIEW_TRACED_SHADOW_REFERENCE)
	{
		uint4 traced = gShadowTrace[gPushConstants.frameIndex][0].lights;
		if (!gFragmentShadowMask || traced.x == SHADER_TYPES_NO_SHADOW)
			return float4(1.0, 0.0, 1.0, 1.0);
		// the rays' and the reference's views too: the filter passes them through then (see ShadowDenoise)
		float visibility = gTextures[SHADER_TYPES_SHADOW_MASK_TEXTURE].Load(int3(int2(gFragmentPixel), 0)).r;
		return float4(pow(float3(visibility), 2.2), 1.0);
	}
	float4 color = ShadeFragment(input, isFrontFace);
	if (gMaterialData[input.materialId].checker != 0.0 && debugView == DEBUG_VIEW_NONE)
		color.rgb = GroundHorizonHaze(color.rgb, input.vertex.position, viewId);

	// alpha 1: the color target is cleared to alpha 0, so ComputeMain draws the backdrop where nothing opaque is
	// within a half float (the sun's disk in a sharp reflection is tens of thousands, times the environment's intensity)
	return float4(min(color.rgb, 65000.0), 1.0);
}

// how far a transmissive surface moves since the last frame, in render pixels, past which temporal upscaling takes its
// pixels as reactive (see TransmissiveMotionReactivity)
static const float kTransmissiveReactiveMotionPixels = 1.0;

// how far temporal upscaling is to take a transmissive fragment's pixel as reactive, by how far its surface moved since
// the last frame (its own move, by this frame's view projection: not the eye's). the transmissive phase isn't in the
// pre-pass, so its pixels have the motion of what is behind them (see UpscaleMotion), and where the glass moves over
// what doesn't, its edges and what it reflects were taken as still: IridescentDishWithOlives' cover lifting left trails
// over its dome and along its rim (the user's pose 0.7811,0.4122,0.1465,22.123,-77.115), which the reactive mask, FSR's
// for what has no motion of its own, takes out (the frame's samples taken rather than the history)
float TransmissiveMotionReactivity(VertexOutput input)
{
	ViewData view = gViewData[gPushConstants.frameIndex][gPushConstants.viewId];
	float4 current = mul(view.viewProjection, float4(input.vertex.position, 1.0));
	float4 previous = mul(view.viewProjection, float4(input.previousPosition, 1.0));
	if (current.w <= 0.0 || previous.w <= 0.0)
		return 0.0;
	float2 moved = (previous.xy / previous.w - current.xy / current.w) * 0.5 * view.viewport.zw;
	return saturate(length(moved) / kTransmissiveReactiveMotionPixels);
}

[shader("fragment")]
FragmentOutput FragmentMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	FragmentOutput output;
	output.color = MainColor(input, isFrontFace, uint2(input.hPos.xy), gPushConstants.viewId);
	// a transmissive surface's: alpha past 1 by how reactive its pixel is (ComputeMain's composite takes it into the
	// reactive mask; the rest only tells alpha 0, nothing drawn, from the rest)
	if (gShadingTransmissive)
		output.color.a += TransmissiveMotionReactivity(input);
	return output;
}

// a pixel's triangle's perspective correct barycentrics at the pixel's center, and their screen space derivatives (the
// next pixel's less the pixel's, as the fragment stage's quads have them), from its corners in clip space and the
// pixel's normalized device coordinates in the view's viewport, of the given size in pixels (Schied and Dachsbacher,
// "Deferred Attribute Interpolation for Memory-Efficient Deferred Shading", in The Forge's visibility buffer's form;
// normalized device coordinates' y points down here, as the framebuffer's)
struct Barycentrics
{
	float3 lambda;
	float3 dx;
	float3 dy;
};

Barycentrics PixelBarycentrics(float4 clip0, float4 clip1, float4 clip2, float2 ndc, float2 size)
{
	Barycentrics result;
	float3 invW = 1.0 / float3(clip0.w, clip1.w, clip2.w);
	float2 ndc0 = clip0.xy * invW.x;
	float2 ndc1 = clip1.xy * invW.y;
	float2 ndc2 = clip2.xy * invW.z;
	float2 a = ndc2 - ndc1;
	float2 b = ndc0 - ndc1;
	float invDet = 1.0 / (a.x * b.y - a.y * b.x);
	result.dx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet * invW;
	result.dy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet * invW;
	float dxSum = dot(result.dx, float3(1.0));
	float dySum = dot(result.dy, float3(1.0));
	float2 delta = ndc - ndc0;
	float interpInvW = invW.x + delta.x * dxSum + delta.y * dySum;
	float interpW = 1.0 / interpInvW;
	result.lambda = interpW * (float3(invW.x, 0.0, 0.0) + delta.x * result.dx + delta.y * result.dy);
	// per pixel (normalized device coordinates span 2 over the viewport)
	result.dx *= 2.0 / size.x;
	result.dy *= 2.0 / size.y;
	dxSum *= 2.0 / size.x;
	dySum *= 2.0 / size.y;
	result.dx = (result.lambda * interpInvW + result.dx) / (interpInvW + dxSum) - result.lambda;
	result.dy = (result.lambda * interpInvW + result.dy) / (interpInvW + dySum) - result.lambda;
	return result;
}

// the visibility buffer's shading of a pixel (see ComputeMainVisibility): the surface its triangle has there
// (gVisibility: the draw record, and the triangle, SV_PrimitiveID, and the instance, by the record's primitive bits), its
// corners placed as the vertex stage places them (PlaceVertex) and interpolated at the pixel's center by perspective
// correct barycentrics, whose screen space derivatives are the texture lookups' and the tangent frames' (see
// gTexCoordDx), then shaded as FragmentMain shades it (MainColor) into the main color. what nothing opaque covers is
// cleared (alpha 0: the backdrop's, see ComputeMain); points and lines are left to Main
void ShadeVisibility(int2 pixel)
{
	uint2 extent;
	gMainColor.GetDimensions(extent.x, extent.y);
	if (any(pixel >= int2(extent)))
		return;
	// the traced reflections: none here, unless Shade records one (see TracedReflectionRecord, TracedCoatRecord)
	if (TracedReflectionsOn())
	{
		TracedReflectionRecord(uint2(pixel), float3(0.0, 0.0, 1.0), 0.0, kReflectionUnknown, kReflectionUnknown);
		TracedCoatRecord(uint2(pixel), float3(0.0, 0.0, 1.0), 0.0, 0.0);
	}
	uint frameIndex = gPushConstants.frameIndex;
	float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(pixel, 0)).r;
	uint viewId;
	if (depth <= 0.0 || !ViewIndexAt(pixel, viewId))
	{
		gMainColor[pixel] = 0.0;
		return;
	}
	uint2 visibility = gVisibility.Load(int3(pixel, 0));
	uint record = visibility.x & ~VISIBILITY_EXTENDED;
	DrawRecord draw = gDrawRecords[frameIndex][record];
	if ((draw.flags & DRAW_FLAG_TRIANGLES) == 0u)
		return;
	uint primitive = visibility.y & ((1u << draw.primitiveBits) - 1u);
	uint instanceIndex = visibility.y >> draw.primitiveBits;
	uint first = draw.firstIndex + 3u * primitive;
	ModelInstance instance = gModelInstances[frameIndex][draw.modelInstanceId + instanceIndex];
	float4x4 viewProjection = gViewData[frameIndex][viewId].viewProjection;
	float4 viewport = gViewData[frameIndex][viewId].viewport;
	VertexOutput corners[3];
	float4 clip[3];
	for (uint cornerIt = 0; cornerIt < 3; cornerIt++)
	{
		corners[cornerIt] = PlaceVertex(draw, record, gIndexBuffer[first + cornerIt], instanceIndex, instance, frameIndex);
		clip[cornerIt] = mul(viewProjection, float4(corners[cornerIt].vertex.position, 1.0));
	}
	float2 ndc = (float2(pixel) + 0.5 - viewport.xy) / viewport.zw * 2.0 - 1.0;
	Barycentrics b = PixelBarycentrics(clip[0], clip[1], clip[2], ndc, viewport.zw);

	VertexOutput input = corners[0];
	input.vertex.position = b.lambda.x * corners[0].vertex.position + b.lambda.y * corners[1].vertex.position + b.lambda.z * corners[2].vertex.position;
	input.vertex.normal = b.lambda.x * corners[0].vertex.normal + b.lambda.y * corners[1].vertex.normal + b.lambda.z * corners[2].vertex.normal;
	input.vertex.tangent = b.lambda.x * corners[0].vertex.tangent + b.lambda.y * corners[1].vertex.tangent + b.lambda.z * corners[2].vertex.tangent;
	input.vertex.texCoord01 = b.lambda.x * corners[0].vertex.texCoord01 + b.lambda.y * corners[1].vertex.texCoord01 + b.lambda.z * corners[2].vertex.texCoord01;
	input.vertex.color = b.lambda.x * corners[0].vertex.color + b.lambda.y * corners[1].vertex.color + b.lambda.z * corners[2].vertex.color;
	input.hPos = float4(float2(pixel) + 0.5, depth, 1.0);
	input.previousPosition = input.vertex.position;
	gPositionDx = b.dx.x * corners[0].vertex.position + b.dx.y * corners[1].vertex.position + b.dx.z * corners[2].vertex.position;
	gPositionDy = b.dy.x * corners[0].vertex.position + b.dy.y * corners[1].vertex.position + b.dy.z * corners[2].vertex.position;
	gTexCoordDx = b.dx.x * corners[0].vertex.texCoord01 + b.dx.y * corners[1].vertex.texCoord01 + b.dx.z * corners[2].vertex.texCoord01;
	gTexCoordDy = b.dy.x * corners[0].vertex.texCoord01 + b.dy.y * corners[1].vertex.texCoord01 + b.dy.z * corners[2].vertex.texCoord01;

	// which side faces the eye, as the rasterizer has it: the side its corners' order turns counter-clockwise seen from the
	// eye (its geometric normal toward the eye: counter-clockwise on the screen, which the projection doesn't mirror) is
	// the front, but for a mirroring instance's, drawn with clockwise front faces. in world space: a triangle with a
	// corner behind the eye (the floor beside the camera) has that corner's projection mirrored, and its winding on the
	// screen with it
	float3 geometric = cross(corners[1].vertex.position - corners[0].vertex.position, corners[2].vertex.position - corners[0].vertex.position);
	bool facing = dot(geometric, gViewData[frameIndex][viewId].eyePosition.xyz - corners[0].vertex.position) > 0.0;
	bool mirrored = determinant(float3x3(instance.modelTransform[0].xyz, instance.modelTransform[1].xyz, instance.modelTransform[2].xyz)) < 0.0;
	bool isFrontFace = facing != mirrored;

	gMainColor[pixel] = MainColor(input, isFrontFace, uint2(pixel), viewId);
}

// what is drawn at a pixel for the visibility buffer's sorting: bit 0 something, bit 1 of a material with an extended
// layer (MATERIAL_FLAGS_EXTENDED), which a kernel specialized without them can't shade, as the pre-pass marked its
// record (VISIBILITY_EXTENDED)
uint VisibilityDrawn(int2 pixel)
{
	uint2 extent;
	gMainColor.GetDimensions(extent.x, extent.y);
	if (any(pixel >= int2(extent)) || gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(pixel, 0)).r <= 0.0)
		return 0u;
	return (gVisibility.Load(int3(pixel, 0)).x & VISIBILITY_EXTENDED) != 0u ? 3u : 1u;
}

// where a list of gVisibilityTiles starts (VISIBILITY_TILES_*): after both dispatches' thread group counts, the tiles
// without a material with an extended layer, then those with, as many as the frame has tiles each
uint VisibilityTileFirst(uint list)
{
	uint2 extent;
	gMainColor.GetDimensions(extent.x, extent.y);
	uint2 tiles = (extent + SHADER_TYPES_VISIBILITY_TILE_SIZE - 1u) / SHADER_TYPES_VISIBILITY_TILE_SIZE;
	return 6u + (list - VISIBILITY_TILES_SPECIALIZED) * tiles.x * tiles.y;
}

groupshared uint gsVisibilityDrawn;

// the tiles of a frame with materials that have an extended layer, sorted (the Main Visibility Classify pass): a tile with
// a pixel of those to the generic kernel's list, the others to the specialized kernel's (gVisibilityTiles), each counted
// in its list's indirect dispatch, and the tiles with something drawn counted by kind (gVisibilityStatistics). in the
// shading kernel itself, the check and its barriers cost 0.45 ms even in frames that skipped them, at Sponza and San
// Miguel (the compiler's, not the check's); here it costs 0.21 to 0.25 ms (at SheenChair and CarConcept, with one
// load a pixel or four)
[shader("compute")]
[numthreads(SHADER_TYPES_VISIBILITY_TILE_SIZE, SHADER_TYPES_VISIBILITY_TILE_SIZE, 1)]
void ComputeMainVisibilityClassify(uint3 threadId: SV_DispatchThreadID, uint3 groupId: SV_GroupID, uint groupIndex: SV_GroupIndex)
{
	if (groupIndex == 0u)
		gsVisibilityDrawn = 0u;
	GroupMemoryBarrierWithGroupSync();
	uint drawn = VisibilityDrawn(int2(threadId.xy));
	if (drawn != 0u)
		InterlockedOr(gsVisibilityDrawn, drawn);
	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0u)
	{
		bool extended = (gsVisibilityDrawn & 2u) != 0u;
		uint list = extended ? VISIBILITY_TILES_GENERIC : VISIBILITY_TILES_SPECIALIZED;
		uint tile;
		InterlockedAdd(gVisibilityTiles[3u * (list - VISIBILITY_TILES_SPECIALIZED)], 1u, tile);
		gVisibilityTiles[VisibilityTileFirst(list) + tile] = groupId.x | (groupId.y << 16);
		if (gsVisibilityDrawn != 0u)
			InterlockedAdd(
				gVisibilityStatistics[gPushConstants.frameIndex][extended ? VISIBILITY_STATISTIC_EXTENDED : VISIBILITY_STATISTIC_PLAIN], 1u);
	}
}

// the visibility buffer's shading (the Main Visibility pass, in place of Main's opaque triangles, so that they aren't
// drawn a second time after the pre-pass): a tile of pixels a group (ShadeVisibility), the dispatch's
// (PushConstants::visibilityTiles 0: every tile, by the kernel the frame's materials need, see Renderer), or a list's of
// gVisibilityTiles, where the frame's tiles were sorted: the specialized kernel's without a material with an extended
// layer, the generic kernel's with (see ComputeMainVisibilityClassify)
[shader("compute")]
[numthreads(SHADER_TYPES_VISIBILITY_TILE_SIZE, SHADER_TYPES_VISIBILITY_TILE_SIZE, 1)]
void ComputeMainVisibility(uint3 threadId: SV_DispatchThreadID, uint3 groupId: SV_GroupID, uint3 localId: SV_GroupThreadID)
{
	uint2 pixel = threadId.xy;
	uint list = gPushConstants.visibilityTiles;
	if (list != VISIBILITY_TILES_ALL)
	{
		uint tile = gVisibilityTiles[VisibilityTileFirst(list) + groupId.x];
		pixel = uint2(tile & 0xffffu, tile >> 16) * SHADER_TYPES_VISIBILITY_TILE_SIZE + localId.xy;
	}
	ShadeVisibility(int2(pixel));
}

// the scatter pass's color attachments: the light, and the multiscatter color
struct ScatterOutput
{
	float4 color : SV_Target0;
	float4 albedo : SV_Target1;
};

// a fragment of the scatter pass: the light entering a scattering material, and the material (+ 1) in alpha, and its
// multiscatter color, which the blur's profile is scaled by
[shader("fragment")]
ScatterOutput FragmentScatter(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	gShadingViewId = gPushConstants.viewId;
	gFragmentPixel = uint2(input.hPos.xy);
	gFragmentShadowMask = false;
	gFragmentGiNear = false;
	gFragmentEmitters = false;
	ScatterOutput output;
	output.color = ShadeFragment(input, isFrontFace, true);
	MaterialData material = gMaterialData[input.materialId];
	output.albedo = float4(MultiscatterColor(material, input.vertex.texCoord01), 1.0);
	return output;
}

// discards a fragment where its material's alpha mask cuts it out, as ShadeFragment does
void CutOut(VertexOutput input, MaterialData material)
{
	VertexP3fN3fTa4fT014fC4f v = input.vertex;
	if (material.alphaCutoff <= 0.0)
		return;
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u && SampleView(material.baseColorView, v.texCoord01).a < material.alphaCutoff)
		discard;
	if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u && SampleView(material.alphaView, v.texCoord01).r < material.alphaCutoff)
		discard;
}

// a fragment of the shadow pass: depth only, but cut out where the material is (alpha mask)
[shader("fragment")]
void FragmentShadow(VertexOutput input)
{
	gShadingViewId = gPushConstants.viewId;
	CutOut(input, gMaterialData[input.materialId]);
}

// a fragment of the Shadow Transmittance pass (see SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE): what a transmissive
// surface lets through of the light, as TransmittedLight takes what is behind it: its transmission of the dielectric
// (the rest is a diffuse, which it blocks), tinted by its base color, attenuated through its volume's thickness, and less
// what its two faces reflect at normal incidence; and its depth. a volume's faces toward the light alone are drawn, each
// for the whole thickness, as the view through it takes it at its front face
[shader("fragment")]
float4 FragmentShadowTransmittance(VertexOutput input) : SV_Target
{
	gShadingViewId = gPushConstants.viewId;
	MaterialData material = gMaterialData[input.materialId];
	CutOut(input, material);
	VertexP3fN3fTa4fT014fC4f v = input.vertex;
	float3 color = v.color.rgb * material.color.rgb;
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
		color *= SampleView(material.baseColorView, v.texCoord01).rgb;
	float metallic = material.metallic;
	if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
		metallic *= SampleView(material.metallicRoughnessView, v.texCoord01).g;
	float transmission = (material.flags & MATERIAL_FLAG_TRANSMISSION) != 0u ? material.transmission.x : 0.0;
	if ((material.flags & MATERIAL_FLAG_TRANSMISSION_TEXTURE) != 0u)
		transmission *= SampleView(material.transmissionView, v.texCoord01).r;
	float thickness = material.transmission.y;
	if ((material.flags & MATERIAL_FLAG_THICKNESS_TEXTURE) != 0u)
		thickness *= SampleView(material.thicknessView, v.texCoord01).r;
	thickness *= input.modelScale;
	float ior = material.specularColor.a;
	float f0 = (ior - 1.0) / (ior + 1.0);
	float faces = 1.0 - f0 * f0;
	float3 through = transmission * (1.0 - metallic) * color *
					 VolumeAttenuation(material.attenuationColor.rgb, material.transmission.z, thickness) * faces * faces;
	return float4(through, input.hPos.z);
}

// the pre-pass's color attachments: the geometry normal, as the shading takes it (the vertex normal toward the side
// seen), 0.5 + 0.5 n in rgb, and 1 in alpha where the material takes light from behind (diffuse transmission, volume
// scatter): what the traced shadows' rays start from (see ComputeShadowTrace; SHADER_TYPES_PREPASS_NORMAL_TEXTURE); and
// where the point was in the last frame, in the render target's pixels from where it is, the jitters taken out
// (SHADER_TYPES_PREPASS_MOTION_TEXTURE): what temporal upscaling reprojects what moves by
struct PrepassOutput
{
	float4 normal : SV_Target0;
	float4 motion : SV_Target1;
	// the visibility buffer (see gVisibility): the draw record, and the triangle and the instance
	uint2 visibility : SV_Target2;
};

// a fragment of the pre-pass: its depth, cut out as the main pass's are, and its geometry normal, before the main pass
// shades what it wrote (each pixel once, see DepthTest::kEqual)
[shader("fragment")]
PrepassOutput FragmentPrepass(VertexOutput input, bool isFrontFace : SV_IsFrontFace, uint primitive : SV_PrimitiveID)
{
	gShadingViewId = gPushConstants.viewId;
	MaterialData material = gMaterialData[input.materialId];
	CutOut(input, material);
	VertexP3fN3fTa4fT014fC4f v = input.vertex;
	float3 normal;
	if (dot(v.normal, v.normal) > 0.0)
		normal = (isFrontFace ? 1.0 : -1.0) * normalize(v.normal);
	else
	{
		// lines and points without normals: toward the eye
		float3 eye = gViewData[gPushConstants.frameIndex][gPushConstants.viewId].eyePosition.xyz;
		normal = normalize(eye - v.position);
	}
	bool fromBehind = (material.flags & (MATERIAL_FLAG_DIFFUSE_TRANSMISSION | MATERIAL_FLAG_VOLUME_SCATTER)) != 0u;
	PrepassOutput output;
	output.normal = float4(normal * 0.5 + 0.5, fromBehind ? 1.0 : 0.0);
	ViewData view = gViewData[gPushConstants.frameIndex][gPushConstants.viewId];
	float4 current = mul(view.viewProjection, float4(v.position, 1.0));
	float4 previous = mul(view.previousViewProjection, float4(input.previousPosition, 1.0));
	float2 motion = previous.w > 0.0 ? ((previous.xy / previous.w - view.jitter.zw) - (current.xy / current.w - view.jitter.xy)) * 0.5 * view.viewport.zw : 0.0;
	output.motion = float4(motion, 0.0, 0.0);
	// (the record marked where its material has an extended layer, which sorts its tile for the generic kernel)
	uint extended = (material.flags & MATERIAL_FLAGS_EXTENDED) != 0u ? VISIBILITY_EXTENDED : 0u;
	output.visibility = uint2(input.drawRecord | extended, primitive | input.visibilityInstance);
	return output;
}

// a blended fragment: pushed on its pixel's list (see OitNode), for ComputeMain to sort and blend. the depth test must
// come first (it writes the lists, not the attachments), so it does: early fragment tests
[shader("fragment")]
[earlydepthstencil]
void FragmentTransparent(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	gShadingViewId = gPushConstants.viewId;
	gFragmentPixel = uint2(input.hPos.xy);
	gFragmentShadowMask = false;
	gFragmentGiNear = false;
	gFragmentEmitters = false;
	float4 color = ShadeFragment(input, isFrontFace);
	if (color.a <= 0.0)
		return;

	uint node;
	InterlockedAdd(gOitCounter[0], 1u, node);
	if (node >= gPushConstants.oitNodeCapacity)
		return;

	uint pixel = uint(input.hPos.y) * gPushConstants.framebufferWidth + uint(input.hPos.x);
	uint next;
	InterlockedExchange(gOitHeads[pixel], node, next);

	OitNode result;
	result.color = PackR11G11B10(color.rgb * saturate(color.a));
	result.alphaDepth = (uint(saturate(color.a) * 255.0 + 0.5) << 24) | ((0x3f800000u - asuint(saturate(input.hPos.z))) >> 6);
	result.next = next;
	gOitNodes[node] = result;
}
