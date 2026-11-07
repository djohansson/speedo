#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace client::testassets
{

// the test assets the client downloads into the test asset directory (the environment's TestAssetsPath), from the manifests
// fetch-test-assets.ps1 reads (resources/test-asset-manifests): the quick tier's (quick.txt, a model or two per feature),
// or all of them (Morgan McGuire's archive, the glTF sample models, the sample environments)
enum class Set : uint8_t
{
	kQuick,
	kFull,
};

// a set's files, by their manifests' sizes: all of them, and those not in the test asset directory (at their size)
struct SetSize
{
	uint64_t byteCount = 0;
	uint32_t fileCount = 0;
	uint64_t missingByteCount = 0;
	uint32_t missingFileCount = 0;
};
[[nodiscard]] SetSize Measure(Set set);

// a download (see Start), as the ui shows it: its task writes it, the ui reads it (and asks it to stop)
struct Download
{
	Set set = Set::kQuick;
	std::chrono::steady_clock::time_point start;
	std::atomic_uint64_t byteCount{0};
	std::atomic_uint64_t bytesDone{0};
	std::atomic_uint32_t fileCount{0};
	std::atomic_uint32_t filesDone{0};
	std::atomic_uint32_t failedCount{0};
	std::atomic_bool extracting{false};
	std::atomic_bool cancel{false}; // the ui's
	std::atomic_bool done{false}; // the task's, last
	std::mutex mutex;
	std::string current; // the file started last (under mutex)
	std::string error; // the first failure (under mutex)
};

// downloads set's files that aren't in the test asset directory (at their size) on the application's executor: a task, its
// transfers 8 at once (libcurl), each written into its file as it comes (a .part, mapped at its size) and checked against
// its manifest's size and sha256 (one that doesn't match is kept as .unverified); then extracts what the manifests say
// (Bistro's parts). a failed transfer is tried 3 times. it stops when asked (Download::cancel) or when the application
// exits, leaving no .part
[[nodiscard]] std::shared_ptr<Download> Start(Set set);

// the ui (ui thread): File > Download Test Assets..., and the window offering the sets (on a first launch with none of them,
// unless automated: SPEEDO_AUTOLOAD_EXIT or SPEEDO_BACKGROUND; the choice is remembered in the user profile,
// test-assets.txt, and a chosen set unfinished is resumed at startup), and a download's progress
void DrawMenuItem();
void Draw();

} // namespace client::testassets
