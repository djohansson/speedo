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
    template <class Archive>
    void serialize(Archive &archive)
    {
        archive(/*CEREAL_NVP(path), */CEREAL_NVP(size), CEREAL_NVP(timeStamp), CEREAL_NVP(sha2));
    }

    void clear()
    {
        path.clear();
        size = 0;
        timeStamp.clear();
        sha2.clear();
    }

    std::filesystem::path path;
    int64_t size = 0;
    std::string timeStamp;
    std::vector<unsigned char> sha2;
};

struct SourceFileInfo : public FileInfo
{
    std::string loaderType;
    std::string loaderVersion;
};

std::string getFileTimeStamp(const std::filesystem::path &filePath);

FileState getFileInfo(
    const std::filesystem::path &filePath,
    FileInfo &outFileInfo,
    bool sha2Enable);

using LoadFileInfoFromJSONFn = std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, const std::string&)>;

FileState getFileInfo(
    const std::filesystem::path &filePath,
    const std::string &id,
    const std::string &loaderType,
    const std::string &loaderVersion,
    std::istream &jsonStream,
    LoadFileInfoFromJSONFn loadJSON,
    FileInfo &outFileInfo,
    bool sha2Enable);

void loadBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::istream&)> loadOp,
    bool sha2Enable);

void saveBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::iostream&)> saveOp,
    bool sha2Enable);

using LoadFileFn = std::function<void(std::istream&)>;
using SaveFileFn = std::function<void(std::iostream&)>;

void loadCachedSourceFile(
    const std::filesystem::path &sourceFilePath,
    const std::filesystem::path &cacheFilePath,
    LoadFileFn loadSourceFileFn,
    LoadFileFn loadBinaryCacheFn,
    SaveFileFn saveBinaryCacheFn);
