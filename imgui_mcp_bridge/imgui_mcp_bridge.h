#pragma once

#include <d3d11.h>
#include <dxgi.h>

struct ImGuiTestEngine;

struct ImGuiMcpBridgeConfig
{
    int                 Port = 8086;
    ImGuiTestEngine*    TestEngine = nullptr;
    ID3D11Device*       D3DDevice = nullptr;
    IDXGISwapChain*     SwapChain = nullptr;
};

// Initialize the MCP bridge. Call after ImGuiTestEngine_Start().
// Registers the remote_control test and starts the TCP server.
bool ImGuiMcpBridge_Init(const ImGuiMcpBridgeConfig& config);

// Shut down the MCP bridge. Call before ImGuiTestEngine_Stop().
void ImGuiMcpBridge_Shutdown();

// Returns true if shutdown was requested via the bridge (e.g. "shutdown" command).
bool ImGuiMcpBridge_WantsShutdown();
