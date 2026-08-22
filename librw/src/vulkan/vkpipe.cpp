#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <unordered_map>

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

static bool32 vehicleAlphaPass;
static bool32 dynamicObjectPass;

struct AtomicMotionRecord
{
	float32 model[16];
	float32 motion[4];
	uint64 serial;
	bool32 initialized;
};

static std::unordered_map<Atomic*, AtomicMotionRecord> atomicMotionHistory;

void
resetAtomicMotionHistory(void)
{
	atomicMotionHistory.clear();
}

static void
packAtomicRootMotion(Atomic *atomic, bool32 potentiallyDynamic,
                     PushConstants &push)
{
	// Affine matrices always have (0,0,0,1) in this row. The shaders restore
	// those constants before use, so the four slots are safe debug-only
	// transport for previous-current root translation and validity.
	float32 motion[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if(gvk.sgsrMode != SGSR_OFF && potentiallyDynamic){
		AtomicMotionRecord &record = atomicMotionHistory[atomic];
		if(record.serial == gvk.motionFrameSerial){
			memcpy(motion, record.motion, sizeof(motion));
		}else{
			if(record.initialized && record.serial+1 == gvk.motionFrameSerial){
				const SceneData *scene = getSceneData();
				for(int32 eye = 0; eye < 2; eye++){
					// SGSR2 receives projection jitter separately. Root velocity is
					// therefore measured between unjittered frames, matching the
					// reference dynamic-motion contract.
					const float32 *currentVp =
						gvk.stereoViewProjectionUnjittered[eye];
					const float32 *previousVp = scene->previousViewProj[eye];
					const float32 *currentModel = push.model;
					const float32 *previousModel = record.model;
					float32 currentClip[4], previousClip[4];
					for(int32 row = 0; row < 4; row++){
						currentClip[row] =
							currentVp[row]*currentModel[12] +
							currentVp[4+row]*currentModel[13] +
							currentVp[8+row]*currentModel[14] +
							currentVp[12+row];
						previousClip[row] =
							previousVp[row]*previousModel[12] +
							previousVp[4+row]*previousModel[13] +
							previousVp[8+row]*previousModel[14] +
							previousVp[12+row];
					}
					if(isfinite(currentClip[3]) && isfinite(previousClip[3]) &&
					   fabsf(currentClip[3]) > 1.0e-6f &&
					   fabsf(previousClip[3]) > 1.0e-6f){
						const float32 x = currentClip[0]/currentClip[3] -
						                  previousClip[0]/previousClip[3];
						const float32 y = currentClip[1]/currentClip[3] -
						                  previousClip[1]/previousClip[3];
						if(isfinite(x) && isfinite(y) && x*x+y*y <= 4.0f){
							motion[eye*2+0] = x;
							motion[eye*2+1] = y;
						}
					}
				}
			}
			memcpy(record.model, push.model, sizeof(record.model));
			memcpy(record.motion, motion, sizeof(record.motion));
			record.serial = gvk.motionFrameSerial;
			record.initialized = 1;
		}
	}
	push.model[3] = motion[0];
	push.model[7] = motion[1];
	push.model[11] = motion[2];
	push.model[15] = motion[3];
}

void
setVehicleAlphaPass(bool32 enabled)
{
	vehicleAlphaPass = enabled;
}

void
setDynamicObjectPass(bool32 enabled)
{
	dynamicObjectPass = enabled;
}

void
destroyStaticBuffer(VkBuffer &buffer, VkDeviceMemory &memory)
{
	if(buffer != VK_NULL_HANDLE || memory != VK_NULL_HANDLE)
		retireBuffer(buffer, memory);
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

		VkBufferMemoryBarrier ready = {};
		ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		ready.dstAccessMask =
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
			VK_ACCESS_INDEX_READ_BIT;
		ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		ready.buffer = *bufferOut;
		ready.offset = 0;
		ready.size = size;
		vkCmdPipelineBarrier(commandBuffer,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
		                     0, 0, nil, 1, &ready, 0, nil);
		endOneShot(commandBuffer);
	}

	// A streamed model may be drawn later in this same frame. Submission order
	// makes the copy visible first; defer the upload source until the frame
	// fence proves that both operations have completed.
	retireBuffer(staging, stagingMemory);
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
	Geometry *geometry = atomic->geometry;
	// Vice City's static world is prelit and has LIGHT unset. Peds, vehicles,
	// held weapons and their moving parts use dynamic lighting, which gives us
	// a conservative backend-level dynamic-object classifier without exposing
	// game entity types to librw.
	// Do not infer motion from Geometry::LIGHT: some static road/building
	// sectors carry that flag and produced alternating diagnostic triangles.
	// Game entities mark their synchronous clump explicitly; deferred vehicle
	// glass/body alpha atomics use the dedicated vehicle pass; skinned peds are
	// always persistent dynamic geometry.
	packAtomicRootMotion(atomic,
		dynamicObjectPass || vehicleAlphaPass || shader == SHADER_SKIN, push);

	// Lighting, resolved per atomic exactly as gl3's lightingCB does: world
	// geometry is unlit (prelight only), peds and vehicles take the world's
	// ambient plus its first directional -- Vice City registers exactly one.
	push.ambientLight[0] = push.ambientLight[1] = push.ambientLight[2] = 0.0f;
	// w carries the fog switch for this draw. The game turns fog on for the
	// world and off for the sky, the water and anything it wants left flat,
	// and the scene block is uploaded once a frame, so the per-draw state
	// has to travel with the draw.
	push.ambientLight[3] = gstate.fogEnabled ? 1.0f : 0.0f;
	push.lightDirColour[0] = 0.0f;
	push.lightDirColour[1] = 0.0f;
	push.lightDirColour[2] = 1.0f;
	push.lightDirColour[3] = 0.0f;	// packed RGBA8 black: no directional
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

	const VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &header->vbo, &vertexOffset);
	vkCmdBindIndexBuffer(commandBuffer, header->ibo, 0, VK_INDEX_TYPE_UINT16);

	if(boneOffset != nil){
		VkDescriptorSet boneSet = getBoneDescriptor();
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                        getPipelineLayout(), 2, 1, &boneSet,
		                        1, boneOffset);
	}

	// Opaque meshes first, blended meshes second. File order interleaves them,
	// and a blended triangle drawn before the opaque mesh behind it has only
	// the background to blend against -- the classic see-through-body artifact.
	// Vehicle alpha atomics frequently mix opaque bodywork and transparent side
	// glass.  Keep depth writes for the opaque pass, but do not let that glass
	// hide VR hands, held weapons and tracers rendered later in the frame.
	for(int32 blendPass = 0; blendPass < 2; blendPass++){
	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++, inst++){
		Material *material = inst->material;
		Raster *raster = material && material->texture ?
			material->texture->raster : nil;
		const bool textureAlpha = rasterHasAlpha(raster);

		const bool wantsBlend =
			inst->vertexAlpha || (material && material->color.alpha != 0xFF) ||
			textureAlpha;
		if(wantsBlend != (blendPass == 1))
			continue;

		if(material && material->texture){
			SetRenderState(TEXTUREFILTER, material->texture->getFilter());
			SetRenderState(TEXTUREADDRESSU, material->texture->getAddressU());
			SetRenderState(TEXTUREADDRESSV, material->texture->getAddressV());
		}
		SetRenderState(VERTEXALPHA,
			inst->vertexAlpha ||
			(material && material->color.alpha != 0xFF) ||
			textureAlpha ? 1 : 0);

		const uint32 savedZWrite = gstate.zWriteEnabled;
		if(vehicleAlphaPass && wantsBlend)
			gstate.zWriteEnabled = 0;
		VkPipeline pipeline = getPipeline(shader,
		                                  (VkPrimitiveTopology)header->primType);
		gstate.zWriteEnabled = savedZWrite;
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
