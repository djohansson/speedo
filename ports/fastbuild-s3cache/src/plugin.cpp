// FBuild's cache plugin (FASTBuild's Code/Tools/FBuild/FBuildCore/Cache/CachePluginInterface.h) over S3: a local directory
// in front, then the remote prefix, the shared Backblaze B2 bucket's speedo/fastbuild/ (see docs/notes/build.md,
// "FASTBuild's cache").
//
// Retrieving takes the local directory's file, else the remote object (an anonymous GET: the bucket is public), which is
// then kept locally. Publishing writes the local directory, and where this machine writes the shared caches
// (SPEEDO_BINARY_CACHE_WRITE, as vcpkg's binary cache does, with the AWS credentials: AWS_ACCESS_KEY_ID and
// AWS_SECRET_ACCESS_KEY, else AWS_PROFILE's in ~/.aws/credentials) the remote object too, a PUT signed with AWS Signature
// V4 (curl's). SPEEDO_BINARY_CACHE=0 leaves the remote out, as it does vcpkg's. The remote never fails a build: an error is
// a miss, and after kRemoteFailureLimit in a row the remote is left out for the rest of the build.
//
// The configuration (.CachePluginDLLConfig, which the launcher writes): url=<the remote prefix's https url, ending in
// />;region=<its region>. The local directory is FBuild's cache path (.CachePath, FASTBUILD_CACHE_PATH), else
// ~/.fastbuild/cache, each entry in a directory named by its key's first two characters.
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#	define STDCALL __stdcall
#	define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#	define STDCALL
#	define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{

using OutputFunc = void(STDCALL*)(const char* message);

// the remote's transfers failing in a row before it is left out for the rest of the build
constexpr int kRemoteFailureLimit = 8;
constexpr long kConnectTimeoutMs = 5000;
constexpr long kTransferTimeoutMs = 120000;

enum class Result
{
	kOk,
	kMissing,
	kDenied,
	kFailed,
};

struct State
{
	std::filesystem::path local;
	std::string url;
	std::string region;
	std::string credentials; // key id and secret, for writing: never printed
	bool read = false;
	bool write = false;
	bool verbose = false;
	OutputFunc output = nullptr;
	std::atomic<bool> remoteRead{false};
	std::atomic<bool> remoteWrite{false};
	std::atomic<int> remoteFailures{0};
	std::atomic<unsigned> temporaryCount{0};
	std::mutex handlesMutex;
	std::vector<CURL*> handles;
	unsigned generation = 0;
};

State* gState = nullptr;
std::atomic<unsigned> gGeneration{0};

void Print(const std::string& message)
{
	std::string line = std::format("FBuildS3Cache: {}\n", message);
	if (gState != nullptr && gState->output != nullptr)
		gState->output(line.c_str());
	else
		std::fputs(line.c_str(), stderr);
}

std::string Environment(const char* name)
{
	const char* value = std::getenv(name);
	return value != nullptr ? value : "";
}

std::filesystem::path Home()
{
	std::string home = Environment("HOME");
	if (home.empty())
		home = Environment("USERPROFILE");
	return home;
}

bool ReadFile(const std::filesystem::path& path, std::vector<char>& out)
{
	std::FILE* file = std::fopen(path.string().c_str(), "rb");
	if (file == nullptr)
		return false;
	bool ok = std::fseek(file, 0, SEEK_END) == 0;
	long size = ok ? std::ftell(file) : -1;
	ok = ok && size > 0 && std::fseek(file, 0, SEEK_SET) == 0;
	if (ok)
	{
		out.resize(static_cast<size_t>(size));
		ok = std::fread(out.data(), 1, out.size(), file) == out.size();
	}
	std::fclose(file);
	return ok;
}

// written beside it and renamed over it, so that a reader never sees part of an entry
bool WriteFile(const std::filesystem::path& path, const void* data, size_t size)
{
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	auto temporary = path;
	temporary += std::format(".{}.{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()), gState->temporaryCount++);
	std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = std::fwrite(data, 1, size, file) == size;
	ok = std::fclose(file) == 0 && ok;
	if (ok)
		std::filesystem::rename(temporary, path, error);
	if (!ok || error)
	{
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

std::filesystem::path LocalPath(std::string_view key)
{
	return gState->local / std::string(key.substr(0, 2)) / std::string(key);
}

// AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, else the profile's (AWS_PROFILE, default) in the shared credentials file
std::string Credentials()
{
	std::string id = Environment("AWS_ACCESS_KEY_ID");
	std::string secret = Environment("AWS_SECRET_ACCESS_KEY");
	if (!id.empty() && !secret.empty())
		return id + ":" + secret;
	std::string profile = Environment("AWS_PROFILE");
	if (profile.empty())
		profile = "default";
	std::filesystem::path path = Environment("AWS_SHARED_CREDENTIALS_FILE");
	if (path.empty())
		path = Home() / ".aws" / "credentials";
	std::vector<char> bytes;
	if (!ReadFile(path, bytes))
		return {};
	auto trim = [](std::string_view text)
	{
		while (!text.empty() && std::strchr(" \t\r", text.front()) != nullptr)
			text.remove_prefix(1);
		while (!text.empty() && std::strchr(" \t\r", text.back()) != nullptr)
			text.remove_suffix(1);
		return text;
	};
	std::string_view text(bytes.data(), bytes.size());
	bool inProfile = false;
	id.clear();
	secret.clear();
	while (!text.empty())
	{
		size_t end = text.find('\n');
		std::string_view line = trim(text.substr(0, end));
		text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
		if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
		{
			inProfile = trim(line.substr(1, line.size() - 2)) == profile;
			continue;
		}
		size_t equals = line.find('=');
		if (!inProfile || equals == std::string_view::npos)
			continue;
		std::string_view key = trim(line.substr(0, equals));
		std::string_view value = trim(line.substr(equals + 1));
		if (key == "aws_access_key_id")
			id = value;
		else if (key == "aws_secret_access_key")
			secret = value;
	}
	return !id.empty() && !secret.empty() ? id + ":" + secret : std::string();
}

#if defined(__linux__)
// the system's certificate bundle, by the distributions' paths: the OpenSSL curl is built with looks in its own prefix
// (as the client's test assets download, src/client/testassets.cpp)
const char* CertificateBundle()
{
	for (const char* path :
		 {"/etc/ssl/certs/ca-certificates.crt",
		  "/etc/pki/tls/certs/ca-bundle.crt",
		  "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
		  "/etc/ssl/ca-bundle.pem",
		  "/etc/ssl/cert.pem"})
		if (std::error_code error; std::filesystem::exists(path, error))
			return path;
	return nullptr;
}
#endif

// this thread's curl handle (FBuild's workers retrieve and publish at once), reset for a transfer: its connections kept
CURL* Handle()
{
	thread_local CURL* handle = nullptr;
	thread_local unsigned generation = 0;
	if (handle == nullptr || generation != gState->generation)
	{
		handle = curl_easy_init();
		generation = gState->generation;
		std::scoped_lock lock(gState->handlesMutex);
		gState->handles.push_back(handle);
	}
	else
		curl_easy_reset(handle);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, kTransferTimeoutMs);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, "speedo-fastbuild-s3cache");
#if defined(__linux__)
	static const char* const kBundle = CertificateBundle();
	if (kBundle != nullptr)
		curl_easy_setopt(handle, CURLOPT_CAINFO, kBundle);
#endif
	return handle;
}

size_t Append(char* data, size_t size, size_t count, void* user)
{
	auto* body = static_cast<std::vector<char>*>(user);
	body->insert(body->end(), data, data + size * count);
	return size * count;
}

size_t Discard(char*, size_t size, size_t count, void*)
{
	return size * count;
}

Result Perform(CURL* handle, const char* what, std::string_view key)
{
	CURLcode code = curl_easy_perform(handle);
	if (code != CURLE_OK)
	{
		if (gState->verbose)
			Print(std::format("{} {}: {}", what, key, curl_easy_strerror(code)));
		return Result::kFailed;
	}
	long status = 0;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
	if (status >= 200 && status < 300)
		return Result::kOk;
	if (gState->verbose)
		Print(std::format("{} {}: http {}", what, key, status));
	if (status == 404)
		return Result::kMissing;
	if (status == 401 || status == 403)
		return Result::kDenied;
	return Result::kFailed;
}

Result Get(std::string_view key, std::vector<char>& body)
{
	CURL* handle = Handle();
	std::string url = gState->url + std::string(key);
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &Append);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
	Result result = Perform(handle, "retrieve", key);
	return result == Result::kOk && body.empty() ? Result::kMissing : result;
}

Result Put(std::string_view key, const void* data, size_t size)
{
	CURL* handle = Handle();
	std::string url = gState->url + std::string(key);
	std::string sigv4 = std::format("aws:amz:{}:s3", gState->region);
	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
	// over https the payload isn't hashed into the signature (S3's UNSIGNED-PAYLOAD), which curl takes from this header
	headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
	headers = curl_slist_append(headers, "Expect:");
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(handle, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(handle, CURLOPT_AWS_SIGV4, sigv4.c_str());
	curl_easy_setopt(handle, CURLOPT_USERPWD, gState->credentials.c_str());
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &Discard);
	Result result = Perform(handle, "publish", key);
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
	curl_slist_free_all(headers);
	return result;
}

void RemoteSucceeded()
{
	gState->remoteFailures.store(0);
}

void RemoteFailed()
{
	if (++gState->remoteFailures == kRemoteFailureLimit)
	{
		gState->remoteRead.store(false);
		gState->remoteWrite.store(false);
		Print(std::format("{} failed {} times in a row: left out for the rest of the build", gState->url, kRemoteFailureLimit));
	}
}

void Configure(std::string_view config)
{
	while (!config.empty())
	{
		size_t end = config.find(';');
		std::string_view entry = config.substr(0, end);
		config.remove_prefix(end == std::string_view::npos ? config.size() : end + 1);
		size_t equals = entry.find('=');
		if (equals == std::string_view::npos)
			continue;
		std::string_view key = entry.substr(0, equals);
		std::string_view value = entry.substr(equals + 1);
		if (key == "url")
			gState->url = value;
		else if (key == "region")
			gState->region = value;
	}
	if (!gState->url.empty() && gState->url.back() != '/')
		gState->url += '/';
}

} // namespace

PLUGIN_EXPORT bool STDCALL CacheInitEx(
	const char* cachePath, bool cacheRead, bool cacheWrite, bool cacheVerbose, const char* userConfig, OutputFunc outputFunc)
{
	gState = new State;
	gState->generation = ++gGeneration;
	gState->read = cacheRead;
	gState->write = cacheWrite;
	gState->verbose = cacheVerbose;
	gState->output = outputFunc;
	gState->local = cachePath != nullptr && *cachePath != '\0' ? std::filesystem::path(cachePath) : Home() / ".fastbuild" / "cache";
	Configure(userConfig != nullptr ? userConfig : "");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	bool remote = !gState->url.empty() && Environment("SPEEDO_BINARY_CACHE") != "0";
	gState->remoteRead.store(remote && cacheRead);
	if (remote && cacheWrite && !Environment("SPEEDO_BINARY_CACHE_WRITE").empty())
	{
		gState->credentials = Credentials();
		if (gState->credentials.empty() || gState->region.empty())
			Print("SPEEDO_BINARY_CACHE_WRITE is set, but there are no AWS credentials (or no region): not writing the remote");
		else
			gState->remoteWrite.store(true);
	}
	if (cacheVerbose)
		Print(std::format(
			"local {} ({}{}), remote {} ({}{})",
			gState->local.string(),
			cacheRead ? "read" : "",
			cacheWrite ? " write" : "",
			remote ? gState->url : "off",
			gState->remoteRead.load() ? "read" : "",
			gState->remoteWrite.load() ? " write" : ""));
	return true;
}

PLUGIN_EXPORT void STDCALL CacheShutdown()
{
	if (gState == nullptr)
		return;
	for (CURL* handle : gState->handles)
		curl_easy_cleanup(handle);
	curl_global_cleanup();
	delete gState;
	gState = nullptr;
}

PLUGIN_EXPORT bool STDCALL CacheRetrieve(const char* cacheId, void*& data, unsigned long long& dataSize)
{
	if (gState == nullptr || !gState->read)
		return false;
	std::string_view key(cacheId);
	std::vector<char> body;
	bool found = false;
	auto path = LocalPath(key);
	if (ReadFile(path, body))
	{
		found = true;
		// used: the last to go when the cache is trimmed (CacheTrim, oldest first)
		std::error_code error;
		std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
	}
	else if (gState->remoteRead.load())
	{
		Result result = Get(key, body);
		if (result == Result::kOk)
		{
			found = true;
			RemoteSucceeded();
			WriteFile(path, body.data(), body.size());
		}
		else if (result == Result::kMissing)
			RemoteSucceeded();
		else
			RemoteFailed();
	}
	if (!found)
		return false;
	data = std::malloc(body.size());
	if (data == nullptr)
		return false;
	std::memcpy(data, body.data(), body.size());
	dataSize = body.size();
	return true;
}

PLUGIN_EXPORT bool STDCALL CachePublish(const char* cacheId, const void* data, unsigned long long dataSize)
{
	if (gState == nullptr || !gState->write)
		return false;
	std::string_view key(cacheId);
	bool stored = WriteFile(LocalPath(key), data, static_cast<size_t>(dataSize));
	if (gState->remoteWrite.load())
	{
		Result result = Put(key, data, static_cast<size_t>(dataSize));
		if (result == Result::kOk)
		{
			stored = true;
			RemoteSucceeded();
		}
		else if (result == Result::kDenied)
		{
			if (gState->remoteWrite.exchange(false))
				Print(std::format("no write access to {}: publishing locally only", gState->url));
		}
		else
			RemoteFailed();
	}
	return stored;
}

PLUGIN_EXPORT void STDCALL CacheFreeMemory(void* data, unsigned long long /*dataSize*/)
{
	std::free(data);
}

// the local directory's entries and their size (-cacheinfo)
PLUGIN_EXPORT bool STDCALL CacheOutputInfo(bool /*showProgress*/)
{
	if (gState == nullptr)
		return false;
	size_t count = 0;
	uintmax_t bytes = 0;
	std::error_code error;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(gState->local, error))
		if (entry.is_regular_file(error))
		{
			++count;
			bytes += entry.file_size(error);
		}
	Print(std::format("local {}: {} entries, {} MiB; remote {}", gState->local.string(), count, bytes >> 20, gState->url.empty() ? "off" : gState->url));
	return true;
}

// the local directory trimmed to sizeMiB, the least recently used first (-cachetrim); the remote expires by the bucket's
// lifecycle rule
PLUGIN_EXPORT bool STDCALL CacheTrim(bool /*showProgress*/, unsigned int sizeMiB)
{
	if (gState == nullptr)
		return false;
	struct Entry
	{
		std::filesystem::path path;
		std::filesystem::file_time_type time;
		uintmax_t size;
	};
	std::vector<Entry> entries;
	uintmax_t total = 0;
	std::error_code error;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(gState->local, error))
		if (entry.is_regular_file(error))
		{
			entries.push_back({entry.path(), entry.last_write_time(error), entry.file_size(error)});
			total += entries.back().size;
		}
	std::ranges::sort(entries, {}, &Entry::time);
	uintmax_t limit = static_cast<uintmax_t>(sizeMiB) << 20;
	size_t removed = 0;
	for (const auto& entry : entries)
	{
		if (total <= limit)
			break;
		if (std::filesystem::remove(entry.path, error))
		{
			total -= entry.size;
			++removed;
		}
	}
	Print(std::format("trimmed {} entries from {}: {} MiB left", removed, gState->local.string(), total >> 20));
	return true;
}
