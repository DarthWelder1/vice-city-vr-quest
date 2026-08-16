#version 450

#include "rw_common.glsl"

layout(constant_id = 0) const int RW_ALPHA_TEST = 0;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in float fragWorldSprite;

layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outDynamicMotion;

void main()
{
	vec4 colour = fragColour * texture(diffuseTexture, fragTexCoord);
	if(RW_ALPHA_TEST == 1){
		if(colour.a < push.surfaceProps.w)
			discard;
	}
	// No fog on 2D: the HUD, fonts and menus are composited, not in the world.
	outColour = colour;
	// The Im2D pipeline disables attachment-1 writes. Keep a conventional zero
	// output for interface compatibility; it never reaches the motion image.
	outDynamicMotion = vec2(0.0);
}
