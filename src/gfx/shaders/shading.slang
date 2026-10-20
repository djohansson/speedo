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
	if (GiDebug() != 0u)
	{
		float3 debug = GiIrradiance(position, normal);
		return debug.x >= 0.0 ? debug : float3(1.0, 0.0, 1.0);
	}
	GiSample gi = GiSampleAt(position, normal);
	float3 indirect = gi.valid ? GiEvaluate(gi, normal) : -1.0;
	// and the specular: the environment as much as the indirect light sees of it (its share of the environment's
	// irradiance, which is all of it unoccluded), and, as the surface gets rough, the indirect light's radiance around the
	// reflection (its harmonics along it: a cosine lobe, as wide as the roughest): glossy reflections of the scene, without
	// the sky reflected inside a building
	if (indirect.x >= 0.0)
	{
		float3 luminance = float3(0.2126, 0.7152, 0.0722);
		float seen = saturate(dot(indirect, luminance) / max(dot(EnvironmentIrradiance(environment, normal), luminance), 1e-4));
		float rough = smoothstep(0.2, 0.9, roughness);
		environmentRadiance = lerp(environmentRadiance * seen, GiEvaluate(gi, reflected), rough);
	}
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
		result = result * (1.0 - coatFresnel) +
				 surface.occlusion * surface.clearcoat * EnvironmentBrdfApprox(float3(0.04), 1.0, surface.clearcoatRoughness, coatNDotV) *
					 EnvironmentRadiance(
						 environment,
						 DominantDirection(surface.clearcoatNormal, reflect(-view, surface.clearcoatNormal), surface.clearcoatRoughness),
						 surface.clearcoatRoughness);

	uint clusterFirst;
	uint clusterCount;
	bool clustered = ClusterLights(position, clusterFirst, clusterCount);
	for (uint clusterIt = 0; clusterIt < clusterCount; clusterIt++)
	{
		uint lightIt = clustered ? gClusterLights[gPushConstants.frameIndex][clusterFirst + clusterIt] : clusterIt;
		LightData light = gLights[lightIt];
		float3 toLight;
		float attenuation = LightAttenuation(lightIt, light, position, surface.geometryNormal, toLight);
		if (attenuation <= 0.0)
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
