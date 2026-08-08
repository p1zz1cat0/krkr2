#pragma once

#include "IFileBackend.h"

//---------------------------------------------------------------------------
// 经 cocos2d::FileUtils 读取打包资源（Android / iOS 等）
//---------------------------------------------------------------------------
class CocosFileBackend : public IFileBackend {
public:
    std::unique_ptr<tTJSBinaryStream> Open(const ttstr &storageName,
                                           const ttstr &localName,
                                           tjs_uint32 flags) override;
    std::unique_ptr<tTJSBinaryStream> OpenRead(const ttstr &path) override;
    bool Exists(const ttstr &path) override;

private:
    static std::string ResolvePath(const ttstr &path);
};
