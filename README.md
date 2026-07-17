# NV12 颜色转换基准

这是一个仅支持 Windows 的 CMake 项目，用于比较 OpenCL、D3D11、D3D11On12
和 D3D12 的 NV12 到 BGRA 实现。每个可执行文件只包含一个 backend，因此
每次运行只测量对应的实现。

基准测试目标如下：

- `compare_opencl`：使用 `cc_opencl.cpp`
- `compare_d3d11`：使用 D3D11 实现
- `compare_d3d11_dynamic`：使用 `WRITE_DISCARD` 动态上传的 D3D11 实现
- `compare_d3d11on12`：通过 D3D11On12 在 D3D12 设备和命令队列上运行 D3D11 实现
- `compare_d3d12`：原生 D3D12 compute 实现
- `compare_d3d12_upload_copy`：使用 `UPLOAD -> DEFAULT` 输入拷贝的 D3D12 实现

项目中包含所有实现源码、OpenCL headers、x64 OpenCL import library 和 OpenCL
loader DLL。配置、编译和启动过程不依赖原始 `autoscopia` 仓库中的文件。

测量范围包括 CPU 到 GPU 上传、转换、GPU 同步和 GPU 到 CPU 回读，与导出函数的
实际行为一致。

D3D12 backend 会根据可执行文件选择传输路径。当 `D3D12_OPTIONS16` 报告支持时，输入
使用 `D3D12_HEAP_TYPE_GPU_UPLOAD` heap（通常通过 Resizable BAR 访问 CPU 可见的
显存）。在 UMA（统一内存架构，即 CPU 与 GPU 共享物理内存）适配器上，还会使用持久映射的 `CUSTOM` write-back UAV 直接作为
GPU 输出，从而省去额外的回读拷贝。不支持这些组合时，会自动回退到映射的
`UPLOAD` 输入和 `DEFAULT` 到 `READBACK` 输出。实际选择的路径会显示在基准输出
的 `Transfer` 字段中。

D3D12 实现会复用内部 frame slot。输入、参数和 CPU 可读输出资源在 slot 的整个
生命周期内保持映射。compute dispatch 使用 root constants 和 root buffer
descriptors，避免每帧更新 descriptor heap。每次调用仍会在公共转换 API 要求的
外部指针和内部资源之间进行拷贝。

## 编译

在 Visual Studio x64 Developer Command Prompt 中运行，让 CMake 自动选择已安装
的 Visual Studio 版本：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
```

## 运行

```powershell
.\build\Release\compare_opencl.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d11.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d11on12.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d12.exe --seconds 10 --warmup 20
```

D3D12 默认使用 `GPU_UPLOAD` 输入资源。另一条 `UPLOAD -> DEFAULT` 输入路径由
单独的 `compare_d3d12_upload_copy.exe` 提供：

```powershell
.\build\Release\compare_d3d12.exe --seconds 10 --warmup 20
.\build\Release\compare_d3d12_upload_copy.exe --seconds 10 --warmup 20
```

请对所有 backend 使用相同的测试时长，并一次只运行一个进程。默认输入是
1920x1080、BT.709、limited-range 的 NV12 帧；可以通过 `--width` 和 `--height`
选择其他偶数尺寸。每个程序都会报告平均 FPS 和单帧耗时。

程序会打印所有 DXGI adapters 以及本次运行选择的 adapter。测试指定适配器时，
设置 `DX_CL_ADAPTER_VENDOR`，例如 NVIDIA 使用 `0x10de`，Intel 使用 `0x8086`。
也支持 `DX_CL_ADAPTER_INDEX`，但不同 API 路径下的枚举顺序可能变化，因此推荐
使用 VendorId。两个变量都未设置时，使用 Windows 默认硬件适配器。
