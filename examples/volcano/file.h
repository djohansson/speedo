#pragma once

#include "utils.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <tuple>

#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

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
};

template <class Archive>
void serialize(Archive& archive, FileInfo& fi)
{
    archive(
        cereal::make_nvp("path", fi.path),
        cereal::make_nvp("size", fi.size),
        cereal::make_nvp("timeStamp", fi.timeStamp),
        cereal::make_nvp("sha2", fi.sha2));
}

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

template <typename T>
std::optional<T> loadJSONObject(std::istream& stream, const std::string& name)
{
    cereal::JSONInputArchive json(stream);
    T outValue = {};
    json(cereal::make_nvp(name, outValue));
    return std::move(outValue);
};

template <typename T>
void saveJSONObject(T&& object, std::ostream& stream, const std::string& name)
{
    cereal::JSONOutputArchive json(stream);
    json(cereal::make_nvp(name, std::move(object)));
};

template <typename T>
std::tuple<std::optional<T>, FileState> loadJSONObject(const std::filesystem::path& filePath, const std::string& name)
{
    auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(std::nullopt, FileState::Missing);

    mio::mmap_istreambuf fileStreamBuf(filePath.string());
    std::istream fileStream(&fileStreamBuf);

    return std::make_tuple(loadJSONObject<T>(fileStream, name), FileState::Valid);
}

template <typename T>
void saveJSONObject(T&& object, const std::filesystem::path& filePath, const std::string& name)
{
    mio::mmap_ostreambuf fileStreamBuf(filePath.string());
	std::ostream fileStream(&fileStreamBuf);
    saveJSONObject(std::move(object), fileStream, name);
}

template <typename T>
class ScopedJSONFileObject : Noncopyable
{
public:

    ScopedJSONFileObject(const std::filesystem::path& filePath, const std::string& name)
    : myObject(std::get<0>(loadJSONObject<T>(filePath, name)).value_or(T{}))
    , myFilePath(filePath)
    , myName(name)
    { }

    ScopedJSONFileObject(ScopedJSONFileObject&& other)
    : myObject(std::move(other.myObject))
    , myFilePath(std::move(other.myFilePath))
    , myName(std::move(other.myName))
    {
        other.myObject.reset();
        other.myFilePath.clear();
        other.myName.clear();
    }

    ~ScopedJSONFileObject()
    {
        if (myObject)
            saveJSONObject(std::move(*myObject), myFilePath, myName);
    }

    T& object() { return *myObject; }
    const T& object() const { return *myObject; }

private:

    std::optional<T> myObject;
    std::filesystem::path myFilePath;
    std::string myName;
};
