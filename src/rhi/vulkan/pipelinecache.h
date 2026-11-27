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

// a file of the cache per version of the shaders (see Pipeline::InternalSetCacheVersion): <stem>.<version><extension>
// beside where the one file was (pipeline.<16 hex digits>.cache). a pipeline cache's data is all or nothing, a pipeline
// once made stays in it, so one file took every version's kernels a shader was ever edited into and was started over at
// its cap (kPipelineCacheByteCount), which left the version in use cold with the rest: the quick tier's profiles on
// KosmicKrisp, where a version's kernels are 150 to 220 MB, reached it every two or three shader changes. per version, a
// file holds one version's, an edit starts a file and loses nothing, going back to a version finds its file, and what
// goes past the cap is the files least recently written (EvictPipelineCaches)
[[nodiscard]] std::filesystem::path VersionedPipelineCachePath(const std::filesystem::path& cacheFilePath, std::string_view version);
// the versions' files beside cacheFilePath, the least recently written removed until the rest are byteCount or less in
// all (never keep, the one just saved); returns what is left, in bytes
uint64_t EvictPipelineCaches(const std::filesystem::path& cacheFilePath, uint64_t byteCount, const std::filesystem::path& keep);
// how much data the cache holds
[[nodiscard]] size_t PipelineCacheByteCount(DeviceHandle<kVk> device, PipelineCacheHandle<kVk> pipelineCache);

[[nodiscard]] PipelineCacheHandle<kVk> LoadPipelineCache(const std::filesystem::path& cacheFilePath, Device<kVk>& device, const Instance<kVk>& instance);
[[nodiscard]] std::expected<core::file::Record, std::error_code> SavePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<kVk> device,
	PhysicalDeviceProperties<kVk> physicalDeviceProperties,
	PipelineCacheHandle<kVk> pipelineCache);

} // namespace rhi::pipeline
