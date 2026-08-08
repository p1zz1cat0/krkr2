#pragma once

#include "tjsCommHead.h"
#include <cstdio>

//---------------------------------------------------------------------------
// 基于 stdio 的本地只读二进制流
//---------------------------------------------------------------------------
class tTVPStdioFileStream : public tTJSBinaryStream {
    FILE *File = nullptr;

public:
    explicit tTVPStdioFileStream(FILE *file);
    ~tTVPStdioFileStream() override;

    tjs_uint64 Seek(tjs_int64 offset, tjs_int whence) override;
    tjs_uint Read(void *buffer, tjs_uint read_size) override;
    tjs_uint Write(const void *buffer, tjs_uint write_size) override;
    tjs_uint64 GetSize() override;
};
