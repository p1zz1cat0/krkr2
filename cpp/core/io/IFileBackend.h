#pragma once

#include "tjsCommHead.h"
#include <memory>

//---------------------------------------------------------------------------
// 平台无关的本地文件访问抽象（Phase 1 骨架）
//---------------------------------------------------------------------------
struct IFileBackend {
    virtual ~IFileBackend() = default;

    // 完整打开（读写等）；不支持时返回 nullptr，由链上下一个 backend 处理
    virtual std::unique_ptr<tTJSBinaryStream>
    Open(const ttstr &storageName, const ttstr &localName, tjs_uint32 flags);

    virtual std::unique_ptr<tTJSBinaryStream> OpenRead(const ttstr &path);
    virtual bool Exists(const ttstr &path) = 0;
};

void TVPSetFileBackend(std::unique_ptr<IFileBackend> backend);
IFileBackend *TVPGetFileBackend();

// 由 base 实现：构建默认链（末尾为 LocalFileBackend）
void TVPInitFileBackend();
