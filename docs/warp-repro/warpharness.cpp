// one dispatch of a DXIL compute shader on WARP (the d3d10warp.dll beside this executable, or the system's): a root
// signature made from the shader's reflection (its cbuffers root constants, everything else in tables), every slot a
// null view of its binding's type, then whether the device survives the dispatch
// usage: warpharness <file.dxil> [debug]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <d3d12shader.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define CHECK(x)                                                                                                      \
	do                                                                                                                \
	{                                                                                                                 \
		HRESULT hr_ = (x);                                                                                            \
		if (FAILED(hr_))                                                                                              \
		{                                                                                                             \
			std::printf("FAILED %s: 0x%08lx\n", #x, static_cast<unsigned long>(hr_));                                \
			return 3;                                                                                                 \
		}                                                                                                             \
	} while (0)

struct Binding
{
	D3D12_SHADER_INPUT_BIND_DESC desc;
	UINT constants = 0; // a cbuffer's 32 bit values
};

static DXGI_FORMAT FormatOf(D3D_RESOURCE_RETURN_TYPE type)
{
	switch (type)
	{
	case D3D_RETURN_TYPE_UINT: return DXGI_FORMAT_R32_UINT;
	case D3D_RETURN_TYPE_SINT: return DXGI_FORMAT_R32_SINT;
	default: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: warpharness <file.dxil> [debug]\n");
		return 2;
	}
	std::ifstream file(argv[1], std::ios::binary);
	std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (code.empty())
	{
		std::printf("no %s\n", argv[1]);
		return 2;
	}
	const bool debug = argc > 2 && std::string(argv[2]) == "debug";

	HMODULE dxcModule = LoadLibraryA("dxcompiler.dll");
	if (!dxcModule)
	{
		std::printf("no dxcompiler.dll\n");
		return 2;
	}
	auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance"));
	IDxcUtils* utils = nullptr;
	CHECK(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
	DxcBuffer buffer{code.data(), code.size(), 0};
	ID3D12ShaderReflection* reflection = nullptr;
	CHECK(utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection)));
	D3D12_SHADER_DESC shaderDesc{};
	CHECK(reflection->GetDesc(&shaderDesc));

	std::vector<Binding> bindings;
	for (UINT it = 0; it < shaderDesc.BoundResources; it++)
	{
		Binding binding{};
		CHECK(reflection->GetResourceBindingDesc(it, &binding.desc));
		if (binding.desc.Type == D3D_SIT_CBUFFER)
		{
			D3D12_SHADER_BUFFER_DESC bufferDesc{};
			CHECK(reflection->GetConstantBufferByName(binding.desc.Name)->GetDesc(&bufferDesc));
			binding.constants = (bufferDesc.Size + 3) / 4;
		}
		bindings.push_back(binding);
	}

	if (debug)
	{
		ID3D12Debug* debugLayer = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugLayer))))
			debugLayer->EnableDebugLayer();
	}
	IDXGIFactory4* factory = nullptr;
	CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
	IDXGIAdapter* adapter = nullptr;
	CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
	ID3D12Device* device = nullptr;
	CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
	char warpPath[MAX_PATH] = "";
	GetModuleFileNameA(GetModuleHandleA("d3d10warp.dll"), warpPath, MAX_PATH);

	// the root signature: a cbuffer's root constants, the rest in two tables (resources, samplers)
	std::vector<D3D12_ROOT_PARAMETER1> parameters;
	std::vector<D3D12_DESCRIPTOR_RANGE1> resources;
	std::vector<D3D12_DESCRIPTOR_RANGE1> samplers;
	UINT resourceCount = 0;
	UINT samplerCount = 0;
	for (const auto& binding : bindings)
	{
		const auto& d = binding.desc;
		const UINT count = d.BindCount == 0 ? 16 : d.BindCount;
		const D3D12_DESCRIPTOR_RANGE_FLAGS flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
		switch (d.Type)
		{
		case D3D_SIT_CBUFFER:
			parameters.push_back(D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {.ShaderRegister = d.BindPoint, .RegisterSpace = d.Space, .Num32BitValues = binding.constants},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
			break;
		case D3D_SIT_SAMPLER:
			samplers.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, count, d.BindPoint, d.Space, flags, samplerCount});
			samplerCount += count;
			break;
		case D3D_SIT_UAV_RWTYPED:
		case D3D_SIT_UAV_RWSTRUCTURED:
		case D3D_SIT_UAV_RWBYTEADDRESS:
			resources.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, count, d.BindPoint, d.Space, flags | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, resourceCount});
			resourceCount += count;
			break;
		default:
			resources.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, count, d.BindPoint, d.Space, flags | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, resourceCount});
			resourceCount += count;
			break;
		}
	}
	const UINT resourceTable = static_cast<UINT>(parameters.size());
	if (!resources.empty())
		parameters.push_back({.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = {static_cast<UINT>(resources.size()), resources.data()}, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
	const UINT samplerTable = static_cast<UINT>(parameters.size());
	if (!samplers.empty())
		parameters.push_back({.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = {static_cast<UINT>(samplers.size()), samplers.data()}, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{.Version = D3D_ROOT_SIGNATURE_VERSION_1_1, .Desc_1_1 = {static_cast<UINT>(parameters.size()), parameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}};
	ID3DBlob* blob = nullptr;
	ID3DBlob* error = nullptr;
	if (FAILED(D3D12SerializeVersionedRootSignature(&rootDesc, &blob, &error)))
	{
		std::printf("root signature: %s\n", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
		return 3;
	}
	ID3D12RootSignature* rootSignature = nullptr;
	CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));

	auto start = std::chrono::steady_clock::now();
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{.pRootSignature = rootSignature, .CS = {code.data(), code.size()}};
	ID3D12PipelineState* pso = nullptr;
	CHECK(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	const double createSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	// every slot a null view of its binding's type
	ID3D12DescriptorHeap* resourceHeap = nullptr;
	ID3D12DescriptorHeap* samplerHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resourceCount ? resourceCount : 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
	CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&resourceHeap)));
	heapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, samplerCount ? samplerCount : 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
	CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&samplerHeap)));
	const UINT resourceIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const UINT samplerIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	UINT resourceIt = 0;
	UINT samplerIt = 0;
	for (const auto& binding : bindings)
	{
		const auto& d = binding.desc;
		const UINT count = d.BindCount == 0 ? 16 : d.BindCount;
		for (UINT element = 0; element < count; element++)
		{
			if (d.Type == D3D_SIT_CBUFFER)
				break;
			if (d.Type == D3D_SIT_SAMPLER)
			{
				D3D12_SAMPLER_DESC sampler{.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .MaxLOD = D3D12_FLOAT32_MAX};
				device->CreateSampler(&sampler, {samplerHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{samplerIt++} * samplerIncrement});
				continue;
			}
			D3D12_CPU_DESCRIPTOR_HANDLE dst{resourceHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{resourceIt++} * resourceIncrement};
			const bool uav = d.Type == D3D_SIT_UAV_RWTYPED || d.Type == D3D_SIT_UAV_RWSTRUCTURED || d.Type == D3D_SIT_UAV_RWBYTEADDRESS;
			if (uav)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
				if (d.Type == D3D_SIT_UAV_RWSTRUCTURED)
					view = {.Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = {.NumElements = 1, .StructureByteStride = d.NumSamples ? d.NumSamples : 4}};
				else if (d.Type == D3D_SIT_UAV_RWBYTEADDRESS)
					view = {.Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = {.NumElements = 1, .Flags = D3D12_BUFFER_UAV_FLAG_RAW}};
				else if (d.Dimension == D3D_SRV_DIMENSION_TEXTURE3D)
					view = {.Format = FormatOf(d.ReturnType), .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D, .Texture3D = {.WSize = 1}};
				else if (d.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY)
					view = {.Format = FormatOf(d.ReturnType), .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY, .Texture2DArray = {.ArraySize = 1}};
				else
					view = {.Format = FormatOf(d.ReturnType), .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
				device->CreateUnorderedAccessView(nullptr, nullptr, &view, dst);
				continue;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC view{.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
			if (d.Type == D3D_SIT_STRUCTURED)
			{
				view.Format = DXGI_FORMAT_UNKNOWN;
				view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				view.Buffer = {.NumElements = 1, .StructureByteStride = d.NumSamples ? d.NumSamples : 4};
			}
			else if (d.Type == D3D_SIT_BYTEADDRESS)
			{
				view.Format = DXGI_FORMAT_R32_TYPELESS;
				view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				view.Buffer = {.NumElements = 1, .Flags = D3D12_BUFFER_SRV_FLAG_RAW};
			}
			else if (d.Type == D3D_SIT_RTACCELERATIONSTRUCTURE)
			{
				view.Format = DXGI_FORMAT_UNKNOWN;
				view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			}
			else
			{
				view.Format = FormatOf(d.ReturnType);
				switch (d.Dimension)
				{
				case D3D_SRV_DIMENSION_TEXTURE3D: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; view.Texture3D.MipLevels = 1; break;
				case D3D_SRV_DIMENSION_TEXTURECUBE: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; view.TextureCube.MipLevels = 1; break;
				case D3D_SRV_DIMENSION_TEXTURE2DARRAY: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray = {.MipLevels = 1, .ArraySize = 1}; break;
				case D3D_SRV_DIMENSION_BUFFER: view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; view.Buffer.NumElements = 1; break;
				default: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1; break;
				}
			}
			device->CreateShaderResourceView(nullptr, &view, dst);
		}
	}

	ID3D12CommandQueue* queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
	CHECK(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
	ID3D12CommandAllocator* allocator = nullptr;
	CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
	ID3D12GraphicsCommandList* list = nullptr;
	CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, IID_PPV_ARGS(&list)));
	ID3D12DescriptorHeap* heaps[] = {resourceHeap, samplerHeap};
	list->SetDescriptorHeaps(2, heaps);
	list->SetComputeRootSignature(rootSignature);
	if (!resources.empty())
		list->SetComputeRootDescriptorTable(resourceTable, resourceHeap->GetGPUDescriptorHandleForHeapStart());
	if (!samplers.empty())
		list->SetComputeRootDescriptorTable(samplerTable, samplerHeap->GetGPUDescriptorHandleForHeapStart());
	list->Dispatch(1, 1, 1);
	CHECK(list->Close());
	ID3D12CommandList* lists[] = {list};
	ID3D12Fence* fence = nullptr;
	CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	start = std::chrono::steady_clock::now();
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence, 1);
	HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, event);
	const DWORD wait = WaitForSingleObject(event, 120000);
	const double runSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const HRESULT removed = device->GetDeviceRemovedReason();
	std::printf(
		"%s: %u bindings, pipeline %.2f s, dispatch %.2f s%s, device %s (0x%08lx), warp %s\n",
		argv[1],
		static_cast<unsigned>(bindings.size()),
		createSeconds,
		runSeconds,
		wait == WAIT_TIMEOUT ? " (timed out)" : "",
		removed == S_OK ? "ok" : "REMOVED",
		static_cast<unsigned long>(removed),
		warpPath);
	return removed == S_OK ? 0 : 1;
}
