#include "tjsCommHead.h"

#include "StorageIntf.h"
#include "MsgIntf.h"

//---------------------------------------------------------------------------
// tTVPArchive（自 StorageIntf.cpp 提取，供工具链最小存储层使用）
//---------------------------------------------------------------------------
void tTVPArchive::NormalizeInArchiveStorageName(ttstr &name) {
    if(name.IsEmpty())
        return;

    tjs_char *ptr = name.Independ();
    while(*ptr) {
        if(*ptr >= TJS_W('A') && *ptr <= TJS_W('Z'))
            *ptr += TJS_W('a') - TJS_W('A');
        else if(*ptr == TJS_W('\\'))
            *ptr = TJS_W('/');
        ptr++;
    }

    ptr = name.Independ();
    tjs_char *org_ptr = ptr;
    tjs_char *dest = ptr;
    while(*ptr) {
        if(*ptr != TJS_W('/')) {
            *dest = *ptr;
            ptr++;
            dest++;
        } else {
            if(ptr != org_ptr) {
                *dest = *ptr;
                ptr++;
                dest++;
            }
            while(*ptr == TJS_W('/'))
                ptr++;
        }
    }
    *dest = 0;

    name.FixLen();
}

//---------------------------------------------------------------------------
void tTVPArchive::AddToHash() {
    const tjs_uint count = GetCount();
    for(tjs_uint i = 0; i < count; i++) {
        ttstr name = GetName(i);
        NormalizeInArchiveStorageName(name);
        Hash.Add(name, i);
    }
}

//---------------------------------------------------------------------------
tTJSBinaryStream *tTVPArchive::CreateStream(const ttstr &name) {
    if(name.IsEmpty())
        return nullptr;

    if(!Init) {
        Init = true;
        AddToHash();
    }

    tjs_uint *p = Hash.Find(name);
    if(!p)
        TVPThrowExceptionMessage(TVPStorageInArchiveNotFound, name,
                                 ArchiveName);

    return CreateStreamByIndex(*p);
}

//---------------------------------------------------------------------------
bool tTVPArchive::IsExistent(const ttstr &name) {
    if(name.IsEmpty())
        return false;

    if(!Init) {
        Init = true;
        AddToHash();
    }

    return Hash.Find(name) != nullptr;
}

//---------------------------------------------------------------------------
tjs_int tTVPArchive::GetFirstIndexStartsWith(const ttstr &prefix) {
    const tjs_uint total_count = GetCount();
    tjs_int s = 0, e = total_count;
    while(e - s > 1) {
        const tjs_int m = (e + s) / 2;
        if(!(GetName(m) < prefix)) {
            e = m;
        } else {
            s = m;
        }
    }
    if(s < total_count && GetName(s).StartsWith(prefix))
        return s;
    return -1;
}
