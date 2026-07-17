#pragma once

#include "color_types.h"

#include <windows.h>

#include <cstddef>
#include <limits>

namespace colorconv {

struct ColorParams {
    float k[3]{};
    float range[4]{};
};

struct Nv12Layout {
    UINT width = 0;
    UINT height = 0;
    UINT destination_stride = 0;
    UINT y_stride = 0;
    UINT uv_stride = 0;
    UINT y_bytes = 0;
    UINT uv_bytes = 0;
    UINT output_bytes = 0;
};

inline bool to_uint(std::size_t value, UINT& result) {
    if (value > (std::numeric_limits<UINT>::max)())
        return false;
    result = static_cast<UINT>(value);
    return true;
}

inline bool product_to_uint(std::size_t a, std::size_t b, UINT& result) {
    if (a != 0 && b > (std::numeric_limits<UINT>::max)() / a)
        return false;
    return to_uint(a * b, result);
}

inline bool validate_nv12(std::size_t width, std::size_t height,
                          std::size_t destination_stride,
                          std::size_t y_stride, std::size_t uv_stride,
                          Nv12Layout& layout) {
    if (width < 2 || height < 2 || (width & 1) || (height & 1) ||
        y_stride < width || uv_stride < width ||
        width > (std::numeric_limits<UINT>::max)() / 4 ||
        destination_stride < width * 4)
        return false;

    return to_uint(width, layout.width) &&
           to_uint(height, layout.height) &&
           to_uint(destination_stride, layout.destination_stride) &&
           to_uint(y_stride, layout.y_stride) &&
           to_uint(uv_stride, layout.uv_stride) &&
           product_to_uint(y_stride, height, layout.y_bytes) &&
           product_to_uint(uv_stride, height / 2, layout.uv_bytes) &&
           product_to_uint(width * 4, height, layout.output_bytes);
}

inline ColorParams make_color_params(const asco::ColorInfo& color_info) {
    ColorParams params{};
    const bool bt601 =
        color_info.trans_matrix == asco::ColorTransMatrix::BT601;
    params.k[0] = bt601 ? .299f : .2126f;
    params.k[1] = bt601 ? .587f : .7152f;
    params.k[2] = bt601 ? .114f : .0722f;

    const bool full_range =
        color_info.nominal_range == asco::ColorNominalRange::_0_255;
    params.range[0] = full_range ? 0.0f : 16.0f / 255.0f;
    params.range[1] = full_range ? 1.0f : 255.0f / 219.0f;
    params.range[2] = params.range[0];
    params.range[3] = full_range ? 1.0f : 255.0f / 224.0f;
    return params;
}

}  // namespace colorconv
