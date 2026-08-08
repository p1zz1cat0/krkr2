#include "ArchiveRegistry.h"

#include "XP3Archive.h"

#include <vector>

static std::vector<TVPArchiveCreatorFn> g_archiveCreators;

//---------------------------------------------------------------------------
void TVPRegisterArchiveCreator(TVPArchiveCreatorFn creator) {
    if(creator)
        g_archiveCreators.push_back(creator);
}

//---------------------------------------------------------------------------
void TVPClearArchiveCreators() { g_archiveCreators.clear(); }

//---------------------------------------------------------------------------
void TVPInitToolArchiveCreators() {
    TVPClearArchiveCreators();
    TVPRegisterArchiveCreator(tTVPXP3Archive::Create);
}

//---------------------------------------------------------------------------
tTVPArchive *TVPOpenArchive(const ttstr &name, bool normalizeFileName) {
    if(g_archiveCreators.empty())
        TVPInitToolArchiveCreators();

    tTJSBinaryStream *st = TVPCreateStream(name);
    if(!st)
        return nullptr;

    for(auto creator : g_archiveCreators) {
        tTVPArchive *archive = creator(name, st, normalizeFileName);
        if(archive)
            return archive;
        st->SetPosition(0);
    }

    delete st;
    return nullptr;
}
