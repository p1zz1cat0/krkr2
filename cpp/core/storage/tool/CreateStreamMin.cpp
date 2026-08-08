#include "tjsCommHead.h"

#include "IFileBackend.h"
#include "StdFileBackend.h"
#include "StorageIntf.h"

//---------------------------------------------------------------------------
// 工具链用最小 TVPCreateStream：仅支持本地文件，经 IFileBackend 访问
//---------------------------------------------------------------------------
static bool g_storageMinInitialized = false;

static void TVPEnsureStorageMinInitialized() {
    if(g_storageMinInitialized)
        return;
    TVPSetFileBackend(std::make_unique<StdFileBackend>());
    g_storageMinInitialized = true;
}

//---------------------------------------------------------------------------
tTJSBinaryStream *TVPCreateStream(const ttstr &_name, tjs_uint32 flags) {
    TVPEnsureStorageMinInitialized();

    IFileBackend *backend = TVPGetFileBackend();
    if(!backend)
        return nullptr;

    if(auto stream = backend->Open(_name, _name, flags))
        return stream.release();
    return nullptr;
}
