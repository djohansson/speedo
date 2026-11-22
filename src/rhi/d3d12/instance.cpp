// the D3D12 backend's instance: DXGI's factory, and the adapters as physical devices (each an IDXGIAdapter4 it holds), the
// highest performance first; WARP, Microsoft's software adapter, alone where SPEEDO_D3D12_WARP is set (1, or a
// d3d10warp.dll's path: lavapipe's role on Windows, CI's runners have no gpu). D3D12's debug layer is enabled before
// any device is made, where validating: a debug build (SPEEDO_GRAPHICS_VALIDATION_LEVEL), or SPEEDO_D3D12_VALIDATION=1
// (assettest's -Validate on D3D12)
#include <rhi/d3d12/backend.h>

#include <rhi/instance.h>

#include <core/assert.h>

#include <cstdlib>
#include <memory>
#include <print>
#include <string_view>
#include <utility>

namespace rhi
{

namespace instance
{

static bool EnvironmentIs(const char* name, std::string_view value)
{
	const char* setting = std::getenv(name);
	return setting != nullptr && std::string_view(setting) == value;
}

} // namespace instance

template <>
InstanceCreateDesc<kD3D12>::InstanceCreateDesc(std::string&& applicationName, std::string&& engineName, bool presentation, std::string&& driverCachePath) noexcept
	: applicationName(std::forward<std::string>(applicationName))
	, engineName(std::forward<std::string>(engineName))
	, presentation(presentation)
	, driverCachePath(std::forward<std::string>(driverCachePath))
{}

template <>
InstanceCreateDesc<kD3D12>::InstanceCreateDesc(InstanceCreateDesc&& other) noexcept
	: applicationName(std::move(other.applicationName))
	, engineName(std::move(other.engineName))
	, presentation(other.presentation)
	, driverCachePath(std::move(other.driverCachePath))
{}

template <>
Instance<kD3D12>::Instance(InstanceCreateDesc<kD3D12>&& desc)
	: myConfig(std::forward<InstanceCreateDesc<kD3D12>>(desc))
{
	auto* object = new d3d12::InstanceObject{};
	object->validation = SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0 || instance::EnvironmentIs("SPEEDO_D3D12_VALIDATION", "1");
	if (object->validation)
	{
		ID3D12Debug1* debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			// the gpu's side too where asked (much slower): SPEEDO_D3D12_GPU_VALIDATION=1
			if (instance::EnvironmentIs("SPEEDO_D3D12_GPU_VALIDATION", "1"))
				debug->SetEnableGPUBasedValidation(TRUE);
			d3d12::Release(debug);
			std::println("D3D12: the debug layer is on");
		}
		else
		{
			std::println(stderr, "D3D12: validation asked for, but the debug layer isn't installed (Graphics Tools, an optional feature of Windows)");
			object->validation = false;
		}
	}
	// the device removed reports' breadcrumbs and page faults (crashreport.cpp), set before any device is made
	d3d12::EnableDeviceRemovedReports();
	d3d12::Check(
		CreateDXGIFactory2(object->validation ? DXGI_CREATE_FACTORY_DEBUG : 0U, IID_PPV_ARGS(&object->factory)), "CreateDXGIFactory2");
	myInstance = object;

	// WARP: Windows' own (SPEEDO_D3D12_WARP=1), or a d3d10warp.dll of Microsoft's NuGet package's (its path, see
	// scripts/fetch-warp.ps1: newer than Windows', compiling the shaders at a pipeline's creation on every core where
	// Windows' 10.0.26100 compiles each at its first dispatch on one, minutes the client's start). loaded here and kept
	// until the instance goes, so that D3D12, loading d3d10warp.dll by its name, finds it loaded (a module of that name
	// already in the process is the one it gets)
	const char* warpSetting = std::getenv("SPEEDO_D3D12_WARP");
	const std::string_view warpValue = warpSetting != nullptr ? warpSetting : "";
	const bool warp = !warpValue.empty() && warpValue != "0";
	if (warp && warpValue != "1")
	{
		object->warp = LoadLibraryExA(warpSetting, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		ENSUREF(object->warp != nullptr, "SPEEDO_D3D12_WARP={}: not a d3d10warp.dll that loads (error {})", warpValue, GetLastError());
		std::println("D3D12: WARP from {}", warpValue);
	}
	IDXGIAdapter4* adapter = nullptr;
	for (UINT adapterIt = 0;
		 object->factory->EnumAdapterByGpuPreference(adapterIt, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
		 DXGI_ERROR_NOT_FOUND;
		 adapterIt++)
	{
		DXGI_ADAPTER_DESC3 adapterDesc{};
		adapter->GetDesc3(&adapterDesc);
		const bool software = (adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
		// one that can make a device of feature level 12.1 (the rest the device checks itself, failing hard)
		if (software != warp || FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr)))
		{
			d3d12::Release(adapter);
			continue;
		}
		auto* handle = reinterpret_cast<d3d12::PhysicalDeviceObject*>(adapter);
		myPhysicalDevices.push_back(handle);
		myPhysicalDeviceInfos.emplace(handle, std::make_unique<PhysicalDeviceInfo<kD3D12>>());
		adapter = nullptr;
	}
}

template <>
Instance<kD3D12>::~Instance()
{
	for (auto* device : myPhysicalDevices)
		reinterpret_cast<IDXGIAdapter4*>(device)->Release();
	if (myInstance != nullptr)
	{
		d3d12::Release(myInstance->factory);
		if (myInstance->warp != nullptr)
			FreeLibrary(myInstance->warp);
		delete myInstance;
	}
}

} // namespace rhi
