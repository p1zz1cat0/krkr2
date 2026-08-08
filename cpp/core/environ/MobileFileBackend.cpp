#include "MobileFileBackend.h"

//---------------------------------------------------------------------------
std::unique_ptr<tTJSBinaryStream>
MobileFileBackend::Open(const ttstr &storageName, const ttstr &localName,
                        tjs_uint32 flags) {
    if((flags & TJS_BS_ACCESS_MASK) == TJS_BS_READ) {
        if(auto stream = Cocos.Open(storageName, localName, flags))
            return stream;
    }
    return Local.Open(storageName, localName, flags);
}

//---------------------------------------------------------------------------
std::unique_ptr<tTJSBinaryStream>
MobileFileBackend::OpenRead(const ttstr &path) {
    if(auto stream = Cocos.OpenRead(path))
        return stream;
    return Local.OpenRead(path);
}

//---------------------------------------------------------------------------
bool MobileFileBackend::Exists(const ttstr &path) {
    return Cocos.Exists(path) || Local.Exists(path);
}
