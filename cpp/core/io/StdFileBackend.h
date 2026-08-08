#pragma once

#include "IFileBackend.h"

//---------------------------------------------------------------------------
// 使用 std::filesystem + stdio 的默认本地文件后端
//---------------------------------------------------------------------------
class StdFileBackend : public IFileBackend {
public:
    std::unique_ptr<tTJSBinaryStream> Open(const ttstr &storageName,
                                           const ttstr &localName,
                                           tjs_uint32 flags) override;
    std::unique_ptr<tTJSBinaryStream> OpenRead(const ttstr &path) override;
    bool Exists(const ttstr &path) override;
};
