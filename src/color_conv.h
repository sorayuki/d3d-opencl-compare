#ifndef AUTOSCOPIA_COLORCONV_COLOR_CONV_H_
#define AUTOSCOPIA_COLORCONV_COLOR_CONV_H_

#include "color_types.h"


void nv12_to_bgra_frame(
    const void* src,
    unsigned int width,
    unsigned int height,
    size_t src_stride,
    const asco::ColorInfo& ci,
    size_t dst_stride,
    void* dst);

void i420_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void i420_to_bgra_frame(
    const void* y, size_t y_stride,
    const void* u, size_t u_stride,
    const void* v, size_t v_stride,
    unsigned int width, unsigned int height,
    const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void yuy2_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void yvyu_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void uyvy_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void yv12_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void y800_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void p010_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void bgra_to_i420_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void bgr24_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, bool flipped, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);

void bgra32_to_bgra_frame(
    const void* src,
    unsigned int width, unsigned int height,
    size_t src_stride, bool flipped, const asco::ColorInfo& ci,
    size_t dst_stride, void* dst);


#endif  // AUTOSCOPIA_COLORCONV_COLOR_CONV_H_