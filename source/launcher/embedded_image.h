#pragma once
#include <vector>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "embedded_assets.h"

namespace Launcher {
struct EmbeddedImage {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    UINT width{}, height{};
};

inline EmbeddedImage LoadEmbeddedImage(ID3D11Device* device, int id) {
    using Microsoft::WRL::ComPtr;
    EmbeddedImage out;
    const EmbeddedAsset asset = FindEmbeddedAsset(id);
    // WIC only reads a memory stream it decodes from; the cast is for its non-const signature.
    auto* data = const_cast<BYTE*>(asset.data);
    const DWORD length = DWORD(asset.size);
    if (!data || length == 0) return out;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(data, length)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&out.width, &out.height)) ||
        out.width == 0 || out.height == 0 || out.width > 1024 || out.height > 1024 ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return {};
    std::vector<BYTE> pixels(size_t(out.width) * out.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, out.width * 4, UINT(pixels.size()), pixels.data())))
        return {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = out.width;
    desc.Height = out.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), out.width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &out.view)))
        return {};
    return out;
}

} // namespace Launcher
