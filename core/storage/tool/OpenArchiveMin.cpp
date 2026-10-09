#include "tjsCommHead.h"

#include "StorageIntf.h"
#include "XP3Archive.h"

//---------------------------------------------------------------------------
// 工具链用 TVPOpenArchive：仅尝试 XP3 格式
//---------------------------------------------------------------------------
tTVPArchive *TVPOpenArchive(const ttstr &name, bool normalizeFileName) {
    tTJSBinaryStream *st = TVPCreateStream(name);
    if(!st)
        return nullptr;

    tTVPArchive *archive = tTVPXP3Archive::Create(name, st, normalizeFileName);
    if(archive)
        return archive;

    delete st;
    return nullptr;
}
