#pragma once

#include <cstddef>

#include "color_types.h"

#if defined(BENCHMARK_D3D11) || defined(BENCHMARK_D3D11ON12)
void init_d3d11_colorconv(void* device);
bool is_d3d11_colorconv_avail();
bool d3d11_nv12_to_bgra_frame(
    const asco::ColorInfo& color_info,
    std::size_t width,
    std::size_t height,
    void* destination,
    std::size_t destination_stride,
    const void* source_y,
    std::size_t source_y_stride,
    const void* source_uv,
    std::size_t source_uv_stride);
#elif defined(BENCHMARK_D3D12)
void init_d3d12_colorconv(void* d3d11_device);
bool is_d3d12_colorconv_avail();
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
#elif defined(BENCHMARK_OPENCL)
void init_opencl(void* d3d11_device);
bool is_opencl_avail();
bool opencl_nv12_to_bgra_frame(
    const asco::ColorInfo& color_info,
    std::size_t width,
    std::size_t height,
    void* destination,
    std::size_t destination_stride,
    const void* source_y,
    std::size_t source_y_stride,
    const void* source_uv,
    std::size_t source_uv_stride);
#else
#error A benchmark backend must be selected
#endif
