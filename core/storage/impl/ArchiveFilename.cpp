#include "ArchiveFilename.h"

#include "MsgIntf.h"

extern int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption);

//---------------------------------------------------------------------------
void storeFilename(ttstr &name, const char *narrowName, const ttstr &filename) {
    tjs_int len = TJS_narrowtowidelen(narrowName);
    if(len == -1) {
        ttstr msg("Filename is not encoded in UTF8 in archive:\n");
        TVPShowSimpleMessageBox(msg + filename, TJS_W("Error"));
        TVPThrowExceptionMessage(TJS_W("Invalid archive entry name"));
    }
    tjs_char *p = name.AllocBuffer(len);
    p[TJS_narrowtowide(p, narrowName, len)] = 0;
    name.FixLen();
}
