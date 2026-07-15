#include "backend_api.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kWidth = 1920;
constexpr std::size_t kHeight = 1080;
constexpr std::size_t kYBytes = kWidth * kHeight;
constexpr std::size_t kUvBytes = kWidth * kHeight / 2;
constexpr std::size_t kRgbBytes = kWidth * kHeight * 4;

struct Options {
    double seconds = 5.0;
    unsigned int warmup_frames = 20;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--seconds" && i + 1 < argc) {
            options.seconds = std::stod(argv[++i]);
        } else if (argument == "--warmup" && i + 1 < argc) {
            options.warmup_frames = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (argument == "--help") {
            std::cout << "Usage: " << argv[0]
                      << " [--seconds N] [--warmup N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown or incomplete argument: " + argument);
        }
    }
    if (options.seconds <= 0.0) {
        throw std::invalid_argument("--seconds must be greater than zero");
    }
    return options;
}

ComPtr<ID3D11Device> create_device() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &feature_level, &context);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 1,
            D3D11_SDK_VERSION, &device, &feature_level, &context);
    }
    if (FAILED(result)) {
        throw std::runtime_error("D3D11 hardware device creation failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)) + ")");
    }
    return device;
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

void fill_nv12(std::vector<unsigned char>& input) {
    for (std::size_t y = 0; y < kHeight; ++y) {
        for (std::size_t x = 0; x < kWidth; ++x) {
            input[y * kWidth + x] = static_cast<unsigned char>(16 + ((x + y) % 220));
        }
    }
    auto* uv = input.data() + kYBytes;
    for (std::size_t y = 0; y < kHeight / 2; ++y) {
        for (std::size_t x = 0; x < kWidth; x += 2) {
            uv[y * kWidth + x] = static_cast<unsigned char>(96 + ((x / 2) % 64));
            uv[y * kWidth + x + 1] = static_cast<unsigned char>(96 + (y % 64));
        }
    }
}

bool convert(const asco::ColorInfo& color_info,
             const std::vector<unsigned char>& input,
             std::vector<unsigned char>& output) {
    const void* y = input.data();
    const void* uv = input.data() + kYBytes;
#if defined(BENCHMARK_D3D11)
    return d3d11_nv12_to_bgra_frame(color_info, kWidth, kHeight,
                                    output.data(), kWidth * 4,
                                    y, kWidth, uv, kWidth);
#else
    return opencl_nv12_to_bgra_frame(color_info, kWidth, kHeight,
                                     output.data(), kWidth * 4,
                                     y, kWidth, uv, kWidth);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const auto device = create_device();

#if defined(BENCHMARK_D3D11)
        init_d3d11_colorconv(device.Get());
        if (!is_d3d11_colorconv_avail()) {
            throw std::runtime_error("D3D11 color conversion initialization failed");
        }
#else
        init_opencl(device.Get());
        if (!is_opencl_avail()) {
            throw std::runtime_error(
                "No OpenCL device with D3D11 sharing support is available");
        }
#endif

        std::vector<unsigned char> input(kYBytes + kUvBytes);
        std::vector<unsigned char> output(kRgbBytes);
        fill_nv12(input);
        const auto color_info = make_color_info();

        for (unsigned int i = 0; i < options.warmup_frames; ++i) {
            if (!convert(color_info, input, output)) {
                throw std::runtime_error("Conversion failed during warmup");
            }
        }

        const auto start = Clock::now();
        const auto deadline = start + std::chrono::duration<double>(options.seconds);
        std::uint64_t frames = 0;
        do {
            if (!convert(color_info, input, output)) {
                throw std::runtime_error("Conversion failed during measurement");
            }
            ++frames;
        } while (Clock::now() < deadline);

        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < output.size(); i += 4096) {
            checksum = checksum * 131 + output[i];
        }

        std::cout << "Backend:    " << BENCHMARK_BACKEND_NAME << '\n'
                  << "Resolution: " << kWidth << 'x' << kHeight << " NV12 -> BGRA\n"
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

