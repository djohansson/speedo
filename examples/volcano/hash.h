#pragma once

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <xxhash.h>

class xxh64_filebuf : public std::filebuf
{
public:
    xxh64_filebuf(uint64_t seed = 42);
    ~xxh64_filebuf();

    inline uint64_t digest() const { return XXH64_digest(myState); }
    
protected:
	virtual int sync() override;
    virtual int_type overflow(int_type c = traits_type::eof()) override;
	virtual int_type underflow() override;
    // virtual std::streamsize xsgetn(char_type* s, std::streamsize count) override;
    // virtual std::streamsize xsputn(const char* s, std::streamsize n) override;

private:
    XXH64_state_t* myState = nullptr;
    size_t myTotalSizeRead = 0; // temp debug
    size_t myTotalSizeWritten = 0; // temp debug
	size_t mySyncCount = 0; // temp debug
	size_t myOverflowCount = 0; // temp debug
	size_t myUnderflowCount = 0; // temp debug
};

class xxh64_fstream : public std::fstream
{
public:
    xxh64_fstream(const std::filesystem::path& filePath, std::ios_base::openmode mode);

    inline xxh64_filebuf* rdbuf() { return &myBuffer; }

private:
    xxh64_filebuf myBuffer;
};

template <typename LoopOpT, typename ReadOpT, uint32_t BufferSize = 16384>
uint64_t xxh64_streaming(LoopOpT loopOp, ReadOpT readOp)
{
	auto xxh64State = XXH64_createState();
	assert(xxh64State != nullptr);

	constexpr uint64_t seed = 0;
	auto xxh64ResetResult = XXH64_reset(xxh64State, seed);
	assert(xxh64ResetResult != XXH_ERROR);

	std::array<char, BufferSize> buffer;
	while (loopOp()) 
	{
		size_t size = readOp(buffer.data(), buffer.size());
		auto xxh64AddResult = XXH64_update(xxh64State, buffer.data(), size);
		assert(xxh64AddResult != XXH_ERROR);
	}

	const uint64_t hash = XXH64_digest(xxh64State);
	
	XXH64_freeState(xxh64State);

	return hash;
};
