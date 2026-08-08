#include "tjsCommHead.h"

#include "tjsError.h"
#include "tjsMessage.h"

//---------------------------------------------------------------------------
// 工具链最小消息定义（覆盖 XP3 / MemoryStream 所需符号）
//---------------------------------------------------------------------------
#define TVP_MSG_DECL(name, msg) tTJSMessageHolder name(TJS_W(#name), msg);

TVP_MSG_DECL(TVPReadError, TJS_W("Read error"))
TVP_MSG_DECL(TVPWriteError, TJS_W("Write error"))
TVP_MSG_DECL(TVPInsufficientMemory, TJS_W("Insufficient memory"))
TVP_MSG_DECL(TVPUncompressionFailed, TJS_W("Uncompression failed"))
TVP_MSG_DECL(TVPCannotUnbindXP3EXE, TJS_W("Cannot unbind XP3 executable: %1"))
TVP_MSG_DECL(TVPSpecifiedStorageHadBeenProtected,
             TJS_W("Specified storage had been protected"))
TVP_MSG_DECL(TVPStorageInArchiveNotFound,
             TJS_W("Storage '%1' not found in archive '%2'"))
TVP_MSG_DECL(TVPInfoFailed, TJS_W("(info) Failed."))
TVP_MSG_DECL(TVPInfoDoneWithContains,
             TJS_W("(info) Done. Contains %1 files, %2 segments."))
TVP_MSG_DECL(TVPInfoTryingToReadXp3VirtualFileSystemInformationFrom,
             TJS_W("(info) Trying to read XP3 virtual file system "
                   "information from %1..."))
TVP_MSG_DECL(TVPTjsCharMustBeTwoOrFour,
             TJS_W("sizeof(tjs_char) must be 2 or 4."))

//---------------------------------------------------------------------------
ttstr TVPFormatMessage(const tjs_char *msg, const ttstr &p1) {
    tjs_char *buf = new tjs_char[TJS_strlen(msg) + p1.GetLen() + 1];
    tjs_char *p = buf;
    for(; *msg; msg++, p++) {
        if(*msg == TJS_W('%') && msg[1] == TJS_W('1')) {
            TJS_strcpy(p, p1.c_str());
            p += p1.GetLen() - 1;
            msg++;
            continue;
        }
        if(*msg == TJS_W('%') && msg[1] == TJS_W('%')) {
            *p = TJS_W('%');
            msg++;
            continue;
        }
        *p = *msg;
    }
    *p = 0;
    ttstr ret(buf);
    delete[] buf;
    return ret;
}

//---------------------------------------------------------------------------
ttstr TVPFormatMessage(const tjs_char *msg, const ttstr &p1, const ttstr &p2) {
    tjs_char *buf =
        new tjs_char[TJS_strlen(msg) + p1.GetLen() + p2.GetLen() + 1];
    tjs_char *p = buf;
    for(; *msg; msg++, p++) {
        if(*msg == TJS_W('%') && msg[1] == TJS_W('1')) {
            TJS_strcpy(p, p1.c_str());
            p += p1.GetLen() - 1;
            msg++;
            continue;
        }
        if(*msg == TJS_W('%') && msg[1] == TJS_W('2')) {
            TJS_strcpy(p, p2.c_str());
            p += p2.GetLen() - 1;
            msg++;
            continue;
        }
        if(*msg == TJS_W('%') && msg[1] == TJS_W('%')) {
            *p = TJS_W('%');
            msg++;
            continue;
        }
        *p = *msg;
    }
    *p = 0;
    ttstr ret(buf);
    delete[] buf;
    return ret;
}

//---------------------------------------------------------------------------
void TVPThrowExceptionMessage(const tjs_char *msg) { throw eTJSError(msg); }

void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr &p1,
                              tjs_int num) {
    throw eTJSError(TVPFormatMessage(msg, p1, ttstr(num)));
}

void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr &p1) {
    throw eTJSError(TVPFormatMessage(msg, p1));
}

void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr &p1,
                              const ttstr &p2) {
    throw eTJSError(TVPFormatMessage(msg, p1, p2));
}
