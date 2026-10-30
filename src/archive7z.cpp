#include "archive7z.hpp"

#include <algorithm>
#include <cstring>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "util.hpp"

namespace archive7z {

namespace {

struct MemoryStream {
    ISeekInStream vt;
    const unsigned char* data;
    size_t size;
    size_t pos;
};

SRes memRead(ISeekInStreamPtr p, void* buf, size_t* size) {
    auto* s = Z7_CONTAINER_FROM_VTBL(p, MemoryStream, vt);
    size_t n = std::min(*size, s->size - s->pos);
    if (n) std::memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *size = n;
    return SZ_OK;
}

SRes memSeek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
    auto* s = Z7_CONTAINER_FROM_VTBL(p, MemoryStream, vt);
    Int64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? (Int64)s->pos : (Int64)s->size;
    Int64 target = base + *pos;
    if (target < 0) return SZ_ERROR_PARAM;
    s->pos = (size_t)std::min<Int64>(target, (Int64)s->size);
    *pos = (Int64)s->pos;
    return SZ_OK;
}

std::string describe(SRes res) {
    switch (res) {
        case SZ_ERROR_DATA: return "the compressed data is corrupted";
        case SZ_ERROR_MEM: return "not enough memory";
        case SZ_ERROR_CRC: return "a checksum does not match (the archive is damaged)";
        case SZ_ERROR_UNSUPPORTED: return "it uses an unsupported compression method or is password protected";
        case SZ_ERROR_NO_ARCHIVE: return "it is not a 7z archive";
        case SZ_ERROR_ARCHIVE: return "the archive headers are damaged";
        case SZ_ERROR_INPUT_EOF: return "the archive is incomplete (download cut off?)";
        default: return "7z error " + std::to_string(res);
    }
}

}  // namespace

bool isArchive(const std::string& bytes) {
    return bytes.size() >= 6 && std::memcmp(bytes.data(), "7z\xBC\xAF\x27\x1C", 6) == 0;
}

bool extract(const std::string& bytes, std::vector<Entry>& entries, std::string& error) {
    static bool crcReady = false;
    if (!crcReady) {
        CrcGenerateTable();
        crcReady = true;
    }
    MemoryStream mem;
    mem.vt.Read = memRead;
    mem.vt.Seek = memSeek;
    mem.data = reinterpret_cast<const unsigned char*>(bytes.data());
    mem.size = bytes.size();
    mem.pos = 0;

    const size_t kBufSize = (size_t)1 << 18;
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    std::vector<Byte> lookBuf(kBufSize);
    look.buf = lookBuf.data();
    look.bufSize = kBufSize;
    look.realStream = &mem.vt;
    LookToRead2_INIT(&look)

    ISzAlloc allocImp = {SzAlloc, SzFree};
    ISzAlloc allocTemp = {SzAllocTemp, SzFreeTemp};
    CSzArEx db;
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &allocImp, &allocTemp);
    if (res != SZ_OK) {
        error = "cannot open the archive: " + describe(res);
        SzArEx_Free(&db, &allocImp);
        return false;
    }

    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    std::vector<UInt16> name;
    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        Entry e;
        size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        name.assign(len ? len : 1, 0);
        SzArEx_GetFileNameUtf16(&db, i, name.data());
        e.name = util::utf16ToUtf8(name.data(), len ? len - 1 : 0);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        e.isDir = SzArEx_IsDir(&db, i) != 0;
        if (!e.isDir) {
            size_t offset = 0, processed = 0;
            res = SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &processed,
                                 &allocImp, &allocTemp);
            if (res == SZ_OK)
                e.data.assign(reinterpret_cast<const char*>(outBuffer) + offset, processed);
            else
                e.error = describe(res);
        }
        entries.push_back(std::move(e));
    }
    ISzAlloc_Free(&allocImp, outBuffer);
    SzArEx_Free(&db, &allocImp);
    return true;
}

}  // namespace archive7z
