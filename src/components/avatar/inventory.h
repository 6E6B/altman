#pragma once
#ifdef _WIN32
    #include <d3d11.h>
    using TextureType = ID3D11ShaderResourceView*;
#else
    using TextureType = void*;
#endif

void RenderInventoryTab();