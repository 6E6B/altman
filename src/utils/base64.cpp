#include "base64.h"

#include <array>
#include <algorithm>

namespace {
    constexpr std::string_view BASE64_CHARS = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    constexpr char PADDING_CHAR = '=';
    constexpr std::size_t ENCODE_BLOCK_SIZE = 3;
    constexpr std::size_t DECODE_BLOCK_SIZE = 4;

    constexpr std::array<std::uint8_t, 256> buildDecodeLookup() noexcept {
        std::array<std::uint8_t, 256> lookup{};
        lookup.fill(255);
        
        for (std::size_t i = 0; i < BASE64_CHARS.size(); ++i) {
            lookup[static_cast<std::uint8_t>(BASE64_CHARS[i])] = static_cast<std::uint8_t>(i);
        }
        return lookup;
    }
    
    constexpr auto DECODE_LOOKUP = buildDecodeLookup();

    constexpr bool isBase64Char(char c) noexcept {
        return (c >= 'A' && c <= 'Z') || 
               (c >= 'a' && c <= 'z') || 
               (c >= '0' && c <= '9') || 
               c == '+' || c == '/';
    }
    
    constexpr void encodeBlock(const std::array<std::uint8_t, 3>& input, 
                               std::array<char, 4>& output) noexcept {
        output[0] = BASE64_CHARS[(input[0] & 0xFC) >> 2];
        output[1] = BASE64_CHARS[((input[0] & 0x03) << 4) | ((input[1] & 0xF0) >> 4)];
        output[2] = BASE64_CHARS[((input[1] & 0x0F) << 2) | ((input[2] & 0xC0) >> 6)];
        output[3] = BASE64_CHARS[input[2] & 0x3F];
    }

    constexpr void encodePartialBlock(const std::array<std::uint8_t, 3>& input, 
                                      std::array<char, 4>& output, 
                                      std::size_t validBytes) noexcept {
        std::array<std::uint8_t, 3> padded{};
        for (std::size_t i = 0; i < validBytes; ++i) {
            padded[i] = input[i];
        }
        
        encodeBlock(padded, output);
        
        for (std::size_t i = validBytes + 1; i < 4; ++i) {
            output[i] = PADDING_CHAR;
        }
    }

    constexpr std::size_t decodeBlock(const std::array<std::uint8_t, 4>& input, 
                                      std::array<std::uint8_t, 3>& output) noexcept {
        output[0] = (input[0] << 2) | ((input[1] & 0x30) >> 4);
        output[1] = ((input[1] & 0x0F) << 4) | ((input[2] & 0x3C) >> 2);
        output[2] = ((input[2] & 0x03) << 6) | input[3];
        return 3;
    }
}

std::string base64_encode(std::span<const std::uint8_t> data) {
    if (data.empty()) 
        return {};

    const std::size_t outputSize = ((data.size() + 2) / 3) * 4;
    std::string result;
    result.reserve(outputSize);

    std::array<std::uint8_t, 3> inputBlock{};
    std::array<char, 4> outputBlock{};
    std::size_t blockIndex = 0;

    for (const std::uint8_t byte : data) {
        inputBlock[blockIndex++] = byte;
        
        if (blockIndex == ENCODE_BLOCK_SIZE) {
            encodeBlock(inputBlock, outputBlock);
            result.append(outputBlock.data(), DECODE_BLOCK_SIZE);
            blockIndex = 0;
            inputBlock.fill(0);
        }
    }

    if (blockIndex > 0) {
        encodePartialBlock(inputBlock, outputBlock, blockIndex);
        result.append(outputBlock.data(), DECODE_BLOCK_SIZE);
    }

    return result;
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    return base64_encode(std::span<const std::uint8_t>(data));
}

std::vector<std::uint8_t> base64_decode(std::string_view encoded) {
    if (encoded.empty()) 
        return {};

    std::size_t encodedLen = encoded.size();
    while (encodedLen > 0 && encoded[encodedLen - 1] == PADDING_CHAR) {
        --encodedLen;
    }

    const std::size_t outputSize = (encodedLen * 3) / 4;
    std::vector<std::uint8_t> result;
    result.reserve(outputSize);

    std::array<std::uint8_t, 4> inputBlock{};
    std::array<std::uint8_t, 3> outputBlock{};
    std::size_t blockIndex = 0;

    for (std::size_t i = 0; i < encodedLen; ++i) {
        const char c = encoded[i];
        
        if (!isBase64Char(c)) 
            continue;
        
        inputBlock[blockIndex++] = DECODE_LOOKUP[static_cast<std::uint8_t>(c)];
        
        if (blockIndex == DECODE_BLOCK_SIZE) {
            decodeBlock(inputBlock, outputBlock);
            result.insert(result.end(), outputBlock.begin(), outputBlock.end());
            blockIndex = 0;
            inputBlock.fill(0);
        }
    }

    if (blockIndex > 0) {
        for (std::size_t i = blockIndex; i < DECODE_BLOCK_SIZE; ++i) {
            inputBlock[i] = 0;
        }
        
        decodeBlock(inputBlock, outputBlock);
        
        const std::size_t validBytes = blockIndex - 1;
        result.insert(result.end(), outputBlock.begin(), outputBlock.begin() + validBytes);
    }

    return result;
}