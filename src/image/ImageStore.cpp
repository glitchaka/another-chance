#include "image/ImageStore.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>

namespace ac::image {
using Microsoft::WRL::ComPtr;

Store::~Store() { Clear(); }

void Store::Clear() {
    for (auto& entry : images_) {
        if (entry.second.handle) DeleteObject(entry.second.handle);
    }
    images_.clear();
}

const Bitmap* Store::Find(const std::wstring& url) const {
    auto found = images_.find(url);
    return found == images_.end() ? nullptr : &found->second;
}

bool Store::Decode(const std::wstring& url, const std::string& bytes) {
    if (url.empty() || bytes.empty() || bytes.size() > 4 * 1024 * 1024) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
            static_cast<DWORD>(bytes.size())))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height))) return false;
    if (!width || !height || width > 4096 || height > 4096 ||
        static_cast<uint64_t>(width) * height > 16 * 1024 * 1024) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf()))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
           WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return false;
    }

    const UINT stride = width * 4;
    const UINT bytesCount = stride * height;
    if (FAILED(converter->CopyPixels(nullptr, stride, bytesCount,
                                     static_cast<BYTE*>(pixels)))) {
        DeleteObject(bitmap);
        return false;
    }

    auto existing = images_.find(url);
    if (existing != images_.end() && existing->second.handle) {
        DeleteObject(existing->second.handle);
    }
    images_.insert_or_assign(url, Bitmap{bitmap,
        static_cast<int>(width), static_cast<int>(height)});
    return true;
}
} // namespace ac::image
