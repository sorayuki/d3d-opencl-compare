#include "cc_d3d12.h"

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D12Device> g_dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12CommandAllocator> g_allocator;
ComPtr<ID3D12GraphicsCommandList> g_commands;
ComPtr<ID3D12Fence> g_fence;
ComPtr<ID3D12RootSignature> g_root_signature;
ComPtr<ID3D12PipelineState> g_nv12_to_bgra;
ComPtr<ID3D12DescriptorHeap> g_descriptors;
UINT g_descriptor_size = 0;
UINT64 g_fence_value = 0;
D3D12_HEAP_TYPE g_cpu_to_gpu_heap_type = D3D12_HEAP_TYPE_UPLOAD;
bool g_direct_mapped_output = false;
std::mutex g_run_mutex;
std::atomic_bool g_enabled{true};
std::atomic_bool g_operational{false};
std::atomic_bool g_gpu_upload_allocation_fallback{false};
std::atomic_bool g_direct_output_allocation_fallback{false};

struct EventHandle {
    HANDLE value = nullptr;

    EventHandle() = default;
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    ~EventHandle() {
        if (value)
            CloseHandle(value);
    }

    void reset(HANDLE new_value = nullptr) {
        if (value)
            CloseHandle(value);
        value = new_value;
    }
};

EventHandle g_fence_event;

struct Params {
    float k[3];
    float pad;
    float range[4];
    UINT w, h;
    UINT ys, us, vs, os;
    UINT reserved[2];
};

static_assert(sizeof(Params) == 64, "HLSL constant-buffer layout changed");

const char* kShader = R"(
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

ByteAddressBuffer A : register(t0);
ByteAddressBuffer B : register(t1);
RWBuffer<uint> O : register(u0);

uint2 load_two_bytes(ByteAddressBuffer buffer, uint byte_offset) {
    uint lane = byte_offset & 3;
    uint packed = buffer.Load(byte_offset & ~3);
    if (lane < 3) {
        uint pair = (packed >> (lane * 8)) & 0xffff;
        return uint2(pair & 255, pair >> 8);
    }
    return uint2(packed >> 24, buffer.Load((byte_offset & ~3) + 4) & 255);
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

uint pack_bgra_pixel(uint y, uint u, uint v) {
    uint3 rgb = uint3(round(255 * yuv_to_rgb(y, u, v).zyx));
    return rgb.x | (rgb.y << 8) | (rgb.z << 16) | 0xff000000;
}

[numthreads(32, 2, 1)]
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
)";

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_description(
    UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

D3D12_HEAP_PROPERTIES shared_output_heap_properties() {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

bool create_cpu_to_gpu_buffer(UINT64 bytes, ComPtr<ID3D12Resource>& result) {
    const auto description = buffer_description(bytes);
    auto heap = heap_properties(g_cpu_to_gpu_heap_type);
    HRESULT create_result = g_dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&result));
#if defined(HAVE_D3D12_GPU_UPLOAD_HEAP)
    if (FAILED(create_result) &&
        g_cpu_to_gpu_heap_type == D3D12_HEAP_TYPE_GPU_UPLOAD) {
        // Feature support can change after a driver reset, and an allocation
        // can still fail under memory pressure. Preserve the portable path.
        g_gpu_upload_allocation_fallback.store(true,
                                               std::memory_order_relaxed);
        heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        create_result = g_dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&result));
    }
#endif
    return SUCCEEDED(create_result);
}

struct GPUInputBuffer {
    size_t bytes = 0;
    ComPtr<ID3D12Resource> buffer;

    explicit GPUInputBuffer(size_t buffer_bytes) : bytes(buffer_bytes) {
        create_cpu_to_gpu_buffer(buffer_bytes, buffer);
    }

    bool Load(const void* data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer > bytes ||
            size > bytes - offset_in_buffer)
            return false;

        void* mapped = nullptr;
        const D3D12_RANGE read_range{0, 0};
        if (FAILED(buffer->Map(0, &read_range, &mapped)))
            return false;
        std::memcpy(static_cast<unsigned char*>(mapped) + offset_in_buffer,
                    data, size);
        const D3D12_RANGE written_range{offset_in_buffer,
                                        offset_in_buffer + size};
        buffer->Unmap(0, &written_range);
        return true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view() const {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = DXGI_FORMAT_R32_TYPELESS;
        description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Buffer.FirstElement = 0;
        description.Buffer.NumElements = static_cast<UINT>(bytes / sizeof(UINT));
        description.Buffer.StructureByteStride = 0;
        description.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return description;
    }
};

struct GPUOutputBuffer {
    size_t bytes = 0;
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Resource> readback;
    bool direct_mapped = false;

    explicit GPUOutputBuffer(size_t buffer_bytes) : bytes(buffer_bytes) {
        const auto output_description = buffer_description(
            buffer_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (g_direct_mapped_output) {
            const auto shared_heap = shared_output_heap_properties();
            if (SUCCEEDED(g_dev->CreateCommittedResource(
                    &shared_heap, D3D12_HEAP_FLAG_NONE, &output_description,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&buffer)))) {
                direct_mapped = true;
                return;
            }
            g_direct_output_allocation_fallback.store(
                true, std::memory_order_relaxed);
            buffer.Reset();
        }

        const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(g_dev->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &output_description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&buffer)))) {
            return;
        }

        const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const auto readback_description = buffer_description(buffer_bytes);
        if (FAILED(g_dev->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback)))) {
            buffer.Reset();
        }
    }

    bool valid() const {
        return buffer && (direct_mapped || readback);
    }

    ID3D12Resource* cpu_read_resource() const {
        return direct_mapped ? buffer.Get() : readback.Get();
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC view() const {
        D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
        description.Format = DXGI_FORMAT_R32_UINT;
        description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        description.Buffer.FirstElement = 0;
        description.Buffer.NumElements = static_cast<UINT>(bytes / sizeof(UINT));
        description.Buffer.StructureByteStride = 0;
        description.Buffer.CounterOffsetInBytes = 0;
        description.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        return description;
    }
};

struct GPUParamsBuffer {
    size_t bytes = 0;
    ComPtr<ID3D12Resource> buffer;

    explicit GPUParamsBuffer(size_t buffer_bytes) : bytes(buffer_bytes) {
        const UINT64 allocation_size =
            (static_cast<UINT64>(buffer_bytes) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
            ~(static_cast<UINT64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1);
        create_cpu_to_gpu_buffer(allocation_size, buffer);
    }

    bool Load(const void* data, size_t size, size_t offset_in_buffer) {
        if (!buffer || !data || offset_in_buffer != 0 || size != bytes)
            return false;
        void* mapped = nullptr;
        const D3D12_RANGE read_range{0, 0};
        if (FAILED(buffer->Map(0, &read_range, &mapped)))
            return false;
        std::memcpy(mapped, data, size);
        const D3D12_RANGE written_range{0, size};
        buffer->Unmap(0, &written_range);
        return true;
    }
};

template<class T>
class BufPool {
public:
    explicit BufPool(size_t threshold) : threshold_(threshold) {}

    std::shared_ptr<T> Load(const void* data, UINT bytes) {
        auto result = Acquire(bytes);
        if (!result || !result->Load(data, bytes, 0))
            return {};
        return result;
    }

    std::shared_ptr<T> Acquire(UINT bytes) {
        if (bytes > (std::numeric_limits<UINT>::max)() - (sizeof(UINT) - 1))
            return {};
        bytes = (bytes + sizeof(UINT) - 1) & ~(sizeof(UINT) - 1);
        T value = Acquire_(bytes);
        return std::shared_ptr<T>(new T(std::move(value)), [this](T* item) {
            Release(std::move(*item));
            delete item;
        });
    }

private:
    std::multimap<size_t, T> pool_;
    std::multimap<size_t, T> old_pool_;
    std::mutex mutex_;
    size_t pooled_bytes_ = 0;
    size_t threshold_;

    T Acquire_(UINT bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (auto* pool : {&old_pool_, &pool_}) {
            const auto found = pool->find(bytes);
            if (found == pool->end())
                continue;
            T result = std::move(found->second);
            pool->erase(found);
            if (pool == &pool_)
                pooled_bytes_ -= result.bytes;
            return result;
        }

        lock.unlock();
        return T(bytes);
    }

    void Release(T&& buffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!buffer.buffer)
            return;
        if constexpr (std::is_same_v<T, GPUOutputBuffer>) {
            if (!buffer.valid())
                return;
        }
        const size_t bytes = buffer.bytes;
        pool_.emplace(bytes, std::move(buffer));
        pooled_bytes_ += bytes;
        if (pooled_bytes_ > threshold_) {
            old_pool_.clear();
            old_pool_.swap(pool_);
            pooled_bytes_ = 0;
        }
    }
};

BufPool<GPUInputBuffer> g_input_pool(16 * 1024 * 1024);
BufPool<GPUOutputBuffer> g_output_pool(8 * 1024 * 1024);
BufPool<GPUParamsBuffer> g_params_pool(1 * 1024 * 1024);

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    return barrier;
}

bool execute(const GPUInputBuffer& y, const GPUInputBuffer& uv,
             GPUOutputBuffer& output, const GPUParamsBuffer& params,
             UINT work_width, UINT work_height) {
    if (!y.buffer || !uv.buffer || !output.valid() || !params.buffer)
        return false;

    const UINT groups_x = (work_width + 31) / 32;
    const UINT groups_y = (work_height + 1) / 2;
    if (groups_x == 0 || groups_y == 0 ||
        groups_x > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
        groups_y > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
        return false;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        g_descriptors->GetCPUDescriptorHandleForHeapStart();
    auto y_view = y.view();
    g_dev->CreateShaderResourceView(y.buffer.Get(), &y_view, cpu);
    cpu.ptr += g_descriptor_size;
    auto uv_view = uv.view();
    g_dev->CreateShaderResourceView(uv.buffer.Get(), &uv_view, cpu);
    cpu.ptr += g_descriptor_size;
    auto output_view = output.view();
    g_dev->CreateUnorderedAccessView(output.buffer.Get(), nullptr,
                                     &output_view, cpu);

    if (FAILED(g_allocator->Reset()) ||
        FAILED(g_commands->Reset(g_allocator.Get(), g_nv12_to_bgra.Get()))) {
        g_operational.store(false, std::memory_order_release);
        return false;
    }

    ID3D12DescriptorHeap* descriptor_heaps[] = {g_descriptors.Get()};
    g_commands->SetDescriptorHeaps(1, descriptor_heaps);
    g_commands->SetComputeRootSignature(g_root_signature.Get());
    g_commands->SetComputeRootConstantBufferView(
        0, params.buffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        g_descriptors->GetGPUDescriptorHandleForHeapStart();
    g_commands->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += static_cast<UINT64>(2) * g_descriptor_size;
    g_commands->SetComputeRootDescriptorTable(2, gpu);
    g_commands->Dispatch(groups_x, groups_y, 1);

    if (output.direct_mapped) {
        auto output_complete = uav_barrier(output.buffer.Get());
        g_commands->ResourceBarrier(1, &output_complete);
    } else {
        auto to_copy = transition(output.buffer.Get(),
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_commands->ResourceBarrier(1, &to_copy);
        g_commands->CopyBufferRegion(output.readback.Get(), 0,
                                     output.buffer.Get(), 0, output.bytes);
        auto to_uav = transition(output.buffer.Get(),
                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_commands->ResourceBarrier(1, &to_uav);
    }

    if (FAILED(g_commands->Close())) {
        g_operational.store(false, std::memory_order_release);
        return false;
    }
    ID3D12CommandList* command_lists[] = {g_commands.Get()};
    g_queue->ExecuteCommandLists(1, command_lists);

    const UINT64 fence_value = ++g_fence_value;
    if (FAILED(g_queue->Signal(g_fence.Get(), fence_value))) {
        g_operational.store(false, std::memory_order_release);
        return false;
    }
    const UINT64 completed_value = g_fence->GetCompletedValue();
    if (completed_value == (std::numeric_limits<UINT64>::max)()) {
        g_operational.store(false, std::memory_order_release);
        return false;
    }
    if (completed_value < fence_value) {
        if (FAILED(g_fence->SetEventOnCompletion(fence_value,
                                                 g_fence_event.value))) {
            g_operational.store(false, std::memory_order_release);
            return false;
        }
        if (WaitForSingleObject(g_fence_event.value, INFINITE) != WAIT_OBJECT_0) {
            g_operational.store(false, std::memory_order_release);
            return false;
        }
    }
    const UINT64 final_completed_value = g_fence->GetCompletedValue();
    if (final_completed_value == (std::numeric_limits<UINT64>::max)() ||
        final_completed_value < fence_value) {
        g_operational.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool copyback_bgra(GPUOutputBuffer& output, void* destination,
                   UINT destination_stride, UINT width, UINT height) {
    ID3D12Resource* cpu_resource = output.cpu_read_resource();
    if (!cpu_resource || !destination || destination_stride < width * 4)
        return false;

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, output.bytes};
    if (FAILED(cpu_resource->Map(0, &read_range, &mapped)))
        return false;

    const size_t row_bytes = static_cast<size_t>(width) * 4;
    auto* destination_bytes = static_cast<unsigned char*>(destination);
    const auto* source_bytes = static_cast<const unsigned char*>(mapped);
    if (destination_stride == row_bytes) {
        std::memcpy(destination_bytes, source_bytes, row_bytes * height);
    } else {
        for (UINT row = 0; row < height; ++row) {
            std::memcpy(destination_bytes + static_cast<size_t>(row) * destination_stride,
                        source_bytes + static_cast<size_t>(row) * row_bytes,
                        row_bytes);
        }
    }
    const D3D12_RANGE written_range{0, 0};
    cpu_resource->Unmap(0, &written_range);
    return true;
}

void fill_params(const asco::ColorInfo& color_info, Params& params,
                 UINT width, UINT height) {
    params = {};
    params.k[0] = color_info.trans_matrix == asco::ColorTransMatrix::BT601
                      ? .299f : .2126f;
    params.k[1] = color_info.trans_matrix == asco::ColorTransMatrix::BT601
                      ? .587f : .7152f;
    params.k[2] = color_info.trans_matrix == asco::ColorTransMatrix::BT601
                      ? .114f : .0722f;
    params.range[0] = color_info.nominal_range ==
                              asco::ColorNominalRange::_0_255
                          ? 0.0f : 16.0f / 255.0f;
    params.range[1] = color_info.nominal_range ==
                              asco::ColorNominalRange::_0_255
                          ? 1.0f : 255.0f / 219.0f;
    params.range[2] = params.range[0];
    params.range[3] = color_info.nominal_range ==
                              asco::ColorNominalRange::_0_255
                          ? 1.0f : 255.0f / 224.0f;
    params.w = width;
    params.h = height;
}

bool size_to_uint(size_t value, UINT& result) {
    if (value > (std::numeric_limits<UINT>::max)())
        return false;
    result = static_cast<UINT>(value);
    return true;
}

bool product_to_uint(size_t a, size_t b, UINT& result) {
    if (a != 0 && b > (std::numeric_limits<UINT>::max)() / a)
        return false;
    return size_to_uint(a * b, result);
}

bool create_root_signature(ID3D12Device* device,
                           ComPtr<ID3D12RootSignature>& result) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = parameters;
    description.NumStaticSamplers = 0;
    description.pStaticSamplers = nullptr;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    const HRESULT serialize_result = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serialize_result)) {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    return SUCCEEDED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&result)));
}

bool create_pipeline(ID3D12Device* device, ID3D12RootSignature* root_signature,
                     ComPtr<ID3D12PipelineState>& result) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compile_result = D3DCompile(
        kShader, std::strlen(kShader), nullptr, nullptr, nullptr,
        "nv12_to_bgra_frame", "cs_5_1", flags, 0, &bytecode, &errors);
    if (FAILED(compile_result)) {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = root_signature;
    description.CS.pShaderBytecode = bytecode->GetBufferPointer();
    description.CS.BytecodeLength = bytecode->GetBufferSize();
    return SUCCEEDED(device->CreateComputePipelineState(
        &description, IID_PPV_ARGS(&result)));
}

bool supports_gpu_upload_heap(ID3D12Device* device) {
#if defined(HAVE_D3D12_GPU_UPLOAD_HEAP)
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
    return SUCCEEDED(device->CheckFeatureSupport(
               D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options))) &&
           options.GPUUploadHeapSupported;
#else
    (void)device;
    return false;
#endif
}

bool supports_direct_mapped_output(ID3D12Device* device) {
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    architecture.NodeIndex = 0;
    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_ARCHITECTURE, &architecture,
            sizeof(architecture))) || !architecture.UMA) {
        return false;
    }

    const auto heap = shared_output_heap_properties();
    const auto description = buffer_description(
        sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> probe;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&probe)))) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, 0};
    if (FAILED(probe->Map(0, &read_range, &mapped)) || !mapped)
        return false;
    const D3D12_RANGE written_range{0, 0};
    probe->Unmap(0, &written_range);
    return true;
}

}  // namespace

void init_d3d12_colorconv(void* d3d11_device) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (g_dev || !d3d11_device)
        return;

    auto* source_device = static_cast<ID3D11Device*>(d3d11_device);
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(source_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)))
        return;

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device))))
        return;

    const bool gpu_upload_heap_supported =
        supports_gpu_upload_heap(device.Get());
    const bool direct_mapped_output_supported =
        supports_direct_mapped_output(device.Get());

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_description.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queue_description,
                                          IID_PPV_ARGS(&queue))))
        return;

    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              IID_PPV_ARGS(&allocator))))
        return;

    ComPtr<ID3D12RootSignature> root_signature;
    if (!create_root_signature(device.Get(), root_signature))
        return;

    ComPtr<ID3D12PipelineState> pipeline;
    if (!create_pipeline(device.Get(), root_signature.Get(), pipeline))
        return;

    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands))) || FAILED(commands->Close()))
        return;

    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence))))
        return;

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
    descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_description.NumDescriptors = 3;
    descriptor_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descriptor_description.NodeMask = 0;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    if (FAILED(device->CreateDescriptorHeap(&descriptor_description,
                                            IID_PPV_ARGS(&descriptors))))
        return;

    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle)
        return;

    g_dev = std::move(device);
    g_queue = std::move(queue);
    g_allocator = std::move(allocator);
    g_commands = std::move(commands);
    g_fence = std::move(fence);
    g_root_signature = std::move(root_signature);
    g_nv12_to_bgra = std::move(pipeline);
    g_descriptors = std::move(descriptors);
    g_descriptor_size = g_dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_fence_value = 0;
#if defined(HAVE_D3D12_GPU_UPLOAD_HEAP)
    g_cpu_to_gpu_heap_type = gpu_upload_heap_supported
                                 ? D3D12_HEAP_TYPE_GPU_UPLOAD
                                 : D3D12_HEAP_TYPE_UPLOAD;
#else
    (void)gpu_upload_heap_supported;
    g_cpu_to_gpu_heap_type = D3D12_HEAP_TYPE_UPLOAD;
#endif
    g_direct_mapped_output = direct_mapped_output_supported;
    g_gpu_upload_allocation_fallback.store(false, std::memory_order_relaxed);
    g_direct_output_allocation_fallback.store(false,
                                               std::memory_order_relaxed);
    g_fence_event.reset(event_handle);
    g_operational.store(true, std::memory_order_release);
}

bool is_d3d12_colorconv_avail() {
    return g_enabled.load(std::memory_order_relaxed) &&
           g_operational.load(std::memory_order_acquire) &&
           g_dev && g_queue && g_allocator && g_commands &&
           g_fence && g_root_signature && g_nv12_to_bgra && g_descriptors &&
           g_descriptor_size != 0 && g_fence_event.value;
}

void d3d12_colorconv_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

const char* d3d12_colorconv_transfer_mode() {
    if (!g_operational.load(std::memory_order_acquire))
        return "uninitialized";
    const bool allocation_fallback =
        g_gpu_upload_allocation_fallback.load(std::memory_order_relaxed) ||
        g_direct_output_allocation_fallback.load(std::memory_order_relaxed);
    bool gpu_upload_selected = false;
#if defined(HAVE_D3D12_GPU_UPLOAD_HEAP)
    gpu_upload_selected =
        g_cpu_to_gpu_heap_type == D3D12_HEAP_TYPE_GPU_UPLOAD;
#endif
    if (gpu_upload_selected && g_direct_mapped_output) {
        return allocation_fallback
                   ? "GPU_UPLOAD input; mapped shared-UAV output (allocation fallback used)"
                   : "GPU_UPLOAD input; mapped shared-UAV output";
    }
    if (gpu_upload_selected) {
        return allocation_fallback
                   ? "GPU_UPLOAD input; DEFAULT-to-READBACK output (allocation fallback used)"
                   : "GPU_UPLOAD input; DEFAULT-to-READBACK output";
    }
    if (g_direct_mapped_output) {
        return allocation_fallback
                   ? "mapped UPLOAD input; mapped shared-UAV output (allocation fallback used)"
                   : "mapped UPLOAD input; mapped shared-UAV output";
    }
    return allocation_fallback
               ? "mapped UPLOAD input; DEFAULT-to-READBACK output (allocation fallback used)"
               : "mapped UPLOAD input; DEFAULT-to-READBACK output";
}

bool d3d12_nv12_to_bgra_frame(
    const asco::ColorInfo& color_info, size_t width_value, size_t height_value,
    void* destination, size_t destination_stride_value, const void* source_y,
    size_t source_y_stride_value, const void* source_uv,
    size_t source_uv_stride_value) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (!is_d3d12_colorconv_avail() || !destination || !source_y || !source_uv ||
        width_value < 2 || height_value < 2 || (width_value & 1) ||
        (height_value & 1))
        return false;

    UINT width, height, destination_stride, source_y_stride, source_uv_stride;
    UINT y_bytes, uv_bytes, output_bytes;
    if (!size_to_uint(width_value, width) ||
        !size_to_uint(height_value, height) ||
        !size_to_uint(destination_stride_value, destination_stride) ||
        !size_to_uint(source_y_stride_value, source_y_stride) ||
        !size_to_uint(source_uv_stride_value, source_uv_stride) ||
        source_y_stride_value < width_value ||
        source_uv_stride_value < width_value ||
        width_value > (std::numeric_limits<UINT>::max)() / 4 ||
        destination_stride_value < width_value * 4 ||
        destination_stride_value >
            (std::numeric_limits<size_t>::max)() / height_value ||
        !product_to_uint(source_y_stride_value, height_value, y_bytes) ||
        !product_to_uint(source_uv_stride_value, height_value / 2, uv_bytes) ||
        !product_to_uint(width_value * 4, height_value, output_bytes))
        return false;

    auto y_buffer = g_input_pool.Load(source_y, y_bytes);
    auto uv_buffer = g_input_pool.Load(source_uv, uv_bytes);
    auto output = g_output_pool.Acquire(output_bytes);
    if (!y_buffer || !uv_buffer || !output || !y_buffer->buffer ||
        !uv_buffer->buffer || !output->valid())
        return false;

    Params params{};
    fill_params(color_info, params, width, height);
    params.ys = source_y_stride;
    params.us = source_uv_stride;
    params.os = width;
    auto params_buffer = g_params_pool.Load(&params, sizeof(params));
    if (!params_buffer || !params_buffer->buffer)
        return false;

    return execute(*y_buffer, *uv_buffer, *output, *params_buffer,
                   width / 2, height / 2) &&
           copyback_bgra(*output, destination, destination_stride,
                         width, height);
}
