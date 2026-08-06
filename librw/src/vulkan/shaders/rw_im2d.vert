#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

// Immediate-mode 2D: HUD, fonts, menus. The game submits vertices already in
// screen space, as the fixed-function pipeline expected.
//
// They cannot simply be written out as clip coordinates. Doing that puts the
// same NDC position in both eyes, and because the Quest's per-eye frustums are
// asymmetric, one NDC position is a different direction in each eye -- the two
// images pull apart and never converge.
//
// Instead the screen plane is placed in the world in front of the head by
// scene.im2dTransform, and each eye projects it with its own matrix. The
// result is a stable panel at a comfortable distance rather than something the
// eyes cannot fuse.

// x, y screen; z screen depth; w CAMERA Z IN METRES -- Im2DVertex's
// setRecipCameraZ stores the reciprocal of what it is handed, so this field
// always ends up holding the real distance, never its reciprocal.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;

void main()
{
	// im2dTransform places the screen plane at scene.im2dParams.x metres from
	// the eye. Every vertex carries its true camera-space depth as 1/w, so
	// scaling its offset from the eye along the same ray puts it where it
	// actually belongs: coronas and other world sprites land on their light
	// source instead of floating on the panel in front of the face, and sort
	// against the scene through the depth test.
	//
	// HUD and font vertices come from CSprite2d, whose depth is the near clip
	// plane; clamping to the panel distance leaves those exactly where they
	// already were.
	vec4 plane = scene.im2dTransform * vec4(inPosition.xy, 0.0, 1.0);
	float panel = scene.im2dParams.x;
	float depth = max(inPosition.w, panel);
	vec3 eye = scene.im2dParams.yzw;
	vec3 world = eye + (plane.xyz - eye) * (depth/panel);
	gl_Position = scene.viewProj[gl_ViewIndex] * vec4(world, 1.0);
	fragColour = inColour * push.materialColour;
	fragTexCoord = inTexCoord;
}
