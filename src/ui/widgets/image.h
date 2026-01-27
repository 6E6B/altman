#pragma once
#include "network/http.h"

#include <expected>
#include <format>
#include <string>


#ifdef _WIN32
#include <d3d11.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#ifdef __OBJC__
#import <Metal/Metal.h>
#endif
#endif

class TextureHandle {
    public:
        TextureHandle() = default;

        explicit TextureHandle(void *retainedTexture) : m_texture(retainedTexture) {
        }

#if !defined(_WIN32) && defined(__OBJC__)
        explicit TextureHandle(id<MTLTexture> texture) :
            m_texture(texture ? const_cast<void *>(static_cast<const void *>(CFBridgingRetain(texture))) : nullptr) {
        }
#endif

        ~TextureHandle() {
            reset();
        }

        TextureHandle(const TextureHandle &) = delete;
        TextureHandle &operator=(const TextureHandle &) = delete;

        TextureHandle(TextureHandle &&other) noexcept : m_texture(other.m_texture) {
            other.m_texture = nullptr;
        }

        TextureHandle &operator=(TextureHandle &&other) noexcept {
            if (this != &other) {
                reset();
                m_texture = other.m_texture;
                other.m_texture = nullptr;
            }
            return *this;
        }

        void reset() {
            if (m_texture) {
#ifdef _WIN32
                static_cast<ID3D11ShaderResourceView *>(m_texture)->Release();
#else
                CFRelease(m_texture);
#endif
                m_texture = nullptr;
            }
        }

        void reset(void *retainedTexture) {
            reset();
            m_texture = retainedTexture;
        }

#if !defined(_WIN32) && defined(__OBJC__)
        void reset(id<MTLTexture> texture) {
            reset();
            if (texture) {
                m_texture = const_cast<void *>(static_cast<const void *>(CFBridgingRetain(texture)));
            }
        }
#endif

        [[nodiscard]] void *get() const {
            return m_texture;
        }
        [[nodiscard]] explicit operator bool() const {
            return m_texture != nullptr;
        }

    private:
        void *m_texture = nullptr;
};

struct TextureLoadResult {
        TextureHandle texture;
        int width = 0;
        int height = 0;
};

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromMemory(const void *data, size_t data_size);

[[nodiscard]]
std::expected<TextureLoadResult, std::string> LoadTextureFromFile(const char *file_name);

[[nodiscard]]
inline std::expected<TextureLoadResult, std::string> LoadImageFromUrl(const std::string &url) {
    auto resp = HttpClient::get(url);
    if (resp.status_code != 200 || resp.text.empty()) {
        return std::unexpected(std::format("HTTP error: {}", resp.status_code));
    }
    return LoadTextureFromMemory(resp.text.data(), resp.text.size());
}
