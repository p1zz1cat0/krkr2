#include "IFileBackend.h"
#include "LocalFileBackend.h"

//---------------------------------------------------------------------------
void TVPInitFileBackend() {
    if(TVPGetFileBackend())
        return;

    TVPSetFileBackend(std::make_unique<LocalFileBackend>());
}
