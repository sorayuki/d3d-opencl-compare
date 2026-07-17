#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 200

#include "cc_opencl.h"
#include <CL/opencl.hpp>
#include <CL/cl_d3d11.h>
#include <cassert>
#include <vector>
#include <string>
#include <list>
#include <tuple>
#include <optional>
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
    ID3D11Device* shared_d3d11dev_ = nullptr;

    bool available_ = false;
    bool opencl_enabled_ = true;
    cl::Platform clPlat_;
    cl::Context ctx_;
    cl::CommandQueue queue_;
    cl::Program program_;
    std::string device_name_;
    UINT preferred_vendor_id_ = 0;
    bool isClShareMemory_ = false;
    bool isClSvmSupported_ = false;

    std::atomic_size_t poolSizeSum_{ 0 };

    template<class BufT>
    class Pool {
        using Item = std::pair<size_t, BufT>;
        std::mutex mutex_;
        std::list<Item> data_;
    public:
        BufT retain(size_t size) {
            std::unique_lock lg(mutex_);
            for (auto it = data_.begin(); it != data_.end(); ++it) {
                if (it->first == size) {
                    auto ret = it->second;
                    poolSizeSum_ -= size;
                    data_.erase(it);
                    return ret;
                }
            }

            if constexpr (std::is_same_v<BufT, cl::Buffer>) {
                return cl::Buffer{ ctx_, CL_MEM_READ_WRITE, size };
            }
            else {
                return clSVMAlloc(ctx_(), CL_MEM_READ_WRITE, size, 0);
            }
        }

        void release(BufT buf, size_t size) {
            std::unique_lock lg(mutex_);
            data_.emplace_back(std::make_pair(size, buf));
            poolSizeSum_ += size;

            while (data_.size() > 12 || poolSizeSum_ > 64 * 1024 * 1024) {
                auto last = data_.back();
                poolSizeSum_ -= last.first;
                if constexpr (std::is_same_v<BufT, void*>) {
                    clSVMFree(ctx_(), last.second);
                }
                data_.pop_back();
            }
        }

        static Pool& Get() {
            static Pool instance;
            return instance;
        }
    };

    class CCClBuffer {
        size_t size_;
        void* hostPtr_;

        cl::Buffer buffer_;
        void* svmPtr_ = nullptr;

        bool isWrite_ = false;

        void Init(void* ptr, size_t size, bool isWrite) {
            cl_int ret;

            size_ = size;
            hostPtr_ = ptr;
            isWrite_ = isWrite;

            if (isClSvmSupported_) {
                svmPtr_ = Pool<void*>::Get().retain(size);
                assert(svmPtr_);

                if (isWrite_ == false) {
                    if (isClShareMemory_) {
                        ret = clEnqueueSVMMap(queue_(), true, CL_MAP_WRITE_INVALIDATE_REGION, svmPtr_, size_, 0, nullptr, nullptr);
                        assert(ret == CL_SUCCESS);

                        memcpy(svmPtr_, hostPtr_, size_);
                        ret = clEnqueueSVMUnmap(queue_(), svmPtr_, 0, nullptr, nullptr);
                        assert(ret == CL_SUCCESS);
                    }
                    else {
                        ret = clEnqueueSVMMemcpy(queue_(), true, svmPtr_, hostPtr_, size_, 0, nullptr, nullptr);
                        assert(ret == CL_SUCCESS);
                    }
                }
            }
            else {
                buffer_ = Pool<cl::Buffer>::Get().retain(size);
                assert(buffer_.get());

                if (isWrite_ == false) {
                    ret = queue_.enqueueWriteBuffer(buffer_, true, 0, size_, ptr);
                    assert(ret == CL_SUCCESS);
                }
            }
        }
    public:
        CCClBuffer(void* ptr, size_t size, bool isWrite) {
            Init(ptr, size, isWrite);
        }

        CCClBuffer(const void* ptr, size_t size) {
            Init((void*)ptr, size, false);
        }

        ~CCClBuffer() {
            if (isWrite_) {
                if (svmPtr_) {
                    if (isClShareMemory_) {
                        clEnqueueSVMMap(queue_(), true, CL_MAP_READ, svmPtr_, size_, 0, nullptr, nullptr);
                        memcpy(hostPtr_, svmPtr_, size_);
                        clEnqueueSVMUnmap(queue_(), svmPtr_, 0, nullptr, nullptr);
                    }
                    else {
                        clEnqueueSVMMemcpy(queue_(), true, hostPtr_, svmPtr_, size_, 0, nullptr, nullptr);
                    }
                }
                else {
                    queue_.enqueueReadBuffer(buffer_, true, 0, size_, hostPtr_);
                }
            }

            if (svmPtr_) {
                Pool<void*>::Get().release(svmPtr_, size_);
            }
            else {
                Pool<cl::Buffer>::Get().release(buffer_, size_);
            }
        }

        operator cl::Buffer& () {
            return buffer_;
        }

        operator void* () {
            return svmPtr_;
        }
    };
};

static void InitCLEnv() try {
    static bool initialize = false;
    if (initialize || !opencl_enabled_)
        return;

    if (sizeof(void*) == 4)
        throw std::runtime_error("32bit opencl is buggy.");

    initialize = true;

    std::vector<std::tuple<cl::Platform, cl::Device>> platdevlist;
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    for (auto& p : platforms) {
        std::string platver = p.getInfo<CL_PLATFORM_VERSION>();
        if (platver.find("OpenCL 2.") != std::string::npos ||
            platver.find("OpenCL 3.") != std::string::npos)
        {
            cl::vector<cl::Device> devices;
            p.getDevices(CL_DEVICE_TYPE_ALL, &devices);

            for (auto& dev : devices) {
                auto devver = dev.getInfo<CL_DEVICE_VERSION>();
                if (devver.find("OpenCL 2.") != std::string::npos ||
                    devver.find("OpenCL 3.") != std::string::npos)
                {
                    platdevlist.emplace_back(std::make_tuple(p, dev));
                }
            }
        }
    }

    std::tuple<cl::Platform, cl::Device> selected;

    // Prefer the OpenCL device from the same vendor as the D3D adapter.
    if (std::get<0>(selected)() == 0) {
        for (const auto& candidate : platdevlist) {
            const auto vendor = std::get<1>(candidate).getInfo<CL_DEVICE_VENDOR>();
            const bool nvidia = preferred_vendor_id_ == 0x10de &&
                                vendor.find("NVIDIA") != std::string::npos;
            const bool intel = preferred_vendor_id_ == 0x8086 &&
                               vendor.find("Intel") != std::string::npos;
            if (nvidia || intel) {
                selected = candidate;
                break;
            }
        }
        if (std::get<0>(selected)() == 0 && !platdevlist.empty())
            selected = platdevlist.front();
    }

    if (std::get<0>(selected)() == 0)
        return;

    clPlat_ = std::get<0>(selected)();
    cl_context_properties ctxprops[] = {
        CL_CONTEXT_PLATFORM, (cl_context_properties)clPlat_(),
        0
    };
    ctx_ = cl::Context(cl::vector<cl::Device>{ std::get<1>(selected) }, ctxprops);
    queue_ = cl::CommandQueue{ ctx_, std::get<1>(selected), cl::QueueProperties::None };
    device_name_ = std::get<1>(selected).getInfo<CL_DEVICE_NAME>();
    isClSvmSupported_ = std::get<1>(selected).getInfo<CL_DEVICE_SVM_CAPABILITIES>() > 0;
    if (isClSvmSupported_) {
        // 看看是不是真的支持 SVM。目前 x86 下 isClSvmSupported_ 是 true，但是实际上不支持。
        auto svm_ptr = Pool<void*>::Get().retain(16);
        if (!svm_ptr) {
            isClSvmSupported_ = false;
        } else {
            Pool<void*>::Get().release(svm_ptr, 16);
        }
    }

    {
        cl_device_info info;
        size_t infosize = 0;
        clGetDeviceInfo(std::get<1>(selected)(), CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof(info), &info, &infosize);
        if (infosize == sizeof(info) && info) {
            isClShareMemory_ = true;
        }
    }

    // 编译kernel程序
    std::string sourcecode{ R"OPENCLCODE(
        uchar3 yuv2rgb(uchar3 yuv, float3 k, float4 yuvscale) {
            float3 f =
                (convert_float3(yuv) / (float3)(255.0f) - (float3)(yuvscale.x, yuvscale.z, yuvscale.z))
                * (float3)(yuvscale.y, 2.0f * yuvscale.w, 2.0f * yuvscale.w)
                - (float3)(0.0f, 1.0f, 1.0f)
                ;

            float r = f.x + f.z * (1.0f - k.x);
            float g = f.x - f.y * (1.0f - k.z) * k.z / k.y - f.z * (1.0f - k.x) * k.x / k.y;
            float b = f.x + f.y * (1.0f - k.z);
            // Match the D3D11 shader's round(255 * rgb) before narrowing to 8 bits.
            // The unsuffixed conversion truncates on the OpenCL implementation used
            // here, which made OpenCL output one code value lower for many pixels.
            return convert_uchar3_sat_rte((float3)(r, g, b) * (float3)(255.0f));
        }

        uchar3 rgb2yuv(uchar3 rgb, float3 k, float4 yuvscale) {
            float3 f = convert_float3(rgb) / (float3)(255.0f);
            float y = clamp(dot(f, k), 0.0f, 1.0f);
            float v = clamp((f.x - y) / (1.0f - k.x), -1.0f, 1.0f);
            float u = clamp((f.z - y) / (1.0f - k.z), -1.0f, 1.0f);
            return convert_uchar3_sat_rte((float3)(
                y / yuvscale.y + yuvscale.x,
                u / yuvscale.w / 2.0f + 0.5f,
                v / yuvscale.w / 2.0f + 0.5f
            ) * (float3)(255.0f));
        }

        uchar rgb2y(uchar3 rgb, float3 k, float4 yuvscale) {
            float3 f = convert_float3(rgb) / (float3)(255.0f);
            float y = clamp(dot(f, k), 0.0f, 1.0f);
            return convert_uchar_sat_rte((y / yuvscale.y + yuvscale.x) * 255.0f);
        }

        kernel void bgra_to_i420_frame(
            global const uchar* src, int stride,
            global uchar* dstY, int strideY,
            global uchar* dstU, int strideU,
            global uchar* dstV, int strideV,
            const float3 k, const float4 yuvscale
        ) {
            const int xd2 = get_global_id(0);
            const int yd2 = get_global_id(1);
            const int x = xd2 * 2;
            const int y = yd2 * 2;

            for(int j = 0; j < 2; ++j) {
                for(int i = 0; i < 2; ++i) {
                    uchar4 srcvec = vload4(x + i, src + (y + j) * stride);
                    if (i == 0 && j == 0) {
                        uchar3 yuv = rgb2yuv(srcvec.zyx, k, yuvscale);
                        dstY[(y + j) * strideY + (x + i)] = yuv.x;
                        dstU[yd2 * strideU + xd2] = yuv.y;
                        dstV[yd2 * strideV + xd2] = yuv.z;
                    } else {
                        dstY[(y + j) * strideY + (x + i)] = rgb2y(srcvec.zyx, k, yuvscale);
                    }
                }
            }
        }

        kernel void nv12_to_bgra_frame(
            global uchar* dst, int stride,
            global const uchar* srcY, int strideY,
            global const uchar* srcUV, int strideUV,
            const float3 k, const float4 yuvscale
        ) {
            const int xd2 = get_global_id(0);
            const int yd2 = get_global_id(1);
            const int x = xd2 * 2;
            const int y = yd2 * 2;

            uchar2 uv = vload2(xd2, srcUV + yd2 * strideUV);

            for(int j = 0; j < 2; ++j) {
                for(int i = 0; i < 2; ++i) {
                    uchar3 rgb = yuv2rgb((uchar3)(srcY[(y + j) * strideY + (x + i)], uv.x, uv.y), k, yuvscale);
                    vstore4((uchar4)(rgb.zyx, 255), x + i, dst + (y + j) * stride);
                }
            }
        }

        kernel void i420_to_bgra_frame(
            global uchar* dst, int stride,
            global const uchar* srcY, int strideY,
            global const uchar* srcU, int strideU,
            global const uchar* srcV, int strideV,
            const float3 k, const float4 yuvscale
        ) {
            const int xd2 = get_global_id(0);
            const int yd2 = get_global_id(1);
            const int x = xd2 * 2;
            const int y = yd2 * 2;

            uchar su = srcU[yd2 * strideU + xd2];
            uchar sv = srcV[yd2 * strideV + xd2];

            for(int j = 0; j < 2; ++j) {
                for(int i = 0; i < 2; ++i) {
                    uchar sy = srcY[(y + j) * strideY + (x + i)];
                    uchar3 rgb = yuv2rgb((uchar3)(sy, su, sv), k, yuvscale);
                    vstore4((uchar4)(rgb.zyx, 255), x + i, dst + (y + j) * stride);
                }
            }
        }

        kernel void i420_to_image2d(
            write_only image2d_t dst,
            global const uchar* srcY, int strideY,
            global const uchar* srcU, int strideU,
            global const uchar* srcV, int strideV,
            const float3 k, const float4 yuvscale
        ) {
            const int xd2 = get_global_id(0);
            const int yd2 = get_global_id(1);
            const int x = xd2 * 2;
            const int y = yd2 * 2;

            uchar su = srcU[yd2 * strideU + xd2];
            uchar sv = srcV[yd2 * strideV + xd2];

            for(int j = 0; j < 2; ++j) {
                for(int i = 0; i < 2; ++i) {
                    uchar sy = srcY[(y + j) * strideY + (x + i)];
                    uchar3 rgb = yuv2rgb((uchar3)(sy, su, sv), k, yuvscale);
                    write_imagef(dst, (int2)(x + i, y + j), convert_float4((uchar4)(rgb, 255)) / (float4)(255.0, 255.0, 255.0, 255.0));
                }
            }
        }

        kernel void yuy2_to_bgra_frame(
            global uchar* dst, int dst_stride,
            global const uchar* src, int src_stride,
            const float3 k, const float4 yuvscale
        ) {
            const int xd2 = get_global_id(0);
            const int x = xd2 * 2;
            const int y = get_global_id(1);

            uchar su = src[y * src_stride + xd2 * 4 + 1];
            uchar sv = src[y * src_stride + xd2 * 4 + 3];
            for(int i = 0; i < 2; ++i) {
                uchar sy = src[y * src_stride + xd2 * 4 + i * 2];
                uchar3 rgb = yuv2rgb((uchar3)(sy, su, sv), k, yuvscale);
                vstore4((uchar4)(rgb.zyx, 255), x + i, dst + y * dst_stride);
            }
        }
    )OPENCLCODE" };

    std::vector<std::string> programStrings;
    programStrings.push_back(sourcecode);

    program_ = cl::Program(ctx_, programStrings);
    try {
        program_.build("-cl-std=CL2.0");
    }
    catch (...) {
        cl_int buildErr = CL_SUCCESS;
        auto buildInfo = program_.getBuildInfo<CL_PROGRAM_BUILD_LOG>(&buildErr);
        for (auto& pair : buildInfo) {
            MessageBoxA(NULL, pair.second.c_str(), "error", MB_ICONERROR);
        }

        return;
    }

    available_ = true;
}
catch (...) {
    available_ = false;
}

class CLEnv {
    bool initialize_ = false;
public:
    CLEnv() {
    }

    ~CLEnv() {
    }
};

static CLEnv cl_env_;

std::tuple<cl_float3, cl_float4> GetConvConst(const asco::ColorInfo& ci) {
    cl_float3 k;
    cl_float4 yuvscale;

    if (ci.trans_matrix == asco::ColorTransMatrix::BT601) {
        k.x = 0.299f;
        k.y = 0.587f;
        k.z = 0.114f;
    }
    else {
        k.x = 0.2126f;
        k.y = 0.7152f;
        k.z = 0.0722f;
    }

    if (ci.nominal_range == asco::ColorNominalRange::_0_255) {
        yuvscale.x = 0.0f;
        yuvscale.y = 1.0f;
        yuvscale.z = 0.0f;
        yuvscale.w = 1.0f;
    }
    else {
        yuvscale.x = 16.0f / 255.0f;
        yuvscale.y = 255.0f / (235.0f - 16.0f);
        yuvscale.z = 16.0f / 255.0f;
        yuvscale.w = 255.0f / (240.0f - 16.0f);
    }

    return { k, yuvscale };
}


void init_opencl(void* d3d11Dev) {
    shared_d3d11dev_ = (ID3D11Device*)d3d11Dev;
    preferred_vendor_id_ = 0;
    if (shared_d3d11dev_) {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(shared_d3d11dev_->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description)))
            preferred_vendor_id_ = description.VendorId;
    }
    InitCLEnv();
}

bool is_opencl_avail() {
    return available_ && opencl_enabled_;
}

void opencl_set_enabled(bool enabled) {
    opencl_enabled_ = enabled;
}

struct OpenCLInputBuf::Internal: public CCClBuffer {
    using CCClBuffer::CCClBuffer;
};

OpenCLInputBuf::OpenCLInputBuf(const void* ptr, size_t size)
{
    data_ = std::make_shared<OpenCLInputBuf::Internal>(ptr, size);
}

OpenCLInputBuf::~OpenCLInputBuf()
{
}

bool opencl_bgra_to_i420_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    const void* src, size_t stride,
    void* dstY, size_t strideY,
    void* dstU, size_t strideU,
    void* dstV, size_t strideV
) {
    auto impl = [&](auto t) {
        using BufT = decltype(t);

        if (!is_opencl_avail())
            return false;

        auto func = cl::KernelFunctor<
            BufT, cl_uint,
            BufT, cl_uint, BufT, cl_uint, BufT, cl_uint,
            cl_float3, cl_float4>
            (program_, "bgra_to_i420_frame");

        auto [k, yuvscale] = GetConvConst(ci);

        CCClBuffer srcbuf{ src, stride * height },
            ybuf{ dstY, height * strideY, true },
            ubuf{ dstU, height / 2 * strideU, true },
            vbuf{ dstV, height / 2 * strideV, true };

        func(cl::EnqueueArgs(queue_, cl::NDRange(width / 2, height / 2)),
            srcbuf, (cl_uint)stride, ybuf, (cl_uint)strideY, ubuf, (cl_uint)strideU, vbuf, (cl_uint)strideV,
            k, yuvscale
        );

        return true;
    };

    if (isClSvmSupported_)
        return impl((void*)nullptr);
    else
        return impl(cl::Buffer{});
}


bool opencl_nv12_to_bgra_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    void* dst, size_t stride,
    const void* srcY, size_t strideY,
    const void* srcUV, size_t strideUV
) {
    auto impl = [&](auto t) {
        using BufT = decltype(t);

        if (!is_opencl_avail())
            return false;

        auto [k, yuvscale] = GetConvConst(ci);

        auto func = cl::KernelFunctor<
            BufT, cl_uint,
            BufT, cl_uint, BufT, cl_uint,
            cl_float3, cl_float4>
            (program_, "nv12_to_bgra_frame");

        CCClBuffer dstbuf{ dst, stride * height, true },
            ybuf{ srcY, height * strideY },
            uvbuf{ srcUV, height / 2 * strideUV };

        func(cl::EnqueueArgs(queue_, cl::NDRange(width / 2, height / 2)),
            dstbuf, (cl_uint)stride, ybuf, (cl_uint)strideY, uvbuf, (cl_uint)strideUV,
            k, yuvscale
        );

        return true;
    };

    if (isClSvmSupported_)
        return impl((void*)nullptr);
    else
        return impl(cl::Buffer{});
}


bool opencl_i420_to_bgra_frame(
    const asco::ColorInfo& ci,
    size_t width, size_t height,
    const void* y, size_t y_stride,
    const void* u, size_t u_stride,
    const void* v, size_t v_stride,
    void* dst, size_t dst_stride
) {
    auto impl = [&](auto t) {
        using BufT = decltype(t);

        if (!is_opencl_avail())
            return false;

        auto [k, yuvscale] = GetConvConst(ci);

        auto func = cl::KernelFunctor<
            BufT, cl_uint,
            BufT, cl_uint, BufT, cl_uint, BufT, cl_uint,
            cl_float3, cl_float4>
            (program_, "i420_to_bgra_frame");

        CCClBuffer dstbuf{ dst, dst_stride * height, true },
            ybuf{ y, y_stride * height },
            ubuf{ u, u_stride * height / 2 },
            vbuf{ v, v_stride * height / 2 };

        func(cl::EnqueueArgs(queue_, cl::NDRange(width / 2, height / 2)),
            dstbuf, (cl_uint)dst_stride,
            ybuf, (cl_uint)y_stride, ubuf, (cl_uint)u_stride, vbuf, (cl_uint)v_stride,
            k, yuvscale
        );

        return true;
    };

    if (isClSvmSupported_)
        return impl((void*)nullptr);
    else
        return impl(cl::Buffer{});
}


bool opencl_i420_to_image2d(
    const asco::ColorInfo& ci,
    const OpenCLInputBuf& y, size_t strideY,
    const OpenCLInputBuf& u, size_t strideU,
    const OpenCLInputBuf& v, size_t strideV,
    size_t width, size_t height,
    void* tex2d
) {
    return false;
}



bool opencl_yuy2_to_bgra_frame(
    const asco::ColorInfo& ci,
    unsigned int width, unsigned int height,
    const void* src, size_t src_stride,
    void* dst, size_t dst_stride
) {
    auto impl = [&](auto t) {
        using BufT = decltype(t);

        if (!is_opencl_avail())
            return false;

        auto [k, yuvscale] = GetConvConst(ci);

        auto func = cl::KernelFunctor<
            BufT, cl_int, BufT, cl_int,
            cl_float3, cl_float4>
            (program_, "yuy2_to_bgra_frame");

        CCClBuffer dstbuf{ dst, dst_stride * height, true },
            srcbuf{ src, src_stride * height };

        func(cl::EnqueueArgs(queue_, cl::NDRange(width / 2, height)),
            dstbuf, (cl_int)dst_stride,
            srcbuf, (cl_int)src_stride,
            k, yuvscale
        );

        return true;
    };

    if (isClSvmSupported_)
        return impl((void*)nullptr);
    else
        return impl(cl::Buffer{});
}

const char* opencl_device_name() {
    return device_name_.empty() ? "unknown" : device_name_.c_str();
}
