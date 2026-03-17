#include "imgui_mcp_capture.h"

#include <vector>
#include <cstring>

// stb_image_write for PNG encoding
// STBIWDEF defaults to 'static' when STB_IMAGE_WRITE_IMPLEMENTATION is defined,
// so each translation unit gets its own copy - no linker conflicts.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "thirdparty/stb/imstb_image_write.h"

//-----------------------------------------------------------------------------
// Screen Capture Callback
//-----------------------------------------------------------------------------

bool ImGuiMcpCapture_ScreenCaptureFunc(ImGuiID viewport_id, int x, int y, int w, int h, unsigned int* pixels, void* user_data)
{
    (void)viewport_id;
    auto* data = static_cast<ImGuiMcpCaptureUserData*>(user_data);
    if (!data || !data->D3DDevice || !data->SwapChain)
        return false;

    ID3D11DeviceContext* ctx = nullptr;
    data->D3DDevice->GetImmediateContext(&ctx);
    if (!ctx)
        return false;

    // Get the backbuffer
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = data->SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr) || !backbuffer)
    {
        ctx->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC bb_desc;
    backbuffer->GetDesc(&bb_desc);

    // Create a staging texture for the requested region
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = w;
    staging_desc.Height = h;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = bb_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    hr = data->D3DDevice->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || !staging)
    {
        backbuffer->Release();
        ctx->Release();
        return false;
    }

    // Copy the requested region from backbuffer to staging
    D3D11_BOX src_box = {};
    src_box.left = x;
    src_box.top = y;
    src_box.right = x + w;
    src_box.bottom = y + h;
    src_box.front = 0;
    src_box.back = 1;
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, backbuffer, 0, &src_box);

    // Map and copy pixels
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        staging->Release();
        backbuffer->Release();
        ctx->Release();
        return false;
    }

    // Copy row by row (pitch may differ)
    for (int row = 0; row < h; row++)
    {
        const unsigned char* src = static_cast<const unsigned char*>(mapped.pData) + row * mapped.RowPitch;
        unsigned int* dst = pixels + row * w;
        memcpy(dst, src, w * 4);
    }

    ctx->Unmap(staging, 0);
    staging->Release();
    backbuffer->Release();
    ctx->Release();

    return true;
}

//-----------------------------------------------------------------------------
// PNG Encoding (via stb_image_write callback)
//-----------------------------------------------------------------------------

static void stbi_write_callback(void* context, void* data, int size)
{
    auto* buf = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}

std::string ImGuiMcpCapture_EncodePNG(const unsigned int* pixels, int w, int h)
{
    std::vector<unsigned char> png_data;
    png_data.reserve(w * h * 4);

    int result = stbi_write_png_to_func(stbi_write_callback, &png_data, w, h, 4, pixels, w * 4);
    if (!result)
        return {};

    return ImGuiMcpCapture_Base64Encode(png_data.data(), png_data.size());
}

//-----------------------------------------------------------------------------
// Base64 Encoding
//-----------------------------------------------------------------------------

std::string ImGuiMcpCapture_Base64Encode(const unsigned char* data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        unsigned int n = (static_cast<unsigned int>(data[i]) << 16);
        if (i + 1 < len) n |= (static_cast<unsigned int>(data[i + 1]) << 8);
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);

        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3F] : '=');
    }

    return result;
}
