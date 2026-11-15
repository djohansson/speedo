// the D3D12 backend's crash reports (docs/backends-plan.md, step 6): DRED, D3D12's device removed extended data, on before
// the device is made (SPEEDO_D3D12_DRED=0 off): its automatic breadcrumbs (each command list's operations, and how far
// the gpu got through them), their contexts (the passes' names, CrashBreadcrumb's events) and the page faults' allocations.
// a call that finds the device removed (a present's, a list's close, ...: d3d12::Check) reads them into the report
// (rhi/crashreport.h's CrashFatal): the lists the gpu hadn't finished, each with its operations around the last it
// finished and the passes they were in, and where it faulted, which allocation was there or had been
#include <rhi/d3d12/backend.h>

#include <rhi/crashreport.h>

#include <core/assert.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace rhi::d3d12
{

namespace crashreport
{

std::atomic<ID3D12Device*> gDevice = nullptr;

std::string_view OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
	switch (op)
	{
#define SPEEDO_D3D12_OP(name) \
	case D3D12_AUTO_BREADCRUMB_OP_##name: return #name;
		SPEEDO_D3D12_OP(SETMARKER)
		SPEEDO_D3D12_OP(BEGINEVENT)
		SPEEDO_D3D12_OP(ENDEVENT)
		SPEEDO_D3D12_OP(DRAWINSTANCED)
		SPEEDO_D3D12_OP(DRAWINDEXEDINSTANCED)
		SPEEDO_D3D12_OP(EXECUTEINDIRECT)
		SPEEDO_D3D12_OP(DISPATCH)
		SPEEDO_D3D12_OP(COPYBUFFERREGION)
		SPEEDO_D3D12_OP(COPYTEXTUREREGION)
		SPEEDO_D3D12_OP(COPYRESOURCE)
		SPEEDO_D3D12_OP(CLEARRENDERTARGETVIEW)
		SPEEDO_D3D12_OP(CLEARUNORDEREDACCESSVIEW)
		SPEEDO_D3D12_OP(CLEARDEPTHSTENCILVIEW)
		SPEEDO_D3D12_OP(RESOURCEBARRIER)
		SPEEDO_D3D12_OP(PRESENT)
		SPEEDO_D3D12_OP(RESOLVEQUERYDATA)
		SPEEDO_D3D12_OP(BEGINSUBMISSION)
		SPEEDO_D3D12_OP(ENDSUBMISSION)
		SPEEDO_D3D12_OP(WRITEBUFFERIMMEDIATE)
		SPEEDO_D3D12_OP(BUILDRAYTRACINGACCELERATIONSTRUCTURE)
		SPEEDO_D3D12_OP(EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO)
		SPEEDO_D3D12_OP(COPYRAYTRACINGACCELERATIONSTRUCTURE)
		SPEEDO_D3D12_OP(SETPIPELINESTATE1)
		SPEEDO_D3D12_OP(BARRIER)
		SPEEDO_D3D12_OP(BEGIN_COMMAND_LIST)
#undef SPEEDO_D3D12_OP
	default: break;
	}
	return "(another)";
}

std::string Narrow(const wchar_t* text)
{
	if (text == nullptr)
		return {};
	const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (count <= 1)
		return {};
	std::string result(static_cast<size_t>(count - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count, nullptr, nullptr);
	return result;
}

std::string NameOf(const char* narrow, const wchar_t* wide)
{
	if (narrow != nullptr && *narrow != '\0')
		return narrow;
	auto name = Narrow(wide);
	return name.empty() ? std::string("(unnamed)") : name;
}

// what DRED kept: the lists the gpu hadn't finished, and the page fault
std::string Report(ID3D12Device* device)
{
	if (device == nullptr)
		return "DRED: no device\n";
	ID3D12DeviceRemovedExtendedData1* dred = nullptr;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
		return "DRED: none (SPEEDO_D3D12_DRED=0, or the runtime has none)\n";

	std::string report;
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
	{
		// each list the gpu began and didn't finish, its operations from a few before the last it finished to a few after
		constexpr uint32_t kOpsAround = 4;
		uint32_t unfinished = 0;
		for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext)
		{
			const uint32_t last = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
			if (last == 0 || last >= node->BreadcrumbCount)
				continue;
			unfinished++;
			report += std::format(
				"DRED: list {} on queue {}: {} of {} operations done\n",
				NameOf(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
				NameOf(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
				last,
				node->BreadcrumbCount);
			// the pass each operation is in: the last event begun before it and not ended (the contexts' strings)
			const uint32_t first = last > kOpsAround ? last - kOpsAround : 0;
			const uint32_t end = std::min(node->BreadcrumbCount, last + kOpsAround);
			for (uint32_t opIt = first; opIt < end; opIt++)
			{
				std::string context;
				for (uint32_t contextIt = 0; contextIt < node->BreadcrumbContextsCount; contextIt++)
					if (node->pBreadcrumbContexts[contextIt].BreadcrumbIndex <= opIt)
						context = Narrow(node->pBreadcrumbContexts[contextIt].pContextString);
				report += std::format(
					"  {} {} {}{}\n",
					opIt < last ? "done   " : (opIt == last ? "FAILED?" : "pending"),
					opIt,
					OpName(node->pCommandHistory[opIt]),
					context.empty() ? std::string() : std::format(" ({})", context));
			}
		}
		if (unfinished == 0)
			report += "DRED: no list was left unfinished\n";
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA != 0)
	{
		report += std::format("DRED: a page fault at {:#x}\n", pageFault.PageFaultVA);
		for (const auto* node = pageFault.pHeadExistingAllocationNode; node != nullptr; node = node->pNext)
			report += std::format("  there: {}\n", NameOf(node->ObjectNameA, node->ObjectNameW));
		for (const auto* node = pageFault.pHeadRecentFreedAllocationNode; node != nullptr; node = node->pNext)
			report += std::format("  freed there: {}\n", NameOf(node->ObjectNameA, node->ObjectNameW));
	}
	dred->Release();
	return report;
}

} // namespace crashreport

void EnableDeviceRemovedReports()
{
	if (const char* value = std::getenv("SPEEDO_D3D12_DRED"); value != nullptr && std::string_view(value) == "0")
		return;
	ID3D12DeviceRemovedExtendedDataSettings1* settings = nullptr;
	if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
	{
		std::println(stderr, "D3D12: no DRED settings (device removed reports will say less)");
		return;
	}
	settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	settings->Release();
}

void SetDeviceRemovedReportDevice(ID3D12Device* device) noexcept
{
	crashreport::gDevice = device;
}

bool IsDeviceRemoved(HRESULT result) noexcept
{
	return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_RESET ||
		   result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void ReportDeviceRemoved(HRESULT result, std::string_view what)
{
	auto* device = crashreport::gDevice.load();
	const HRESULT reason = device != nullptr ? device->GetDeviceRemovedReason() : result;
	CrashFatal(
		std::format("{:#010x} (the device's removed reason {:#010x})", static_cast<uint32_t>(result), static_cast<uint32_t>(reason)),
		std::format("D3D12: {} found the device removed\n{}", what, crashreport::Report(device)));
}

} // namespace rhi::d3d12
