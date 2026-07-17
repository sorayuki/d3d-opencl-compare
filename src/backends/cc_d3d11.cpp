#include "cc_d3d11.h"
#include "../colorconv_common.h"
#include <cstring>
#include <d3d11.h>
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
#include <d3d11on12.h>
#include <d3d12.h>
#include "../d3d12_resource_helpers.h"
#endif
#include <d3dcompiler.h>
#include <string>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::mutex g_api_mutex;
std::mutex g_run_mutex;
bool g_enabled = true;

struct D3D11Runtime {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    ComPtr<ID3D11On12Device1> on12;
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D11Query> completion_query;
    bool direct_output = false;
#endif

    bool create(void* source_device);
    bool valid() const { return device && context; }
};

struct Params {
    float k[3];
    float inv_k_y;
    float range[4];
    UINT w, h;
    UINT ys, us, vs, os;
    UINT reserved[2];
};


const char *kShader = R"(
cbuffer P : register(b0) {
    float3 k;
    float inv_k_y;
    float4 range;
    uint w;
    uint h;
    uint ys;
    uint us;
    uint vs;
    uint os;
    uint2 reserved;
}

ByteAddressBuffer A : register(t0);
ByteAddressBuffer B : register(t1);
ByteAddressBuffer C : register(t2);
RWByteAddressBuffer O : register(u0);
RWBuffer<uint> OY : register(u1);
RWBuffer<uint> OU : register(u2);
RWBuffer<uint> OV : register(u3);
RWTexture2D<float4> OF : register(u0);

uint load_byte(ByteAddressBuffer buffer, uint byte_offset) {
    uint packed = buffer.Load(byte_offset & ~3);
    return (packed >> ((byte_offset & 3) * 8)) & 255;
}

uint load_dword(ByteAddressBuffer buffer, uint byte_offset) {
    uint aligned_offset = byte_offset & ~3;
    uint low = buffer.Load(aligned_offset);
    uint shift = (byte_offset & 3) * 8;
    if (shift == 0)
        return low;
    uint high = buffer.Load(aligned_offset + 4);
    return (low >> shift) | (high << (32 - shift));
}

uint2 load_byte2(ByteAddressBuffer buffer, uint byte_offset) {
    uint lane = byte_offset & 3;
    uint packed = buffer.Load(byte_offset & ~3);
    if (lane < 3) {
        uint pair = (packed >> (lane * 8)) & 0xffff;
        return uint2(pair & 255, pair >> 8);
    }
    return uint2(packed >> 24, buffer.Load((byte_offset & ~3) + 4) & 255);
}

void store_dword(RWByteAddressBuffer buffer, uint byte_offset, uint value) {
    buffer.Store(byte_offset, value);
}

float3 yuv_to_rgb(uint y, uint u, uint v) {
    float3 f = (float3(y, u, v) / 255 - float3(range.x, range.z, range.z))
         * float3(range.y, 2 * range.w, 2 * range.w)
         - float3(0, 1, 1);
    return saturate(float3(
        f.x + f.z * (1 - k.x),
        f.x - f.y * (1 - k.z) * k.z / k.y - f.z * (1 - k.x) * k.x / k.y,
        f.x + f.y * (1 - k.z)));
}

uint pack_bgra_rgb(float3 rgb) {
    uint3 bgra = uint3(round(255 * rgb.zyx));
    return bgra.x | (bgra.y << 8) | (bgra.z << 16) | 0xff000000;
}

uint pack_bgra_pixel(uint y, uint u, uint v) {
    return pack_bgra_rgb(yuv_to_rgb(y, u, v));
}

float3 rgb_to_yuv(uint3 q) {
    float3 f = q / 255.;
    float yy = saturate(dot(f, k));
    float uu = clamp((f.z - yy) / (1 - k.z), -1, 1);
    float vv = clamp((f.x - yy) / (1 - k.x), -1, 1);
    return float3((yy / range.y + range.x) * 255,
                  (uu / range.w / 2 + .5) * 255,
                  (vv / range.w / 2 + .5) * 255);
}

[numthreads(8, 8, 1)]
void nv12_to_bgra_frame(uint3 id : SV_DispatchThreadID) {
    uint2 pixel = id.xy * 2;
    if (pixel.x >= w || pixel.y >= h) return;

    uint uvp = id.y * us + pixel.x;
    uint2 uv = load_byte2(B, uvp);
    uint y0 = pixel.y * ys + pixel.x;
    uint y1 = y0 + ys;
    uint o0 = pixel.y * os + pixel.x;
    uint o1 = o0 + os;
    uint2 top = load_byte2(A, y0);
    uint2 bottom = load_byte2(A, y1);

    O.Store(o0 * 4, pack_bgra_pixel(top.x, uv.x, uv.y));
    O.Store((o0 + 1) * 4, pack_bgra_pixel(top.y, uv.x, uv.y));
    O.Store(o1 * 4, pack_bgra_pixel(bottom.x, uv.x, uv.y));
    O.Store((o1 + 1) * 4, pack_bgra_pixel(bottom.y, uv.x, uv.y));
}

[numthreads(8, 8, 1)]
void i420_to_bgra_frame(uint3 id : SV_DispatchThreadID) {
    uint2 pixel = id.xy * 2;
    if (pixel.x >= w || pixel.y >= h) return;

    uint u = load_byte(B, id.y * us + id.x);
    uint v = load_byte(C, id.y * vs + id.x);
    uint y0 = pixel.y * ys + pixel.x;
    uint y1 = y0 + ys;
    uint o0 = pixel.y * os + pixel.x;
    uint o1 = o0 + os;
    uint2 top = load_byte2(A, y0);
    uint2 bottom = load_byte2(A, y1);

    O.Store(o0 * 4, pack_bgra_pixel(top.x, u, v));
    O.Store((o0 + 1) * 4, pack_bgra_pixel(top.y, u, v));
    O.Store(o1 * 4, pack_bgra_pixel(bottom.x, u, v));
    O.Store((o1 + 1) * 4, pack_bgra_pixel(bottom.y, u, v));
}

[numthreads(8, 8, 1)]
void i420_to_image2d(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy / 2;
    OF[id.xy] = float4(yuv_to_rgb(load_byte(A, id.y * ys + id.x), load_byte(B, q.y * us + q.x), load_byte(C, q.y * vs + q.x)).zyx, 1);
}

void write_yuy2_pair(uint2 id, uint x, uint packed) {
    uint y0 = packed & 255;
    uint u = (packed >> 8) & 255;
    uint y1 = (packed >> 16) & 255;
    uint v = packed >> 24;

    float2 luma = (float2(y0, y1) / 255 - range.xx) * range.yy;
    float2 chroma = (float2(u, v) / 255 - range.zz) * (2 * range.ww) - 1;
    float2 one_minus_k = 1 - k.xz;
    float2 green = chroma * one_minus_k.yx * k.zx * inv_k_y;
    float3 chroma_rgb = float3(
        chroma.y * one_minus_k.x,
        -green.x - green.y,
        chroma.x * one_minus_k.y);

    uint output = id.y * os + x;
    O.Store(output * 4, pack_bgra_rgb(saturate(luma.xxx + chroma_rgb)));
    O.Store((output + 1) * 4, pack_bgra_rgb(saturate(luma.yyy + chroma_rgb)));
}

[numthreads(8, 8, 1)]
void yuy2_to_bgra_frame(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2;
    if (x >= w || id.y >= h) return;
    uint packed = A.Load(id.y * ys + id.x * 4);
    write_yuy2_pair(id.xy, x, packed);
}

[numthreads(8, 8, 1)]
void yuy2_to_bgra_frame_unaligned(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2;
    if (x >= w || id.y >= h) return;
    uint packed = load_dword(A, id.y * ys + id.x * 4);
    write_yuy2_pair(id.xy, x, packed);
}

[numthreads(8, 8, 1)]
void bgra_to_i420_frame(uint3 id : SV_DispatchThreadID) {
    uint2 pixel = id.xy * 2;
    if (pixel.x >= w || pixel.y >= h) return;

    uint p0 = pixel.y * ys + pixel.x * 4;
    uint p1 = p0 + ys;
    uint2 top = uint2(load_dword(A, p0), load_dword(A, p0 + 4));
    uint2 bottom = uint2(load_dword(A, p1), load_dword(A, p1 + 4));
    float3 t0 = rgb_to_yuv(uint3((top.x >> 16) & 255, (top.x >> 8) & 255, top.x & 255));
    float3 t1 = rgb_to_yuv(uint3((top.y >> 16) & 255, (top.y >> 8) & 255, top.y & 255));
    float3 b0 = rgb_to_yuv(uint3((bottom.x >> 16) & 255, (bottom.x >> 8) & 255, bottom.x & 255));
    float3 b1 = rgb_to_yuv(uint3((bottom.y >> 16) & 255, (bottom.y >> 8) & 255, bottom.y & 255));

    uint y0 = pixel.y * os + pixel.x;
    uint y1 = y0 + os;
    OY[y0] = round(t0.x);
    OY[y0 + 1] = round(t1.x);
    OY[y1] = round(b0.x);
    OY[y1 + 1] = round(b1.x);
    OU[id.y * us + id.x] = round(t0.y);
    OV[id.y * vs + id.x] = round(t0.z);
}
)";


struct GPUBuffer {
    D3D11Runtime* runtime = nullptr;
    size_t bytes = 0;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;

    GPUBuffer(D3D11Runtime& runtime_value, size_t bytes, bool output = false,
              DXGI_FORMAT format = DXGI_FORMAT_R8_UINT)
        : runtime(&runtime_value), bytes(bytes)
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = (UINT)bytes;
#if defined(D3D11_DYNAMIC_INPUT_UPLOAD)
        d.Usage = output ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
        d.CPUAccessFlags = output ? 0 : D3D11_CPU_ACCESS_WRITE;
#else
        d.Usage = D3D11_USAGE_DEFAULT;
#endif
        d.BindFlags = output ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
        const bool raw = !output || format == DXGI_FORMAT_R32_TYPELESS;
        d.MiscFlags = raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0;
        if (FAILED(runtime->device->CreateBuffer(&d, nullptr, &buffer)))
            return;
        if (output) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = format;
            v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            v.Buffer.NumElements = format == DXGI_FORMAT_R32_UINT || raw
                ? d.ByteWidth / sizeof(UINT)
                : d.ByteWidth;
            v.Buffer.Flags = raw ? D3D11_BUFFER_UAV_FLAG_RAW : 0;
            if (FAILED(runtime->device->CreateUnorderedAccessView(buffer.Get(), &v, &uav)))
                buffer.Reset();
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            v.BufferEx.NumElements = d.ByteWidth / sizeof(UINT);
            v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(runtime->device->CreateShaderResourceView(buffer.Get(), &v, &srv)))
                buffer.Reset();
        }
    }

    bool Load(const void* data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer > bytes ||
            size > bytes - offset_in_buffer)
            return false;

#if defined(D3D11_DYNAMIC_INPUT_UPLOAD)
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(runtime->context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped)))
            return false;
        std::memcpy(static_cast<unsigned char *>(mapped.pData) +
                        offset_in_buffer,
                    data, size);
        runtime->context->Unmap(buffer.Get(), 0);
        return true;
#else
        if (offset_in_buffer == 0 && size == bytes) {
            runtime->context->UpdateSubresource(buffer.Get(), 0, nullptr, data, 0, 0);
            return true;
        }

        D3D11_BOX box{};
        box.left = (UINT)offset_in_buffer;
        box.right = (UINT)(offset_in_buffer + size);
        box.bottom = 1;
        box.back = 1;
        runtime->context->UpdateSubresource(buffer.Get(), 0, &box, data, 0, 0);
        return true;
#endif
    }
};


struct GPUOutputBuffer: GPUBuffer {
    ComPtr<ID3D11Buffer> staging;
    bool mapped = false;
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    ComPtr<ID3D12Resource> direct_resource;
    void* direct_mapped = nullptr;
    bool direct = false;
#endif

    GPUOutputBuffer(D3D11Runtime& runtime_value, size_t bytes,
                    DXGI_FORMAT format = DXGI_FORMAT_R8_UINT)
        : GPUBuffer(runtime_value, bytes, true, format)
    {
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
        if (runtime->direct_output && format == DXGI_FORMAT_R8_UINT) {
            if (d3d12_common::create_write_back_buffer(runtime->d3d12.Get(), bytes,
                                                       direct_resource,
                                                       direct_mapped)) {
                D3D11_RESOURCE_FLAGS flags{};
                flags.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                flags.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                ComPtr<ID3D11Buffer> wrapped;
                if (SUCCEEDED(runtime->on12->CreateWrappedResource(
                        direct_resource.Get(), &flags,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        IID_PPV_ARGS(&wrapped)))) {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_R32_TYPELESS;
                    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    view.Buffer.NumElements = static_cast<UINT>(bytes / 4);
                    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                    ComPtr<ID3D11UnorderedAccessView> wrapped_uav;
                    if (SUCCEEDED(runtime->device->CreateUnorderedAccessView(
                            wrapped.Get(), &view, &wrapped_uav))) {
                        buffer = std::move(wrapped);
                        uav = std::move(wrapped_uav);
                        direct = true;
                        return;
                    }
                }
                direct_resource.Reset();
                direct_mapped = nullptr;
            }
        }
#endif
        D3D11_BUFFER_DESC staging_desc = {};
        staging_desc.ByteWidth = (UINT)bytes;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        if (FAILED(runtime->device->CreateBuffer(&staging_desc, nullptr, &staging)))
            return;
    }

    ~GPUOutputBuffer() {
        UnmapBuffer();
    }

    void* MapBuffer() {
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
        if (direct) {
            if (runtime->completion_query) {
                runtime->context->End(runtime->completion_query.Get());
                runtime->context->Flush();
                while (runtime->context->GetData(runtime->completion_query.Get(), nullptr, 0,
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
                    YieldProcessor();
            }
            return direct_mapped;
        }
#endif
        if (!buffer || !staging || mapped)
            return nullptr;
        runtime->context->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped_resource{};
        if (FAILED(runtime->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped_resource)))
            return nullptr;
        mapped = true;
        return mapped_resource.pData;
    }

    void UnmapBuffer() {
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
        if (direct)
            return;
#endif
        if (!staging || !mapped)
            return;
        runtime->context->Unmap(staging.Get(), 0);
        mapped = false;
    }

    bool valid() const {
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
        return buffer && uav && (direct || staging);
#else
        return buffer && uav && staging;
#endif
    }
};


struct PackedGPUOutputBuffer: GPUOutputBuffer {
    PackedGPUOutputBuffer(D3D11Runtime& runtime, size_t bytes)
        : GPUOutputBuffer(runtime, bytes, DXGI_FORMAT_R32_TYPELESS) {}
};


struct GPUParamsBuffer {
    D3D11Runtime* runtime = nullptr;
    size_t bytes = 0;
    ComPtr<ID3D11Buffer> buffer;

    GPUParamsBuffer(D3D11Runtime& runtime_value, size_t bytes)
        : runtime(&runtime_value), bytes(bytes) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = (UINT)bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        runtime->device->CreateBuffer(&desc, nullptr, &buffer);
    }

    bool Load(const void *data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer != 0 || size != bytes)
            return false;
        runtime->context->UpdateSubresource(buffer.Get(), 0, nullptr, data, 0, 0);
        return true;
    }
};


bool shader(ID3D11Device* device, const char *entry,
            ComPtr<ID3D11ComputeShader> &out) {
    ComPtr<ID3DBlob> b, e;
    const bool optimize =
        strcmp(entry, "nv12_to_bgra_frame") == 0 ||
        strcmp(entry, "yuy2_to_bgra_frame") == 0 ||
        strcmp(entry, "yuy2_to_bgra_frame_unaligned") == 0;
    const UINT flags = optimize
        ? D3DCOMPILE_OPTIMIZATION_LEVEL3
        : 0;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, entry,
                            "cs_5_0", flags, 0, &b, &e);
    if (FAILED(hr)) {
        if (e)
            OutputDebugStringA((const char *)e->GetBufferPointer());
        return false;
    }
    return SUCCEEDED(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out));
}


template<class T>
class BufPool {
public:
    explicit BufPool(D3D11Runtime& runtime, size_t threshold)
        : runtime_(runtime), threshold_(threshold) {}

    std::shared_ptr<T> Load(const void *data, UINT bytes) {
        auto ret = Aquire(bytes);
        if (ret)
            ret->Load(data, bytes, 0);
        return ret;
    }

    std::shared_ptr<T> Aquire(UINT bytes) {
        if (bytes > (std::numeric_limits<UINT>::max)() - (sizeof(UINT) - 1))
            return {};
        bytes = (bytes + sizeof(UINT) - 1) & ~(sizeof(UINT) - 1);
        T value = Acquire_(bytes);
        return std::shared_ptr<T>(new T(std::move(value)), [this](T* value) {
            Release(std::move(*value));
            delete value;
        });
    }

private:
    D3D11Runtime& runtime_;
    std::multimap<size_t, T> pool_;
    std::multimap<size_t, T> oldpool_;
    std::mutex pool_mutex_;
    size_t bytes_ = 0;
    size_t threshold_ = 8 * 1024 * 1024; // buffer到达多少字节之后开始清理过期数据

    T Acquire_(UINT bytes) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        for(auto p: { &oldpool_, &pool_ }) {
            auto it = p->find(bytes);
            if (it == p->end()) {
                continue;
            }
            auto ret = std::move(it->second);
            p->erase(it);

            if (p == &pool_) {
                bytes_ -= ret.bytes;
            }

            return ret;
        }

        lock.unlock();
        return T(runtime_, bytes);
    }

    void Release(T&& buf) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        if (!buf.buffer)
            return;
        if constexpr (std::is_base_of_v<GPUOutputBuffer, T>) {
            if (!buf.valid())
                return;
        }
        auto bytes = buf.bytes;
        pool_.emplace(bytes, std::move(buf));
        bytes_ += bytes;

        // 判断是否触发过期数据清理
        if (bytes_ > threshold_) {
            oldpool_.clear();
            oldpool_.swap(pool_);
            bytes_ = 0;
        }
    }
};


bool copyback(GPUOutputBuffer &x, void *dst, UINT bytes) {
    if (!x.valid())
        return false;
    auto ptr = (const unsigned char*)x.MapBuffer();
    if (!ptr)
        return false;
    memcpy(dst, ptr, bytes);
    x.UnmapBuffer();
    return true;
}


bool copyback_bgra(GPUOutputBuffer &x, void *dst, UINT dst_stride, UINT w, UINT h) {
    if (!x.valid())
        return false;
    auto ptr = static_cast<const unsigned char *>(x.MapBuffer());
    if (!ptr)
        return false;
    if (dst_stride == w * 4) {
        memcpy(dst, ptr, static_cast<size_t>(w) * h * 4);
    } else {
        const size_t row_bytes = static_cast<size_t>(w) * 4;
        for (UINT row = 0; row < h; ++row) {
            memcpy(static_cast<unsigned char *>(dst) +
                       static_cast<size_t>(row) * dst_stride,
                   ptr + static_cast<size_t>(row) * row_bytes,
                   row_bytes);
        }
    }
    x.UnmapBuffer();
    return true;
}


void params(const asco::ColorInfo &ci, Params &p, UINT w, UINT h) {
    p = {};
    const auto common = colorconv::make_color_params(ci);
    std::memcpy(p.k, common.k, sizeof(p.k));
    p.inv_k_y = 1.f / p.k[1];
    std::memcpy(p.range, common.range, sizeof(p.range));
    p.w = w;
    p.h = h;
}


bool run_shader(ComPtr<ID3D11ComputeShader> &cs, const GPUBuffer *a, const GPUBuffer *b,
         const GPUBuffer *c, const GPUBuffer *o, const GPUBuffer *oy,
         const GPUBuffer *ou, const GPUBuffer *ov,
         const GPUParamsBuffer *params_buffer, UINT w, UINT h, UINT group_width = 8,
         UINT group_height = 8) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (!cs || !params_buffer || !params_buffer->buffer ||
        !params_buffer->runtime)
        return false;
    auto& runtime = *params_buffer->runtime;
    ID3D11UnorderedAccessView *uv[4] = {
        o ? o->uav.Get() : nullptr,
        oy ? oy->uav.Get() : nullptr,
        ou ? ou->uav.Get() : nullptr,
        ov ? ov->uav.Get() : nullptr};
    ID3D11ShaderResourceView *sv[3] = {
        a ? a->srv.Get() : nullptr,
        b ? b->srv.Get() : nullptr,
        c ? c->srv.Get() : nullptr};
    runtime.context->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11Buffer *constant_buffer = params_buffer->buffer.Get();
    runtime.context->CSSetConstantBuffers(0, 1, &constant_buffer);
    runtime.context->CSSetShaderResources(0, 3, sv);
    runtime.context->CSSetUnorderedAccessViews(0, 4, uv, nullptr);
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    ID3D11Resource* wrapped_resource = nullptr;
    if (runtime.direct_output && o) {
        auto* output = static_cast<const GPUOutputBuffer*>(o);
        if (output->direct) {
            wrapped_resource = output->buffer.Get();
            runtime.on12->AcquireWrappedResources(&wrapped_resource, 1);
        }
    }
#endif
    runtime.context->Dispatch((w + group_width - 1) / group_width,
                              (h + group_height - 1) / group_height, 1);
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    if (wrapped_resource)
        runtime.on12->ReleaseWrappedResources(&wrapped_resource, 1);
#endif
    ID3D11ShaderResourceView *zs[3] = {};
    ID3D11UnorderedAccessView *zu[4] = {};
    runtime.context->CSSetShaderResources(0, 3, zs);
    runtime.context->CSSetUnorderedAccessViews(0, 4, zu, nullptr);
    return true;
}


struct D3D11ShaderSet {
    ComPtr<ID3D11ComputeShader> nv12_to_bgra, i420_to_bgra;
    ComPtr<ID3D11ComputeShader> i420_to_image2d, yuy2_to_bgra;
    ComPtr<ID3D11ComputeShader> yuy2_to_bgra_unaligned, bgra_to_i420;

    bool create(ID3D11Device* device) {
        return shader(device, "nv12_to_bgra_frame", nv12_to_bgra) &&
               shader(device, "i420_to_bgra_frame", i420_to_bgra) &&
               shader(device, "i420_to_image2d", i420_to_image2d) &&
               shader(device, "yuy2_to_bgra_frame", yuy2_to_bgra) &&
               shader(device, "yuy2_to_bgra_frame_unaligned", yuy2_to_bgra_unaligned) &&
               shader(device, "bgra_to_i420_frame", bgra_to_i420);
    }
};

bool D3D11Runtime::create(void* source) {
    auto* source_device = static_cast<ID3D11Device*>(source);
    if (!source_device)
        return false;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(source_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)))
        return false;

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL feature_level{};
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    device = source_device;
    source_device->GetImmediateContext(&context);
    HRESULT hr = device && context ? S_OK : E_FAIL;
#else
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &device, &feature_level, &context);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
            ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &device,
            &feature_level, &context);
#endif
    (void)feature_level;
    if (FAILED(hr))
        return false;
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
#if defined(D3D11ON12_UMA_DIRECT_OUTPUT)
    if (SUCCEEDED(device.As(&on12)) &&
        SUCCEEDED(on12->GetD3D12Device(IID_PPV_ARGS(&d3d12))) &&
        d3d12_common::supports_uma_write_back(d3d12.Get())) {
        D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
        direct_output = SUCCEEDED(device->CreateQuery(&query_desc, &completion_query));
    }
#endif
    return true;
}

struct D3D11Converter {
    D3D11Runtime runtime;
    D3D11ShaderSet shaders;
    BufPool<GPUBuffer> input_pool;
    BufPool<GPUOutputBuffer> output_pool;
    BufPool<PackedGPUOutputBuffer> packed_pool;
    BufPool<GPUParamsBuffer> params_pool;
    bool operational = false;

    D3D11Converter()
        : input_pool(runtime, 16 * 1024 * 1024),
          output_pool(runtime, 8 * 1024 * 1024),
          packed_pool(runtime, 8 * 1024 * 1024),
          params_pool(runtime, 1 * 1024 * 1024) {}

    bool initialize(void* source) {
        return runtime.create(source) && shaders.create(runtime.device.Get()) &&
               (operational = true);
    }

    bool nv12(const asco::ColorInfo&, size_t, size_t, void*, size_t,
              const void*, size_t, const void*, size_t);
    bool i420(const asco::ColorInfo&, size_t, size_t, const void*, size_t,
              const void*, size_t, const void*, size_t, void*, size_t);
    bool yuy2(const asco::ColorInfo&, size_t, size_t, const void*, size_t,
              void*, size_t);
    bool bgra_to_i420(const asco::ColorInfo&, size_t, size_t, const void*,
                     size_t, void*, size_t, void*, size_t, void*, size_t);
};

bool D3D11Converter::nv12(const asco::ColorInfo& ci, size_t w, size_t h,
                          void* dst, size_t ds, const void* y, size_t ys,
                          const void* uv, size_t us) {
    colorconv::Nv12Layout layout{};
    if (!operational || !dst || !y || !uv ||
        !colorconv::validate_nv12(w, h, ds, ys, us, layout))
        return false;
    auto a = input_pool.Load(y, layout.y_bytes);
    auto b = input_pool.Load(uv, layout.uv_bytes);
    auto o = packed_pool.Aquire(layout.output_bytes);
    if (!a || !b || !o || !a->buffer || !b->buffer || !o->buffer)
        return false;
    Params p{};
    params(ci, p, layout.width, layout.height);
    p.ys = layout.y_stride;
    p.us = layout.uv_stride;
    p.os = layout.width;
    auto constants = params_pool.Load(&p, sizeof(p));
    return constants && constants->buffer &&
           run_shader(shaders.nv12_to_bgra, a.get(), b.get(), nullptr, o.get(),
                      nullptr, nullptr, nullptr, constants.get(),
                      layout.width / 2, layout.height / 2) &&
           copyback_bgra(*o, dst, layout.destination_stride, layout.width,
                         layout.height);
}

bool D3D11Converter::i420(const asco::ColorInfo& ci, size_t w, size_t h,
                          const void* y, size_t ys, const void* u, size_t us,
                          const void* v, size_t vs, void* dst, size_t ds) {
    UINT width, height, y_stride, u_stride, v_stride, out_stride;
    UINT y_bytes, u_bytes, v_bytes, output_bytes;
    if (!operational || !dst || !y || !u || !v || w < 2 || h < 2 ||
        (w & 1) || (h & 1) || w > (std::numeric_limits<UINT>::max)() / 4 ||
        ys < w || us < w / 2 || vs < w / 2 || ds < w * 4 ||
        !colorconv::to_uint(w, width) || !colorconv::to_uint(h, height) ||
        !colorconv::to_uint(ys, y_stride) || !colorconv::to_uint(us, u_stride) ||
        !colorconv::to_uint(vs, v_stride) || !colorconv::to_uint(ds, out_stride) ||
        !colorconv::product_to_uint(ys, h, y_bytes) ||
        !colorconv::product_to_uint(us, h / 2, u_bytes) ||
        !colorconv::product_to_uint(vs, h / 2, v_bytes) ||
        !colorconv::product_to_uint(w * 4, h, output_bytes))
        return false;
    auto a = input_pool.Load(y, y_bytes), b = input_pool.Load(u, u_bytes),
         c = input_pool.Load(v, v_bytes);
    auto o = packed_pool.Aquire(output_bytes);
    if (!a || !b || !c || !o || !a->buffer || !b->buffer || !c->buffer ||
        !o->buffer)
        return false;
    Params p{};
    params(ci, p, width, height);
    p.ys = y_stride; p.us = u_stride; p.vs = v_stride; p.os = width;
    auto constants = params_pool.Load(&p, sizeof(p));
    return constants && constants->buffer &&
           run_shader(shaders.i420_to_bgra, a.get(), b.get(), c.get(), o.get(),
                      nullptr, nullptr, nullptr, constants.get(), width / 2,
                      height / 2) &&
           copyback_bgra(*o, dst, out_stride, width, height);
}

bool D3D11Converter::yuy2(const asco::ColorInfo& ci, size_t w, size_t h,
                          const void* src, size_t ss, void* dst, size_t ds) {
    UINT width, height, source_stride, output_stride, source_bytes, output_bytes;
    if (!operational || !src || !dst || w < 2 || h < 1 || (w & 1) ||
        ss < w * 2 || ds < w * 4 || !colorconv::to_uint(w, width) ||
        !colorconv::to_uint(h, height) || !colorconv::to_uint(ss, source_stride) ||
        !colorconv::to_uint(ds, output_stride) ||
        !colorconv::product_to_uint(ss, h, source_bytes) ||
        !colorconv::product_to_uint(w * 4, h, output_bytes))
        return false;
    auto a = input_pool.Load(src, source_bytes);
    auto o = packed_pool.Aquire(output_bytes);
    if (!a || !o || !a->buffer || !o->buffer)
        return false;
    Params p{};
    params(ci, p, width, height); p.ys = source_stride; p.os = width;
    auto constants = params_pool.Load(&p, sizeof(p));
    auto& shader_object = (source_stride & 3) ? shaders.yuy2_to_bgra_unaligned
                                               : shaders.yuy2_to_bgra;
    return constants && constants->buffer &&
           run_shader(shader_object, a.get(), nullptr, nullptr, o.get(),
                      nullptr, nullptr, nullptr, constants.get(), width / 2,
                      height) &&
           copyback_bgra(*o, dst, output_stride, width, height);
}

bool D3D11Converter::bgra_to_i420(const asco::ColorInfo& ci, size_t w,
                                  size_t h, const void* src, size_t ss,
                                  void* y, size_t ys, void* u, size_t us,
                                  void* v, size_t vs) {
    UINT width, height, source_stride, y_stride, u_stride, v_stride;
    UINT source_bytes, y_bytes, u_bytes, v_bytes;
    if (!operational || !src || !y || !u || !v || w < 2 || h < 2 ||
        (w & 1) || (h & 1) || ss < w * 4 || ys < w || us < w / 2 ||
        vs < w / 2 || !colorconv::to_uint(w, width) ||
        !colorconv::to_uint(h, height) || !colorconv::to_uint(ss, source_stride) ||
        !colorconv::to_uint(ys, y_stride) || !colorconv::to_uint(us, u_stride) ||
        !colorconv::to_uint(vs, v_stride) ||
        !colorconv::product_to_uint(ss, h, source_bytes) ||
        !colorconv::product_to_uint(ys, h, y_bytes) ||
        !colorconv::product_to_uint(us, h / 2, u_bytes) ||
        !colorconv::product_to_uint(vs, h / 2, v_bytes))
        return false;
    auto a = input_pool.Load(src, source_bytes);
    auto oy = output_pool.Aquire(y_bytes), ou = output_pool.Aquire(u_bytes),
         ov = output_pool.Aquire(v_bytes);
    if (!a || !oy || !ou || !ov || !a->buffer || !oy->buffer || !ou->buffer ||
        !ov->buffer)
        return false;
    Params p{};
    params(ci, p, width, height); p.ys = source_stride; p.us = u_stride;
    p.vs = v_stride; p.os = y_stride;
    auto constants = params_pool.Load(&p, sizeof(p));
    return constants && constants->buffer &&
           run_shader(shaders.bgra_to_i420, a.get(), nullptr, nullptr, nullptr,
                      oy.get(), ou.get(), ov.get(), constants.get(), width / 2,
                      height / 2) &&
           copyback(*oy, y, y_bytes) && copyback(*ou, u, u_bytes) &&
           copyback(*ov, v, v_bytes);
}

std::unique_ptr<D3D11Converter> g_converter;

} // namespace

void init_d3d11_colorconv(void* device) {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (g_converter || !device)
        return;
    auto converter = std::make_unique<D3D11Converter>();
    if (converter->initialize(device))
        g_converter = std::move(converter);
}

bool is_d3d11_colorconv_avail() {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    return g_enabled && g_converter && g_converter->operational &&
           g_converter->runtime.valid();
}

void d3d11_colorconv_set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    g_enabled = enabled;
}

bool d3d11_nv12_to_bgra_frame(const asco::ColorInfo& ci, size_t w, size_t h,
                              void* d, size_t ds, const void* y, size_t ys,
                              const void* uv, size_t us) {
    return g_enabled && g_converter &&
           g_converter->nv12(ci, w, h, d, ds, y, ys, uv, us);
}

bool d3d11_i420_to_bgra_frame(const asco::ColorInfo& ci, size_t w, size_t h,
                              const void* y, size_t ys, const void* u, size_t us,
                              const void* v, size_t vs, void* d, size_t ds) {
    return g_enabled && g_converter &&
           g_converter->i420(ci, w, h, y, ys, u, us, v, vs, d, ds);
}

bool d3d11_yuy2_to_bgra_frame(const asco::ColorInfo& ci, size_t w, size_t h,
                              const void* s, size_t ss, void* d, size_t ds) {
    return g_enabled && g_converter && g_converter->yuy2(ci, w, h, s, ss, d, ds);
}

bool d3d11_bgra_to_i420_frame(const asco::ColorInfo& ci, size_t w, size_t h,
                              const void* s, size_t ss, void* y, size_t ys,
                              void* u, size_t us, void* v, size_t vs) {
    return g_enabled && g_converter &&
           g_converter->bgra_to_i420(ci, w, h, s, ss, y, ys, u, us, v, vs);
}

struct D3D11InputBuf::Internal {
    const void *ptr;
};


D3D11InputBuf::D3D11InputBuf(const void *p, size_t) : data_(std::make_shared<Internal>(Internal{p})) {}


bool d3d11_i420_to_image2d(const asco::ColorInfo &ci, const D3D11InputBuf &yb, size_t ys,
                           const D3D11InputBuf &ub, size_t us, const D3D11InputBuf &vb, size_t vs,
                           size_t w, size_t h, void *ptr) {
    return false;
}
