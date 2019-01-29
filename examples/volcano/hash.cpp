#include "hash.h"

#include <iostream>

xxh64_filebuf::xxh64_filebuf(uint64_t seed)
    : myState(XXH64_createState())
{
    auto xxh64ResetResult = XXH64_reset(myState, seed);
    assert(xxh64ResetResult == XXH_OK);
}

xxh64_filebuf::~xxh64_filebuf()
{
    XXH_errorcode result = XXH64_freeState(myState);
    assert(result == XXH_OK);

    std::cout << "(xxh64_filebuf::~xxh64_filebuf) myTotalSizeWritten: " << myTotalSizeWritten << ", myTotalSizeRead: " << myTotalSizeRead << std::endl;
    std::cout << "(xxh64_filebuf::~xxh64_filebuf) mySyncCount: " << mySyncCount << ", myOverflowCount: " << myOverflowCount << ", myUnderflowCount: " << myUnderflowCount << std::endl;
}

int xxh64_filebuf::sync()
{
    mySyncCount++;
}

std::filebuf::int_type xxh64_filebuf::overflow(int_type c)
{
    myOverflowCount++;

    size_t size = epptr() - pbase();
    assert(size > 0);

    myTotalSizeWritten += size;

    auto xxh64AddResult = XXH64_update(myState, pbase(), size);
    assert(xxh64AddResult == XXH_OK);

    //std::cout << "(xxh64_filebuf::overflow) size: " << size << ", c: " << c << ", total size: " << myTotalSizeWritten << std::endl;

    return std::filebuf::overflow(c);
}

std::filebuf::int_type xxh64_filebuf::underflow()
{
    myUnderflowCount++;

    size_t size = egptr() - eback();
    assert(size > 0);
    assert(gptr() == egptr());

    myTotalSizeRead += size;

    auto xxh64AddResult = XXH64_update(myState, eback(), size);
    assert(xxh64AddResult == XXH_OK);

    //std::cout << "(xxh64_filebuf::underflow) size: " << size << ", total size: " << myTotalSizeRead << std::endl;

    return std::filebuf::underflow();
}

// std::streamsize xxh64_filebuf::xsgetn(char_type* s, std::streamsize count)
// {
//     auto size = std::filebuf::xsgetn(s, count);

//     assert(size > 0);

//     myTotalSizeRead += size;

//     auto xxh64AddResult = XXH64_update(myState, s, size);
//     assert(xxh64AddResult == XXH_OK);

//     std::cout << "(xxh64_filebuf::xsgetn) size: " << size << ", total size: " << myTotalSizeRead << std::endl;

//     return size;
// }

// std::streamsize xxh64_filebuf::xsputn(const char* s, std::streamsize n)
// {
//     auto size = std::filebuf::xsputn(s, n);

//     assert(size > 0);

//     myTotalSizeWritten += size;

//     auto xxh64AddResult = XXH64_update(myState, s, size);
//     assert(xxh64AddResult == XXH_OK);

//     std::cout << "(xxh64_filebuf::xsputn) size: " << size << ", total size: " << myTotalSizeWritten << std::endl;

//     return size;
// }

xxh64_fstream::xxh64_fstream(const std::filesystem::path& filePath, std::ios_base::openmode mode)
{
    init(&myBuffer);

    if (!myBuffer.open(filePath, mode))
        setstate(std::ios_base::failbit);

    myBuffer.pubseekoff(0, ios_base::beg);
}
