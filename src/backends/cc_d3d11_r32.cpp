#include "cc_d3d11.h"
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

ID3D11Device *g_dev = nullptr;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11ComputeShader> g_nv12, g_i420, g_i420_image, g_yuy2, g_rgb;
bool g_enabled = true;


struct Params {
    float k[3];
    float pad;
    float range[4];
    UINT w, h;
    UINT ys, us, vs, os;
    UINT reserved[2];
};


const char *kShader = R"(
cbuffer P : register(b0) {
    float3 k;
    float pad;
    float4 range;
    uint w;
    uint h;
    uint ys;
    uint us;
    uint vs;
    uint os;
    uint2 reserved;
}

RWByteAddressBuffer A : register(u4);
RWByteAddressBuffer B : register(u5);
RWByteAddressBuffer C : register(u6);
RWByteAddressBuffer O : register(u0);
RWByteAddressBuffer OY : register(u1);
RWByteAddressBuffer OU : register(u2);
RWByteAddressBuffer OV : register(u3);
RWTexture2D<float4> OF : register(u0);

uint byteAt(RWByteAddressBuffer b, uint p) {
    uint x = b.Load(p & ~3);
    return (x >> ((p & 3) * 8)) & 255;
}

float3 yuv_to_rgb(uint y, uint u, uint v) {
    float3 f = (float3(y, u, v) / 255 -
                float3(range.x, range.z, range.z)) *
               float3(range.y, 2 * range.w, 2 * range.w) - float3(0, 1, 1);
    return saturate(float3(
        f.x + f.z * (1 - k.x),
        f.x - f.y * (1 - k.z) * k.z / k.y -
            f.z * (1 - k.x) * k.x / k.y,
        f.x + f.y * (1 - k.z)));
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
void nv12(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy / 2;
    uint uvp = q.y * us + q.x * 2;
    uint4 rgb = uint4(round(255 * yuv_to_rgb(byteAt(A, id.y * ys + id.x), byteAt(B, uvp), byteAt(B, uvp + 1)).zyx), 255);
    O.Store((id.y * os + id.x) * 4, rgb.x | (rgb.y << 8) | (rgb.z << 16) | (rgb.w << 24));
}

[numthreads(8, 8, 1)]
void i420(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy / 2;
    uint4 rgb = uint4(round(255 * yuv_to_rgb(byteAt(A, id.y * ys + id.x), byteAt(B, q.y * us + q.x), byteAt(C, q.y * vs + q.x)).zyx), 255);
    O.Store((id.y * os + id.x) * 4, rgb.x | (rgb.y << 8) | (rgb.z << 16) | (rgb.w << 24));
}

[numthreads(8, 8, 1)]
void i420_image(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy / 2;
    OF[id.xy] = float4(yuv_to_rgb(byteAt(A, id.y * ys + id.x), byteAt(B, q.y * us + q.x), byteAt(C, q.y * vs + q.x)).zyx, 1);
}

[numthreads(8, 8, 1)]
void yuy2(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 q = uint2(id.x / 2, id.y);
    uint zp = id.y * ys + q.x * 4;
    uint y = byteAt(A, zp + ((id.x & 1) ? 2 : 0));
    uint4 rgb = uint4(round(255 * yuv_to_rgb(y, byteAt(A, zp + 1), byteAt(A, zp + 3)).zyx), 255);
    O.Store((id.y * os + id.x) * 4, rgb.x | (rgb.y << 8) | (rgb.z << 16) | (rgb.w << 24));
}

[numthreads(8, 8, 1)]
void bgra(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint p0 = id.y * ys + id.x * 4;
    uint4 z = uint4(byteAt(A,p0), byteAt(A,p0+1), byteAt(A,p0+2), byteAt(A,p0+3));
    float3 t = rgb_to_yuv(z.zyx);
    OY.Store((id.y * os + id.x) * 4, round(t.x));
    if (!(id.x & 1) && !(id.y & 1)) {
        uint2 q = id.xy / 2;
        OU.Store((q.y * (os / 2) + q.x) * 4, round(t.y));
        OV.Store((q.y * (os / 2) + q.x) * 4, round(t.z));
    }
}
)";


struct GPUBuffer {
    size_t bytes = 0;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> uav;

    GPUBuffer(size_t bytes): bytes(bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, &buffer)))
            return;
        D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        v.Buffer.NumElements = d.ByteWidth / 4;
        v.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        g_dev->CreateUnorderedAccessView(buffer.Get(), &v, &uav);
    }

    bool Load(const void* data, size_t size, size_t offset_in_buffer) {
        if (size + offset_in_buffer > bytes)
            return false;
        
        D3D11_BOX box{};
        box.left = (UINT)offset_in_buffer;
        box.right = (UINT)(offset_in_buffer + size);
        box.bottom = 1;
        box.back = 1;
        g_ctx->UpdateSubresource(buffer.Get(), 0, &box, data, 0, 0);
        return true;
    }
};


struct GPUOutputBuffer: GPUBuffer {
    ComPtr<ID3D11Buffer> staging;
    bool mapped = false;

    GPUOutputBuffer(size_t bytes): GPUBuffer(bytes) {
        D3D11_BUFFER_DESC staging_desc = {};
        staging_desc.ByteWidth = bytes;
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


bool shader(const char *entry, ComPtr<ID3D11ComputeShader> &out) {
    ComPtr<ID3DBlob> b, e;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, entry,
                            "cs_5_0", 0, 0, &b, &e);
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
    std::shared_ptr<T> Load(const void *data, UINT bytes) {
        auto ret = Aquire(bytes);
        ret->Load(data, bytes, 0);
        return ret;
    }

    std::shared_ptr<T> Aquire(UINT bytes) {
        T value = Acquire_(bytes);
        return std::shared_ptr<T>(new T(std::move(value)), [this](T* value) {
            Release(std::move(*value));
            delete value;
        });
    }


private:
    std::map<size_t, T> pool_;
    std::map<size_t, T> oldpool_;
    std::mutex pool_mutex_;
    size_t bytes_ = 0;

    T Acquire_(UINT bytes) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        for(auto p: { &oldpool_, &pool_ }) {
            auto it = p->lower_bound(bytes);
            if (it == p->end() || (it->first > bytes * 2)) {
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
        auto bytes = buf.bytes;
        pool_.emplace(bytes, std::move(buf));
        bytes_ += bytes;

        int threshold;

        if constexpr (std::is_same_v<T, GPUBuffer>) {
            threshold = 16 * 1024 * 1024; // 输入缓冲区，16MB大小
        } else {
            threshold = 16 * 1024 * 1024; // 输出缓冲区，16MB大小
        }

        if (bytes_ > threshold) { // 暂定，64M的池大小，超出就会清理
            oldpool_.clear();
            oldpool_.swap(pool_);
            bytes_ = 0;
        }
    }
};


BufPool<GPUBuffer> g_input_pool;
BufPool<GPUOutputBuffer> g_output_pool;

bool copyback(GPUOutputBuffer &x, void *dst, UINT dst_pitch, UINT w, UINT h, bool rgba) {
    if (!x.buffer || !x.staging)
        return false;
    auto ptr = (const unsigned char*)x.MapBuffer();
    for (UINT y = 0; y < h; y++) {
        const UINT *src = (const UINT *)(ptr + y * w * 4);
        if (rgba)
            memcpy((char *)dst + y * dst_pitch, src, w * 4);
        else {
            for (UINT x = 0; x < w; x++)
                ((unsigned char *)dst)[y * dst_pitch + x] = (unsigned char)src[x];
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
    p.range[0] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 0 : 16.f / 255;
    p.range[1] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 1 : 255.f / 219;
    p.range[2] = p.range[0];
    p.range[3] = ci.nominal_range == asco::ColorNominalRange::_0_255 ? 1 : 255.f / 224;
    p.w = w;
    p.h = h;
}


bool run(ComPtr<ID3D11ComputeShader> &cs, const GPUBuffer *a, const GPUBuffer *b,
         const GPUBuffer *c, const GPUBuffer *o, const GPUBuffer *oy,
         const GPUBuffer *ou, const GPUBuffer *ov,
         const Params &p, UINT w, UINT h) {
    if (!cs)
        return false;
    D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sd{&p, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &cb)))
        return false;
    ID3D11UnorderedAccessView *uv[7] = {
        o ? o->uav.Get() : nullptr,
        oy ? oy->uav.Get() : nullptr,
        ou ? ou->uav.Get() : nullptr,
        ov ? ov->uav.Get() : nullptr,
        a ? a->uav.Get() : nullptr,
        b ? b->uav.Get() : nullptr,
        c ? c->uav.Get() : nullptr};
    g_ctx->CSSetShader(cs.Get(), nullptr, 0);
    g_ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g_ctx->CSSetUnorderedAccessViews(0, 7, uv, nullptr);
    g_ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11UnorderedAccessView *zu[7] = {};
    g_ctx->CSSetUnorderedAccessViews(0, 7, zu, nullptr);
    g_ctx->Flush();
    return true;
}

} // namespace


void init_d3d11_colorconv(void *d) {
    g_dev = (ID3D11Device *)d;
    if (!g_dev)
        return;
    g_dev->GetImmediateContext(&g_ctx);
    shader("nv12", g_nv12);
    shader("i420", g_i420);
    shader("i420_image", g_i420_image);
    shader("yuy2", g_yuy2);
    shader("bgra", g_rgb);
}


bool is_d3d11_colorconv_avail() {
    return g_enabled && g_dev && g_ctx && g_nv12 && g_i420 && g_i420_image && g_yuy2 && g_rgb;
}


void d3d11_colorconv_set_enabled(bool enabled) {
    g_enabled = enabled;
}


bool d3d11_nv12_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, void *d, size_t ds,
                              const void *y, size_t ys, const void *uv, size_t us) {
    if (!is_d3d11_colorconv_avail() || !d || !y || !uv || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    auto a = g_input_pool.Load(y, (UINT)(ys * h));
    auto b = g_input_pool.Load(uv, (UINT)(us * (h / 2)));
    auto o = g_output_pool.Aquire((UINT)(w * h * 4));
    if (!a->buffer || !b->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, (UINT)w, (UINT)h);
    p.ys = (UINT)ys;
    p.us = (UINT)us;
    p.os = (UINT)w;
    bool ok = run(g_nv12, a.get(), b.get(), nullptr, o.get(), nullptr, nullptr, nullptr,
                  p, (UINT)w, (UINT)h) &&
              copyback(*o, d, (UINT)ds, (UINT)w, (UINT)h, true);
    return ok;
}


bool d3d11_i420_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *y,
                              size_t ys, const void *u, size_t us, const void *v, size_t vs,
                              void *d, size_t ds) {
    if (!is_d3d11_colorconv_avail() || !d || !y || !u || !v || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    auto a = g_input_pool.Load(y, (UINT)(ys * h));
    auto b = g_input_pool.Load(u, (UINT)(us * (h / 2)));
    auto c = g_input_pool.Load(v, (UINT)(vs * (h / 2)));
    auto o = g_output_pool.Aquire((UINT)(w * h * 4));
    if (!a->buffer || !b->buffer || !c->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, (UINT)w, (UINT)h);
    p.ys = (UINT)ys;
    p.us = (UINT)us;
    p.vs = (UINT)vs;
    p.os = (UINT)w;
    bool ok = run(g_i420, a.get(), b.get(), c.get(), o.get(), nullptr, nullptr, nullptr,
                  p, (UINT)w, (UINT)h) &&
              copyback(*o, d, (UINT)ds, (UINT)w, (UINT)h, true);
    return ok;
}


bool d3d11_yuy2_to_bgra_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *s,
                              size_t ss, void *d, size_t ds) {
    if (!is_d3d11_colorconv_avail() || !s || !d || w < 2 || h < 1 || (w & 1))
        return false;
    auto a = g_input_pool.Load(s, (UINT)(ss * h));
    auto o = g_output_pool.Aquire((UINT)(w * h * 4));
    if (!a->buffer || !o->buffer)
        return false;
    Params p;
    params(ci, p, (UINT)w, (UINT)h);
    p.ys = (UINT)ss;
    p.os = (UINT)w;
    bool ok = run(g_yuy2, a.get(), nullptr, nullptr, o.get(), nullptr, nullptr, nullptr,
                  p, (UINT)w, (UINT)h) &&
              copyback(*o, d, (UINT)ds, (UINT)w, (UINT)h, true);
    return ok;
}


bool d3d11_bgra_to_i420_frame(const asco::ColorInfo &ci, size_t w, size_t h, const void *s,
                              size_t ss, void *y, size_t ys, void *u, size_t us, void *v,
                              size_t vs) {
    if (!is_d3d11_colorconv_avail() || !s || !y || !u || !v || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    auto a = g_input_pool.Load(s, (UINT)(ss * h));
    auto oy = g_output_pool.Aquire((UINT)(w * h * 4));
    auto ou = g_output_pool.Aquire((UINT)(w * h));
    auto ov = g_output_pool.Aquire((UINT)(w * h));
    if (!a->buffer || !oy->buffer || !ou->buffer || !ov->buffer)
        return false;
    Params p;
    params(ci, p, (UINT)w, (UINT)h);
    p.ys = (UINT)ss;
    p.os = (UINT)w;
    bool ok = run(g_rgb, a.get(), nullptr, nullptr, nullptr, oy.get(), ou.get(), ov.get(),
                  p, (UINT)w, (UINT)h) &&
              copyback(*oy, y, (UINT)ys, (UINT)w, (UINT)h, false) &&
              copyback(*ou, u, (UINT)us, (UINT)w / 2, (UINT)h / 2, false) &&
              copyback(*ov, v, (UINT)vs, (UINT)w / 2, (UINT)h / 2, false);
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
