#include "IFileBackend.h"

//---------------------------------------------------------------------------
std::unique_ptr<tTJSBinaryStream>
IFileBackend::Open(const ttstr & /*storageName*/, const ttstr &localName,
                   tjs_uint32 flags) {
    if((flags & TJS_BS_ACCESS_MASK) == TJS_BS_READ)
        return OpenRead(localName);
    return nullptr;
}

//---------------------------------------------------------------------------
std::unique_ptr<tTJSBinaryStream>
IFileBackend::OpenRead(const ttstr & /*path*/) {
    return nullptr;
}
