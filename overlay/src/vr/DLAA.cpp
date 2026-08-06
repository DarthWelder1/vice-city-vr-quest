#include "common.h"
#include "DLAA.h"

#if defined(GTA_VR_OPENXR) && defined(RW_D3D12)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <float.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include "../../vendor/librw/src/d3d12/rwd3d12impl.h"

namespace Dlaa
{
namespace
{
enum { EYE_COUNT = 2 };

struct EyeState
{
	ID3D12Resource *output;
	ID3D12Resource *motion;
	D3D12_GPU_DESCRIPTOR_HANDLE outputSrv;
	D3D12_CPU_DESCRIPTOR_HANDLE motionUavCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE motionUavGpu;
	uint32 outputSrvIndex;
	uint32 motionUavIndex;
	D3D12_RESOURCE_STATES outputState;
	D3D12_RESOURCE_STATES motionState;
	uint32 width;
	uint32 height;
	bool optionsSet;
	bool historyValid;
	float previousView[16];
	float previousProjection[16];

	EyeState() : output(nil), motion(nil), outputSrvIndex(UINT32_MAX),
		motionUavIndex(UINT32_MAX), outputState(D3D12_RESOURCE_STATE_COMMON),
		motionState(D3D12_RESOURCE_STATE_COMMON), width(0), height(0),
		optionsSet(false), historyValid(false)
	{
		outputSrv.ptr = 0;
		motionUavCpu.ptr = motionUavGpu.ptr = 0;
		memset(previousView, 0, sizeof(previousView));
		memset(previousProjection, 0, sizeof(previousProjection));
	}
};

bool gInitialized;
bool gDeviceAttached;
bool gSupported;
bool gFrameReady;
bool gLastEvaluationSucceeded;
uint32 gSuccessfulEvaluationsThisFrame;
uint32 gEvaluationLogCount;
uint64 gFrameSerial;
uint64 gHistoryResetCount;
char gStatus[192] = "Streamline not initialized";
wchar_t gPluginDirectory[MAX_PATH];
const wchar_t *gPluginPaths[1];
sl::FrameToken *gFrameToken;
float gJitterX;
float gJitterY;
EyeState gEye[EYE_COUNT];
ID3D12RootSignature *gMotionRootSignature;
ID3D12PipelineState *gMotionPipeline;

const char *ResultName(sl::Result result)
{
	switch(result){
	case sl::Result::eOk: return "OK";
	case sl::Result::eErrorInvalidParameter: return "invalid parameter";
	case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
	case sl::Result::eErrorOSOutOfDate: return "OS out of date";
	case sl::Result::eErrorOSDisabledHWS: return "hardware scheduling disabled";
	case sl::Result::eErrorDeviceNotCreated: return "device not created";
	case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter";
	case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
	case sl::Result::eErrorNoPlugins: return "plugins not found";
	case sl::Result::eErrorNGXFailed: return "NGX initialization failed";
	case sl::Result::eErrorNotInitialized: return "not initialized";
	case sl::Result::eErrorInvalidIntegration: return "invalid integration";
	case sl::Result::eErrorMissingInputParameter: return "missing input";
	case sl::Result::eErrorMissingConstants: return "missing constants";
	case sl::Result::eErrorDuplicatedConstants: return "duplicated frame constants";
	case sl::Result::eErrorCommonConstantsMissing: return "common constants missing";
	case sl::Result::eErrorComputeFailed: return "compute failed";
	case sl::Result::eErrorFeatureMissing: return "DLSS plugin missing";
	case sl::Result::eErrorFeatureNotSupported: return "DLSS not supported";
	case sl::Result::eErrorFeatureFailedToLoad: return "DLSS plugin failed to load";
	default: return "Streamline error";
	}
}

void WriteLog(const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message)-1] = '\0';

	FILE *file = fopen("streamline_dlaa.log", "a");
	if(file){
		SYSTEMTIME now;
		GetLocalTime(&now);
		fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
			now.wSecond, now.wMilliseconds, message);
		fclose(file);
	}
}

void StreamlineLog(sl::LogType type, const char *message)
{
	const char *level = type == sl::LogType::eError ? "ERROR" :
		type == sl::LogType::eWarn ? "WARN" : "INFO";
	WriteLog("[Streamline/%s] %s", level, message ? message : "");
}

void SetStatus(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(gStatus, sizeof(gStatus), format, args);
	va_end(args);
	gStatus[sizeof(gStatus)-1] = '\0';
	WriteLog("[DLAA] %s", gStatus);
}

bool FindPluginDirectory()
{
	DWORD length = GetModuleFileNameW(nil, gPluginDirectory, MAX_PATH);
	if(length == 0 || length >= MAX_PATH)
		return false;
	while(length > 0 && gPluginDirectory[length-1] != L'\\' &&
	      gPluginDirectory[length-1] != L'/')
		length--;
	if(length == 0)
		return false;
	gPluginDirectory[length-1] = L'\0';
	gPluginPaths[0] = gPluginDirectory;
	return true;
}

D3D12_HEAP_PROPERTIES DefaultHeap()
{
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	return heap;
}

bool CreateTexture(ID3D12Device *device, uint32 width, uint32 height,
	DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES initialState, ID3D12Resource **resource)
{
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	D3D12_HEAP_PROPERTIES heap = DefaultHeap();
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&desc, initialState, nil, IID_PPV_ARGS(resource)));
}

bool CreateMotionPipeline()
{
	if(gMotionRootSignature && gMotionPipeline)
		return true;
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;
	static const char *shaderSource =
		"cbuffer MotionConstants : register(b0) {"
		" row_major float4x4 currentClipToView;"
		" row_major float4x4 currentViewToPreviousView;"
		" row_major float4x4 previousViewToClip;"
		" float2 jitterPixels; float2 inverseSize;"
		" uint sourceLeft; uint width; uint height; uint padding; };"
		"Texture2D<float> depthTexture : register(t0);"
		"RWTexture2D<float2> motionOutput : register(u0);"
		"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
		" if(id.x >= width || id.y >= height) return;"
		" float2 pixel = float2(id.xy) + 0.5;"
		" float2 currentPixel = pixel-jitterPixels;"
		" float2 currentNdc = float2(currentPixel.x*2.0*inverseSize.x-1.0,"
		"  1.0-currentPixel.y*2.0*inverseSize.y);"
		" float depth = depthTexture.Load(int3(id.x+sourceLeft,id.y,0));"
		" float4 currentView = mul(float4(currentNdc,depth,1.0),currentClipToView);"
		" if(abs(currentView.w) < 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" currentView /= currentView.w;"
		" float4 previousView = mul(currentView,currentViewToPreviousView);"
		" float4 previousClip = mul(previousView,previousViewToClip);"
		" if(previousClip.w <= 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" float2 previousNdc = previousClip.xy/previousClip.w;"
		" float2 previousPixel = float2((previousNdc.x+1.0)*0.5*width,"
		"  (1.0-previousNdc.y)*0.5*height);"
		" motionOutput[id.xy] = clamp(previousPixel-currentPixel,"
		"  float2(-32768.0,-32768.0),float2(32768.0,32768.0)); }";
	ID3DBlob *shader = nil;
	ID3DBlob *errors = nil;
	HRESULT result = D3DCompile(shaderSource, strlen(shaderSource),
		"vice_city_vr_camera_motion", nil, nil, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
	if(FAILED(result)){
		if(errors)
			WriteLog("[DLAA] Camera motion shader error: %s",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		if(shader) shader->Release();
		return false;
	}
	if(errors) errors->Release();

	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	for(uint32 index = 0; index < 2; index++){
		ranges[index].RangeType = index == 0 ?
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[index].NumDescriptors = 1;
		ranges[index].BaseShaderRegister = 0;
		ranges[index].RegisterSpace = 0;
		ranges[index].OffsetInDescriptorsFromTableStart = 0;
	}
	D3D12_ROOT_PARAMETER parameters[3] = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.ShaderRegister = 0;
	parameters[0].Constants.RegisterSpace = 0;
	parameters[0].Constants.Num32BitValues = 56;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(uint32 index = 0; index < 2; index++){
		parameters[index+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[index+1].DescriptorTable.NumDescriptorRanges = 1;
		parameters[index+1].DescriptorTable.pDescriptorRanges = &ranges[index];
		parameters[index+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = ARRAY_SIZE(parameters);
	rootDesc.pParameters = parameters;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	ID3DBlob *serialized = nil;
	ID3DBlob *rootErrors = nil;
	result = D3D12SerializeRootSignature(&rootDesc,
		D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
	if(FAILED(result) || !serialized){
		if(rootErrors)
			WriteLog("[DLAA] Camera motion root signature error: %s",
				(const char*)rootErrors->GetBufferPointer());
		if(rootErrors) rootErrors->Release();
		if(serialized) serialized->Release();
		shader->Release();
		return false;
	}
	if(rootErrors) rootErrors->Release();
	result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
		serialized->GetBufferSize(), IID_PPV_ARGS(&gMotionRootSignature));
	serialized->Release();
	if(FAILED(result)){
		shader->Release();
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
	pipelineDesc.pRootSignature = gMotionRootSignature;
	pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
	pipelineDesc.CS.BytecodeLength = shader->GetBufferSize();
	result = device->CreateComputePipelineState(&pipelineDesc,
		IID_PPV_ARGS(&gMotionPipeline));
	shader->Release();
	if(FAILED(result)){
		gMotionRootSignature->Release();
		gMotionRootSignature = nil;
		return false;
	}
	gMotionRootSignature->SetName(L"Vice City VR camera motion root signature");
	gMotionPipeline->SetName(L"Vice City VR camera motion pipeline");
	return true;
}

void ReleaseMotionPipeline()
{
	if(gMotionPipeline)
		rw::d3d12::deferRelease(gMotionPipeline);
	if(gMotionRootSignature)
		rw::d3d12::deferRelease(gMotionRootSignature);
	gMotionPipeline = nil;
	gMotionRootSignature = nil;
}

void ReleaseEye(int eye)
{
	if(eye < 0 || eye >= EYE_COUNT)
		return;
	EyeState &state = gEye[eye];
	if(gInitialized && gDeviceAttached && state.optionsSet){
		sl::ViewportHandle viewport(eye);
		slFreeResources(sl::kFeatureDLSS, viewport);
	}
	if(state.output)
		rw::d3d12::deferRelease(state.output);
	if(state.motion)
		rw::d3d12::deferRelease(state.motion);
	if(state.outputSrvIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.outputSrvIndex, UINT32_MAX, UINT32_MAX);
	if(state.motionUavIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.motionUavIndex, UINT32_MAX, UINT32_MAX);
	state = EyeState();
}

bool CreateEyeResources(int eye, uint32 width, uint32 height)
{
	if(eye < 0 || eye >= EYE_COUNT || width == 0 || height == 0)
		return false;
	EyeState &state = gEye[eye];
	if(state.output && state.motion && state.width == width && state.height == height)
		return true;
	ReleaseEye(eye);

	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;
	if(!CreateTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
	   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.output) ||
	   !CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT,
	   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.motion)){
		ReleaseEye(eye);
		return false;
	}
	state.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.width = width;
	state.height = height;

	D3D12_CPU_DESCRIPTOR_HANDLE outputSrvCpu = {};
	if(!rw::d3d12::allocateShaderResourceDescriptor(&outputSrvCpu, &state.outputSrv,
	   &state.outputSrvIndex) ||
	   !rw::d3d12::allocateShaderResourceDescriptor(&state.motionUavCpu,
	   &state.motionUavGpu, &state.motionUavIndex)){
		ReleaseEye(eye);
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(state.output, &srv, outputSrvCpu);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = DXGI_FORMAT_R16G16_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(state.motion, nil, &uav, state.motionUavCpu);

	wchar_t name[64];
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR DLAA output eye %d", eye);
	state.output->SetName(name);
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR motion vectors eye %d", eye);
	state.motion->SetName(name);

	sl::DLSSOptions options{};
	options.mode = sl::DLSSMode::eDLAA;
	options.outputWidth = width;
	options.outputHeight = height;
	options.dlaaPreset = sl::DLSSPreset::ePresetK;
	options.colorBuffersHDR = sl::Boolean::eFalse;
	options.useAutoExposure = sl::Boolean::eTrue;
	options.alphaUpscalingEnabled = sl::Boolean::eFalse;
	sl::ViewportHandle viewport(eye);
	const sl::Result result = slDLSSSetOptions(viewport, options);
	if(result != sl::Result::eOk){
		SetStatus("DLAA options failed for eye %d: %s (%d)", eye,
			ResultName(result), (int)result);
		ReleaseEye(eye);
		return false;
	}
	state.optionsSet = true;
	WriteLog("[DLAA] Allocated eye %d temporal targets %ux%u", eye, width, height);
	return true;
}

void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
	D3D12_RESOURCE_STATES &current, D3D12_RESOURCE_STATES next)
{
	if(!list || !resource || current == next)
		return;
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = current;
	barrier.Transition.StateAfter = next;
	list->ResourceBarrier(1, &barrier);
	current = next;
}

void CopyMatrix(sl::float4x4 &destination, const float source[16])
{
	memcpy(&destination[0].x, source, 16*sizeof(float));
}

bool GenerateCameraMotion(const EyeInput &input, EyeState &state,
	ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *heap)
{
	if(!state.historyValid)
		return true;
	if(!gMotionRootSignature || !gMotionPipeline || !list || !heap ||
	   input.depthShaderResourceView == 0)
		return false;
	struct MotionConstants {
		float currentClipToView[16];
		float currentViewToPreviousView[16];
		float previousViewToClip[16];
		float jitterPixels[2];
		float inverseSize[2];
		uint32 sourceLeft;
		uint32 width;
		uint32 height;
		uint32 padding;
	} constants = {};
	static_assert(sizeof(MotionConstants) == 56*sizeof(uint32),
		"Camera motion root constants must match HLSL layout");
	sl::float4x4 currentView, currentProjection, currentClipToView;
	sl::float4x4 currentCameraToWorld, currentViewToPreviousView;
	sl::float4x4 previousView, previousProjection, previousCameraToWorld;
	CopyMatrix(currentView, input.view);
	CopyMatrix(currentProjection, input.projection);
	CopyMatrix(previousView, state.previousView);
	CopyMatrix(previousProjection, state.previousProjection);
	sl::matrixFullInvert(currentClipToView, currentProjection);
	sl::matrixFullInvert(currentCameraToWorld, currentView);
	sl::matrixFullInvert(previousCameraToWorld, previousView);
	// Work in camera-centred space. Multiplying absolute world/view matrices
	// loses the sub-pixel camera motion that DLAA needs once Vice City's world
	// coordinates grow large enough.
	sl::calcCameraToPrevCamera(currentViewToPreviousView,
		currentCameraToWorld, previousCameraToWorld);
	memcpy(constants.currentClipToView, &currentClipToView[0].x,
		sizeof(constants.currentClipToView));
	memcpy(constants.currentViewToPreviousView, &currentViewToPreviousView[0].x,
		sizeof(constants.currentViewToPreviousView));
	memcpy(constants.previousViewToClip, &previousProjection[0].x,
		sizeof(constants.previousViewToClip));
	constants.jitterPixels[0] = gJitterX;
	constants.jitterPixels[1] = gJitterY;
	constants.inverseSize[0] = 1.0f/(float)input.width;
	constants.inverseSize[1] = 1.0f/(float)input.height;
	constants.sourceLeft = input.sourceLeft;
	constants.width = input.width;
	constants.height = input.height;
	ID3D12DescriptorHeap *heaps[] = { heap };
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(gMotionRootSignature);
	list->SetPipelineState(gMotionPipeline);
	list->SetComputeRoot32BitConstants(0, 56, &constants, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = { input.depthShaderResourceView };
	list->SetComputeRootDescriptorTable(1, depthView);
	list->SetComputeRootDescriptorTable(2, state.motionUavGpu);
	list->Dispatch((input.width+7)/8, (input.height+7)/8, 1);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = state.motion;
	list->ResourceBarrier(1, &barrier);
	return true;
}

void BuildConstants(const EyeInput &input, EyeState &state, sl::Constants &constants)
{
	sl::float4x4 currentView, currentProjection;
	CopyMatrix(currentView, input.view);
	CopyMatrix(currentProjection, input.projection);
	constants.cameraViewToClip = currentProjection;
	sl::matrixFullInvert(constants.clipToCameraView, currentProjection);

	sl::float4x4 cameraToWorld;
	sl::matrixFullInvert(cameraToWorld, currentView);
	constants.cameraRight = sl::float3(cameraToWorld[0].x, cameraToWorld[0].y,
		cameraToWorld[0].z);
	constants.cameraUp = sl::float3(cameraToWorld[1].x, cameraToWorld[1].y,
		cameraToWorld[1].z);
	constants.cameraFwd = sl::float3(cameraToWorld[2].x, cameraToWorld[2].y,
		cameraToWorld[2].z);
	constants.cameraPos = sl::float3(cameraToWorld[3].x, cameraToWorld[3].y,
		cameraToWorld[3].z);

	if(state.historyValid){
		sl::float4x4 previousView, previousProjection, previousCameraToWorld;
		CopyMatrix(previousView, state.previousView);
		CopyMatrix(previousProjection, state.previousProjection);
		sl::matrixFullInvert(previousCameraToWorld, previousView);
		sl::float4x4 currentViewToPreviousView, currentClipToPreviousView;
		sl::calcCameraToPrevCamera(currentViewToPreviousView,
			cameraToWorld, previousCameraToWorld);
		sl::matrixMul(currentClipToPreviousView, constants.clipToCameraView,
			currentViewToPreviousView);
		sl::matrixMul(constants.clipToPrevClip, currentClipToPreviousView,
			previousProjection);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);
	}else{
		const float identity[16] = {
			1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
		};
		CopyMatrix(constants.clipToPrevClip, identity);
		CopyMatrix(constants.prevClipToClip, identity);
	}

	constants.jitterOffset = sl::float2(gJitterX, gJitterY);
	constants.mvecScale = sl::float2(1.0f/input.width, 1.0f/input.height);
	constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	constants.cameraNear = input.nearPlane;
	constants.cameraFar = input.farPlane;
	constants.cameraFOV = 2.0f*atanf(1.0f/fabsf(input.projection[0]));
	constants.cameraAspectRatio = (float)input.width/(float)input.height;
	constants.motionVectorsInvalidValue = -32768.0f;
	constants.depthInverted = sl::Boolean::eFalse;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D = sl::Boolean::eFalse;
	constants.reset = state.historyValid ? sl::Boolean::eFalse : sl::Boolean::eTrue;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated = sl::Boolean::eFalse;
	constants.motionVectorsJittered = sl::Boolean::eFalse;
}
}

void InitializeEarly()
{
	if(gInitialized)
		return;
	FILE *file = fopen("streamline_dlaa.log", "w");
	if(file)
		fclose(file);

	sl::Feature features[] = { sl::kFeatureDLSS };
	sl::Preferences preferences{};
	preferences.showConsole = false;
	preferences.logLevel = sl::LogLevel::eDefault;
	preferences.logMessageCallback = StreamlineLog;
	preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
		sl::PreferenceFlags::eDisableDebugText;
	preferences.featuresToLoad = features;
	preferences.numFeaturesToLoad = 1;
	preferences.renderAPI = sl::RenderAPI::eD3D12;
	if(FindPluginDirectory()){
		preferences.pathsToPlugins = gPluginPaths;
		preferences.numPathsToPlugins = 1;
	}

	const sl::Result result = slInit(preferences);
	if(result != sl::Result::eOk){
		SetStatus("Streamline init failed: %s (%d)", ResultName(result), (int)result);
		return;
	}
	gInitialized = true;
	SetStatus("Streamline initialized; waiting for D3D12 device");
}

void AttachDevice()
{
	if(!gInitialized || gDeviceAttached)
		return;
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device){
		SetStatus("Streamline initialized, but D3D12 device is unavailable");
		return;
	}
	sl::Result result = slSetD3DDevice(device);
	if(result != sl::Result::eOk){
		SetStatus("Streamline device attach failed: %s (%d)", ResultName(result), (int)result);
		return;
	}
	gDeviceAttached = true;
	LUID luid = device->GetAdapterLuid();
	sl::AdapterInfo adapter{};
	adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
	adapter.deviceLUIDSizeInBytes = sizeof(luid);
	result = slIsFeatureSupported(sl::kFeatureDLSS, adapter);
	gSupported = result == sl::Result::eOk;
	if(gSupported && !CreateMotionPipeline()){
		gSupported = false;
		SetStatus("DLAA camera motion pipeline creation failed");
	}else if(gSupported)
		SetStatus("DLSS/DLAA supported; Streamline camera motion ready");
	else
		SetStatus("DLSS/DLAA unavailable: %s (%d)", ResultName(result), (int)result);
}

bool BeginFrame(float jitterX, float jitterY)
{
	gFrameReady = false;
	gLastEvaluationSucceeded = false;
	gSuccessfulEvaluationsThisFrame = 0;
	gFrameToken = nil;
	gJitterX = jitterX;
	gJitterY = jitterY;
	if(!gInitialized || !gDeviceAttached || !gSupported)
		return false;
	gFrameSerial++;
	if(gFrameSerial <= 4 || (gFrameSerial % 300) == 0)
		WriteLog("[DLAA] Frame %llu jitter=(%.4f,%.4f) history=(%d,%d) resets=%llu",
			(unsigned long long)gFrameSerial, jitterX, jitterY,
			gEye[0].historyValid ? 1 : 0, gEye[1].historyValid ? 1 : 0,
			(unsigned long long)gHistoryResetCount);
	// Let Streamline generate the unique token.  A caller-owned temporal sample
	// counter can legally pause while jitter is disabled, but a frame token may
	// never be reused for a later rendered frame.
	const sl::Result result = slGetNewFrameToken(gFrameToken);
	if(result != sl::Result::eOk || !gFrameToken){
		SetStatus("DLAA frame token failed: %s (%d)", ResultName(result), (int)result);
		return false;
	}
	gFrameReady = true;
	return true;
}

bool EvaluateEye(int eye, const EyeInput &input, EyeOutput *output)
{
	if(output){
		output->color = nil;
		output->shaderResourceView = 0;
		output->width = output->height = 0;
	}
	if(!gFrameReady || !gFrameToken || !output || eye < 0 || eye >= EYE_COUNT ||
	   !input.color || !input.depth || input.width == 0 || input.height == 0)
		return false;
	if(!CreateEyeResources(eye, input.width, input.height)){
		SetStatus("DLAA target allocation failed for eye %d (%ux%u)", eye,
			input.width, input.height);
		return false;
	}
	EyeState &state = gEye[eye];
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	ID3D12DescriptorHeap *heap = rw::d3d12::getShaderResourceHeap();
	if(!list || !heap)
		return false;

	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12DescriptorHeap *heaps[] = { heap };
	list->SetDescriptorHeaps(1, heaps);
	// Streamline's internal camera-motion pass samples depth from x=0 and cannot
	// address the right-eye half of our double-wide depth resource. Generate the
	// same current-to-previous pixel motion locally, including sourceLeft, and
	// tell DLAA that camera motion is already present in this buffer.
	if(state.historyValid){
		if(!GenerateCameraMotion(input, state, list, heap)){
			SetStatus("DLAA camera motion generation failed for eye %d", eye);
			state.historyValid = false;
			return false;
		}
	}else{
		const float zeroMotion[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		list->ClearUnorderedAccessViewFloat(state.motionUavGpu, state.motionUavCpu,
			state.motion, zeroMotion, 0, nil);
		D3D12_RESOURCE_BARRIER uavBarrier = {};
		uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uavBarrier.UAV.pResource = state.motion;
		list->ResourceBarrier(1, &uavBarrier);
	}
	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	sl::ViewportHandle viewport(eye);
	sl::Constants constants{};
	BuildConstants(input, state, constants);
	if(eye == 0 && (gFrameSerial <= 4 || (gFrameSerial % 300) == 0))
		WriteLog("[DLAA] Reprojection reset=%d clipPrevT=(%.7f,%.7f,%.7f)",
			constants.reset == sl::Boolean::eTrue ? 1 : 0,
			constants.clipToPrevClip[3].x, constants.clipToPrevClip[3].y,
			constants.clipToPrevClip[3].z);
	sl::Result result = slSetConstants(constants, *gFrameToken, viewport);
	if(result != sl::Result::eOk){
		SetStatus("DLAA constants failed for eye %d: %s (%d)", eye,
			ResultName(result), (int)result);
		return false;
	}

	const uint32 shaderRead = (uint32)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	sl::Resource color(sl::ResourceType::eTex2d, input.color, shaderRead);
	sl::Resource depth(sl::ResourceType::eTex2d, input.depth, shaderRead);
	sl::Resource motion(sl::ResourceType::eTex2d, state.motion, shaderRead);
	sl::Resource destination(sl::ResourceType::eTex2d, state.output,
		(uint32)D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	sl::Extent sourceExtent = { 0, input.sourceLeft, input.width, input.height };
	sl::Extent localExtent = { 0, 0, input.width, input.height };
	sl::ResourceTag colorTag(&color, sl::kBufferTypeScalingInputColor,
		sl::ResourceLifecycle::eValidUntilEvaluate, &sourceExtent);
	sl::ResourceTag depthTag(&depth, sl::kBufferTypeDepth,
		sl::ResourceLifecycle::eValidUntilEvaluate, &sourceExtent);
	sl::ResourceTag motionTag(&motion, sl::kBufferTypeMotionVectors,
		sl::ResourceLifecycle::eValidUntilEvaluate, &localExtent);
	sl::ResourceTag outputTag(&destination, sl::kBufferTypeScalingOutputColor,
		sl::ResourceLifecycle::eValidUntilEvaluate, &localExtent);
	const sl::BaseStructure *inputs[] = {
		&viewport, &colorTag, &outputTag, &depthTag, &motionTag
	};
	result = slEvaluateFeature(sl::kFeatureDLSS, *gFrameToken, inputs,
		ARRAY_SIZE(inputs), reinterpret_cast<sl::CommandBuffer*>(list));
	if(result != sl::Result::eOk){
		if(gEvaluationLogCount++ < 16)
			SetStatus("DLAA evaluate failed for eye %d: %s (%d)", eye,
				ResultName(result), (int)result);
		state.historyValid = false;
		return false;
	}
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	memcpy(state.previousView, input.view, sizeof(state.previousView));
	memcpy(state.previousProjection, input.projection, sizeof(state.previousProjection));
	state.historyValid = true;
	gSuccessfulEvaluationsThisFrame++;
	gLastEvaluationSucceeded = gSuccessfulEvaluationsThisFrame == EYE_COUNT;
	if(gEvaluationLogCount == 0){
		WriteLog("[DLAA] First evaluation succeeded (%ux%u per eye)",
			input.width, input.height);
		gEvaluationLogCount++;
	}
	output->color = state.output;
	output->shaderResourceView = state.outputSrv.ptr;
	output->width = state.width;
	output->height = state.height;
	return true;
}

void ResetHistory()
{
	const bool wasValid = gEye[0].historyValid || gEye[1].historyValid;
	for(int eye = 0; eye < EYE_COUNT; eye++)
		gEye[eye].historyValid = false;
	gFrameReady = false;
	gHistoryResetCount++;
	if(wasValid)
		WriteLog("[DLAA] History reset #%llu after frame %llu",
			(unsigned long long)gHistoryResetCount,
			(unsigned long long)gFrameSerial);
}

void ReleaseResources()
{
	for(int eye = 0; eye < EYE_COUNT; eye++)
		ReleaseEye(eye);
	gFrameReady = false;
	gFrameToken = nil;
}

void Shutdown()
{
	ReleaseResources();
	ReleaseMotionPipeline();
	if(gInitialized){
		WriteLog("[DLAA] Shutting down Streamline");
		slShutdown();
	}
	gInitialized = false;
	gDeviceAttached = false;
	gSupported = false;
	gLastEvaluationSucceeded = false;
	strcpy(gStatus, "Streamline shut down");
}

bool IsInitialized() { return gInitialized; }
bool IsSupported() { return gSupported; }
bool WasLastEvaluationSuccessful() { return gLastEvaluationSucceeded; }
const char *GetStatus() { return gStatus; }
}

#else

namespace Dlaa
{
void InitializeEarly() {}
void AttachDevice() {}
bool BeginFrame(float, float) { return false; }
bool EvaluateEye(int, const EyeInput&, EyeOutput*) { return false; }
void ResetHistory() {}
void ReleaseResources() {}
void Shutdown() {}
bool IsInitialized() { return false; }
bool IsSupported() { return false; }
bool WasLastEvaluationSuccessful() { return false; }
const char *GetStatus() { return "DLAA requires OpenXR and D3D12"; }
}

#endif
