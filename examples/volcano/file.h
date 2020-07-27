#pragma once

#include "utils.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <tuple>

#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/optional.hpp>

#include <mio/mmap_streambuf.hpp>

#include <picosha2.h>

enum class FileState : uint8_t
{
    Missing,
    Stale,
    Valid,
};

struct FileInfo
{
    std::string path;
    int64_t size = 0;
    std::string timeStamp;
    std::vector<unsigned char> sha2;

    template <class Archive>
    void serialize(Archive& archive)
    {
        archive(
            CEREAL_NVP(path),
            CEREAL_NVP(size),
            CEREAL_NVP(timeStamp),
            CEREAL_NVP(sha2));
    }

};

std::string getFileTimeStamp(const std::filesystem::path &filePath);

std::tuple<FileState, FileInfo> getFileInfo(
    const std::filesystem::path &filePath,
    bool sha2Enable);

using LoadFileInfoFromJSONFn = std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, const std::string&)>;

std::tuple<FileState, FileInfo> getFileInfo(
    const std::filesystem::path &filePath,
    const std::string &id,
    const std::string &loaderType,
    const std::string &loaderVersion,
    std::istream &jsonStream,
    LoadFileInfoFromJSONFn loadJSON,
    bool sha2Enable);

std::tuple<FileState, FileInfo> loadBinaryFile(
    const std::filesystem::path &filePath,
    std::function<void(std::istream&)> loadOp,
    bool sha2Enable);

std::tuple<FileState, FileInfo> saveBinaryFile(
    const std::filesystem::path &filePath,
    std::function<void(std::iostream&)> saveOp,
    bool sha2Enable);

using LoadFileFn = std::function<void(std::istream&)>;
using SaveFileFn = std::function<void(std::iostream&)>;

void loadCachedSourceFile(
    const std::filesystem::path &sourceFilePath,
    const std::filesystem::path &cacheFilePath,
    const std::string& loaderType,
	const std::string& loaderVersion,
    LoadFileFn loadSourceFileFn,
    LoadFileFn loadBinaryCacheFn,
    SaveFileFn saveBinaryCacheFn);

template <typename T, typename Archive>
T loadObject(std::istream& stream, const std::string& name)
{
    Archive archive(stream);
    T outValue = {};
    archive(cereal::make_nvp(name, outValue));
    return std::move(outValue);
};

template <typename T, typename Archive>
void saveObject(const T& object, std::ostream& stream, const std::string& name)
{
    Archive json(stream);
    json(cereal::make_nvp(name, object));
};

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState> loadObject(const std::filesystem::path& filePath, const std::string& name)
{
    auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(std::nullopt, FileState::Missing);

    mio::mmap_istreambuf fileStreamBuf(filePath.string());
    std::istream fileStream(&fileStreamBuf);

    return std::make_tuple(std::make_optional(loadObject<T, Archive>(fileStream, name)), FileState::Valid);
}

template <typename T, typename Archive>
void saveObject(const T& object, const std::filesystem::path& filePath, const std::string& name)
{
    mio::mmap_ostreambuf fileStreamBuf(filePath.string());
	std::ostream fileStream(&fileStreamBuf);
    saveObject<T, Archive>(object, fileStream, name);
}

enum class FileAccessMode
{
    ReadOnly,
    ReadWrite
};

template <typename T, FileAccessMode Mode, typename InputArchive = cereal::JSONInputArchive, typename OutputArchive = cereal::JSONOutputArchive, bool SaveOnClose = false>
class FileObject : public Noncopyable, public T
{
    // todo: implement mechanism to only write changes when contents have changed.

public:

    FileObject(
        const std::filesystem::path& filePath,
        const std::string& name,
        T&& defaultObject = T{})
    : T(std::get<0>(loadObject<T, InputArchive>(filePath, name)).value_or(std::move(defaultObject)))
    , myFilePath(filePath)
    , myName(name)
    {
    }

    FileObject(FileObject&& other) noexcept
    : T(std::move(other))
    , myFilePath(std::exchange(other.myFilePath, {}))
    , myName(std::exchange(other.myName, {}))
    {
    }

    void reload()
    {
        static_cast<T&>(*this) = std::get<0>(
            loadObject<T, InputArchive>(myFilePath, myName)).value_or(std::move(static_cast<T&>(*this)));
    }

    template <FileAccessMode M = Mode>
    typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type save() const
    {
        saveObject<T, OutputArchive>(static_cast<const T&>(*this), myFilePath, myName);
    }

    ~FileObject()
    {
        if constexpr(SaveOnClose)
            if (!myFilePath.empty())
                save();
    }

private:

    std::filesystem::path myFilePath;
    std::string myName;
};

template <typename T>
using ReadOnlyJSONFileObject = FileObject<T, FileAccessMode::ReadOnly>;

template <typename T>
using ReadWriteJSONFileObject = FileObject<T, FileAccessMode::ReadWrite>;

template <typename T>
using AutoReadWriteJSONFileObject = FileObject<T, FileAccessMode::ReadWrite, cereal::JSONInputArchive, cereal::JSONOutputArchive, true>;
