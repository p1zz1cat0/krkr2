#include "EnvironFileBackendInit.h"

#include "IFileBackend.h"

#if defined(TVP_HAS_MOBILE_FILE_BACKEND)
#include "MobileFileBackend.h"
#endif

//---------------------------------------------------------------------------
void TVPInitEnvironFileBackends() {
    if(TVPGetFileBackend())
        return;

#if defined(TVP_HAS_MOBILE_FILE_BACKEND)
    TVPSetFileBackend(std::make_unique<MobileFileBackend>());
#else
    TVPInitFileBackend();
#endif
}
