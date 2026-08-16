#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

// Immediate-mode 3D: water, glass, rain, skidmarks, coronas, tyre marks and the
// VR hands all arrive through this path. Vertices are in world space already
// (im3DTransform applies the object matrix on the CPU), so only the per-eye
// view-projection is applied here.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out float fragFog;
layout(location = 3) flat out vec2 fragDynamicMotion;

void main()
{
	mat4 model = RwModelMatrix();
	vec4 world = model * vec4(inPosition, 1.0);
	gl_Position = scene.viewProj[gl_ViewIndex] * world;
	vec4 rootMotion = RwRootScreenMotion();
	fragDynamicMotion = gl_ViewIndex == 0 ? rootMotion.xy : rootMotion.zw;
	fragColour = inColour * push.materialColour;
	fragTexCoord = inTexCoord;
	fragFog = scene.fogParams.w > 0.5 ?
		clamp((scene.fogParams.y - gl_Position.w) * scene.fogParams.z, 0.0, 1.0) :
		1.0;
}
