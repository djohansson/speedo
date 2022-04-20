#pragma once

#include "utils.h"

#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <tuple>

#include <cereal/archives/json.hpp>

#include <picosha2.h>

enum class FileState : uint8_t
{
	Missing,
	Stale,
	Valid,
};

enum class FileAccessMode : uint8_t
{
	ReadOnly,
	ReadWrite
};

struct FileInfo
{
	std::filesystem::path path;
	uintmax_t size = 0;
	std::string timeStamp;
	std::array<uint8_t, 32> sha2;
};

std::string getFileTimeStamp(const std::filesystem::path& filePath);

std::tuple<FileState, FileInfo> getFileInfo(const std::filesystem::path& filePath, bool sha2Enable);

using LoadFileInfoFromJSONFn =
	std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, std::string_view)>;

template <const char* Id, const char* LoaderType, const char* LoaderVersion>
std::tuple<FileState, FileInfo> getFileInfo(
	const std::filesystem::path& filePath,
	std::istream& jsonStream,
	LoadFileInfoFromJSONFn loadJSONFn,
	bool sha2Enable);

using LoadFileFn = std::function<bool(std::istream&)>;
using SaveFileFn = std::function<bool(std::iostream&)>;

std::tuple<FileState, FileInfo>
loadBinaryFile(const std::filesystem::path& filePath, LoadFileFn loadOp, bool sha2Enable);

std::tuple<FileState, FileInfo>
saveBinaryFile(const std::filesystem::path& filePath, SaveFileFn saveOp, bool sha2Enable);

template <const char* LoaderType, const char* LoaderVersion>
void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	const std::filesystem::path& cacheFilePath,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn);

template <typename T, typename Archive>
T loadObject(std::istream& stream, std::string_view name);

template <typename T, typename Archive>
void saveObject(const T& object, std::ostream& stream, std::string_view name);

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState>
loadObject(const std::filesystem::path& filePath, std::string_view name);

template <typename T, typename Archive>
void saveObject(const T& object, const std::filesystem::path& filePath, std::string_view name);

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive, // todo: make this more watertight by using conditional
	bool SaveOnClose = false>
class FileObject
	: public Noncopyable
	, public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.

public:
	constexpr FileObject() noexcept = default;
	FileObject(const std::filesystem::path& filePath, T&& defaultObject = T{});
	FileObject(FileObject&& other) noexcept;
	~FileObject();

	FileObject& operator=(FileObject&& other) noexcept;

	// bool isDirty() const noexcept
	// {
	// 	FileInfo info;
	// 	picosha2::hash256(
	// 		,
	// 		,
	// 		info.sha2.begin(),
	// 		info.sha2.end());
	// 	return myInfo.sha2 == info.sha2;
	// }

	void swap(FileObject& rhs) noexcept;
	friend void swap(FileObject& lhs, FileObject& rhs) noexcept { lhs.swap(rhs); }

	void reload();

	template <FileAccessMode M = Mode>
	typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type save() const;

private:
	FileInfo myInfo;
};

template <typename T>
using ReadOnlyJSONFileObject =
	FileObject<T, FileAccessMode::ReadOnly, cereal::JSONInputArchive, cereal::JSONOutputArchive>;

template <typename T>
using ReadWriteJSONFileObject =
	FileObject<T, FileAccessMode::ReadWrite, cereal::JSONInputArchive, cereal::JSONOutputArchive>;

template <typename T>
using AutoSaveJSONFileObject = FileObject<
	T,
	FileAccessMode::ReadWrite,
	cereal::JSONInputArchive,
	cereal::JSONOutputArchive,
	true>;

#include "file.inl"
