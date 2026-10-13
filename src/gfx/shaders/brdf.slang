// the microfacet terms: fresnel, GGX, Smith, anisotropy, ior, iridescence and sheen. a part of shaders.slang, which includes it after capi.h and sky.h

// the glTF metallic-roughness brdf (the spec's appendix B): GGX distribution, height correlated Smith visibility, and
// Schlick's Fresnel, mixing a Lambertian diffuse and the specular by the Fresnel term
float3 FresnelSchlick(float3 f0, float f90, float cosine)
{
	return f0 + (f90 - f0) * pow(saturate(1.0 - cosine), 5.0);
}

float DistributionGgx(float nDotH, float alpha)
{
	float alpha2 = alpha * alpha;
	float d = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	return alpha2 / (kPi * d * d);
}

float VisibilitySmithGgxCorrelated(float nDotL, float nDotV, float alpha)
{
	float alpha2 = alpha * alpha;
	float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - alpha2) + alpha2);
	float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - alpha2) + alpha2);
	float sum = ggxV + ggxL;
	return sum > 0.0 ? 0.5 / sum : 0.0;
}

// the specular reflectance of a uniform environment, integrated over the hemisphere: Karis' analytic fit to the
// split sum's lookup table ("Physically Based Shading on Mobile")
float3 EnvironmentBrdfApprox(float3 f0, float f90, float roughness, float nDotV)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + f90 * ab.y;
}

// KHR_materials_anisotropy's GGX: the distribution and height correlated visibility with a roughness along t (alphaT)
// and along b (alphaB), as the Khronos sample viewer has them
float DistributionGgxAnisotropic(float nDotH, float tDotH, float bDotH, float alphaT, float alphaB)
{
	float a2 = alphaT * alphaB;
	float3 f = float3(alphaB * tDotH, alphaT * bDotH, a2 * nDotH);
	float w2 = a2 / dot(f, f);
	return a2 * w2 * w2 / kPi;
}

float VisibilityGgxAnisotropic(
	float nDotL, float nDotV, float tDotV, float bDotV, float tDotL, float bDotL, float alphaT, float alphaB)
{
	float ggxV = nDotL * length(float3(alphaT * tDotV, alphaB * bDotV, nDotV));
	float ggxL = nDotV * length(float3(alphaT * tDotL, alphaB * bDotL, nDotL));
	float sum = ggxV + ggxL;
	return sum > 0.0 ? 0.5 / sum : 0.0;
}

// KHR_materials_iridescence's thin film Fresnel (Belcour and Barla, "A Practical Extension to Microfacet Theory for the
// Modeling of Varying Iridescence"), as the Khronos sample viewer has it: the reflectance of a film of an ior and
// thickness (nanometers) over a base of f0, seen at cosine from outside (ior outsideIor)
float IorToFresnel0(float transmittedIor, float incidentIor)
{
	float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
	return r * r;
}

float3 IorToFresnel0(float3 transmittedIor, float incidentIor)
{
	float3 r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
	return r * r;
}

float3 Fresnel0ToIor(float3 fresnel0)
{
	float3 root = sqrt(fresnel0);
	return (1.0 + root) / (1.0 - root);
}

// the spectral sensitivity of an optical path difference (nanometers), as rgb (linear rec 709)
float3 IridescenceSensitivity(float opd, float3 shift)
{
	float phase = 2.0 * kPi * opd * 1.0e-9;
	const float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
	const float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
	const float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
	float3 xyz = val * sqrt(2.0 * kPi * var) * cos(pos * phase + shift) * exp(-(phase * phase) * var);
	xyz.x += 9.7470e-14 * sqrt(2.0 * kPi * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) * exp(-4.5282e+09 * phase * phase);
	xyz /= 1.0685e-7;
	return float3(
		dot(float3(3.2404542, -1.5371385, -0.4985314), xyz),
		dot(float3(-0.9692660, 1.8760108, 0.0415560), xyz),
		dot(float3(0.0556434, -0.2040259, 1.0572252), xyz));
}

float3 IridescenceFresnel(float outsideIor, float filmIor, float cosine, float thickness, float3 baseF0)
{
	// the film fades out to the outside's ior as it gets thinner
	float ior = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));
	float sinTheta2Sq = (outsideIor / ior) * (outsideIor / ior) * (1.0 - cosine * cosine);
	float cosTheta2Sq = 1.0 - sinTheta2Sq;
	if (cosTheta2Sq < 0.0) // total internal reflection
		return 1.0;
	float cosTheta2 = sqrt(cosTheta2Sq);

	// the first interface
	float r0 = IorToFresnel0(ior, outsideIor);
	float r12 = r0 + (1.0 - r0) * pow(saturate(1.0 - cosine), 5.0);
	float t121 = 1.0 - r12;
	float phi12 = ior < outsideIor ? kPi : 0.0;
	float phi21 = kPi - phi12;

	// the second
	float3 baseIor = Fresnel0ToIor(clamp(baseF0, 0.0, 0.9999));
	float3 r1 = IorToFresnel0(baseIor, ior);
	float3 r23 = r1 + (1.0 - r1) * pow(saturate(1.0 - cosTheta2), 5.0);
	float3 phi23 = select(baseIor < ior, float3(kPi), float3(0.0));

	// the phase shift, and the reflectance's terms
	float opd = 2.0 * ior * thickness * cosTheta2;
	float3 phi = phi21 + phi23;
	float3 r123 = clamp(r12 * r23, 1e-5, 0.9999);
	float3 rootR123 = sqrt(r123);
	float3 rs = t121 * t121 * r23 / (1.0 - r123);
	float3 result = r12 + rs;
	float3 cm = rs - t121;
	for (uint m = 1; m <= 2; m++)
	{
		cm *= rootR123;
		result += cm * 2.0 * IridescenceSensitivity(float(m) * opd, float(m) * phi);
	}
	return max(result, 0.0);
}

// KHR_materials_sheen's lobe: the Charlie distribution and Estevez and Kulla's visibility (as the Khronos sample viewer
// has them: interpolated by alpha, the visibility at most 1), of a roughness at least 0.07
float SheenAlpha(float roughness)
{
	float clamped = max(roughness, 0.07);
	return clamped * clamped;
}

float DistributionCharlie(float nDotH, float roughness)
{
	float alpha = SheenAlpha(roughness);
	float sin2 = max(1.0 - nDotH * nDotH, 0.0);
	return (2.0 + 1.0 / alpha) * pow(sin2, 0.5 / alpha) / (2.0 * kPi);
}

float SheenLambda(float cosine, float alpha)
{
	float t = (1.0 - alpha) * (1.0 - alpha);
	float a = lerp(25.3245, 21.5473, t);
	float b = lerp(3.32435, 3.82987, t);
	float c = lerp(0.16801, 0.19823, t);
	float d = lerp(-1.27393, -1.97760, t);
	float e = lerp(-4.85967, -4.32054, t);
	float x = cosine < 0.5 ? cosine : 0.5;
	float l = a / (1.0 + b * pow(x, c)) + d * x + e;
	if (cosine < 0.5)
		return exp(l);
	float x1 = 1.0 - cosine;
	return exp(2.0 * l - (a / (1.0 + b * pow(x1, c)) + d * x1 + e));
}

float VisibilitySheen(float nDotL, float nDotV, float roughness)
{
	float alpha = SheenAlpha(roughness);
	return saturate(1.0 / ((1.0 + SheenLambda(nDotV, alpha) + SheenLambda(nDotL, alpha)) * 4.0 * nDotV * nDotL));
}

// the sheen lobe's directional albedo (white): integrated numerically for cos 0 to 1 (columns) and roughness 0 to 1
// (rows), 12 each, at most 1 (grazing views of the smoothest sheens exceed it). what the layers below lose to it
static const float kSheenAlbedo[144] = {
	1.0000, 0.6230, 0.1468, 0.0241, 0.0024, 0.0001, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	1.0000, 0.8200, 0.2877, 0.0804, 0.0165, 0.0024, 0.0002, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	1.0000, 1.0000, 0.6423, 0.3549, 0.1788, 0.0833, 0.0355, 0.0135, 0.0042, 0.0010, 0.0001, 0.0000,
	1.0000, 0.9785, 0.6775, 0.4508, 0.2882, 0.1790, 0.1070, 0.0607, 0.0317, 0.0144, 0.0049, 0.0007,
	0.9801, 0.8316, 0.6178, 0.4494, 0.3218, 0.2274, 0.1573, 0.1061, 0.0682, 0.0404, 0.0205, 0.0070,
	0.7804, 0.6928, 0.5361, 0.4115, 0.3153, 0.2401, 0.1804, 0.1339, 0.0966, 0.0661, 0.0412, 0.0211,
	0.6296, 0.5733, 0.4550, 0.3625, 0.2906, 0.2324, 0.1844, 0.1456, 0.1131, 0.0850, 0.0601, 0.0381,
	0.5157, 0.4748, 0.3833, 0.3140, 0.2598, 0.2150, 0.1772, 0.1463, 0.1199, 0.0961, 0.0741, 0.0534,
	0.4310, 0.3971, 0.3244, 0.2714, 0.2298, 0.1949, 0.1652, 0.1410, 0.1202, 0.1012, 0.0831, 0.0652,
	0.3704, 0.3396, 0.2801, 0.2379, 0.2049, 0.1771, 0.1532, 0.1341, 0.1178, 0.1030, 0.0886, 0.0741,
	0.3306, 0.3019, 0.2510, 0.2156, 0.1880, 0.1648, 0.1448, 0.1291, 0.1161, 0.1044, 0.0931, 0.0817,
	0.3104, 0.2841, 0.2381, 0.2064, 0.1817, 0.1609, 0.1431, 0.1294, 0.1183, 0.1086, 0.0994, 0.0902,
};

float SheenAlbedo(float cosine, float roughness)
{
	float2 at = saturate(float2(cosine, roughness)) * 11.0;
	uint2 low = min(uint2(at), uint2(10, 10));
	float2 t = at - float2(low);
	float a = lerp(kSheenAlbedo[low.y * 12 + low.x], kSheenAlbedo[low.y * 12 + low.x + 1], t.x);
	float b = lerp(kSheenAlbedo[(low.y + 1) * 12 + low.x], kSheenAlbedo[(low.y + 1) * 12 + low.x + 1], t.x);
	return lerp(a, b, t.y);
}
