cmake_minimum_required(VERSION 3.21)

project(dx_cl_compare LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This benchmark requires Windows (D3D11).")
endif()

set(BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
set(OPENCL_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencl")

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

add_executable(compare_d3d11_r8 src/main.cpp "${BACKEND_DIR}/cc_d3d11_r8.cpp")
configure_benchmark(compare_d3d11_r8 "D3D11 R8 (00dde8a)" BENCHMARK_D3D11)
target_link_libraries(compare_d3d11_r8 PRIVATE d3dcompiler)

add_executable(compare_d3d11_r32 src/main.cpp "${BACKEND_DIR}/cc_d3d11_r32.cpp")
configure_benchmark(compare_d3d11_r32 "D3D11 R32 (31fd0d9)" BENCHMARK_D3D11)
target_link_libraries(compare_d3d11_r32 PRIVATE d3dcompiler)

add_executable(compare_opencl src/main.cpp "${BACKEND_DIR}/cc_opencl.cpp")
configure_benchmark(compare_opencl "OpenCL" BENCHMARK_OPENCL)
target_include_directories(compare_opencl PRIVATE "${OPENCL_SDK}/include")
target_link_libraries(compare_opencl PRIVATE "${OPENCL_SDK}/lib/OpenCL.lib")
add_custom_command(TARGET compare_opencl POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENCL_SDK}/bin/OpenCL.dll"
        "$<TARGET_FILE_DIR:compare_opencl>/OpenCL.dll")
