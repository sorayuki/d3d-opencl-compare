#include "cc_d3d11.h"
#include <cstring>
#include <d3d11.h>
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

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
std::mutex g_run_mutex;
ComPtr<ID3D11ComputeShader> g_nv12_to_bgra, g_i420_to_bgra;
ComPtr<ID3D11ComputeShader> g_i420_to_image2d, g_yuy2_to_bgra;
ComPtr<ID3D11ComputeShader> g_yuy2_to_bgra_unaligned;
ComPtr<ID3D11ComputeShader> g_bgra_to_i420;
bool g_enabled = true;


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

Buffer<uint> A : register(t0);
Buffer<uint> B : register(t1);
Buffer<uint> C : register(t2);
RWBuffer<uint> O : register(u0);
RWBuffer<uint> OY : register(u1);
RWBuffer<uint> OU : register(u2);
RWBuffer<uint> OV : register(u3);
RWTexture2D<float4> OF : register(u0);

uint load_byte(Buffer<uint> buffer, uint byte_offset) {
    uint packed = buffer[byte_offset >> 2];
    return (packed >> ((byte_offset & 3) * 8)) & 255;
}

uint load_dword(Buffer<uint> buffer, uint byte_offset) {
    uint element = byte_offset >> 2;
    uint low = buffer[element];
    uint shift = (byte_offset & 3) * 8;
    if (shift == 0)
        return low;
    uint high = buffer[element + 1];
    return (low >> shift) | (high << (32 - shift));
}

uint2 load_two_bytes(Buffer<uint> buffer, uint byte_offset) {
    uint lane = byte_offset & 3;
    uint element = byte_offset >> 2;
    uint packed = buffer[element];
    if (lane < 3) {
        uint pair = (packed >> (lane * 8)) & 0xffff;
        return uint2(pair & 255, pair >> 8);
    }
    return uint2(packed >> 24, buffer[element + 1] & 255);
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
    uint2 uv = load_two_bytes(B, uvp);
    uint y0 = pixel.y * ys + pixel.x;
    uint y1 = y0 + ys;
    uint o0 = pixel.y * os + pixel.x;
    uint o1 = o0 + os;
    uint2 top = load_two_bytes(A, y0);
    uint2 bottom = load_two_bytes(A, y1);

    O[o0] = pack_bgra_pixel(top.x, uv.x, uv.y);
    O[o0 + 1] = pack_bgra_pixel(top.y, uv.x, uv.y);
    O[o1] = pack_bgra_pixel(bottom.x, uv.x, uv.y);
    O[o1 + 1] = pack_bgra_pixel(bottom.y, uv.x, uv.y);
}

[numthreads(8, 8, 1)]
void i420_to_bgra_frame(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy / 2;
    uint y = load_byte(A, id.y * ys + id.x);
    uint u = load_byte(B, q.y * us + q.x);
    uint v = load_byte(C, q.y * vs + q.x);
    O[id.y * os + id.x] = pack_bgra_pixel(y, u, v);
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
    O[output] = pack_bgra_rgb(saturate(luma.xxx + chroma_rgb));
    O[output + 1] = pack_bgra_rgb(saturate(luma.yyy + chroma_rgb));
}

[numthreads(8, 8, 1)]
void yuy2_to_bgra_frame(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2;
    if (x >= w || id.y >= h) return;
    uint packed = A[(id.y * ys >> 2) + id.x];
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
    if (id.x >= w || id.y >= h) return;
    uint p0 = id.y * ys + id.x * 4;
    uint4 z = uint4(load_byte(A, p0), load_byte(A, p0+1), load_byte(A, p0+2), load_byte(A, p0+3));
    float3 t = rgb_to_yuv(z.zyx);
    OY[id.y * os + id.x] = round(t.x);
    if (!(id.x & 1) && !(id.y & 1)) {
        uint2 q = id.xy / 2;
        OU[q.y * us + q.x] = round(t.y);
        OV[q.y * vs + q.x] = round(t.z);
    }
}
)";


struct GPUBuffer {
    size_t bytes = 0;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;

    GPUBuffer(size_t bytes, bool output = false, DXGI_FORMAT format = DXGI_FORMAT_R8_UINT)
        : bytes(bytes) 
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
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, &buffer)))
            return;
        if (output) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = format;
            v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            v.Buffer.NumElements = format == DXGI_FORMAT_R32_UINT
                ? d.ByteWidth / sizeof(UINT)
                : d.ByteWidth;
            if (FAILED(g_dev->CreateUnorderedAccessView(buffer.Get(), &v, &uav)))
                buffer.Reset();
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_UINT;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = d.ByteWidth / sizeof(UINT);
            if (FAILED(g_dev->CreateShaderResourceView(buffer.Get(), &v, &srv)))
                buffer.Reset();
        }
    }

    bool Load(const void* data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer > bytes ||
            size > bytes - offset_in_buffer)
            return false;

#if defined(D3D11_DYNAMIC_INPUT_UPLOAD)
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_ctx->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped)))
            return false;
        std::memcpy(static_cast<unsigned char *>(mapped.pData) +
                        offset_in_buffer,
                    data, size);
        g_ctx->Unmap(buffer.Get(), 0);
        return true;
#else
        if (offset_in_buffer == 0 && size == bytes) {
            g_ctx->UpdateSubresource(buffer.Get(), 0, nullptr, data, 0, 0);
            return true;
        }

        D3D11_BOX box{};
        box.left = (UINT)offset_in_buffer;
        box.right = (UINT)(offset_in_buffer + size);
        box.bottom = 1;
        box.back = 1;
        g_ctx->UpdateSubresource(buffer.Get(), 0, &box, data, 0, 0);
        return true;
#endif
    }
};


struct GPUOutputBuffer: GPUBuffer {
    ComPtr<ID3D11Buffer> staging;
    bool mapped = false;

    GPUOutputBuffer(size_t bytes, DXGI_FORMAT format = DXGI_FORMAT_R8_UINT)
        : GPUBuffer(bytes, true, format) 
    {
        D3D11_BUFFER_DESC staging_desc = {};
        staging_desc.ByteWidth = (UINT)bytes;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        if (FAILED(g_dev->CreateBuffer(&staging_desc, nullptr, &staging)))
            return;
    }

    ~GPUOutputBuffer() {
        UnmapBuffer();
    }

    void* MapBuffer() {
        if (!buffer || !staging || mapped)
            return nullptr;
        g_ctx->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped_resource{};
        if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped_resource)))
            return nullptr;
        mapped = true;
        return mapped_resource.pData;
    }

    void UnmapBuffer() {
        if (!staging || !mapped)
            return;
        g_ctx->Unmap(staging.Get(), 0);
        mapped = false;
    }
};


struct PackedGPUOutputBuffer: GPUOutputBuffer {
    explicit PackedGPUOutputBuffer(size_t bytes)
        : GPUOutputBuffer(bytes, DXGI_FORMAT_R32_UINT) {}
};


struct GPUParamsBuffer {
    size_t bytes = 0;
    ComPtr<ID3D11Buffer> buffer;

    explicit GPUParamsBuffer(size_t bytes) : bytes(bytes) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = (UINT)bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&desc, nullptr, &buffer);
    }

    bool Load(const void *data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer != 0 || size != bytes)
            return false;
        g_ctx->UpdateSubresource(buffer.Get(), 0, nullptr, data, 0, 0);
        return true;
    }
};


bool shader(const char *entry, ComPtr<ID3D11ComputeShader> &out) {
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
    return SUCCEEDED(g_dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out));
}


template<class T>
class BufPool {
public:
    BufPool(size_t threshold) : threshold_(threshold) {}

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
        return T(bytes);
    }

    void Release(T&& buf) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        if (!buf.buffer)
            return;
        if constexpr (std::is_base_of_v<GPUOutputBuffer, T>) {
            if (!buf.staging)
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


BufPool<GPUBuffer> g_input_pool(16 * 1024 * 1024);
BufPool<GPUOutputBuffer> g_output_pool(8 * 1024 * 1024);
BufPool<PackedGPUOutputBuffer> g_packed_output_pool(8 * 1024 * 1024);
BufPool<GPUParamsBuffer> g_params_pool(1 * 1024 * 1024);

bool copyback(GPUOutputBuffer &x, void *dst, UINT bytes) {
    if (!x.buffer || !x.staging)
        return false;
    auto ptr = (const unsigned char*)x.MapBuffer();
    if (!ptr)
        return false;
    memcpy(dst, ptr, bytes);
    x.UnmapBuffer();
    return true;
}


bool copyback_bgra(GPUOutputBuffer &x, void *dst, UINT dst_stride, UINT w, UINT h) {
    if (!x.buffer || !x.staging)
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
    p.k[0] = ci.trans_matrix == asco::ColorTransMatrix::BT601 ? .299f : .2126f;
    p.k[1] = ci.trans_matrix == asco::ColorTransMatrix::BT601 ? .587f : .7152f;
    p.k[2] = ci.trans_matrix == asco::ColorTransMatrix::BT601 ? .114f : .0722f;
    p.inv_k_y = 1.f / p.k[1];
    p.range[0] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 0 : 16.f / 255;
    p.range[1] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 1 : 255.f / 219;
    p.range[2] = p.range[0];
    p.range[3] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 1 : 255.f / 224;
    p.w = w;
    p.h = h;
}


bool run_shader(ComPtr<ID3D11ComputeShader> &cs, const GPUBuffer *a, const GPUBuffer *b,
         const GPUBuffer *c, const GPUBuffer *o, const GPUBuffer *oy,
         const GPUBuffer *ou, const GPUBuffer *ov,
         const GPUParamsBuffer *params_buffer, UINT w, UINT h, UINT group_width = 8,
         UINT group_height = 8) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (!cs || !params_buffer || !params_buffer->buffer)
        return false;
    ID3D11UnorderedAccessView *uv[4] = {
        o ? o->uav.Get() : nullptr,
        oy ? oy->uav.Get() : nullptr,
        ou ? ou->uav.Get() : nullptr,
        ov ? ov->uav.Get() : nullptr};
    ID3D11ShaderResourceView *sv[3] = {
        a ? a->srv.Get() : nullptr,
        b ? b->srv.Get() : nullptr,
        c ? c->srv.Get() : nullptr};
    g_ctx->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11Buffer *constant_buffer = params_buffer->buffer.Get();
    g_ctx->CSSetConstantBuffers(0, 1, &constant_buffer);
    g_ctx->CSSetShaderResources(0, 3, sv);
    g_ctx->CSSetUnorderedAccessViews(0, 4, uv, nullptr);
    g_ctx->Dispatch((w + group_width - 1) / group_width,
                    (h + group_height - 1) / group_height, 1);
    ID3D11ShaderResourceView *zs[3] = {};
    ID3D11UnorderedAccessView *zu[4] = {};
    g_ctx->CSSetShaderResources(0, 3, zs);
    g_ctx->CSSetUnorderedAccessViews(0, 4, zu, nullptr);
    return true;
}


} // namespace


void init_d3d11_colorconv(void *d) {
    auto source_device = static_cast<ID3D11Device *>(d);
    if (!source_device)
        return;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(source_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter))) {
        return;
    }

    static const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level{};
    ComPtr<ID3D11Device> conversion_device;
    ComPtr<ID3D11DeviceContext> conversion_context;
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        &conversion_device, &feature_level, &conversion_context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels + 1, ARRAYSIZE(feature_levels) - 1,
            D3D11_SDK_VERSION,
            &conversion_device, &feature_level, &conversion_context);
    }
    if (FAILED(hr))
        return;

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(conversion_device.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    g_dev = std::move(conversion_device);
    g_ctx = std::move(conversion_context);
    shader("nv12_to_bgra_frame", g_nv12_to_bgra);
    shader("i420_to_bgra_frame", g_i420_to_bgra);
    shader("i420_to_image2d", g_i420_to_image2d);
    shader("yuy2_to_bgra_frame", g_yuy2_to_bgra);
    shader("yuy2_to_bgra_frame_unaligned", g_yuy2_to_bgra_unaligned);
    shader("bgra_to_i420_frame", g_bgra_to_i420);
}


bool is_d3d11_colorconv_avail() {
    return g_enabled && g_dev && g_ctx &&
           g_nv12_to_bgra && g_i420_to_bgra && g_i420_to_image2d &&
           g_yuy2_to_bgra && g_yuy2_to_bgra_unaligned && g_bgra_to_i420;
}


void d3d11_colorconv_set_enabled(bool enabled) {
    g_enabled = enabled;
}


bool d3d11_nv12_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, void *d, size_t ds,
                              const void *y, size_t ys, const void *uv, size_t us) {
    if (!is_d3d11_colorconv_avail() || !d || !y || !uv || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    const UINT width = (UINT)w, height = (UINT)h;
    const UINT y_stride = (UINT)ys, uv_stride = (UINT)us;
    const UINT output_stride = (UINT)ds;
    const UINT y_bytes = (UINT)(ys * h);
    const UINT uv_bytes = (UINT)(us * (h / 2));
    const UINT output_bytes = (UINT)(w * 4 * h);
    auto a = g_input_pool.Load(y, y_bytes);
    auto b = g_input_pool.Load(uv, uv_bytes);
    auto o = g_packed_output_pool.Aquire(output_bytes);
    if (!a || !b || !o || !a->buffer || !b->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, width, height);
    p.ys = y_stride;
    p.us = uv_stride;
    p.os = width;
    auto params_buffer = g_params_pool.Load(&p, sizeof(p));
    if (!params_buffer || !params_buffer->buffer)
        return false;
    bool ok = run_shader(g_nv12_to_bgra, a.get(), b.get(), nullptr, o.get(), nullptr, nullptr, nullptr, params_buffer.get(), width / 2, height / 2) &&
              copyback_bgra(*o, d, output_stride, width, height);
    return ok;
}


bool d3d11_i420_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *y,
                              size_t ys, const void *u, size_t us, const void *v, size_t vs,
                              void *d, size_t ds) {
    if (!is_d3d11_colorconv_avail() || !d || !y || !u || !v || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    const UINT width = (UINT)w, height = (UINT)h;
    const UINT y_stride = (UINT)ys, u_stride = (UINT)us;
    const UINT v_stride = (UINT)vs, output_stride = (UINT)ds;
    const UINT y_bytes = (UINT)(ys * h);
    const UINT u_bytes = (UINT)(us * (h / 2));
    const UINT v_bytes = (UINT)(vs * (h / 2));
    const UINT output_bytes = (UINT)(w * 4 * h);
    auto a = g_input_pool.Load(y, y_bytes);
    auto b = g_input_pool.Load(u, u_bytes);
    auto c = g_input_pool.Load(v, v_bytes);
    auto o = g_packed_output_pool.Aquire(output_bytes);
    if (!a || !b || !c || !o || !a->buffer || !b->buffer || !c->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, width, height);
    p.ys = y_stride;
    p.us = u_stride;
    p.vs = v_stride;
    p.os = width;
    auto params_buffer = g_params_pool.Load(&p, sizeof(p));
    if (!params_buffer || !params_buffer->buffer)
        return false;
    bool ok = run_shader(g_i420_to_bgra, a.get(), b.get(), c.get(), o.get(), nullptr, nullptr, nullptr, params_buffer.get(), width, height) &&
              copyback_bgra(*o, d, output_stride, width, height);
    return ok;
}


bool d3d11_yuy2_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *s,
                              size_t ss, void *d, size_t ds) {
    if (!is_d3d11_colorconv_avail() || !s || !d || w < 2 || h < 1 || (w & 1))
        return false;
    const UINT width = (UINT)w, height = (UINT)h;
    const UINT source_stride = (UINT)ss, output_stride = (UINT)ds;
    const UINT source_bytes = (UINT)(ss * h);
    const UINT output_bytes = (UINT)(w * 4 * h);
    auto a = g_input_pool.Load(s, source_bytes);
    auto o = g_packed_output_pool.Aquire(output_bytes);
    if (!a || !o || !a->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, width, height);
    p.ys = source_stride;
    p.os = width;
    auto params_buffer = g_params_pool.Load(&p, sizeof(p));
    if (!params_buffer || !params_buffer->buffer)
        return false;
    auto &yuy2_shader = (source_stride & 3)
        ? g_yuy2_to_bgra_unaligned
        : g_yuy2_to_bgra;
    bool ok = run_shader(yuy2_shader, a.get(), nullptr, nullptr, o.get(), nullptr, nullptr, nullptr, params_buffer.get(), width / 2, height) &&
              copyback_bgra(*o, d, output_stride, width, height);
    return ok;
}


bool d3d11_bgra_to_i420_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *s,
                              size_t ss, void *y, size_t ys, void *u, size_t us, void *v,
                              size_t vs) {
    if (!is_d3d11_colorconv_avail() || !s || !y || !u || !v || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    auto a = g_input_pool.Load(s, (UINT)(ss * h));
    auto oy = g_output_pool.Aquire((UINT)(ys * h));
    auto ou = g_output_pool.Aquire((UINT)(us * (h / 2)));
    auto ov = g_output_pool.Aquire((UINT)(vs * (h / 2)));
    if (!a || !oy || !ou || !ov ||
        !a->buffer || !oy->buffer || !ou->buffer || !ov->buffer)
        return false;
    Params p;
    params(ci, p, (UINT)w, (UINT)h);
    p.ys = (UINT)ss;
    p.us = (UINT)us;
    p.vs = (UINT)vs;
    p.os = (UINT)ys;
    auto params_buffer = g_params_pool.Load(&p, sizeof(p));
    if (!params_buffer || !params_buffer->buffer)
        return false;
    bool ok = run_shader(g_bgra_to_i420, a.get(), nullptr, nullptr, nullptr, oy.get(), ou.get(), ov.get(), params_buffer.get(), (UINT)w, (UINT)h) &&
              copyback(*oy, y, (UINT)(ys * h)) &&
              copyback(*ou, u, (UINT)(us * (h / 2))) &&
              copyback(*ov, v, (UINT)(vs * (h / 2)));
    return ok;
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
