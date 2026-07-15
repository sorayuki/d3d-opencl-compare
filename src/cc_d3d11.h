#pragma once

#include "color_conv.h"

#include <cstddef>
#include <memory>

void init_d3d11_colorconv(void* device);
bool is_d3d11_colorconv_avail();
void d3d11_colorconv_set_enabled(bool enabled);
bool d3d11_nv12_to_bgra_frame(
    const asco::ColorInfo&, std::size_t, std::size_t, void*, std::size_t,
    const void*, std::size_t, const void*, std::size_t);
bool d3d11_i420_to_bgra_frame(
    const asco::ColorInfo&, std::size_t, std::size_t, const void*, std::size_t,
    const void*, std::size_t, const void*, std::size_t, void*, std::size_t);
bool d3d11_yuy2_to_bgra_frame(
    const asco::ColorInfo&, std::size_t, std::size_t, const void*, std::size_t,
    void*, std::size_t);
bool d3d11_bgra_to_i420_frame(
    const asco::ColorInfo&, std::size_t, std::size_t, const void*, std::size_t,
    void*, std::size_t, void*, std::size_t, void*, std::size_t);

struct D3D11InputBuf {
    D3D11InputBuf(const void* pointer, std::size_t size);
    struct Internal;
    std::shared_ptr<Internal> data_;
};

bool d3d11_i420_to_image2d(
    const asco::ColorInfo&, const D3D11InputBuf&, std::size_t,
    const D3D11InputBuf&, std::size_t, const D3D11InputBuf&, std::size_t,
    std::size_t, std::size_t, void* texture2d);

