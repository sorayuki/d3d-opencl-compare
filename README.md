# NV12 color conversion benchmark

This Windows-only CMake project compares the exported NV12-to-BGRA functions
from three `autoscopia/colorconv` implementations. Each executable contains
exactly one backend, so a run measures only that implementation.

The default revisions are:

- `compare_opencl`: copied `cc_opencl.cpp`
- `compare_d3d11_r8`: copied `cc_d3d11.cpp` from `00dde8a`
- `compare_d3d11_r32`: copied `cc_d3d11.cpp` from `31fd0d9`

All implementation sources, OpenCL headers, the x64 OpenCL import library, and
the OpenCL loader DLL are included in this project. Configuration, compilation,
and startup do not read files from the original `autoscopia` repository.

The measured call includes CPU-to-GPU upload, conversion, GPU synchronization,
and GPU-to-CPU readback, matching the behavior of the exported functions.

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
.\build\Release\compare_d3d11_r32.exe --seconds 10 --warmup 20
```

Use the same duration and run one process at a time. The program always uses a
1920x1080 BT.709 limited-range NV12 frame and reports average FPS and frame time.
