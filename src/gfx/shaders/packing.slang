// compact encodings: colors as R11G11B10, directions as signed octahedral. a part of shaders.slang, which includes it after capi.h and sky.h

// non-negative colors as R11G11B10 floats (half floats without their sign and lowest mantissa bits)
uint PackR11G11B10(float3 color)
{
	uint3 bits = f32tof16(clamp(color, 0.0, 65000.0));
	return ((bits.r >> 4) & 0x7ffu) | (((bits.g >> 4) & 0x7ffu) << 11) | (((bits.b >> 5) & 0x3ffu) << 22);
}

float3 UnpackR11G11B10(uint packed)
{
	return f16tof32(uint3((packed & 0x7ffu) << 4, ((packed >> 11) & 0x7ffu) << 4, ((packed >> 22) & 0x3ffu) << 5));
}

float3 SignedOctEncode(float3 n)
{
	float3 OutN;

	n /= (abs(n.x) + abs(n.y) + abs(n.z));

	OutN.y = n.y * 0.5 + 0.5;
	OutN.x = n.x * 0.5 + OutN.y;
	OutN.y = n.x * -0.5 + OutN.y;
	OutN.z = saturate(n.z*FLT_MAX);

	return OutN;
}

float3 SignedOctDecode(float3 n)
{
	float3 OutN;

	OutN.x = (n.x - n.y);
	OutN.y = (n.x + n.y) - 1.0;
	OutN.z = n.z * 2.0 - 1.0;
	OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));
 
	return normalize(OutN);
}
