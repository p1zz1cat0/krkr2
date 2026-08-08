#include "EngineArchiveInit.h"

#include "ArchiveRegistry.h"
#include "XP3Archive.h"

extern tTVPArchive *TVPOpenZIPArchive(const ttstr &name, tTJSBinaryStream *st,
                                      bool normalizeFileName);
extern tTVPArchive *TVPOpen7ZArchive(const ttstr &name, tTJSBinaryStream *st,
                                     bool normalizeFileName);
extern tTVPArchive *TVPOpenTARArchive(const ttstr &name, tTJSBinaryStream *st,
                                      bool normalizeFileName);

//---------------------------------------------------------------------------
void TVPInitEngineArchiveCreators() {
    static bool initialized = false;
    if(initialized)
        return;
    initialized = true;

    TVPClearArchiveCreators();
    TVPRegisterArchiveCreator(TVPOpenZIPArchive);
    TVPRegisterArchiveCreator(TVPOpen7ZArchive);
    TVPRegisterArchiveCreator(TVPOpenTARArchive);
    TVPRegisterArchiveCreator(tTVPXP3Archive::Create);
}
