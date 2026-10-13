// tonemapping and the srgb curves. a part of shaders.slang, which includes it after capi.h and sky.h

// Khronos PBR Neutral: leaves colors up to about 0.76 nearly as they are (base colors render as themselves under a
// white light of 1), and compresses brighter ones towards white, desaturating them
float3 TonemapPbrNeutral(float3 color)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;

	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;

	float peak = max(color.r, max(color.g, color.b));
	if (peak < startCompression)
		return color;

	const float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	color *= newPeak / peak;

	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return lerp(color, newPeak * float3(1.0), g);
}

float3 ApplySRGBCurve(float3 x)
{
	// Approximately pow(x, 1.0 / 2.2)
	return select(x < 0.0031308, 12.92 * x, 1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float3 RemoveSRGBCurve(float3 x)
{
	// Approximately pow(x, 2.2)
	return select(x < 0.04045, x / 12.92, pow((x + 0.055) / 1.055, 2.4));
}

// ACES filmic, Stephen Hill's fit of the RRT and ODT (linear rec 709 in and out)
float3 TonemapAces(float3 color)
{
	const float3x3 inputMatrix = float3x3(
		0.59719, 0.35458, 0.04823,
		0.07600, 0.90834, 0.01566,
		0.02840, 0.13383, 0.83777);
	const float3x3 outputMatrix = float3x3(
		1.60475, -0.53108, -0.07367,
		-0.10208, 1.10813, -0.00605,
		-0.00327, -0.07276, 1.07602);
	float3 v = mul(inputMatrix, color);
	float3 a = v * (v + 0.0245786) - 0.000090537;
	float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
	return mul(outputMatrix, a / b);
}

// AgX's base look, Benjamin Wrensch's sixth order polynomial fit of its sigmoid (linear rec 709 in and out)
float3 TonemapAgx(float3 color)
{
	const float3x3 inset = float3x3(
		0.842479062253094, 0.0784335999999992, 0.0792237451477643,
		0.0423282422610123, 0.878468636469772, 0.0791661274605434,
		0.0423756549057051, 0.0784336, 0.879142973793104);
	const float3x3 outset = float3x3(
		1.19687900512017, -0.0980208811401368, -0.0990297440797205,
		-0.0528968517574562, 1.15190312990417, -0.0989611768448433,
		-0.0529716355144438, -0.0980434501171241, 1.15107367264116);
	const float minEv = -12.47393;
	const float maxEv = 4.026069;

	float3 v = mul(inset, max(color, 1e-10));
	v = saturate((log2(v) - minEv) / (maxEv - minEv));
	float3 v2 = v * v;
	float3 v4 = v2 * v2;
	v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
	v = mul(outset, v);
	// the fit's output is display encoded (a 2.2 gamma): back to linear, for the srgb curve after
	return pow(max(v, 0.0), 2.2);
}

// the tonemapper PushConstants::tonemapper names, of an exposed linear color
float3 Tonemap(float3 color)
{
	switch (gPushConstants.tonemapper)
	{
	case TONEMAPPER_ACES: return TonemapAces(color);
	case TONEMAPPER_AGX: return TonemapAgx(color);
	case TONEMAPPER_REINHARD:
	{
		float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
		return color / (1.0 + luminance);
	}
	case TONEMAPPER_LINEAR: return color;
	default: return TonemapPbrNeutral(color);
	}
}
