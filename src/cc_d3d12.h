#pragma once

#include "color_conv.h"

#include <cstddef>

// The device is an ID3D11Device used only to select the same DXGI adapter as
// the other benchmark backends. The converter creates its own D3D12 device.
void init_d3d12_colorconv(void* d3d11_device);
bool is_d3d12_colorconv_avail();
void d3d12_colorconv_set_enabled(bool enabled);
const char* d3d12_colorconv_transfer_mode();

bool d3d12_nv12_to_bgra_frame(
    const asco::ColorInfo& color_info,
    std::size_t width,
    std::size_t height,
    void* destination,
    std::size_t destination_stride,
    const void* source_y,
    std::size_t source_y_stride,
    const void* source_uv,
    std::size_t source_uv_stride);
