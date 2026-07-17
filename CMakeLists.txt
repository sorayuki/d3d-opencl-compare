cmake_minimum_required(VERSION 3.21)

project(dx_cl_compare LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This benchmark requires Windows (Direct3D).")
endif()

set(BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
set(OPENCL_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencl")

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
    #include <d3d12.h>
    int main() {
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
        (void)options;
        (void)D3D12_FEATURE_D3D12_OPTIONS16;
        (void)D3D12_HEAP_TYPE_GPU_UPLOAD;
        return 0;
    }
]] HAVE_D3D12_GPU_UPLOAD_HEAP)

function(configure_benchmark target backend_name backend_define)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE
        BENCHMARK_BACKEND_NAME="${backend_name}"
        ${backend_define})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE d3d11 dxgi)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_executable(compare_d3d11 src/main.cpp "${BACKEND_DIR}/cc_d3d11.cpp")
configure_benchmark(compare_d3d11 "D3D11 (current colorconv)" BENCHMARK_D3D11)
target_link_libraries(compare_d3d11 PRIVATE d3dcompiler)

add_executable(compare_d3d11_dynamic
    src/main.cpp "${BACKEND_DIR}/cc_d3d11.cpp")
configure_benchmark(compare_d3d11_dynamic
    "D3D11 (dynamic WRITE_DISCARD upload)" BENCHMARK_D3D11)
target_compile_definitions(compare_d3d11_dynamic PRIVATE
    D3D11_DYNAMIC_INPUT_UPLOAD=1)
target_link_libraries(compare_d3d11_dynamic PRIVATE d3dcompiler)

add_executable(compare_d3d11on12 src/main.cpp "${BACKEND_DIR}/cc_d3d11.cpp")
configure_benchmark(compare_d3d11on12 "D3D11On12 (current colorconv)" BENCHMARK_D3D11ON12)
target_link_libraries(compare_d3d11on12 PRIVATE d3d12 d3dcompiler)

add_executable(compare_d3d12 src/main.cpp "${BACKEND_DIR}/cc_d3d12.cpp")
configure_benchmark(compare_d3d12 "D3D12" BENCHMARK_D3D12)
target_link_libraries(compare_d3d12 PRIVATE d3d12 d3dcompiler)
if(HAVE_D3D12_GPU_UPLOAD_HEAP)
    target_compile_definitions(compare_d3d12 PRIVATE HAVE_D3D12_GPU_UPLOAD_HEAP=1)
endif()

add_executable(compare_d3d12_upload_copy src/main.cpp "${BACKEND_DIR}/cc_d3d12.cpp")
configure_benchmark(compare_d3d12_upload_copy "D3D12 (UPLOAD -> DEFAULT)" BENCHMARK_D3D12)
target_compile_definitions(compare_d3d12_upload_copy PRIVATE D3D12_FORCE_UPLOAD_COPY=1)
target_link_libraries(compare_d3d12_upload_copy PRIVATE d3d12 d3dcompiler)
if(HAVE_D3D12_GPU_UPLOAD_HEAP)
    target_compile_definitions(compare_d3d12_upload_copy PRIVATE HAVE_D3D12_GPU_UPLOAD_HEAP=1)
endif()

add_executable(compare_opencl src/main.cpp "${BACKEND_DIR}/cc_opencl.cpp")
configure_benchmark(compare_opencl "OpenCL" BENCHMARK_OPENCL)
target_include_directories(compare_opencl PRIVATE "${OPENCL_SDK}/include")
string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" TARGET_PLATFORM)
string(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" TARGET_PROCESSOR)
if(TARGET_PLATFORM STREQUAL "ARM64" OR TARGET_PROCESSOR MATCHES "^(ARM64|AARCH64)$")
    target_link_libraries(compare_opencl PRIVATE "${OPENCL_SDK}/lib/arm64/OpenCL.lib")
else()
    target_link_libraries(compare_opencl PRIVATE "${OPENCL_SDK}/lib/OpenCL.lib")
    add_custom_command(TARGET compare_opencl POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${OPENCL_SDK}/bin/OpenCL.dll"
            "$<TARGET_FILE_DIR:compare_opencl>/OpenCL.dll")
endif()
