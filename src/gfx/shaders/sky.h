// the procedural sky's gradient (without its sun), shared by the shader modules that evaluate it: what
// gfx::environment::SkyRadiance computes on the cpu, from EnvironmentData's sky fields. d: a unit direction in the
// panorama's space
#pragma once

float3 SkyGradient(EnvironmentData environment, float3 d)
{
	// the sky fades from the horizon up, the ground darkens towards the nadir, with a soft edge between them
	float3 above = lerp(environment.skyHorizon.rgb, environment.skyZenith.rgb, sqrt(max(d.y, 0.0)));
	float3 below = environment.skyGround.rgb * (1.0 - 0.4 * sqrt(max(-d.y, 0.0)));
	return lerp(below, above, smoothstep(-0.02, 0.02, d.y));
}
