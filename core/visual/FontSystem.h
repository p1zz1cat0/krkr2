
#ifndef __FONT_SYSTEM_H__
#define __FONT_SYSTEM_H__

#include "tjsCommHead.h"
#include "tvpfontstruc.h"
#include <string>

class FontSystem {
    tTVPFont DefaultFont;
    bool DefaultLOGFONTCreated;

    bool FontExists(const ttstr &name);

    void ConstructDefaultFont();

public:
    FontSystem();
    ttstr GetBeingFont(ttstr fonts);
    [[nodiscard]] const tTVPFont &GetDefaultFont() const { return DefaultFont; }
};

#endif // __FONT_SYSTEM_H__
