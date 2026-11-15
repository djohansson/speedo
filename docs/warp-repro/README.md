# WARP 1.0.18+ device removal: a resource load's result read after a loop with two exits

A standalone bug report bundle for Microsoft (not part of the build: speedo works around it, see docs/backends-plan.md, "WARP from Microsoft's NuGet package", and gfx/shadercompiler.h's kDxcOptions).

repro.hlsl, compiled with DXC 1.9 (`dxc -T cs_6_8 -E main repro.hlsl -Fo repro.dxil`), dispatched once by
warpharness.cpp (a root signature from the shader's reflection: its cbuffer root constants, the rest in descriptor
tables of null views of each binding's type; one Dispatch(1,1,1)), WARP's d3d10warp.dll beside the executable.

| d3d10warp.dll | repro |
|---|---|
| Windows 11 26300's (10.0.26100.9818) | ok |
| NuGet 1.0.13, 1.0.14.2, 1.0.15, 1.0.16.1, 1.0.17 | ok |
| NuGet 1.0.18, 1.0.19, 1.0.20, 1.0.21, 1.0.22, 1.65535.22-preview | device removed (DXGI_ERROR_DEVICE_REMOVED) at the dispatch |

The DXIL: the loop's header exits to the return's block, its body to the store's, which extracts the rawBufferLoad's
components after the loop (DXC sinks the extractvalues to their use). The debug layer reports nothing; WARP prints
"D3D12: Removing Device." from ExecuteCommandLists.

Build: `clang-cl /std:c++20 /O2 /EHsc /I <dxc include> warpharness.cpp /link d3d12.lib dxgi.lib`, dxcompiler.dll and
dxil.dll beside it (the reflection).
