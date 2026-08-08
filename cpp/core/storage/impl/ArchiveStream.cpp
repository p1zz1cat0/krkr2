#include "StorageIntf.h"

extern tTJSBinaryStream *TVPGetCachedArchiveHandle(void *pointer,
                                                   const ttstr &name);
extern void TVPReleaseCachedArchiveHandle(void *pointer,
                                          tTJSBinaryStream *stream);

//---------------------------------------------------------------------------
TArchiveStream::TArchiveStream(tTVPArchive *owner, tjs_uint64 off,
                               tjs_uint64 len) :
    Owner(owner), StartPos(off), DataLength(len) {
    Owner->AddRef();
    _instr = TVPGetCachedArchiveHandle(Owner, Owner->ArchiveName);
    CurrentPos = 0;
    _instr->SetPosition(off);
}

//---------------------------------------------------------------------------
TArchiveStream::~TArchiveStream() {
    TVPReleaseCachedArchiveHandle(Owner, _instr);
    Owner->Release();
}
