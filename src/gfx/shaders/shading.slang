// the surface a fragment shades (Surface), and its light: Shade. a part of shaders.slang, which includes it after capi.h and sky.h

// what Shade lights: the Lambertian diffuse color, and the specular's Fresnel at normal incidence (f0) and at grazing
// angles (f90): the metallic-roughness model's mix of a dielectric (f0 from the ior, tinted by KHR_materials_specular's
// color, both scaled by its strength) and a metal (f0 the base color), or a specular-glossiness material's
struct Surface
{
	float3 diffuseColor;
	float3 f0;
	float f90;
	float roughness;
	float occlusion;
	float3 normal;
	// KHR_materials_clearcoat: a dielectric layer (f0 0.04) of a strength, roughness and normal over the rest, which
	// keeps what the layer's fresnel leaves; 0 for none
	float clearcoat;
	float clearcoatRoughness;
	float3 clearcoatNormal;
	// KHR_materials_sheen: its color (black for none) and roughness
	float3 sheenColor;
	float sheenRoughness;
	// KHR_materials_transmission: the light through the dielectric (the scene behind it, by its base color, transmission
	// factor and volume attenuation), which the specular reflection lets through
	float3 transmitted;
	// KHR_materials_anisotropy: the strength (0 for none), and the direction it stretches the specular along (t) and
	// across it (b), unit vectors perpendicular to the normal
	float anisotropy;
	float3 anisotropyT;
	float3 anisotropyB;
	// KHR_materials_iridescence: how much of the specular's fresnel is the thin film's (seen from the view)
	float iridescence;
	float3 iridescenceFresnel;
	// KHR_materials_diffuse_transmission: how much of the diffuse is lit from the back instead (0 for none), and its
	// tint
	float diffuseTransmission;
	float3 diffuseTransmissionColor;
	// KHR_materials_retroreflection: the blend from the specular (and the diffuse it leaves) seen from the view to the
	// same seen from the view mirrored about the normal, which turns the lobe back towards the light (0 for none)
	float retroreflection;
	// the geometry's normal, facing the viewer (for the shadows' normal offset)
	float3 geometryNormal;
};

// the base layers' light (the diffuse, and the specular: GGX, height correlated Smith, Schlick; anisotropic if the surface
// is) from a light along toLight, seen from view, per unit of irradiance (times n.l). without sheen, clearcoat or
// transmission
float3 BaseLobe(Surface surface, float3 view, float3 toLight, float nDotL)
{
	float3 normal = surface.normal;
	float alpha = max(surface.roughness * surface.roughness, 1e-3);
	float nDotV = max(dot(normal, view), 1e-4);
	float3 halfway = normalize(toLight + view);
	float vDotH = saturate(dot(view, halfway));
	float nDotH = saturate(dot(normal, halfway));
	float3 fresnel = lerp(FresnelSchlick(surface.f0, surface.f90, vDotH), surface.iridescenceFresnel, surface.iridescence);
	float3 diffuse = (1.0 - fresnel) * (1.0 - surface.diffuseTransmission) * surface.diffuseColor / kPi;
	float3 specular;
	if (surface.anisotropy > 0.0)
	{
		// stretched along t: its roughness there goes from the surface's toward 1 by the strength squared
		float alphaT = lerp(alpha, 1.0, surface.anisotropy * surface.anisotropy);
		float3 t = surface.anisotropyT;
		float3 b = surface.anisotropyB;
		specular = fresnel * DistributionGgxAnisotropic(nDotH, dot(t, halfway), dot(b, halfway), alphaT, alpha) *
				   VisibilityGgxAnisotropic(nDotL, nDotV, dot(t, view), dot(b, view), dot(t, toLight), dot(b, toLight), alphaT, alpha);
	}
	else
	{
		specular = fresnel * DistributionGgx(nDotH, alpha) * VisibilitySmithGgxCorrelated(nDotL, nDotV, alpha);
	}
	return diffuse + specular;
}

// how much of what is below a clearcoat layer it reflects instead, seen from view (the spec's approximation, by the
// layer's normal)
float ClearcoatFresnel(Surface surface, float3 view)
{
	return surface.clearcoat * FresnelSchlick(float3(0.04), 1.0, max(dot(surface.clearcoatNormal, view), 1e-4)).r;
}

// the light a surface reflects towards view: from gLights, and the environment (by occlusion). linear, before exposure
float3 Shade(Surface surface, float3 view, float3 position)
{
	float3 normal = surface.normal;
	float roughness = surface.roughness;
	float alpha = max(roughness * roughness, 1e-3);
	float nDotV = max(dot(normal, view), 1e-4);

	float3 environmentSpecular = lerp(
		EnvironmentBrdfApprox(surface.f0, surface.f90, roughness, nDotV), surface.iridescenceFresnel, surface.iridescence);
	EnvironmentData environment = gEnvironment[0];
	// anisotropy bends the normal the environment is reflected about toward the direction across the stretch (as the
	// Khronos sample viewer)
	float3 reflectionNormal = normal;
	if (surface.anisotropy > 0.0)
	{
		float3 anisotropicTangent = cross(surface.anisotropyB, view);
		float3 anisotropicNormal = cross(anisotropicTangent, surface.anisotropyB);
		float bend = 1.0 - surface.anisotropy * (1.0 - roughness);
		reflectionNormal = normalize(lerp(anisotropicNormal, normal, bend * bend * bend * bend));
	}
	float3 reflected = DominantDirection(reflectionNormal, reflect(-view, reflectionNormal), roughness);
	// KHR_materials_retroreflection: the view mirrored about the normal, whose reflection is the view itself, so the
	// retroreflective lobe sees the environment behind the viewer (the split sum's terms are the same: n.v is)
	float3 retroView = 2.0 * nDotV * normal - view;
	float3 environmentRadiance = EnvironmentRadiance(environment, reflected, roughness);
	if (surface.retroreflection > 0.0)
		environmentRadiance = lerp(
			environmentRadiance,
			EnvironmentRadiance(environment, DominantDirection(reflectionNormal, view, roughness), roughness),
			surface.retroreflection);
	// KHR_materials_diffuse_transmission: that much of the diffuse is lit from the back instead, tinted by the
	// transmission color alone (the spec's diffuse_btdf(diffuseTransmissionColor): not the base color)
	// the indirect lighting's (see GiIrradiance), where its probes cover the point: the environment occluded, and the light
	// the scene bounces. else the environment's
	uint debugView = DebugView();
	if (DebugViewGi(debugView) != 0u)
	{
		float3 debug = GiIrradiance(position, normal, GiDebug(), view);
		return debug.x >= 0.0 ? debug : float3(1.0, 0.0, 1.0);
	}
	// what the near field's rays gathered at the pixel (the main pass's opaque fragments, see GiNearIrradiance), which
	// stands in for the lookup's second try where the first sees no probe
	float4 near = GiNearAtFragment();
	GiSample gi = GiSampleSurface(position, normal, view, near.a < 0.0);
	float giTrust = near.a >= 0.0 ? GiLookupTrust(gi) : 1.0;
	// where a clip holds the point but no probe is seen around it, none of its light: the environment's there (the sky
	// unoccluded) shone into dark interiors along their edges
	float3 indirect = gi.valid ? GiEvaluate(gi, normal) : (gi.inside ? 0.0 : -1.0);
	indirect = GiNearIrradiance(near, indirect, giTrust);
	// the emissive surfaces' light (the main pass's opaque fragments, see EmitterIrradiance): what a white surface reflects
	// of it, -1 where it isn't gathered
	float3 emitted = EmitterIrradiance();
	if (debugView == DEBUG_VIEW_EMITTERS || debugView == DEBUG_VIEW_EMITTERS_RAW || debugView == DEBUG_VIEW_EMITTERS_REFERENCE)
		return emitted.x >= 0.0 ? emitted : float3(1.0, 0.0, 1.0);
	// the debug views of the ambient light: a white surface's diffuse, whatever lights it (the indirect lighting where
	// it covers the point, else the environment)
	if (debugView == DEBUG_VIEW_INDIRECT_DIFFUSE)
		return indirect.x >= 0.0 ? indirect : EnvironmentIrradiance(environment, normal);
	// and the specular: the environment as much as the indirect light sees of it (its share of the environment's
	// irradiance, which is all of it unoccluded), and, as the surface gets rough, the indirect light's radiance around the
	// reflection (its harmonics along it: a cosine lobe, as wide as the roughest): glossy reflections of the scene, without
	// the sky reflected inside a building
	float seen = 1.0;
	float rough = 0.0;
	if (indirect.x >= 0.0)
	{
		float3 luminance = float3(0.2126, 0.7152, 0.0722);
		seen = saturate(dot(indirect, luminance) / max(dot(EnvironmentIrradiance(environment, normal), luminance), 1e-4));
		rough = smoothstep(0.2, 0.9, roughness);
		// the lookup's faces as far as it is trusted, the near field's diffuse light beyond
		environmentRadiance = lerp(environmentRadiance * seen, lerp(indirect, GiEvaluate(gi, reflected), giTrust), rough);
	}
	// where the device has ray queries (View > Ray Traced Reflections): what the reflection meets, traced, where the
	// specular takes enough of it to show. the visibility buffer's (compute): glossy, a ray of the lobe whose sample the
	// Reflection passes filter and add to the main color by the weight stored here, its share left out of this
	// (TracedGlossyReflection); only in the views that show the reflection. the sample is stored as a share of what the
	// probes and the environment give along the ray (as environmentRadiance along the reflection, above), and the weight
	// takes this pixel's environmentRadiance: where the ray sees what they predict (the sky) the share is 1 and the pixel is
	// shaded as untraced, its normal map's detail and all, and what the ray met that they don't predict (a surface, a
	// shadow) comes in at the scale the filter keeps, without noise where it doesn't (a ratio estimator, as Heitz et al.'s
	// for shadows). the radiance itself was filtered and taken by every pixel alike: ChronographWatch's carbon fiber, a
	// gold metal of roughness 0.4 under a fine normal map, came out a blur of blotches where its rays met the strap, its
	// fibers gone, and its frames changed 0.258 of 255 at the user's pose, 0.021 untraced. a fragment's: sharp, in place
	// (TracedReflection)
	float tracedShare = 0.0;
#if BVH_RAY_QUERY
	if (TracedReflectionsOn())
	{
		float strength = dot(environmentSpecular, float3(0.2126, 0.7152, 0.0722)) * surface.occlusion;
		if (TracedGlossyHere())
		{
			bool shown = debugView == DEBUG_VIEW_NONE || debugView == DEBUG_VIEW_INDIRECT || debugView == DEBUG_VIEW_INDIRECT_SPECULAR ||
						 debugView == DEBUG_VIEW_TRACED_REFLECTIONS;
			tracedShare = shown ? TracedGlossyShare(roughness, strength) : 0.0;
			if (tracedShare > 0.0)
			{
				bool valid;
				float3 direction;
				float4 sample =
					TracedGlossyReflection(position, surface.geometryNormal, normal, view, roughness, gFragmentPixel, direction, valid);
				float3 expected = EnvironmentRadiance(environment, direction, roughness);
				if (indirect.x >= 0.0)
					expected = lerp(expected * seen, lerp(indirect, GiEvaluate(gi, direction), giTrust), rough);
				// where what the ray met isn't known (see TracedRadiance), what they predict, unblurred
				if (!valid)
					sample = float4(expected, 0.0);
				float least = kReflectionPredictionLeast / max(gViewData[gPushConstants.frameIndex][gShadingViewId].exposure, 1e-6);
				sample.rgb /= expected + least;
				float3 weight = (debugView == DEBUG_VIEW_TRACED_REFLECTIONS ? float3(tracedShare) : surface.occlusion * environmentSpecular * tracedShare) *
								(environmentRadiance + least);
				TracedReflectionStore(gFragmentPixel, sample, float4(weight, ReflectionPackRoughness(roughness, dot(normal, surface.geometryNormal) < 0.995)));
				environmentRadiance *= 1.0 - tracedShare;
			}
		}
		else
		{
			tracedShare = TracedShare(roughness, strength);
			bool valid = false;
			float3 traced = tracedShare > 0.0 ? TracedReflection(position, surface.geometryNormal, reflected, valid) : 0.0;
			if (valid)
				environmentRadiance = lerp(environmentRadiance, traced, tracedShare);
			else
				tracedShare = 0.0;
		}
	}
#endif
	if (debugView == DEBUG_VIEW_TRACED_REFLECTIONS)
		return tracedShare > 0.0 ? (TracedGlossyHere() ? float3(0.0) : environmentRadiance) : float3(1.0, 0.0, 1.0);
	if (debugView == DEBUG_VIEW_INDIRECT_SPECULAR)
		return surface.occlusion * environmentSpecular * environmentRadiance;
	float3 diffuseEnvironment = surface.diffuseColor * (indirect.x >= 0.0 ? indirect : EnvironmentIrradiance(environment, normal));
	if (surface.diffuseTransmission > 0.0)
	{
		float3 behind = GiIrradiance(position, -normal);
		diffuseEnvironment = lerp(
			diffuseEnvironment,
			surface.diffuseTransmissionColor * (behind.x >= 0.0 ? behind : EnvironmentIrradiance(environment, -normal)),
			surface.diffuseTransmission);
	}
	float3 result = surface.occlusion * (diffuseEnvironment +
										 environmentSpecular * environmentRadiance);
	result += surface.transmitted * (1.0 - environmentSpecular);

	// the sheen over that, which takes its albedo of it, lit by the environment prefiltered for its lobe
	float sheenMax = max(surface.sheenColor.r, max(surface.sheenColor.g, surface.sheenColor.b));
	float sheenAlbedoView = SheenAlbedo(nDotV, surface.sheenRoughness);
	if (sheenMax > 0.0)
		result = result * (1.0 - sheenMax * sheenAlbedoView) +
				 surface.occlusion * surface.sheenColor * sheenAlbedoView *
					 EnvironmentSheenRadiance(environment, reflect(-view, normal), surface.sheenRoughness);

	// and the clearcoat over all of it
	float coatFresnel = ClearcoatFresnel(surface, view);
	float coatNDotV = max(dot(surface.clearcoatNormal, view), 1e-4);
	float coatAlpha = max(surface.clearcoatRoughness * surface.clearcoatRoughness, 1e-3);
	if (surface.clearcoat > 0.0)
	{
		float3 coatReflected = DominantDirection(surface.clearcoatNormal, reflect(-view, surface.clearcoatNormal), surface.clearcoatRoughness);
		float3 coatRadiance = EnvironmentRadiance(environment, coatReflected, surface.clearcoatRoughness);
		// traced as the base's reflection is (a car's paint: a smooth coat over a rougher base)
#if BVH_RAY_QUERY
		float coatStrength = surface.occlusion * surface.clearcoat *
							 EnvironmentBrdfApprox(float3(0.04), 1.0, surface.clearcoatRoughness, coatNDotV).g;
		float coatTraced = TracedReflectionsOn() ? TracedShare(surface.clearcoatRoughness, coatStrength) : 0.0;
		bool coatValid = false;
		float3 coatTracedRadiance = coatTraced > 0.0 ? TracedReflection(position, surface.geometryNormal, coatReflected, coatValid) : 0.0;
		if (coatValid)
			coatRadiance = lerp(coatRadiance, coatTracedRadiance, coatTraced);
#endif
		result = result * (1.0 - coatFresnel) +
				 surface.occlusion * surface.clearcoat * EnvironmentBrdfApprox(float3(0.04), 1.0, surface.clearcoatRoughness, coatNDotV) *
					 coatRadiance;
	}
	if (debugView == DEBUG_VIEW_INDIRECT)
		return result;
	if (debugView == DEBUG_VIEW_DIRECT)
		result = 0.0;
	// the emitters' light on the diffuse, as the indirect light's
	if (emitted.x >= 0.0)
		result += surface.diffuseColor * emitted;

	uint clusterFirst;
	uint clusterCount;
	bool clustered = ClusterLights(position, clusterFirst, clusterCount);
	// the debug view of the lights looped over: a heat map of their count, by its log (1 a dark blue, 15 cyan to green,
	// 255 and more red)
	if (debugView == DEBUG_VIEW_LIGHT_COUNT)
		return clusterCount == 0u ? float3(0.0) : DebugHeat(log2(1.0 + float(clusterCount)) / 8.0);
	// the debug view of the shadows: of the direct light that would reach the point unshadowed (each light's luminance,
	// falloff and n.l), the share that does
	if (debugView == DEBUG_VIEW_SHADOWS)
	{
		float reaching = 0.0;
		float unshadowed = 0.0;
		for (uint clusterIt = 0; clusterIt < clusterCount; clusterIt++)
		{
			uint lightIt = clustered ? gClusterLights[gPushConstants.frameIndex][clusterFirst + clusterIt] : clusterIt;
			LightData light = gLights[lightIt];
			float3 toLight;
			float weight = LightFalloff(light, position, toLight) * saturate(dot(normal, toLight)) *
						   dot(light.intensity.rgb, float3(0.2126, 0.7152, 0.0722));
			if (weight <= 0.0)
				continue;
			unshadowed += weight;
			reaching += weight * dot(ShadowFactor(lightIt, light, position, surface.geometryNormal, toLight), float3(0.2126, 0.7152, 0.0722));
		}
		// no light faces the point: a dark blue, apart from shadowed black
		return unshadowed > 0.0 ? pow(reaching / unshadowed, 2.2) : float3(0.0, 0.0, 0.05);
	}
	for (uint clusterIt = 0; clusterIt < clusterCount; clusterIt++)
	{
		uint lightIt = clustered ? gClusterLights[gPushConstants.frameIndex][clusterFirst + clusterIt] : clusterIt;
		LightData light = gLights[lightIt];
		float3 toLight;
		float3 attenuation = LightAttenuation(lightIt, light, position, surface.geometryNormal, toLight);
		if (all(attenuation <= 0.0))
			continue;

		float3 halfway = normalize(toLight + view);
		float vDotH = saturate(dot(view, halfway));
		float3 lit = 0.0;
		float nDotL = dot(normal, toLight);
		if (nDotL > 0.0)
		{
			float nDotH = saturate(dot(normal, halfway));
			lit = BaseLobe(surface, view, toLight, nDotL);
			// KHR_materials_retroreflection (the minimal retroreflective microfacet model): the same lobe seen from the
			// view mirrored about the normal, which peaks where the light is the viewer
			if (surface.retroreflection > 0.0)
				lit = lerp(lit, BaseLobe(surface, retroView, toLight, nDotL), surface.retroreflection);
			lit *= nDotL;
			if (sheenMax > 0.0)
			{
				float scaling = min(1.0 - sheenMax * sheenAlbedoView, 1.0 - sheenMax * SheenAlbedo(nDotL, surface.sheenRoughness));
				lit = lit * scaling + surface.sheenColor * DistributionCharlie(nDotH, surface.sheenRoughness) *
										  VisibilitySheen(nDotL, nDotV, surface.sheenRoughness) * nDotL;
			}
		}
		// KHR_materials_diffuse_transmission: lights behind the surface shine through its diffuse
		if (surface.diffuseTransmission > 0.0 && nDotL < 0.0)
			lit += surface.diffuseTransmission * surface.diffuseTransmissionColor / kPi * -nDotL;
		float coatNDotL = dot(surface.clearcoatNormal, toLight);
		if (surface.clearcoat > 0.0)
		{
			lit *= 1.0 - coatFresnel;
			if (coatNDotL > 0.0)
				lit += surface.clearcoat * FresnelSchlick(float3(0.04), 1.0, vDotH) *
					   DistributionGgx(saturate(dot(surface.clearcoatNormal, halfway)), coatAlpha) *
					   VisibilitySmithGgxCorrelated(coatNDotL, coatNDotV, coatAlpha) * coatNDotL;
		}
		result += lit * light.intensity.rgb * attenuation;
	}

	return result;
}
