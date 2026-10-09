#include "IFileBackend.h"

static std::unique_ptr<IFileBackend> g_fileBackend;

//---------------------------------------------------------------------------
void TVPSetFileBackend(std::unique_ptr<IFileBackend> backend) {
    g_fileBackend = std::move(backend);
}

//---------------------------------------------------------------------------
IFileBackend *TVPGetFileBackend() { return g_fileBackend.get(); }
