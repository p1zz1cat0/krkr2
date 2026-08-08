#include "ToolStorageInit.h"

#include "ArchiveRegistry.h"
#include "IFileBackend.h"
#include "StdFileBackend.h"
#include "XP3Archive.h"

//---------------------------------------------------------------------------
void TVPInitToolStorage() {
    TVPSetFileBackend(std::make_unique<StdFileBackend>());
    TVPInitToolArchiveCreators();
}
