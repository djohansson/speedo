// which lights reach a point (its cluster's, see SHADER_TYPES_CLUSTER_COUNT) and how much of each: LightAttenuation. a part of shaders.slang, which includes it after capi.h and sky.h

// the direction from position towards a light (toLight), and how much of its intensity reaches there: by distance (inverse
// square, windowed to 0 at the range, KHR_lights_punctual's recommendation), a spot light's cone, and its shadows
// the lights that may reach position, in the view shaded (gShadingViewId): its cluster's, as a range of gClusterLights (true),
// or all of gLights if the view has no clusters (false: first 0, count the light count)
bool ClusterLights(float3 position, out uint first, out uint count)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint viewId = gShadingViewId;
	ClusterView cluster = gClusterViews[frameIndex][viewId];
	first = 0;
	count = gPushConstants.lightCount;
	if (cluster.enabled == 0)
		return false;

	float4 clip = mul(gViewData[frameIndex][viewId].viewProjection, float4(position, 1.0));
	float2 tiles = float2(SHADER_TYPES_CLUSTER_TILES_X, SHADER_TYPES_CLUSTER_TILES_Y);
	uint2 tile = uint2(clamp(floor((clip.xy / clip.w * 0.5 + 0.5) * tiles), 0.0, tiles - 1.0));
	float distance = max(dot(cluster.depthPlane.xyz, position) + cluster.depthPlane.w, 0.0);
	uint slice = uint(clamp(floor(log2(1.0 + distance * cluster.depthInverseUnit) * cluster.depthScale), 0.0, float(SHADER_TYPES_CLUSTER_SLICES - 1)));
	uint2 range = gClusters[frameIndex][viewId * SHADER_TYPES_CLUSTER_COUNT +
										 (slice * SHADER_TYPES_CLUSTER_TILES_Y + tile.y) * SHADER_TYPES_CLUSTER_TILES_X + tile.x];
	first = range.x;
	count = range.y;
	return true;
}

float3 LightAttenuation(uint lightIndex, LightData light, float3 position, float3 geometryNormal, out float3 toLight)
{
	float attenuation = LightFalloff(light, position, toLight);
	if (attenuation <= 0.0)
		return 0.0;
	return attenuation * ShadowFactor(lightIndex, light, position, geometryNormal, toLight);
}

// how much of a light's intensity the fog near the ground lets through to position (see AerialPerspectiveData::fog, where
// the air is drawn), toward it (toLight): a directional light's through the layer above, a point or spot light's along
// the way from it
float LightFog(LightData light, float3 position, float3 toLight)
{
	AerialPerspectiveData data = gAerialPerspectiveData[gPushConstants.frameIndex][0];
	if (data.enabled == 0u || data.fog.x <= 0.0)
		return 1.0;
	float altitude = AerialPerspectiveAltitude(data, position.y);
	if (light.type == LIGHT_TYPE_DIRECTIONAL)
		return exp(-FogDepthToSky(data, altitude, toLight.y));
	float km = distance(light.positionRange.xyz, position) * data.metersPerUnit * 1e-3;
	return exp(-FogDepthAlong(data, altitude, AerialPerspectiveAltitude(data, light.positionRange.y), km));
}

// the direction from position towards a light (toLight), and how much of its intensity reaches there unshadowed: by
// distance and a spot light's cone, through the fog near the ground (LightFog)
float LightFalloff(LightData light, float3 position, out float3 toLight)
{
	float attenuation = 1.0;
	if (light.type == LIGHT_TYPE_DIRECTIONAL)
	{
		toLight = -light.direction.xyz;
	}
	else
	{
		float3 offset = light.positionRange.xyz - position;
		float distance2 = max(dot(offset, offset), 1e-8);
		toLight = offset * rsqrt(distance2);
		// the inverse square in meters (candela over meters squared): the scene's unit, View > Units, see
		// PushConstants::metersPerUnit; the range stays the file's units, as the position is
		float metersPerUnit = gPushConstants.metersPerUnit;
		attenuation = 1.0 / max(distance2 * metersPerUnit * metersPerUnit, 1e-10);
		if (light.positionRange.w > 0.0)
		{
			float ratio = sqrt(distance2) / light.positionRange.w;
			float window = saturate(1.0 - ratio * ratio * ratio * ratio);
			attenuation *= window * window;
		}
		if (light.type == LIGHT_TYPE_SPOT)
		{
			float cone = saturate(dot(light.direction.xyz, -toLight) * light.spotScale + light.spotOffset);
			attenuation *= cone * cone;
		}
	}
	return attenuation > 0.0 ? attenuation * LightFog(light, position, toLight) : 0.0;
}
