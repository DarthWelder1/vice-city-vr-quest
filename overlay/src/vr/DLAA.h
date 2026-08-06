#pragma once

#include <stdint.h>

namespace Dlaa
{
// Native D3D12 resources are kept opaque here so the public VR header does not
// force D3D12 headers into non-D3D builds.
struct EyeInput
{
	void *color;
	void *depth;
	uint64_t depthShaderResourceView;
	uint32_t width;
	uint32_t height;
	uint32_t sourceLeft;
	float view[16];
	float projection[16];
	float nearPlane;
	float farPlane;
};

struct EyeOutput
{
	void *color;
	uint64_t shaderResourceView;
	uint32_t width;
	uint32_t height;
};

// Streamline must be initialized before the first D3D/DXGI call.
void InitializeEarly();

// Called once RenderWare has created the D3D12 device.
void AttachDevice();

bool BeginFrame(float jitterX, float jitterY);
bool EvaluateEye(int eye, const EyeInput &input, EyeOutput *output);
void ResetHistory();
void ReleaseResources();

void Shutdown();
bool IsInitialized();
bool IsSupported();
bool WasLastEvaluationSuccessful();
const char *GetStatus();
}
