#pragma once
#include "StorageIntf.h"
#include "TextEncoding.h"

iTJSTextReadStream *TVPCreateTextStreamForRead(const ttstr &name,
                                               const ttstr &mode);
iTJSTextWriteStream *TVPCreateTextStreamForWrite(const ttstr &name,
                                                 const ttstr &mode);


void TVPSetDefaultReadEncoding(const ttstr &encoding);

const tjs_char *TVPGetDefaultReadEncoding();