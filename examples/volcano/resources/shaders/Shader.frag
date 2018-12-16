#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(constant_id = 0) const uint c_alphaTestMethod = 0;
layout(constant_id = 1) const float c_alphaTestRef = 0.5;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D texSampler;

void main()
{
	if (c_alphaTestMethod == 0)
	{
    	outColor = texture(texSampler, fragTexCoord);
	}
	else if (c_alphaTestMethod == 1)
	{
		vec4 tex = texture(texSampler, fragTexCoord);
		if (tex.a < c_alphaTestRef)
			discard;
		
		outColor = tex;
	}
}
