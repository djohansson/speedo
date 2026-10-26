// the fragment stage: ShadeFragment, and the main, scatter, shadow, transparent and pre-pass entry points. a part of shaders.slang, which includes it after capi.h and sky.h

// the main pass's color attachment, which opaque draws write (blended ones go in the per pixel lists instead, see
// FragmentTransparent)
struct FragmentOutput
{
	float4 color : SV_Target;
};

// a fragment's color and alpha, lit and with emissive added. for the scatter pass (scatterPass): the light entering a
// scattering material (ScatterLight), and its material + 1 in alpha
float4 ShadeFragment(VertexOutput input, bool isFrontFace, bool scatterPass = false)
{
	VertexP3fN3fTa4fT014fC4f v = input.vertex;

	uint materialId = input.materialId;

	MaterialData material = gMaterialData[materialId];

	// linear: the vertex colors hold the materials' diffuse colors, and textures are sampled through srgb formats
	float4 color = v.color * material.color;

	// cutouts (foliage, fences) keep their mask in the diffuse texture's alpha channel, or in a separate texture
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		float4 sample = SampleView(material.baseColorView, v.texCoord01);
		if (sample.a < material.alphaCutoff)
			discard;

		color *= sample;
	}

	if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u && SampleView(material.alphaView, v.texCoord01).r < material.alphaCutoff)
		discard;

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
		emissive *= SampleView(material.emissiveView, v.texCoord01).rgb;

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

	uint viewId = gPushConstants.viewId;
	float3 eye = gViewData[gPushConstants.frameIndex][viewId].eyePosition.xyz;
	float3 view = normalize(eye - v.position);

	// KHR_materials_transmission: that much of the dielectric's diffuse is the light through it instead
	surface.transmitted = 0.0;
	float transmission = 0.0;
	if (Layer(material, MATERIAL_FLAG_TRANSMISSION))
	{
		transmission = material.transmission.x;
		if ((material.flags & MATERIAL_FLAG_TRANSMISSION_TEXTURE) != 0u)
			transmission *= SampleView(material.transmissionView, v.texCoord01).r;
		float thickness = material.transmission.y;
		if ((material.flags & MATERIAL_FLAG_THICKNESS_TEXTURE) != 0u)
			thickness *= SampleView(material.thicknessView, v.texCoord01).r;
		thickness *= input.modelScale;
		float ior = material.specularColor.a;
		float3 light;
		// KHR_materials_dispersion: each color refracted by its own ior, spread around it
		float spread = (ior - 1.0) * 0.025 * material.transmission.w;
		if (spread > 0.0)
		{
			float3 iors = float3(ior - spread, ior, ior + spread);
			for (uint channel = 0; channel < 3; channel++)
				light[channel] = TransmittedLight(
					normal, view, v.position, surface.roughness, iors[channel], thickness, material.attenuationColor.rgb,
					material.transmission.z, viewId)[channel];
		}
		else
		{
			light = TransmittedLight(
				normal, view, v.position, surface.roughness, ior, thickness, material.attenuationColor.rgb, material.transmission.z, viewId);
		}
		float dielectric = transmission * (1.0 - metallic);
		surface.transmitted = dielectric * color.rgb * light;
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
	if (debugView != DEBUG_VIEW_NONE)
		return float4(Shade(surface, view, v.position), color.a);
	return float4(Shade(surface, view, v.position) + scattered + emissive, color.a);
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

[shader("fragment")]
FragmentOutput FragmentMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	gFragmentPixel = uint2(input.hPos.xy);
	gFragmentShadowMask = (gPushConstants.prepassImages & PREPASS_IMAGE_SHADOW_MASK) != 0u;
	gFragmentGiNear = (gPushConstants.prepassImages & PREPASS_IMAGE_GI_NEAR) != 0u;
	gFragmentEmitters = (gPushConstants.prepassImages & PREPASS_IMAGE_EMITTERS) != 0u;
	// the traced shadows' views: the first traced light's, as filtered or as traced
	uint debugView = gPushConstants.debugView;
	if (debugView == DEBUG_VIEW_TRACED_SHADOW || debugView == DEBUG_VIEW_TRACED_SHADOW_RAW ||
		debugView == DEBUG_VIEW_TRACED_SHADOW_REFERENCE)
	{
		FragmentOutput shown;
		uint4 traced = gShadowTrace[gPushConstants.frameIndex][0].lights;
		if (!gFragmentShadowMask || traced.x == SHADER_TYPES_NO_SHADOW)
			shown.color = float4(1.0, 0.0, 1.0, 1.0);
		else
		{
			// the rays' and the reference's views too: the filter passes them through then (see ShadowDenoise)
			float visibility = gTextures[SHADER_TYPES_SHADOW_MASK_TEXTURE].Load(int3(int2(gFragmentPixel), 0)).r;
			shown.color = float4(pow(float3(visibility), 2.2), 1.0);
		}
		return shown;
	}
	float4 color = ShadeFragment(input, isFrontFace);

	// alpha 1: the color target is cleared to alpha 0, so ComputeMain draws the backdrop where nothing opaque is
	// within a half float (the sun's disk in a sharp reflection is tens of thousands, times the environment's intensity)
	FragmentOutput output;
	output.color = float4(min(color.rgb, 65000.0), 1.0);
	return output;
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
	CutOut(input, gMaterialData[input.materialId]);
}

// the pre-pass's color attachment (SHADER_TYPES_PREPASS_NORMAL_TEXTURE): the geometry normal, as the shading takes it (the
// vertex normal toward the side seen), 0.5 + 0.5 n in rgb, and 1 in alpha where the material takes light from behind
// (diffuse transmission, volume scatter): what the traced shadows' rays start from (see ComputeShadowTrace)
struct PrepassOutput
{
	float4 normal : SV_Target0;
};

// a fragment of the pre-pass: its depth, cut out as the main pass's are, and its geometry normal, before the main pass
// shades what it wrote (each pixel once, see DepthTest::kEqual)
[shader("fragment")]
PrepassOutput FragmentPrepass(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
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
	return output;
}

// a blended fragment: pushed on its pixel's list (see OitNode), for ComputeMain to sort and blend. the depth test must
// come first (it writes the lists, not the attachments), so it does: early fragment tests
[shader("fragment")]
[earlydepthstencil]
void FragmentTransparent(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
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
	result.alphaDepth = (uint(saturate(color.a) * 255.0 + 0.5) << 24) | uint(saturate(input.hPos.z) * 16777215.0 + 0.5);
	result.next = next;
	gOitNodes[node] = result;
}
