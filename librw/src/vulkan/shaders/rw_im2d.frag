#version 450

#include "rw_common.glsl"

layout(constant_id = 0) const int RW_ALPHA_TEST = 0;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColour;

void main()
{
	vec4 colour = fragColour * texture(diffuseTexture, fragTexCoord);
	if(RW_ALPHA_TEST == 1){
		if(colour.a < push.surfaceProps.w)
			discard;
	}
	// No fog on 2D: the HUD, fonts and menus are composited, not in the world.
	outColour = colour;
}
