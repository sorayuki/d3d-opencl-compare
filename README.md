# NV12 color conversion benchmark

This Windows-only CMake project compares NV12-to-BGRA implementations using
OpenCL, D3D11, and D3D12. Each executable contains
exactly one backend, so a run measures only that implementation.

The benchmark targets are:

- `compare_opencl`: copied `cc_opencl.cpp`
- `compare_d3d11_r8`: copied `cc_d3d11.cpp` from `00dde8a`
- `compare_d3d12`: native D3D12 compute implementation matching the D3D11 R8 path

All implementation sources, OpenCL headers, the x64 OpenCL import library, and
the OpenCL loader DLL are included in this project. Configuration, compilation,
and startup do not read files from the original `autoscopia` repository.

The measured call includes CPU-to-GPU upload, conversion, GPU synchronization,
and GPU-to-CPU readback, matching the behavior of the exported functions.

The D3D12 backend selects its transfer path at runtime. It uses a
`D3D12_HEAP_TYPE_GPU_UPLOAD` input heap when `D3D12_OPTIONS16` reports support
(typically CPU-visible VRAM through Resizable BAR). On UMA adapters it also
uses a mapped `CUSTOM` write-back UAV for direct GPU output, eliminating the
extra readback copy. Unsupported combinations automatically fall back to a
mapped `UPLOAD` input and `DEFAULT`-to-`READBACK` output. The selected path is
shown as `Transfer` in the benchmark output.

## Build

Run from a Visual Studio x64 developer command prompt. Let CMake select the
installed Visual Studio version:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
```

## Run

```powershell
.\build\Release\compare_opencl.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d11_r8.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d12.exe --seconds 10 --warmup 20
```

Use the same duration and run one process at a time. The default input is a
1920x1080 BT.709 limited-range NV12 frame; `--width` and `--height` select a
different even-sized frame. Each program reports average FPS and frame time.
