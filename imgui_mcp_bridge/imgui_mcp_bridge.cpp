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

            // Wait for the coroutine to process and return the result
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
            // Use test engine's capture system
            ImGuiCaptureArgs args;
            ImGuiCaptureImageBuf image_buf;
            args.InOutputImageBuf = &image_buf;
            args.InFlags = ImGuiCaptureFlags_Instant;

            bool ok = ctx->CaptureScreenshot(args.InFlags);
            if (!ok)
            {
                // Fallback: capture to temp file, read back
                char tmp_path[512];
                snprintf(tmp_path, sizeof(tmp_path), "%s/imgui_mcp_screenshot.png", ".");
                ImStrncpy(args.InOutputFile, tmp_path, sizeof(args.InOutputFile));
                args.InOutputImageBuf = &image_buf;
                ok = ctx->CaptureScreenshot(0);
            }

            if (ok && image_buf.Data && image_buf.Width > 0 && image_buf.Height > 0)
            {
                std::string b64 = ImGuiMcpCapture_EncodePNG(image_buf.Data, image_buf.Width, image_buf.Height);
                resp["ok"] = true;
                resp["image"] = b64;
                resp["width"] = image_buf.Width;
                resp["height"] = image_buf.Height;
            }
            else
            {
                // Direct capture fallback: use our own capture function
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
                ctx->ItemClick(ref.c_str());
                resp["ok"] = true;
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
                ctx->ItemDoubleClick(ref.c_str());
                resp["ok"] = true;
            }
        }
        else if (cmd.Cmd == "check")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemCheck(ref.c_str()); resp["ok"] = true; }
        }
        else if (cmd.Cmd == "uncheck")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemUncheck(ref.c_str()); resp["ok"] = true; }
        }
        else if (cmd.Cmd == "open")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemOpen(ref.c_str()); resp["ok"] = true; }
        }
        else if (cmd.Cmd == "close")
        {
            std::string ref = cmd.Params.value("ref", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else { ctx->ItemClose(ref.c_str()); resp["ok"] = true; }
        }
        else if (cmd.Cmd == "input")
        {
            std::string ref = cmd.Params.value("ref", "");
            std::string text = cmd.Params.value("text", "");
            if (ref.empty()) { resp["ok"] = false; resp["error"] = "Missing 'ref'"; }
            else
            {
                ctx->ItemInput(ref.c_str());
                if (!text.empty())
                    ctx->KeyCharsReplace(text.c_str());
                resp["ok"] = true;
            }
        }
        else if (cmd.Cmd == "key_press")
        {
            // Accept key name as string, map to ImGuiKey
            std::string key = cmd.Params.value("key", "");
            if (key.empty()) { resp["ok"] = false; resp["error"] = "Missing 'key'"; }
            else
            {
                ImGuiKey imgui_key = ImGuiKey_None;
                // Common key mappings
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
                // Skip internal/debug windows
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
// Test Coroutine (registered as a test, runs on the main thread)
//-----------------------------------------------------------------------------

static void RemoteControlTestFunc(ImGuiTestContext* ctx)
{
    printf("[MCP Bridge] Remote control test started\n");

    while (g_Running.load() && !ctx->IsError())
    {
        // Check for commands
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

        if (has_cmd)
        {
            nlohmann::json response = DispatchCommand(ctx, cmd);
            cmd.Promise.set_value(std::move(response));
        }

        ctx->Yield();
    }

    printf("[MCP Bridge] Remote control test ended\n");
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

bool ImGuiMcpBridge_Init(const ImGuiMcpBridgeConfig& config)
{
    g_Config = config;
    g_WantsShutdown.store(false);

    // Setup capture user data
    g_CaptureUserData.D3DDevice = config.D3DDevice;
    g_CaptureUserData.SwapChain = config.SwapChain;

    // Setup screen capture callback on the test engine
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(config.TestEngine);
    test_io.ScreenCaptureFunc = ImGuiMcpCapture_ScreenCaptureFunc;
    test_io.ScreenCaptureUserData = &g_CaptureUserData;

    // Register the remote_control test
    ImGuiTest* test = ImGuiTestEngine_RegisterTest(config.TestEngine, "mcp", "remote_control");
    test->TestFunc = RemoteControlTestFunc;

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

    // Queue the remote_control test to start running
    ImGuiTestEngine_QueueTest(config.TestEngine, test);

    return true;
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
