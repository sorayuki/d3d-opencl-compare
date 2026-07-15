#pragma once

#include <memory>
#include "color_conv.h"

// 初始化 OpenCL 上下文，送入 ID3D11Device 以共享资源
void init_opencl(void* d3d11Dev);

bool is_opencl_avail();

// 设置是否启用 OpenCL。请在使用 OpenCL 的代码之前设置。
void opencl_set_enabled(bool enabled);

struct OpenCLInputBuf {
    OpenCLInputBuf(const void* ptr, size_t size);
    OpenCLInputBuf(const OpenCLInputBuf& rhs) = default;
    ~OpenCLInputBuf();

    struct Internal;
    std::shared_ptr<Internal> data_;
};

bool opencl_nv12_to_bgra_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    void* dst, size_t stride,
    const void* srcY, size_t strideY,
    const void* srcUV, size_t strideUV
);

bool opencl_bgra_to_i420_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    const void* src, size_t stride,
    void* dstY, size_t strideY,
    void* dstU, size_t strideU,
    void* dstV, size_t strideV
);

bool opencl_i420_to_bgra_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    const void* y, size_t y_stride,
    const void* u, size_t u_stride,
    const void* v, size_t v_stride,
    void* dst, size_t dst_stride
);

// image: bgra8888
bool opencl_i420_to_image2d(
    const asco::ColorInfo& ci,
    const OpenCLInputBuf& y, size_t strideY,
    const OpenCLInputBuf& u, size_t strideU,
    const OpenCLInputBuf& v, size_t strideV,
    size_t width, size_t height,
    void* tex2d
);

bool opencl_yuy2_to_bgra_frame(
    const asco::ColorInfo& ci,
    unsigned int width, unsigned int height,
    const void* src, size_t src_stride,
    void* dst, size_t dst_stride
);

