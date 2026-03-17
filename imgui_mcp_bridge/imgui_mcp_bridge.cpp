// Winsock must be included before windows.h (which imgui pulls in)
#include <winsock2.h>
#include <ws2tcpip.h>

#include "imgui_mcp_bridge.h"
#include "imgui_mcp_protocol.h"
#include "imgui_mcp_capture.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_capture_tool.h"

#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <cstdio>

//-----------------------------------------------------------------------------
// Globals
//-----------------------------------------------------------------------------

static ImGuiMcpBridgeConfig     g_Config;
static ImGuiMcpCaptureUserData  g_CaptureUserData;
static std::atomic<bool>        g_Running{false};
static std::atomic<bool>        g_WantsShutdown{false};
static SOCKET                   g_ListenSocket = INVALID_SOCKET;
static std::thread              g_AcceptThread;

// Command queue: TCP thread pushes, test coroutine pops
static std::mutex               g_QueueMutex;
static std::deque<McpCommand>   g_CommandQueue;

// On-demand test queuing: test only runs while processing commands,
// so it doesn't hijack mouse/keyboard input when idle.
static ImGuiTest*               g_Test = nullptr;
static std::atomic<bool>        g_TestRunning{false};

//-----------------------------------------------------------------------------
// TCP Server
//-----------------------------------------------------------------------------

static void HandleClient(SOCKET client_socket)
{
    // Set TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    std::string recv_buf;
    char chunk[4096];

    while (g_Running.load())
    {
        int bytes = recv(client_socket, chunk, sizeof(chunk), 0);
        if (bytes <= 0)
            break;

        recv_buf.append(chunk, bytes);

        // Process complete lines (newline-delimited JSON)
        size_t pos;
        while ((pos = recv_buf.find('\n')) != std::string::npos)
        {
            std::string line = recv_buf.substr(0, pos);
            recv_buf.erase(0, pos + 1);

            if (line.empty())
                continue;

            // Parse JSON
            nlohmann::json req;
            try
            {
                req = nlohmann::json::parse(line);
            }
            catch (...)
            {
                nlohmann::json err_resp;
                err_resp["id"] = 0;
                err_resp["ok"] = false;
                err_resp["error"] = "Invalid JSON";
                std::string out = err_resp.dump() + "\n";
                send(client_socket, out.c_str(), (int)out.size(), 0);
                continue;
            }

            int id = req.value("id", 0);
            std::string cmd = req.value("cmd", "");

            // Create command and push to queue
            McpCommand mcp_cmd;
            mcp_cmd.Id = id;
            mcp_cmd.Cmd = cmd;
            mcp_cmd.Params = req;

            // Get the future before moving the promise
            auto future = mcp_cmd.Promise.get_future();

            {
                std::lock_guard<std::mutex> lock(g_QueueMutex);
                g_CommandQueue.push_back(std::move(mcp_cmd));
            }

            // Wait for the test coroutine to process and return the result.
            // ImGuiMcpBridge_Tick() (called from main loop) will queue the test
            // which will pick up this command.
            nlohmann::json response;
            try
            {
                response = future.get();
            }
            catch (...)
            {
                response["id"] = id;
                response["ok"] = false;
                response["error"] = "Internal error processing command";
            }

            std::string out = response.dump() + "\n";
            send(client_socket, out.c_str(), (int)out.size(), 0);
        }
    }

    closesocket(client_socket);
}

static void AcceptThreadFunc()
{
    while (g_Running.load())
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_ListenSocket, &read_fds);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000; // 200ms

        int sel = select(0, &read_fds, nullptr, nullptr, &timeout);
        if (sel <= 0)
            continue;

        SOCKET client = accept(g_ListenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;

        printf("[MCP Bridge] Client connected\n");

        // Handle one client at a time (single-client MVP)
        HandleClient(client);

        printf("[MCP Bridge] Client disconnected\n");
    }
}

//-----------------------------------------------------------------------------
// Command Dispatcher (runs on test engine coroutine / main thread)
//-----------------------------------------------------------------------------

static nlohmann::json DispatchCommand(ImGuiTestContext* ctx, const McpCommand& cmd)
{
    nlohmann::json resp;
    resp["id"] = cmd.Id;

    try
    {
        if (cmd.Cmd == "ping")
        {
            resp["ok"] = true;
        }
        else if (cmd.Cmd == "screenshot")
        {
            // Direct capture fallback: use our own capture function directly.
            // This is more reliable than going through the test engine's capture
            // system which may require multi-frame orchestration.
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            int vw = (int)viewport->Size.x;
            int vh = (int)viewport->Size.y;
            if (vw > 0 && vh > 0)
            {
                std::vector<unsigned int> pixels(vw * vh);
                bool captured = ImGuiMcpCapture_ScreenCaptureFunc(
                    viewport->ID, 0, 0, vw, vh, pixels.data(), &g_CaptureUserData);
                if (captured)
                {
                    std::string b64 = ImGuiMcpCapture_EncodePNG(pixels.data(), vw, vh);
                    resp["ok"] = true;
                    resp["image"] = b64;
                    resp["width"] = vw;
                    resp["height"] = vh;
                }
                else
                {
                    resp["ok"] = false;
                    resp["error"] = "Screen capture failed";
                }
            }
            else
            {
                resp["ok"] = false;
                resp["error"] = "Invalid viewport size";
            }
        }
        else if (cmd.Cmd == "click")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty())
            {
                resp["ok"] = false;
                resp["error"] = "Missing 'ref' parameter";
            }
            else
            {
                ctx->ItemClick(ref.c_str(), 0, ImGuiTestOpFlags_NoError);
                if (ctx->IsError())
                {
                    resp["ok"] = false;
                    resp["error"] = "Click failed (item not found or not clickable): " + ref;
                }
                else
                {
                    resp["ok"] = true;
                }
            }
        }
        else if (cmd.Cmd == "double_click")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty())
            {
                resp["ok"] = false;
                resp["error"] = "Missing 'ref' parameter";
            }
            else
            {
                ctx->ItemDoubleClick(ref.c_str(), ImGuiTestOpFlags_NoError);
                if (ctx->IsError()) { resp["ok"] = false; resp["error"] = "Double-click failed: " + ref; }
                else { resp["ok"] = true; }
            }
        }
        else if (cmd.Cmd == "check")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemCheck(ref.c_str(), ImGuiTestOpFlags_NoError); resp["ok"] = !ctx->IsError(); if (!resp["ok"]) resp["error"] = "Check failed: " + ref; }
        }
        else if (cmd.Cmd == "uncheck")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemUncheck(ref.c_str(), ImGuiTestOpFlags_NoError); resp["ok"] = !ctx->IsError(); if (!resp["ok"]) resp["error"] = "Uncheck failed: " + ref; }
        }
        else if (cmd.Cmd == "open")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemOpen(ref.c_str(), ImGuiTestOpFlags_NoError); resp["ok"] = !ctx->IsError(); if (!resp["ok"]) resp["error"] = "Open failed: " + ref; }
        }
        else if (cmd.Cmd == "close")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemClose(ref.c_str(), ImGuiTestOpFlags_NoError); resp["ok"] = !ctx->IsError(); if (!resp["ok"]) resp["error"] = "Close failed: " + ref; }
        }
        else if (cmd.Cmd == "input")
        {
            std::string ref = cmd.Params.value("ref", "");
            std::string text = cmd.Params.value("text", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else
            {
                ctx->ItemInput(ref.c_str(), ImGuiTestOpFlags_NoError);
                if (!text.empty() && !ctx->IsError())
                    ctx->KeyCharsReplace(text.c_str());
                resp["ok"] = !ctx->IsError();
                if (!resp["ok"]) resp["error"] = "Input failed: " + ref;
            }
        }
        else if (cmd.Cmd == "key_press")
        {
            std::string key = cmd.Params.value("key", "");
            if (key.empty()) { resp["ok"] = false; resp["error"] = "Missing 'key'"; }
            else
            {
                ImGuiKey imgui_key = ImGuiKey_None;
                if (key == "Enter" || key == "Return") imgui_key = ImGuiKey_Enter;
                else if (key == "Escape") imgui_key = ImGuiKey_Escape;
                else if (key == "Tab") imgui_key = ImGuiKey_Tab;
                else if (key == "Backspace") imgui_key = ImGuiKey_Backspace;
                else if (key == "Delete") imgui_key = ImGuiKey_Delete;
                else if (key == "Space") imgui_key = ImGuiKey_Space;
                else if (key == "Up") imgui_key = ImGuiKey_UpArrow;
                else if (key == "Down") imgui_key = ImGuiKey_DownArrow;
                else if (key == "Left") imgui_key = ImGuiKey_LeftArrow;
                else if (key == "Right") imgui_key = ImGuiKey_RightArrow;
                else if (key == "Home") imgui_key = ImGuiKey_Home;
                else if (key == "End") imgui_key = ImGuiKey_End;
                else if (key == "PageUp") imgui_key = ImGuiKey_PageUp;
                else if (key == "PageDown") imgui_key = ImGuiKey_PageDown;
                else if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
                    imgui_key = (ImGuiKey)(ImGuiKey_A + (key[0] - 'A'));
                else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
                    imgui_key = (ImGuiKey)(ImGuiKey_A + (key[0] - 'a'));

                if (imgui_key != ImGuiKey_None)
                {
                    ctx->KeyPress(imgui_key);
                    resp["ok"] = true;
                }
                else
                {
                    resp["ok"] = false;
                    resp["error"] = "Unknown key: " + key;
                }
            }
        }
        else if (cmd.Cmd == "key_chars")
        {
            std::string text = cmd.Params.value("text", "");
            if (text.empty()) { resp["ok"] = false; resp["error"] = "Missing 'text'"; }
            else { ctx->KeyChars(text.c_str()); resp["ok"] = true; }
        }
        else if (cmd.Cmd == "list_windows")
        {
            nlohmann::json windows = nlohmann::json::array();
            ImGuiContext& g = *GImGui;
            for (ImGuiWindow* window : g.Windows)
            {
                if (!window->Active || window->Hidden)
                    continue;
                if (window->Flags & ImGuiWindowFlags_Tooltip)
                    continue;

                nlohmann::json w;
                w["name"] = window->Name;
                w["x"] = window->Pos.x;
                w["y"] = window->Pos.y;
                w["width"] = window->Size.x;
                w["height"] = window->Size.y;
                w["collapsed"] = window->Collapsed;
                windows.push_back(w);
            }
            resp["ok"] = true;
            resp["windows"] = windows;
        }
        else if (cmd.Cmd == "list_items")
        {
            std::string ref = cmd.Params.value("ref", "");
            int depth = cmd.Params.value("depth", 2);
            if (ref.empty())
            {
                resp["ok"] = false;
                resp["error"] = "Missing 'ref' parameter";
            }
            else
            {
                ImGuiTestItemList items;
                ctx->GatherItems(&items, ref.c_str(), depth);

                nlohmann::json items_json = nlohmann::json::array();
                for (int i = 0; i < items.GetSize(); i++)
                {
                    const ImGuiTestItemInfo* info = items.GetByIndex(i);
                    nlohmann::json item;
                    item["label"] = info->DebugLabel;
                    item["depth"] = info->Depth;
                    item["id"] = info->ID;
                    item["x"] = info->RectFull.Min.x;
                    item["y"] = info->RectFull.Min.y;
                    item["width"] = info->RectFull.GetWidth();
                    item["height"] = info->RectFull.GetHeight();
                    items_json.push_back(item);
                }
                resp["ok"] = true;
                resp["items"] = items_json;
            }
        }
        else if (cmd.Cmd == "item_info")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty())
            {
                resp["ok"] = false;
                resp["error"] = "Missing 'ref' parameter";
            }
            else
            {
                ImGuiTestItemInfo info = ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError);
                if (info.ID != 0)
                {
                    resp["ok"] = true;
                    resp["id"] = info.ID;
                    resp["label"] = info.DebugLabel;
                    resp["x"] = info.RectFull.Min.x;
                    resp["y"] = info.RectFull.Min.y;
                    resp["width"] = info.RectFull.GetWidth();
                    resp["height"] = info.RectFull.GetHeight();
                    resp["status_flags"] = (unsigned int)info.StatusFlags;
                }
                else
                {
                    resp["ok"] = false;
                    resp["error"] = "Item not found: " + ref;
                }
            }
        }
        else if (cmd.Cmd == "item_exists")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty())
            {
                resp["ok"] = false;
                resp["error"] = "Missing 'ref' parameter";
            }
            else
            {
                bool exists = ctx->ItemExists(ref.c_str());
                resp["ok"] = true;
                resp["exists"] = exists;
            }
        }
        else if (cmd.Cmd == "shutdown")
        {
            g_WantsShutdown.store(true);
            resp["ok"] = true;
        }
        else
        {
            resp["ok"] = false;
            resp["error"] = "Unknown command: " + cmd.Cmd;
        }
    }
    catch (const std::exception& e)
    {
        resp["ok"] = false;
        resp["error"] = std::string("Exception: ") + e.what();
    }
    catch (...)
    {
        resp["ok"] = false;
        resp["error"] = "Unknown exception";
    }

    return resp;
}

//-----------------------------------------------------------------------------
// Test Coroutine (registered as a test, queued on-demand)
//
// Processes all pending commands then exits immediately, so the test engine
// releases input control back to the human user between commands.
//-----------------------------------------------------------------------------

static void RemoteControlTestFunc(ImGuiTestContext* ctx)
{
    // Process all commands currently in the queue
    while (g_Running.load())
    {
        McpCommand cmd;
        bool has_cmd = false;
        {
            std::lock_guard<std::mutex> lock(g_QueueMutex);
            if (!g_CommandQueue.empty())
            {
                cmd = std::move(g_CommandQueue.front());
                g_CommandQueue.pop_front();
                has_cmd = true;
            }
        }

        if (!has_cmd)
            break;  // Queue empty — exit test to release input control

        nlohmann::json response = DispatchCommand(ctx, cmd);
        cmd.Promise.set_value(std::move(response));

        // Yield between commands so the UI can update
        ctx->Yield();
    }

    g_TestRunning.store(false);
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

bool ImGuiMcpBridge_Init(const ImGuiMcpBridgeConfig& config)
{
    g_Config = config;
    g_WantsShutdown.store(false);
    g_TestRunning.store(false);

    // Setup capture user data
    g_CaptureUserData.D3DDevice = config.D3DDevice;
    g_CaptureUserData.SwapChain = config.SwapChain;

    // Setup screen capture callback on the test engine
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(config.TestEngine);
    test_io.ScreenCaptureFunc = ImGuiMcpCapture_ScreenCaptureFunc;
    test_io.ScreenCaptureUserData = &g_CaptureUserData;

    // Register the remote_control test (but don't queue it yet — Tick() will
    // queue it on-demand when commands arrive)
    g_Test = ImGuiTestEngine_RegisterTest(config.TestEngine, "mcp", "remote_control");
    g_Test->TestFunc = RemoteControlTestFunc;
    g_Test->Flags |= ImGuiTestFlags_NoGuiWarmUp;  // Skip warmup frames for lower latency

    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        printf("[MCP Bridge] WSAStartup failed\n");
        return false;
    }

    // Create listen socket
    g_ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ListenSocket == INVALID_SOCKET)
    {
        printf("[MCP Bridge] socket() failed\n");
        WSACleanup();
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(g_ListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)config.Port);

    if (bind(g_ListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("[MCP Bridge] bind() failed on port %d\n", config.Port);
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(g_ListenSocket, 1) == SOCKET_ERROR)
    {
        printf("[MCP Bridge] listen() failed\n");
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    printf("[MCP Bridge] Listening on port %d\n", config.Port);

    // Start accept thread
    g_Running.store(true);
    g_AcceptThread = std::thread(AcceptThreadFunc);

    return true;
}

void ImGuiMcpBridge_Tick()
{
    // Called from the main loop each frame. If commands are waiting and the
    // test isn't already running, queue it so the test engine picks them up.
    if (!g_TestRunning.load())
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        if (!g_CommandQueue.empty())
        {
            g_TestRunning.store(true);
            ImGuiTestEngine_QueueTest(g_Config.TestEngine, g_Test);
        }
    }
}

void ImGuiMcpBridge_Shutdown()
{
    printf("[MCP Bridge] Shutting down...\n");

    g_Running.store(false);

    // Close listen socket to unblock accept
    if (g_ListenSocket != INVALID_SOCKET)
    {
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
    }

    // Join accept thread
    if (g_AcceptThread.joinable())
        g_AcceptThread.join();

    // Clear any pending commands
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        while (!g_CommandQueue.empty())
        {
            auto& cmd = g_CommandQueue.front();
            nlohmann::json resp;
            resp["id"] = cmd.Id;
            resp["ok"] = false;
            resp["error"] = "Bridge shutting down";
            cmd.Promise.set_value(resp);
            g_CommandQueue.pop_front();
        }
    }

    WSACleanup();
    printf("[MCP Bridge] Shutdown complete\n");
}

bool ImGuiMcpBridge_WantsShutdown()
{
    return g_WantsShutdown.load();
}
