#pragma once

#include "network/http.h"
#include <string>

#ifdef _WIN32
    #include <d3d11.h>
    using TextureHandle = ID3D11ShaderResourceView*;
#else
    // On macOS with Metal, textures are void* (id<MTLTexture>)
    using TextureHandle = void*;
#endif

// Loads an image from memory into a texture.
// Returns true on success and fills out_texture / out_width / out_height.
// Implementation is platform-specific (D3D11 on Windows, Metal on macOS)
extern bool LoadTextureFromMemory(const void *data,
                                  size_t data_size,
                                  TextureHandle *out_texture,
                                  int *out_width,
                                  int *out_height);

inline bool LoadImageFromUrl(const std::string &url,
                             TextureHandle *out_texture,
                             int *out_width,
                             int *out_height)
{
    auto resp = HttpClient::get(url);
    if (resp.status_code != 200 || resp.text.empty())
        return false;
    return LoadTextureFromMemory(resp.text.data(), resp.text.size(), out_texture, out_width, out_height);
}