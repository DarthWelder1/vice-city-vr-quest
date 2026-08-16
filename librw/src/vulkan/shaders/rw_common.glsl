// Shared uniform layout for the librw Vulkan backend.
//
// Set 0 is per-frame scene data, written once by the VR layer. Set 1 is the
// material texture. Everything that changes per draw travels in push constants,
// which keeps descriptor churn out of the world pass entirely: Vice City issues
// thousands of small draws per frame and a descriptor update per draw would
// dominate the CPU cost on this hardware.

#define RW_MAX_LIGHTS 8
// Must match RW_MAX_BONES in rwvkimpl.h: RenderWare's own ceiling.
#define RW_MAX_BONES 64

layout(set = 0, binding = 0) uniform SceneData {
	// Indexed by gl_ViewIndex. Both entries hold the same matrix in mono.
	// Play space (OpenXR, metres, Y up) to clip.
	//
	// World geometry does not reach this space by itself: RenderWare is Z up
	// with a mirrored X, and the viewer stands wherever the game camera says.
	// That transform is folded into push.model per draw rather than kept here,
	// because the game runs more than one camera per frame and every draw in a
	// frame reads this block at the value it holds when the frame is submitted.
	mat4 viewProj[2];
	mat4 previousViewProj[2];
	vec4 fogColour;
	// x = start, y = end, z = 1/(end-start), w = enabled
	vec4 fogParams;
	vec4 ambient;
	// xyz = direction (pointing from the surface toward the light), w = unused
	vec4 lightDirection[RW_MAX_LIGHTS];
	vec4 lightColour[RW_MAX_LIGHTS];
	// x = active directional light count
	vec4 lightCount;
	// Places the screen-space Im2D plane in the world, in front of the head.
	// Both eyes then project it through their own matrix, which is what makes
	// the HUD and menus converge; writing identical clip coordinates to both
	// eyes does not, because the two frustums are asymmetric.
	mat4 im2dTransform;
	// x = distance the Im2D plane sits at, yzw = eye position in play space.
	// Together they turn a screen vertex plus its own camera depth back into
	// a world point; see rw_im2d.vert.
	vec4 im2dParams;
} scene;

layout(push_constant) uniform PushConstants {
	mat4 model;
	// Material colour modulated with the vertex colour.
	vec4 materialColour;
	// x = ambient, y = diffuse, z = unused, w = alpha test reference
	vec4 surfaceProps;
	// rgb = world ambient, zero for geometry lit purely by prelight.
	vec4 ambientLight;
	// xyz = directional light direction in play space, w = colour as RGBA8.
	vec4 lightDirColour;
} push;

// In MOTION DEBUG the otherwise constant affine W row carries the previous
// root-translation delta and a validity flag. Reconstruct the actual affine
// matrix before transforming vertices. This keeps PushConstants at the
// Vulkan-guaranteed 128 bytes instead of adding another per-draw buffer.
mat4 RwModelMatrix()
{
	mat4 model = push.model;
	model[0].w = 0.0;
	model[1].w = 0.0;
	model[2].w = 0.0;
	model[3].w = 1.0;
	return model;
}

vec4 RwRootScreenMotion()
{
	return vec4(push.model[0].w, push.model[1].w,
	            push.model[2].w, push.model[3].w);
}
