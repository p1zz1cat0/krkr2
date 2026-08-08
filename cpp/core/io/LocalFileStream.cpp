#include "LocalFileStream.h"

#include "MsgIntf.h"

#include <cstring>

#ifdef _WIN32
#include <io.h>
#define TVP_fseek64 _fseeki64
#define TVP_ftell64 _ftelli64
#else
#define TVP_fseek64 fseeko
#define TVP_ftell64 ftello
#endif

//---------------------------------------------------------------------------
tTVPStdioFileStream::tTVPStdioFileStream(FILE *file) : File(file) {}

//---------------------------------------------------------------------------
tTVPStdioFileStream::~tTVPStdioFileStream() {
    if(File) {
        fclose(File);
        File = nullptr;
    }
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPStdioFileStream::Seek(tjs_int64 offset, tjs_int whence) {
    if(!File)
        return 0;
    TVP_fseek64(File, offset, whence);
    return static_cast<tjs_uint64>(TVP_ftell64(File));
}

//---------------------------------------------------------------------------
tjs_uint tTVPStdioFileStream::Read(void *buffer, tjs_uint read_size) {
    if(!File)
        return 0;
    return static_cast<tjs_uint>(fread(buffer, 1, read_size, File));
}

//---------------------------------------------------------------------------
tjs_uint tTVPStdioFileStream::Write(const void *buffer, tjs_uint write_size) {
    if(!File)
        TVPThrowExceptionMessage(TVPWriteError);
    return static_cast<tjs_uint>(fwrite(buffer, 1, write_size, File));
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPStdioFileStream::GetSize() {
    if(!File)
        return 0;
    const tjs_uint64 pos = static_cast<tjs_uint64>(TVP_ftell64(File));
    TVP_fseek64(File, 0, SEEK_END);
    const tjs_uint64 size = static_cast<tjs_uint64>(TVP_ftell64(File));
    TVP_fseek64(File, static_cast<long>(pos), SEEK_SET);
    return size;
}
