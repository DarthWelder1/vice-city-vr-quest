#version 450

#include "rw_common.glsl"

// Alpha test is a specialisation constant rather than a uniform branch. On a
// tiler a discard in the shader disables early-Z for the whole draw, so the
// opaque world pass must compile to a variant that contains no discard at all.
layout(constant_id = 0) const int RW_ALPHA_TEST = 0;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in float fragFog;
layout(location = 3) flat in vec2 fragDynamicMotion;

layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outDynamicMotion;

void main()
{
	// Untextured materials bind a 1x1 white texture, so there is no separate
	// untextured variant to compile or select.
	vec4 colour = fragColour * texture(diffuseTexture, fragTexCoord);

	if(RW_ALPHA_TEST == 1){
		if(colour.a < push.surfaceProps.w)
			discard;
	}

	colour.rgb = mix(scene.fogColour.rgb, colour.rgb, fragFog);
	outColour = colour;
	// Compact dynamic-vector transport. The post pass divides by the same gain.
	outDynamicMotion = clamp(fragDynamicMotion*8.0, vec2(-1.0), vec2(1.0));
}
