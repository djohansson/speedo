// the D3D12 backend's instance: DXGI's factory, and the adapters as physical devices (each an IDXGIAdapter4 it holds), the
// highest performance first; WARP, Microsoft's software adapter, alone where SPEEDO_D3D12_WARP=1 (lavapipe's role on
// Windows: CI's runners have no gpu). D3D12's debug layer is enabled before any device is made, where validating: a
// debug build (SPEEDO_GRAPHICS_VALIDATION_LEVEL), or SPEEDO_D3D12_VALIDATION=1 (assettest's -Validate on D3D12)
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
InstanceCreateDesc<kD3D12>::InstanceCreateDesc(std::string&& applicationName, std::string&& engineName, bool presentation) noexcept
	: applicationName(std::forward<std::string>(applicationName))
	, engineName(std::forward<std::string>(engineName))
	, presentation(presentation)
{}

template <>
InstanceCreateDesc<kD3D12>::InstanceCreateDesc(InstanceCreateDesc&& other) noexcept
	: applicationName(std::move(other.applicationName))
	, engineName(std::move(other.engineName))
	, presentation(other.presentation)
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
	d3d12::Check(
		CreateDXGIFactory2(object->validation ? DXGI_CREATE_FACTORY_DEBUG : 0U, IID_PPV_ARGS(&object->factory)), "CreateDXGIFactory2");
	myInstance = object;

	const bool warp = instance::EnvironmentIs("SPEEDO_D3D12_WARP", "1");
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
		delete myInstance;
	}
}

} // namespace rhi
