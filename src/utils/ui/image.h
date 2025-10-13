#pragma once

#include "network/http.hpp"
#include <d3d11.h>
#include <string>

// Loads an image from a URL into a D3D11 shader resource view.
// Returns true on success and fills out_srv / out_width / out_height.
// Requires the helper LoadTextureFromMemory (declared in main.cpp) to be visible.
extern bool LoadTextureFromMemory(
	const void *data,
	size_t data_size,
	ID3D11ShaderResourceView **out_srv,
	int *out_width,
	int *out_height
);

inline bool
	LoadImageFromUrl(const std::string &url, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height) {
	auto resp = HttpClient::get(url);
	if (resp.status_code != 200 || resp.text.empty()) { return false; }
	return LoadTextureFromMemory(resp.text.data(), resp.text.size(), out_srv, out_width, out_height);
}
