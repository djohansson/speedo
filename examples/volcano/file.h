#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <tuple>

#include <cereal/cereal.hpp>
#include <mio/mmap_streambuf.hpp>
#include <picosha2.h>

enum class FileState
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

struct SourceFileInfo : public FileInfo
{
    std::string loaderType;
    std::string loaderVersion;
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
  