#include "file.h"

std::string getFileTimeStamp(const std::filesystem::path &filePath)
{
    std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::filesystem::last_write_time(filePath).time_since_epoch());
    std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ms);
    std::time_t timestamp = s.count();

    return std::string(std::asctime(std::localtime(&timestamp)));
}

FileState getFileInfo(
    const std::filesystem::path &filePath,
    FileInfo &outFileInfo,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        return FileState::Missing;

    if (sha2Enable)
    {
        mio::mmap_source file(filePath.string());
        outFileInfo.sha2.resize(picosha2::k_digest_size);
        picosha2::hash256(file.begin(), file.end(), outFileInfo.sha2.begin(), outFileInfo.sha2.end());
    }

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);

    return FileState::Valid;
}

FileState getFileInfo(
    const std::filesystem::path &filePath,
    const std::string &id,
    const std::string &loaderType,
    const std::string &loaderVersion,
    std::istream &jsonStream,
    std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, const std::string&)> loadJSON,
    FileInfo &outFileInfo,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        return FileState::Missing;

    std::string _loaderType;
    std::string _loaderVersion;
    FileInfo _fileInfo;
    
    std::tie(_loaderType, _loaderVersion, _fileInfo) =
        loadJSON(jsonStream, id);

    if (loaderType.compare(_loaderType) != 0 ||
        loaderVersion.compare(_loaderVersion) != 0)
        return FileState::Stale;

    int64_t fileSize = std::filesystem::file_size(filePath);
    std::string fileTimeStamp = getFileTimeStamp(filePath);
    std::vector<unsigned char> fileSha2(picosha2::k_digest_size);
    if (sha2Enable)
    {
        mio::mmap_source file(filePath.string());
        picosha2::hash256(file.begin(), file.end(), fileSha2.begin(), fileSha2.end());
    }

    // perhaps add path check as well?
    if (fileSize != _fileInfo.size ||
        fileTimeStamp.compare(_fileInfo.timeStamp) != 0 ||
        sha2Enable ? fileSha2 != _fileInfo.sha2 : false)
        return FileState::Stale;

    outFileInfo.path = filePath;
    outFileInfo.size = fileSize;
    outFileInfo.timeStamp = std::move(fileTimeStamp);
    outFileInfo.sha2 = std::move(fileSha2);

    return FileState::Valid;
}

void loadBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::istream&)> loadOp,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        throw std::runtime_error("Failed to open file.");

    {
        mio::shared_mmap_source file(filePath.string());
        mio::mmap_streambuf fileStreamBuf(file);
        std::istream fileStream(&fileStreamBuf);

        loadOp(fileStream);
    
        if (sha2Enable)
        {
            fileStream.clear();
            fileStream.seekg(0, std::ios_base::beg);

            outFileInfo.sha2.resize(picosha2::k_digest_size);

            picosha2::hash256(
                std::istreambuf_iterator(&fileStreamBuf),
                std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
                outFileInfo.sha2.begin(),
                outFileInfo.sha2.end());
        }
    }

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);
}

void saveBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::iostream&)> saveOp,
    bool sha2Enable)
{
    {
        mio::shared_mmap_sink file(filePath.string());
        mio::mmap_streambuf fileStreamBuf(file);
        std::iostream fileStream(&fileStreamBuf);

        saveOp(fileStream);
        
        fileStreamBuf.truncate();

        if (sha2Enable)
        {
            fileStream.seekg(0, std::ios_base::beg);

            outFileInfo.sha2.resize(picosha2::k_digest_size);

            picosha2::hash256(
                std::istreambuf_iterator(&fileStreamBuf),
                std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
                outFileInfo.sha2.begin(),
                outFileInfo.sha2.end());
        }
    }

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);
}
