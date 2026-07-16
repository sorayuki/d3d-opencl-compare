#pragma once
#include "color_conv.h"
#include <memory>

void init_d3d11_colorconv(void *device);
bool is_d3d11_colorconv_avail();
void d3d11_colorconv_set_enabled(bool enabled);
bool d3d11_nv12_to_bgra_frame(const asco::ColorInfo &, size_t, size_t, void *, size_t, const void *,
                              size_t, const void *, size_t);
bool d3d11_i420_to_bgra_frame(const asco::ColorInfo &, size_t, size_t, const void *, size_t,
                              const void *, size_t, const void *, size_t, void *, size_t);
bool d3d11_yuy2_to_bgra_frame(const asco::ColorInfo &, size_t, size_t, const void *, size_t, void *,
                              size_t);
bool d3d11_bgra_to_i420_frame(const asco::ColorInfo &, size_t, size_t, const void *, size_t, void *,
                              size_t, void *, size_t, void *, size_t);

struct D3D11InputBuf {
    D3D11InputBuf(const void *ptr, size_t size);
    struct Internal;
    std::shared_ptr<Internal> data_;
};

bool d3d11_i420_to_image2d(const asco::ColorInfo &, const D3D11InputBuf &y, size_t yStride,
                           const D3D11InputBuf &u, size_t uStride, const D3D11InputBuf &v,
                           size_t vStride, size_t width, size_t height, void *texture2d);
