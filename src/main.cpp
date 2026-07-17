#include "backend_api.h"

#include <d3d11.h>
#include <dxgi.h>
#if defined(BENCHMARK_D3D11ON12)
#include <d3d11on12.h>
#include <d3d12.h>
#endif
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct Options {
    double seconds = 5.0;
    unsigned int warmup_frames = 20;
    std::size_t width = 1920;
    std::size_t height = 1080;
    std::string output_path;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--seconds" && i + 1 < argc) {
            options.seconds = std::stod(argv[++i]);
        } else if (argument == "--warmup" && i + 1 < argc) {
            options.warmup_frames = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (argument == "--width" && i + 1 < argc) {
            options.width = std::stoull(argv[++i]);
        } else if (argument == "--height" && i + 1 < argc) {
            options.height = std::stoull(argv[++i]);
        } else if (argument == "--output" && i + 1 < argc) {
            options.output_path = argv[++i];
        } else if (argument == "--help") {
            std::cout << "Usage: " << argv[0]
                      << " [--seconds N] [--warmup N] [--width N] [--height N] [--output FILE]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown or incomplete argument: " + argument);
        }
    }
    if (options.seconds <= 0.0) {
        throw std::invalid_argument("--seconds must be greater than zero");
    }
    if (options.width < 2 || options.height < 2 ||
        (options.width & 1) || (options.height & 1)) {
        throw std::invalid_argument("NV12 width and height must be even and at least 2");
    }
    const auto max_size = (std::numeric_limits<std::size_t>::max)();
    if (options.width > max_size / options.height) {
        throw std::invalid_argument("NV12 dimensions are too large");
    }
    const auto pixels = options.width * options.height;
    if (pixels > max_size / 4 || pixels > max_size - pixels / 2) {
        throw std::invalid_argument("NV12 frame allocation is too large");
    }
    return options;
}

struct BenchmarkDevice {
    ComPtr<ID3D11Device> d3d11;
#if defined(BENCHMARK_D3D11ON12)
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
#endif
};

BenchmarkDevice create_device() {
    BenchmarkDevice result;
    ComPtr<IDXGIAdapter> selected_adapter;
    char* vendor_value = nullptr;
    size_t vendor_length = 0;
    if (_dupenv_s(&vendor_value, &vendor_length,
                  "DX_CL_ADAPTER_VENDOR") == 0 && vendor_value) {
        (void)vendor_length;
        char* end = nullptr;
        const unsigned long vendor = std::strtoul(vendor_value, &end, 0);
        if (end != vendor_value && *end == '\0') {
            ComPtr<IDXGIFactory1> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                for (UINT index = 0; !selected_adapter; ++index) {
                    ComPtr<IDXGIAdapter1> candidate;
                    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
                        break;
                    DXGI_ADAPTER_DESC1 description{};
                    if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                        description.VendorId == vendor)
                        selected_adapter = candidate;
                }
            }
        }
        std::free(vendor_value);
    }
    char* index_value = nullptr;
    size_t index_length = 0;
    if (!selected_adapter && _dupenv_s(&index_value, &index_length,
                                       "DX_CL_ADAPTER_INDEX") == 0 && index_value) {
        (void)index_length;
        char* end = nullptr;
        const unsigned long index = std::strtoul(index_value, &end, 10);
        if (end != index_value && *end == '\0') {
            ComPtr<IDXGIFactory1> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                factory->EnumAdapters(index, &selected_adapter);
        }
        std::free(index_value);
    }
    IDXGIAdapter* adapter = selected_adapter.Get();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

#if defined(BENCHMARK_D3D11ON12)
    HRESULT hr = D3D12CreateDevice(
        adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&result.d3d12));
    if (FAILED(hr)) {
        throw std::runtime_error("D3D12 hardware device creation failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(hr)) + ")");
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = result.d3d12->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(&result.queue));
    if (FAILED(hr)) {
        throw std::runtime_error("D3D12 command queue creation failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(hr)) + ")");
    }

    IUnknown* queues[] = {result.queue.Get()};
    hr = D3D11On12CreateDevice(
        result.d3d12.Get(), 0, levels, static_cast<UINT>(std::size(levels)),
        queues, static_cast<UINT>(std::size(queues)), 0, &device, &context,
        &feature_level);
    if (hr == E_INVALIDARG) {
        hr = D3D11On12CreateDevice(
            result.d3d12.Get(), 0, levels + 1, 1, queues,
            static_cast<UINT>(std::size(queues)), 0, &device, &context,
            &feature_level);
    }
#else
    HRESULT hr = D3D11CreateDevice(
        adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &feature_level, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, levels + 1, 1,
            D3D11_SDK_VERSION, &device, &feature_level, &context);
    }
#endif
    if (FAILED(hr)) {
        throw std::runtime_error("D3D11 hardware device creation failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
    result.d3d11 = std::move(device);
    return result;
}

std::string adapter_name(const BenchmarkDevice& device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (!device.d3d11 || FAILED(device.d3d11.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&description)))
        return "unknown";
    const int length = WideCharToMultiByte(CP_UTF8, 0, description.Description,
                                           -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return "unknown";
    std::string name(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                        name.data(), length, nullptr, nullptr);
    name.resize(static_cast<size_t>(length - 1));
    return name;
}

void print_adapters() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
            continue;
        const int length = WideCharToMultiByte(CP_UTF8, 0, description.Description,
                                               -1, nullptr, 0, nullptr, nullptr);
        std::string name(length > 1 ? static_cast<size_t>(length) : 1, '\0');
        if (length > 1) {
            WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                                name.data(), length, nullptr, nullptr);
            name.resize(static_cast<size_t>(length - 1));
        } else {
            name = "unknown";
        }
        std::cout << "Adapter[" << index << "]: " << name
                  << " (Vendor 0x" << std::hex << description.VendorId
                  << ", Flags 0x" << description.Flags << std::dec << ")\n";
    }
}

asco::ColorInfo make_color_info() {
    asco::ColorInfo info{};
    info.primaries = asco::ColorPrimaries::BT709;
    info.trans_func = asco::ColorTransFunc::_709;
    info.trans_matrix = asco::ColorTransMatrix::BT709;
    info.nominal_range = asco::ColorNominalRange::_16_235;
    info.lighting = asco::ColorLighting::Unknown;
    return info;
}

void fill_nv12(std::vector<unsigned char>& input, std::size_t width, std::size_t height) {
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            input[y * width + x] = static_cast<unsigned char>(16 + ((x + y) % 220));
        }
    }
    auto* uv = input.data() + width * height;
    for (std::size_t y = 0; y < height / 2; ++y) {
        for (std::size_t x = 0; x < width; x += 2) {
            uv[y * width + x] = static_cast<unsigned char>(96 + ((x / 2) % 64));
            uv[y * width + x + 1] = static_cast<unsigned char>(96 + (y % 64));
        }
    }
}

bool convert(const asco::ColorInfo& color_info,
             const std::vector<unsigned char>& input,
             std::vector<unsigned char>& output, std::size_t width, std::size_t height) {
    const void* y = input.data();
    const void* uv = input.data() + width * height;
#if defined(BENCHMARK_D3D11) || defined(BENCHMARK_D3D11ON12)
    return d3d11_nv12_to_bgra_frame(color_info, width, height,
                                    output.data(), width * 4,
                                    y, width, uv, width);
#elif defined(BENCHMARK_D3D12)
    return d3d12_nv12_to_bgra_frame(color_info, width, height,
                                    output.data(), width * 4,
                                    y, width, uv, width);
#elif defined(BENCHMARK_OPENCL)
    return opencl_nv12_to_bgra_frame(color_info, width, height,
                                     output.data(), width * 4,
                                     y, width, uv, width);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const auto device = create_device();
        print_adapters();

#if defined(BENCHMARK_D3D11) || defined(BENCHMARK_D3D11ON12)
        init_d3d11_colorconv(device.d3d11.Get());
        if (!is_d3d11_colorconv_avail()) {
            throw std::runtime_error("D3D11 color conversion initialization failed");
        }
#elif defined(BENCHMARK_D3D12)
        init_d3d12_colorconv(device.d3d11.Get());
        if (!is_d3d12_colorconv_avail()) {
            throw std::runtime_error("D3D12 color conversion initialization failed");
        }
#elif defined(BENCHMARK_OPENCL)
        init_opencl(device.d3d11.Get());
        if (!is_opencl_avail()) {
            throw std::runtime_error(
                "No OpenCL device with D3D11 sharing support is available");
        }
#endif

        const std::size_t y_bytes = options.width * options.height;
        std::vector<unsigned char> input(y_bytes + y_bytes / 2);
        std::vector<unsigned char> output(y_bytes * 4);
        fill_nv12(input, options.width, options.height);
        const auto color_info = make_color_info();

        for (unsigned int i = 0; i < options.warmup_frames; ++i) {
            if (!convert(color_info, input, output, options.width, options.height)) {
                throw std::runtime_error("Conversion failed during warmup");
            }
        }

        const auto start = Clock::now();
        const auto deadline = start + std::chrono::duration<double>(options.seconds);
        std::uint64_t frames = 0;
        do {
            if (!convert(color_info, input, output, options.width, options.height)) {
                throw std::runtime_error("Conversion failed during measurement");
            }
            ++frames;
        } while (Clock::now() < deadline);

        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        std::uint64_t checksum = 1469598103934665603ull;
        for (const auto value : output) {
            checksum = (checksum ^ value) * 1099511628211ull;
        }
        if (!options.output_path.empty()) {
            std::ofstream file(options.output_path, std::ios::binary);
            file.write(reinterpret_cast<const char *>(output.data()),
                       static_cast<std::streamsize>(output.size()));
            if (!file)
                throw std::runtime_error("Failed to write output file");
        }

        std::cout << "Backend:    " << BENCHMARK_BACKEND_NAME << '\n';
        std::cout << "Adapter:    " << adapter_name(device) << '\n';
#if defined(BENCHMARK_OPENCL)
        std::cout << "OpenCL device: " << opencl_device_name() << '\n';
#endif
#if defined(BENCHMARK_D3D12)
        std::cout << "Transfer:   " << d3d12_colorconv_transfer_mode() << '\n';
#endif
        std::cout << "Resolution: " << options.width << 'x' << options.height << " NV12 -> BGRA\n"
                  << "Frames:     " << frames << '\n'
                  << std::fixed << std::setprecision(3)
                  << "Elapsed:    " << elapsed << " s\n"
                  << "Average:    " << (static_cast<double>(frames) / elapsed) << " FPS\n"
                  << "Frame time: " << (elapsed * 1000.0 / static_cast<double>(frames)) << " ms\n"
                  << "Checksum:   " << checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
