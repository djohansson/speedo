#include "rayquerywatch.h"

#include <gfx/shaders/capi.h>

#include <core/assert.h>
#include <core/file.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <string_view>
#include <vector>

namespace gfx
{

namespace
{

constexpr size_t kWordCount = RAY_QUERY_WATCH_HEADER_COUNT + (static_cast<size_t>(RAY_QUERY_WATCH_SLOT_COUNT) * RAY_QUERY_WATCH_SLOT_WORD_COUNT);

// a slot's words (see RAY_QUERY_WATCH_SLOT_WORD_COUNT), as RayQueryWatchBegin writes them
constexpr size_t kSlotState = 0;
constexpr size_t kSlotFrame = 1;
constexpr size_t kSlotCandidates = 2;
constexpr size_t kSlotOrigin = 3;
constexpr size_t kSlotDirection = 6;
constexpr size_t kSlotTmax = 9;
constexpr size_t kSlotDispatch = 10;
constexpr size_t kSlotStage = 11;
static_assert(kSlotStage + 1 == RAY_QUERY_WATCH_SLOT_WORD_COUNT);
constexpr uint32_t kLowByte = 0xFFU;
constexpr uint32_t kByteBits = 8;
constexpr uint32_t kAnyHitBit = 1U << kByteBits;
constexpr uint32_t kMovingBit = 2U << kByteBits;

// the watches made, which name their sections (a scene's each: the window's and the file browser's preview)
std::atomic_uint32_t gWatchCount{0};

// the watch's words as they are now (volatile reads: the gpu writes them, and, read after a hang, may still)
std::vector<uint32_t> Snapshot(std::span<const std::byte> memory)
{
	const auto* words = reinterpret_cast<const volatile uint32_t*>(memory.data());
	std::vector<uint32_t> snapshot(std::min(kWordCount, memory.size() / sizeof(uint32_t)));
	for (size_t wordIt = 0; wordIt < snapshot.size(); wordIt++)
		snapshot[wordIt] = words[wordIt];
	return snapshot;
}

// a float of the watch's words as json (which has no nan or infinity)
std::string Float(uint32_t bits)
{
	auto value = std::bit_cast<float>(bits);
	return std::isfinite(value) ? std::format("{}", value) : std::format("\"{}\"", value);
}

std::string_view StageName(uint32_t stage)
{
	switch (stage)
	{
	case RAY_QUERY_WATCH_STAGE_BEGUN: return "begun";
	case RAY_QUERY_WATCH_STAGE_TRACED: return "traced";
	case RAY_QUERY_WATCH_STAGE_CANDIDATES: return "candidates";
	default: return "?";
	}
}

// a ray to replay's numbers on a line (see RayQueryReplayData)
constexpr size_t kReplayNumberCount = 9;
constexpr uint32_t kDefaultReplayRepeatCount = 64;

// the rays to replay, as gRayQueryReplay holds them (see RayQueryReplayData), read from the file
std::vector<glm::vec4> LoadReplay(const char* path)
{
	auto file = core::file::Map(path);
	ENSUREF(file.has_value(), "SPEEDO_RAY_QUERY_REPLAY: {} can't be read ({})", path, file.error().message());
	std::string_view text(reinterpret_cast<const char*>(file->data()), file->size());
	std::vector<glm::vec4> data{glm::vec4{0.0F}};
	uint32_t count = 0;
	size_t lineIt = 0;
	while (!text.empty())
	{
		lineIt++;
		auto end = text.find('\n');
		auto line = text.substr(0, end);
		text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
		line = line.substr(0, line.find('#'));
		std::array<float, kReplayNumberCount> numbers{};
		size_t numberIt = 0;
		const char* first = line.data();
		const char* last = line.data() + line.size();
		while (numberIt < kReplayNumberCount)
		{
			while (first < last && (*first == ' ' || *first == '\t' || *first == ',' || *first == '\r'))
				first++;
			if (first == last)
				break;
			auto [next, error] = std::from_chars(first, last, numbers[numberIt]);
			ENSUREF(error == std::errc{}, "SPEEDO_RAY_QUERY_REPLAY: {} line {}: not a number", path, lineIt);
			first = next;
			numberIt++;
		}
		if (numberIt == 0)
			continue;
		ENSUREF(numberIt == kReplayNumberCount, "SPEEDO_RAY_QUERY_REPLAY: {} line {}: {} numbers, not {}", path, lineIt, numberIt, kReplayNumberCount);
		uint32_t flags = (numbers[7] != 0.0F ? 1U : 0U) | (numbers[8] != 0.0F ? 2U : 0U); //NOLINT(readability-magic-numbers)
		data.emplace_back(numbers[0], numbers[1], numbers[2], numbers[6]); //NOLINT(readability-magic-numbers)
		data.emplace_back(numbers[3], numbers[4], numbers[5], std::bit_cast<float>(flags)); //NOLINT(readability-magic-numbers)
		count++;
	}
	data[0].x = std::bit_cast<float>(count);
	std::println("ray query replay: {} rays from {}, each {} times a frame", count, path, RayQueryReplayRepeatCount());
	return data;
}

} // namespace

bool RayQueryWatchOn()
{
	static const bool kOn = []
	{
		const char* watch = std::getenv("SPEEDO_RAY_QUERY_WATCH");
		return (watch != nullptr && *watch != '\0' && std::string_view(watch) != "0") || RayQueryReplayRayCount() > 0;
	}();
	return kOn;
}

const std::vector<glm::vec4>& RayQueryReplayData()
{
	static const std::vector<glm::vec4> kData = []
	{
		const char* path = std::getenv("SPEEDO_RAY_QUERY_REPLAY");
		return path != nullptr && *path != '\0' ? LoadReplay(path) : std::vector<glm::vec4>{};
	}();
	return kData;
}

uint32_t RayQueryReplayRayCount()
{
	const auto& data = RayQueryReplayData();
	return data.empty() ? 0 : std::bit_cast<uint32_t>(data[0].x);
}

uint32_t RayQueryReplayRepeatCount()
{
	static const uint32_t kRepeatCount = []
	{
		const char* repeat = std::getenv("SPEEDO_RAY_QUERY_REPLAY_REPEAT");
		return repeat != nullptr && *repeat != '\0' ? static_cast<uint32_t>(std::strtoul(repeat, nullptr, 10)) : kDefaultReplayRepeatCount; //NOLINT(readability-magic-numbers)
	}();
	return kRepeatCount;
}

RayQueryWatch::RayQueryWatch(Device& device)
	: myBuffer(BufferCreateDesc{
		  device.CreateDeviceObjectCreateDesc("Ray Query Watch"),
		  kWordCount * sizeof(uint32_t),
		  rhi::BufferUsage::kStorage,
		  rhi::MemoryProperty::kHostVisible | rhi::MemoryProperty::kHostCoherent})
	, myMemory(myBuffer.Map())
	, mySection(std::format("rayQueryWatch{}", gWatchCount.fetch_add(1, std::memory_order_relaxed)))
	, myReplayBuffer(BufferCreateDesc{
		  device.CreateDeviceObjectCreateDesc("Ray Query Replay"),
		  std::max<size_t>(RayQueryReplayData().size(), 1) * sizeof(glm::vec4),
		  rhi::BufferUsage::kStorage,
		  rhi::MemoryProperty::kHostVisible | rhi::MemoryProperty::kHostCoherent})
{
	std::memset(myMemory.data(), 0, myMemory.size());
	myBuffer.Flush(0, myMemory.size());
	rhi::SetCrashReportSection(mySection, [memory = myMemory] { return RayQueryWatchJson(memory); });

	auto replay = myReplayBuffer.Map();
	std::memset(replay.data(), 0, replay.size());
	const auto& data = RayQueryReplayData();
	std::memcpy(replay.data(), data.data(), data.size() * sizeof(glm::vec4));
	myReplayBuffer.Flush(0, replay.size());
	myReplayBuffer.Unmap();
}

RayQueryWatch::~RayQueryWatch()
{
	rhi::SetCrashReportSection(mySection, {});
	auto words = Snapshot(myMemory);
	std::println(
		"{}: {} begun, {} ended, at most {} candidates a query, {} unrecorded, {} replayed rays hit", mySection,
		words[RAY_QUERY_WATCH_BEGUN], words[RAY_QUERY_WATCH_ENDED], words[RAY_QUERY_WATCH_MAX_CANDIDATES],
		words[RAY_QUERY_WATCH_UNRECORDED], words[RAY_QUERY_WATCH_REPLAY_HITS]);
	myBuffer.Unmap();
}

std::string RayQueryWatchJson(std::span<const std::byte> memory)
{
	auto snapshot = Snapshot(memory);
	const auto* words = snapshot.data();
	auto json = std::format(
		"{{\n    \"begun\": {}, \"ended\": {}, \"maxCandidates\": {}, \"unrecorded\": {},\n    \"inFlight\": [",
		words[RAY_QUERY_WATCH_BEGUN], words[RAY_QUERY_WATCH_ENDED], words[RAY_QUERY_WATCH_MAX_CANDIDATES],
		words[RAY_QUERY_WATCH_UNRECORDED]);
	bool first = true;
	for (uint32_t slotIt = 0; slotIt < RAY_QUERY_WATCH_SLOT_COUNT; slotIt++)
	{
		const auto* slot = words + RAY_QUERY_WATCH_HEADER_COUNT + (static_cast<size_t>(slotIt) * RAY_QUERY_WATCH_SLOT_WORD_COUNT);
		uint32_t state = slot[kSlotState];
		if (state == 0)
			continue;
		uint32_t frame = slot[kSlotFrame];
		uint32_t dispatch = slot[kSlotDispatch];
		json += std::format(
			"{}\n      {{\"ticket\": {}, \"frameIndex\": {}, \"anyHit\": {}, \"moving\": {}, \"stage\": \"{}\", \"candidates\": {}, "
			"\"origin\": [{}, {}, {}], \"direction\": [{}, {}, {}], \"tmax\": {}, \"giClip\": {}, \"giStep\": {}}}",
			first ? "" : ",", state - 1, frame & kLowByte, (frame & kAnyHitBit) != 0, (frame & kMovingBit) != 0,
			StageName(slot[kSlotStage]), slot[kSlotCandidates], Float(slot[kSlotOrigin]), Float(slot[kSlotOrigin + 1]),
			Float(slot[kSlotOrigin + 2]), Float(slot[kSlotDirection]), Float(slot[kSlotDirection + 1]),
			Float(slot[kSlotDirection + 2]), Float(slot[kSlotTmax]), dispatch & kLowByte, dispatch >> kByteBits);
		first = false;
	}
	json += "]\n  }";
	return json;
}

} // namespace gfx
