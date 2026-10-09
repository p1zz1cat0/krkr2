#pragma once

#include "CocosFileBackend.h"
#include "LocalFileBackend.h"

//---------------------------------------------------------------------------
// 移动端单一 backend：读时先查打包资源，再走本地文件（非链式回退注册）
//---------------------------------------------------------------------------
class MobileFileBackend : public IFileBackend {
    CocosFileBackend Cocos;
    LocalFileBackend Local;

public:
    std::unique_ptr<tTJSBinaryStream> Open(const ttstr &storageName,
                                           const ttstr &localName,
                                           tjs_uint32 flags) override;

    std::unique_ptr<tTJSBinaryStream> OpenRead(const ttstr &path) override;

    bool Exists(const ttstr &path) override;
};
