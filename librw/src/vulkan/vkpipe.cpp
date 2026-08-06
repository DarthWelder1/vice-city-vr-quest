#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN

#include <android/log.h>
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, "librw-vk", __VA_ARGS__)

namespace rw {
namespace vulkan {

void
destroyStaticBuffer(VkBuffer &buffer, VkDeviceMemory &memory)
{
	if(buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(gvk.device, buffer, nil);
	if(memory != VK_NULL_HANDLE)
		vkFreeMemory(gvk.device, memory, nil);
	buffer = VK_NULL_HANDLE;
	memory = VK_NULL_HANDLE;
}

// Uploads through a temporary staging buffer into device-local memory. World
// geometry is written once and drawn for the lifetime of the model, so the
// staging cost is paid back immediately by keeping the vertex fetch on fast
// memory.
bool32
uploadStaticBuffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkBuffer *bufferOut, VkDeviceMemory *memoryOut)
{
	if(!createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufferOut, memoryOut))
		return 0;

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if(!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &staging, &stagingMemory)){
		destroyStaticBuffer(*bufferOut, *memoryOut);
		return 0;
	}

	void *mapped = nil;
	vkMapMemory(gvk.device, stagingMemory, 0, size, 0, &mapped);
	memcpy(mapped, data, (size_t)size);
	vkUnmapMemory(gvk.device, stagingMemory);

	VkCommandBuffer commandBuffer = beginOneShot();
	if(commandBuffer != VK_NULL_HANDLE){
		VkBufferCopy copy = {};
		copy.size = size;
		vkCmdCopyBuffer(commandBuffer, staging, *bufferOut, 1, &copy);
		endOneShot(commandBuffer);
	}

	vkDestroyBuffer(gvk.device, staging, nil);
	vkFreeMemory(gvk.device, stagingMemory, nil);
	return 1;
}

namespace {

InstanceDataHeader *
instanceMesh(rw::ObjPipeline *, Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1,
	                                    MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_VULKAN;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ?
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->inst = rwNewT(InstanceData, header->numMeshes,
	                      MEMDUR_EVENT | ID_GEOMETRY);

	header->indexBuffer = rwNewT(uint16, header->totalNumIndex,
	                             MEMDUR_EVENT | ID_GEOMETRY);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->offset = offset;
		memcpy((uint8*)header->indexBuffer + inst->offset,
		       mesh->indices, inst->numIndex*2);
		offset += inst->numIndex*2;
		mesh++;
		inst++;
	}

	header->vertexBuffer = nil;
	header->vbo = VK_NULL_HANDLE;
	header->vboMemory = VK_NULL_HANDLE;
	header->ibo = VK_NULL_HANDLE;
	header->iboMemory = VK_NULL_HANDLE;

	if(header->totalNumIndex > 0)
		uploadStaticBuffer(header->indexBuffer, header->totalNumIndex*2,
		             VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		             &header->ibo, &header->iboMemory);

	return header;
}

void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if(geo->flags & Geometry::NATIVE)
		return;

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		if(header->serialNumber != geo->meshHeader->serialNum)
			freeInstanceData(geo);
	}

	if(geo->instData == nil){
		geo->instData = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
	}else if(geo->lockedSinceInst)
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);

	geo->lockedSinceInst = 0;
}

void
uninstance(rw::ObjPipeline *, Atomic *)
{
	assert(0 && "can't uninstance");
}

void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	pipe->instance(atomic);
	if(geo->instData == nil || geo->instData->platform != PLATFORM_VULKAN)
		return;
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

} // namespace

void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_VULKAN)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;

	if(gvk.device != VK_NULL_HANDLE){
		destroyStaticBuffer(header->vbo, header->vboMemory);
		destroyStaticBuffer(header->ibo, header->iboMemory);
	}
	rwFree(header->indexBuffer);
	rwFree(header->vertexBuffer);
	rwFree(header->inst);
	rwFree(header);
}

void *
destroyNativeGeometryData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	const bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	const bool hasNormals = !!(geo->flags & Geometry::NORMALS);
	const bool hasTexCoords = geo->numTexCoordSets > 0;

	const VkDeviceSize size =
		(VkDeviceSize)header->totalNumVertex * sizeof(WorldVertex);

	if(!reinstance){
		header->vertexBuffer = rwNewT(uint8, (uint32)size,
		                              MEMDUR_EVENT | ID_GEOMETRY);
	}else{
		// A reinstance rewrites the whole interleaved block. Doing it
		// wholesale is cheaper than tracking which of the four attributes
		// changed, because they share cache lines in this layout anyway.
		destroyStaticBuffer(header->vbo, header->vboMemory);
	}

	WorldVertex *vertices = (WorldVertex*)header->vertexBuffer;
	V3d *positions = geo->morphTargets[0].vertices;
	V3d *normals = geo->morphTargets[0].normals;
	TexCoords *texCoords = hasTexCoords ? geo->texCoords[0] : nil;
	RGBA *colours = isPrelit ? geo->colors : nil;

	for(uint32 i = 0; i < header->totalNumVertex; i++){
		WorldVertex &v = vertices[i];
		v.position[0] = positions[i].x;
		v.position[1] = positions[i].y;
		v.position[2] = positions[i].z;

		if(hasNormals && normals != nil){
			v.normal[0] = normals[i].x;
			v.normal[1] = normals[i].y;
			v.normal[2] = normals[i].z;
		}else{
			// Unlit geometry still runs the lit shader; an up-facing normal
			// keeps it from going black rather than adding a shader variant.
			v.normal[0] = 0.0f;
			v.normal[1] = 0.0f;
			v.normal[2] = 1.0f;
		}

		if(colours != nil){
			v.colour[0] = colours[i].red;
			v.colour[1] = colours[i].green;
			v.colour[2] = colours[i].blue;
			v.colour[3] = colours[i].alpha;
		}else if(geo->flags & Geometry::LIGHT){
			// The vertex colour is the prelight term of RenderWare's additive
			// sum. Lit geometry without prelight has no emissive contribution
			// -- black -- and takes its brightness from ambient + directional.
			v.colour[0] = v.colour[1] = v.colour[2] = 0x00;
			v.colour[3] = 0xFF;
		}else{
			// Neither prelit nor lit: fixed function draws it full bright.
			// Cutscene character models are exactly this case; a black base
			// here renders them as silhouettes.
			v.colour[0] = v.colour[1] = v.colour[2] = 0xFF;
			v.colour[3] = 0xFF;
		}

		if(texCoords != nil){
			v.texCoord[0] = texCoords[i].u;
			v.texCoord[1] = texCoords[i].v;
		}else{
			v.texCoord[0] = 0.0f;
			v.texCoord[1] = 0.0f;
		}
	}

	// Per-mesh vertex alpha, used by the sorting code to decide whether a mesh
	// needs the transparent pass.
	if(isPrelit){
		InstanceData *inst = header->inst;
		for(uint32 m = 0; m < header->numMeshes; m++, inst++){
			inst->vertexAlpha = 0;
			for(int32 i = 0; i < inst->numVertices; i++)
				if(vertices[inst->minVert + i].colour[3] != 0xFF){
					inst->vertexAlpha = 1;
					break;
				}
		}
	}

	if(size > 0)
		uploadStaticBuffer(header->vertexBuffer, size,
		             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		             &header->vbo, &header->vboMemory);
}

void
defaultUninstanceCB(Geometry *, InstanceDataHeader *)
{
	assert(0 && "can't uninstance");
}

// Shared by the default and skin pipelines: same material walk, same state,
// only the shader variant and the extra bone descriptor differ. boneOffset is
// nil for unskinned geometry.
void
drawAtomicMeshes(Atomic *atomic, InstanceDataHeader *header, uint32 shader,
                 const uint32 *boneOffset)
{
	VkCommandBuffer commandBuffer = gvk.frameCommands;

	PushConstants push;
	Matrix *ltm = atomic->getFrame()->getLTM();
	// rw::Matrix is 3 rights/ups/ats plus a position, stored without the
	// fourth column; expand it into a column-major 4x4.
	float32 objectToWorld[16];
	objectToWorld[0]  = ltm->right.x; objectToWorld[1]  = ltm->right.y;
	objectToWorld[2]  = ltm->right.z; objectToWorld[3]  = 0.0f;
	objectToWorld[4]  = ltm->up.x;    objectToWorld[5]  = ltm->up.y;
	objectToWorld[6]  = ltm->up.z;    objectToWorld[7]  = 0.0f;
	objectToWorld[8]  = ltm->at.x;    objectToWorld[9]  = ltm->at.y;
	objectToWorld[10] = ltm->at.z;    objectToWorld[11] = 0.0f;
	objectToWorld[12] = ltm->pos.x;   objectToWorld[13] = ltm->pos.y;
	objectToWorld[14] = ltm->pos.z;   objectToWorld[15] = 1.0f;
	// Carry the current camera into the draw, so the shader's single per-eye
	// matrix is all that is left to apply. Done here rather than in the scene
	// block because the camera can change again before the frame is submitted.
	multiplyMatrix(push.model, gvk.worldToPlay, objectToWorld);

	// Lighting, resolved per atomic exactly as gl3's lightingCB does: world
	// geometry is unlit (prelight only), peds and vehicles take the world's
	// ambient plus its first directional -- Vice City registers exactly one.
	push.ambientLight[0] = push.ambientLight[1] = push.ambientLight[2] = 0.0f;
	push.ambientLight[3] = 1.0f;
	push.lightDirColour[0] = 0.0f;
	push.lightDirColour[1] = 0.0f;
	push.lightDirColour[2] = 1.0f;
	push.lightDirColour[3] = 0.0f;	// packed RGBA8 black: no directional
	Geometry *geometry = atomic->geometry;
	if((geometry->flags & Geometry::LIGHT) && engine->currentWorld != nil){
		Light *directionals[8];
		WorldLights lights;
		memset(&lights, 0, sizeof(lights));
		lights.directionals = directionals;
		lights.numDirectionals = 8;
		((World*)engine->currentWorld)->enumerateLights(atomic, &lights);

		push.ambientLight[0] = lights.ambient.red;
		push.ambientLight[1] = lights.ambient.green;
		push.ambientLight[2] = lights.ambient.blue;

		if(lights.numDirectionals > 0 && (geometry->flags & Geometry::NORMALS)){
			Light *sun = directionals[0];
			const V3d &at = sun->getFrame()->getLTM()->at;
			// The shader's normals are in play space (push.model includes the
			// camera), so the direction has to be rotated the same way or the
			// dot product compares vectors from two different spaces.
			const float32 *wp = gvk.worldToPlay;
			push.lightDirColour[0] = wp[0]*at.x + wp[4]*at.y + wp[8]*at.z;
			push.lightDirColour[1] = wp[1]*at.x + wp[5]*at.y + wp[9]*at.z;
			push.lightDirColour[2] = wp[2]*at.x + wp[6]*at.y + wp[10]*at.z;
			RGBAf sunColour = sun->color;
			clamp(&sunColour);
			const uint32 packed =
				(uint32)(sunColour.red   * 255.0f) |
				(uint32)(sunColour.green * 255.0f) << 8 |
				(uint32)(sunColour.blue  * 255.0f) << 16 |
				0xFF000000u;
			memcpy(&push.lightDirColour[3], &packed, sizeof(packed));
		}
	}

	// Diagnostic: what lighting skinned models actually receive. Cutscene
	// characters render black while gameplay peds are lit; this says whether
	// the difference is in the game's light data or in this backend. Prints
	// the values as the shader will see them, so a missing LIGHT flag shows
	// up as zero ambient here rather than silence.
	if(shader == SHADER_SKIN){
		static int32 lightProbe = 0;
		if((lightProbe++ % 600) == 0)
			printf("[probe] skin light: amb %.2f %.2f %.2f dirw 0x%08x "
			       "flags 0x%x world %p\n",
			       push.ambientLight[0], push.ambientLight[1],
			       push.ambientLight[2],
			       *(uint32*)&push.lightDirColour[3],
			       geometry->flags, (void*)atomic->world);
	}

	const VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &header->vbo, &vertexOffset);
	vkCmdBindIndexBuffer(commandBuffer, header->ibo, 0, VK_INDEX_TYPE_UINT16);

	VkDescriptorSet sceneSet = getSceneDescriptor();
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        getPipelineLayout(), 0, 1, &sceneSet, 0, nil);

	if(boneOffset != nil){
		VkDescriptorSet boneSet = getBoneDescriptor();
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                        getPipelineLayout(), 2, 1, &boneSet,
		                        1, boneOffset);
	}

	// Opaque meshes first, blended meshes second. File order interleaves them,
	// and a blended triangle drawn before the opaque mesh behind it has only
	// the background to blend against -- the classic see-through-body artifact.
	// Depth writes stay on for the blended pass, matching the D3D12 backend.
	for(int32 blendPass = 0; blendPass < 2; blendPass++){
	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++, inst++){
		Material *material = inst->material;
		Raster *raster = material && material->texture ?
			material->texture->raster : nil;

		const bool wantsBlend =
			inst->vertexAlpha || (material && material->color.alpha != 0xFF);
		if(wantsBlend != (blendPass == 1))
			continue;

		// Diagnostic: the ground renders translucent. Report what pushes
		// world meshes into the blend pass -- vertex alpha from prelight or
		// material alpha -- with the entity position for identification.
		if(wantsBlend && shader == SHADER_WORLD){
			static int32 blendProbe = 0;
			if((blendProbe++ % 400) == 0){
				Matrix *probeLtm = atomic->getFrame()->getLTM();
				printf("[probe] blend mesh: va %d matA %d flags 0x%x "
				       "pos %.0f %.0f %.0f\n",
				       inst->vertexAlpha,
				       material ? material->color.alpha : -1,
				       atomic->geometry->flags,
				       probeLtm->pos.x, probeLtm->pos.y, probeLtm->pos.z);
			}
		}

		if(material && material->texture){
			SetRenderState(TEXTUREFILTER, material->texture->getFilter());
			SetRenderState(TEXTUREADDRESSU, material->texture->getAddressU());
			SetRenderState(TEXTUREADDRESSV, material->texture->getAddressV());
		}
		SetRenderState(VERTEXALPHA,
			inst->vertexAlpha ||
			(material && material->color.alpha != 0xFF) ? 1 : 0);

		VkPipeline pipeline = getPipeline(shader,
		                                  (VkPrimitiveTopology)header->primType);
		if(pipeline == VK_NULL_HANDLE)
			continue;
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		VkDescriptorSet textureSet = getTextureDescriptor(raster);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                        getPipelineLayout(), 1, 1, &textureSet, 0, nil);

		if(material && (geometry->flags & Geometry::MODULATE)){
			push.materialColour[0] = material->color.red / 255.0f;
			push.materialColour[1] = material->color.green / 255.0f;
			push.materialColour[2] = material->color.blue / 255.0f;
			push.materialColour[3] = material->color.alpha / 255.0f;
			push.surfaceProps[0] = material->surfaceProps.ambient;
			push.surfaceProps[1] = material->surfaceProps.diffuse;
			push.surfaceProps[2] = material->surfaceProps.specular;
		}else if(material){
			// No MODULATE flag: the material colour is ignored, exactly as
			// gl3's setMaterial substitutes white. Surface properties still
			// apply -- they scale the lighting, not the colour.
			push.materialColour[0] = push.materialColour[1] =
			push.materialColour[2] = push.materialColour[3] = 1.0f;
			push.surfaceProps[0] = material->surfaceProps.ambient;
			push.surfaceProps[1] = material->surfaceProps.diffuse;
			push.surfaceProps[2] = material->surfaceProps.specular;
		}else{
			push.materialColour[0] = push.materialColour[1] =
			push.materialColour[2] = push.materialColour[3] = 1.0f;
			push.surfaceProps[0] = 1.0f;
			push.surfaceProps[1] = 1.0f;
			push.surfaceProps[2] = 0.0f;
		}
		push.surfaceProps[3] = gstate.alphaTestRef / 255.0f;

		vkCmdPushConstants(commandBuffer, getPipelineLayout(),
		                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                   0, sizeof(push), &push);

		vkCmdDrawIndexed(commandBuffer, inst->numIndex, 1,
		                 inst->offset / 2, 0, 0);
	}
	}
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	if(!gvk.inFrame || header->vbo == VK_NULL_HANDLE ||
	   header->ibo == VK_NULL_HANDLE)
		return;
	drawAtomicMeshes(atomic, header, SHADER_WORLD, nil);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_VULKAN);
	this->impl.instance = vulkan::instance;
	this->impl.uninstance = vulkan::uninstance;
	this->impl.render = vulkan::render;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline *
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

ObjPipeline *
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

}
}

#else

namespace rw {
namespace vulkan {
void *destroyNativeGeometryData(void *object, int32, int32) { return object; }
}
}

#endif
