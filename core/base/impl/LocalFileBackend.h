#pragma once

#include "IFileBackend.h"

//---------------------------------------------------------------------------
// 本地文件系统后端（封装 tTVPLocalFileStream，支持读写）
//---------------------------------------------------------------------------
class LocalFileBackend : public IFileBackend {
public:
    std::unique_ptr<tTJSBinaryStream> Open(const ttstr &storageName,
                                           const ttstr &localName,
                                           tjs_uint32 flags) override;

    std::unique_ptr<tTJSBinaryStream> OpenRead(const ttstr &path) override;

    bool Exists(const ttstr &path) override;
};
