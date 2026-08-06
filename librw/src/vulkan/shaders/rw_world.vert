#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out float fragFog;

// Lighting is evaluated per vertex, matching what the original fixed-function
// RenderWare pipeline did. Moving it to the fragment stage would look slightly
// better and cost noticeably more on a tiler at 1680x1760 per eye.
void main()
{
	vec4 world = push.model * vec4(inPosition, 1.0);
	gl_Position = scene.viewProj[gl_ViewIndex] * world;

	// RenderWare's fixed-function sum: prelight, plus ambient scaled by the
	// material's ambient coefficient, plus the directional term -- additive,
	// not multiplied. Multiplying makes night scenes black: dark prelight
	// times anything stays dark, where the original adds the ambient on top.
	//
	// push.model carries the game camera as well as the object transform, so
	// the normal lands in play space; the light direction was rotated into the
	// same space on the CPU, keeping the dot product honest.
	vec3 normal = normalize(mat3(push.model) * inNormal);
	vec3 colour = inColour.rgb + push.ambientLight.rgb * push.surfaceProps.x;
	vec4 dirColour = unpackUnorm4x8(floatBitsToUint(push.lightDirColour.w));
	colour += dirColour.rgb *
	          max(dot(normal, -push.lightDirColour.xyz), 0.0) *
	          push.surfaceProps.y;

	fragColour = vec4(clamp(colour, 0.0, 1.0), inColour.a) * push.materialColour;
	fragTexCoord = inTexCoord;

	// For a standard projection the clip w is the view-space depth, so no
	// separate view transform is needed just to fog.
	fragFog = scene.fogParams.w > 0.5 ?
		clamp((scene.fogParams.y - gl_Position.w) * scene.fogParams.z, 0.0, 1.0) :
		1.0;
}
