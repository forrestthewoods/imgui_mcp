#pragma once

#include "imgui.h"
#include <d3d11.h>
#include <dxgi.h>
#include <string>

// User data passed to the screen capture callback
struct ImGuiMcpCaptureUserData
{
    ID3D11Device*       D3DDevice = nullptr;
    IDXGISwapChain*     SwapChain = nullptr;
};

// Screen capture callback compatible with ImGuiScreenCaptureFunc signature.
// Captures a region of the backbuffer into an RGBA pixel buffer.
bool ImGuiMcpCapture_ScreenCaptureFunc(ImGuiID viewport_id, int x, int y, int w, int h, unsigned int* pixels, void* user_data);

// Encode raw RGBA pixels to PNG in memory. Returns empty string on failure.
std::string ImGuiMcpCapture_EncodePNG(const unsigned int* pixels, int w, int h);

// Base64 encode binary data.
std::string ImGuiMcpCapture_Base64Encode(const unsigned char* data, size_t len);
