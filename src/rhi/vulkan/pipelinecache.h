#pragma once

#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/pipeline.h>

#include <core/file.h>

#include <expected>
#include <filesystem>
#include <system_error>

// the pipeline cache's file (see Pipeline): loaded when a pipeline is made, if it is the device's, and saved when it is
// destroyed. the backend's own (not part of rhi's api)
namespace rhi::pipeline
{

[[nodiscard]] PipelineCacheHandle<kVk> LoadPipelineCache(const std::filesystem::path& cacheFilePath, Device<kVk>& device, const Instance<kVk>& instance);
[[nodiscard]] std::expected<core::file::Record, std::error_code> SavePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<kVk> device,
	PhysicalDeviceProperties<kVk> physicalDeviceProperties,
	PipelineCacheHandle<kVk> pipelineCache);

} // namespace rhi::pipeline
