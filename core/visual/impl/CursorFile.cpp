//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Windows mouse cursor resources (.cur / .ani)
//---------------------------------------------------------------------------
#include "CursorFile.h"

#include <algorithm>
#include <cstring>

//---------------------------------------------------------------------------
// Byte helpers.  Cursor files are little endian on every platform.
//---------------------------------------------------------------------------
namespace {

constexpr int kMaxCursorSide = 1024;
constexpr size_t kMaxCursorSteps = 512;

tjs_uint16 ReadU16(const tjs_uint8 *p) {
    return (tjs_uint16)(p[0] | (p[1] << 8));
}

tjs_uint32 ReadU32(const tjs_uint8 *p) {
    return (tjs_uint32)p[0] | ((tjs_uint32)p[1] << 8) |
        ((tjs_uint32)p[2] << 16) | ((tjs_uint32)p[3] << 24);
}

int32_t ReadI32(const tjs_uint8 *p) { return (int32_t)ReadU32(p); }

//---------------------------------------------------------------------------
//! Decodes one DIB, the payload of a .cur / .ico entry.
//!
//! Cursor DIBs stack the colour bitmap and a 1 bpp AND mask vertically, so
//! biHeight counts both.  The colour bitmap is bottom-up and 4 byte aligned.
bool DecodeDibImage(const tjs_uint8 *data, size_t size, tTVPCursorImage &out) {
    if(size < 40)
        return false;
    if(ReadU32(data) != 40) // BITMAPINFOHEADER only
        return false;
    if(ReadU32(data + 16) != 0) // BI_RGB only
        return false;

    const int32_t width = ReadI32(data + 4);
    const int32_t rawHeight = ReadI32(data + 8);
    const tjs_uint16 bpp = ReadU16(data + 14);
    const tjs_uint32 paletteCount = ReadU32(data + 32);

    if(width <= 0 || width > kMaxCursorSide)
        return false;
    if(rawHeight <= 0 || rawHeight > 2 * kMaxCursorSide)
        return false;
    if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return false;

    // Icons double biHeight to make room for the AND mask.
    const bool hasMask = (rawHeight % 2) == 0;
    const int height = hasMask ? rawHeight / 2 : rawHeight;
    if(height <= 0 || height > kMaxCursorSide)
        return false;

    size_t offset = 40;
    tjs_uint32 palette[256];
    if(bpp <= 8) {
        const tjs_uint32 count =
            paletteCount ? paletteCount : (1u << bpp);
        if(count > 256 || offset + (size_t)count * 4 > size)
            return false;
        for(tjs_uint32 i = 0; i < count; i++) {
            const tjs_uint8 *p = data + offset + (size_t)i * 4;
            palette[i] = 0xff000000u | ((tjs_uint32)p[2] << 16) |
                ((tjs_uint32)p[1] << 8) | p[0];
        }
        offset += (size_t)count * 4;
    }

    const size_t xorPitch = (((size_t)width * bpp + 31) / 32) * 4;
    const size_t xorSize = xorPitch * (size_t)height;
    if(offset + xorSize > size)
        return false;

    const tjs_uint8 *xorBits = data + offset;
    const size_t andPitch = (((size_t)width + 31) / 32) * 4;
    const tjs_uint8 *andBits = nullptr;
    if(hasMask && offset + xorSize + andPitch * (size_t)height <= size)
        andBits = xorBits + xorSize;

    out.Width = width;
    out.Height = height;
    out.Pixels.assign((size_t)width * (size_t)height * 4, 0);

    bool anyAlpha = false;
    for(int y = 0; y < height; y++) {
        const tjs_uint8 *xorRow = xorBits + (size_t)(height - 1 - y) * xorPitch;
        tjs_uint8 *dst = out.Pixels.data() + (size_t)y * (size_t)width * 4;
        for(int x = 0; x < width; x++) {
            tjs_uint32 rgba;
            switch(bpp) {
                case 1: {
                    const tjs_uint8 bits = xorRow[x >> 3];
                    rgba = palette[(bits >> (7 - (x & 7))) & 1];
                    break;
                }
                case 4: {
                    const tjs_uint8 bits = xorRow[x >> 1];
                    rgba = palette[(x & 1) ? (bits & 0xf) : (bits >> 4)];
                    break;
                }
                case 8:
                    rgba = palette[xorRow[x]];
                    break;
                case 24: {
                    const tjs_uint8 *p = xorRow + (size_t)x * 3;
                    rgba = 0xff000000u | ((tjs_uint32)p[2] << 16) |
                        ((tjs_uint32)p[1] << 8) | p[0];
                    break;
                }
                default: {
                    const tjs_uint8 *p = xorRow + (size_t)x * 4;
                    rgba = ((tjs_uint32)p[3] << 24) |
                        ((tjs_uint32)p[2] << 16) | ((tjs_uint32)p[1] << 8) |
                        p[0];
                    break;
                }
            }
            if((rgba >> 24) != 0)
                anyAlpha = true;
            tjs_uint8 *pixel = dst + (size_t)x * 4;
            // rgba is A << 24 | R << 16 | G << 8 | B; consumers want RGBA8888.
            pixel[0] = (tjs_uint8)((rgba >> 16) & 0xff);
            pixel[1] = (tjs_uint8)((rgba >> 8) & 0xff);
            pixel[2] = (tjs_uint8)(rgba & 0xff);
            pixel[3] = (tjs_uint8)((rgba >> 24) & 0xff);
        }
    }

    // 32 bpp cursors carry their own alpha; the AND mask is a legacy leftover
    // there.  Older cursors leave the alpha at 0 and rely on the mask, so fall
    // back to it whenever the image has no alpha of its own.
    if(bpp < 32 || !anyAlpha) {
        for(int y = 0; y < height; y++) {
            const tjs_uint8 *andRow = andBits
                ? andBits + (size_t)(height - 1 - y) * andPitch
                : nullptr;
            tjs_uint8 *dst = out.Pixels.data() + (size_t)y * (size_t)width * 4;
            for(int x = 0; x < width; x++) {
                const bool masked =
                    andRow && ((andRow[x >> 3] >> (7 - (x & 7))) & 1);
                dst[(size_t)x * 4 + 3] = masked ? 0 : 0xff;
            }
        }
    }

    return true;
}

//---------------------------------------------------------------------------
//! Decodes one icon / cursor image: either a complete .ico / .cur payload
//! (directory plus entries) or a bare DIB, as ANI frames may hold both.
bool DecodeCursorImage(const tjs_uint8 *data, size_t size,
                       tTVPCursorImage &out, int &hotSpotX, int &hotSpotY,
                       bool &hasHotSpot) {
    hasHotSpot = false;

    if(size >= 6) {
        const tjs_uint16 reserved = ReadU16(data);
        const tjs_uint16 type = ReadU16(data + 2);
        const tjs_uint16 count = ReadU16(data + 4);
        if(reserved == 0 && (type == 1 || type == 2) && count > 0 &&
           size >= 6 + (size_t)count * 16) {
            // Games ship 16 / 32 / 48 px entries; keep the largest one, the
            // script only ever sees a single image.
            int best = -1;
            long bestArea = -1;
            for(tjs_uint16 i = 0; i < count; i++) {
                const tjs_uint8 *entry = data + 6 + (size_t)i * 16;
                const tjs_uint32 imageSize = ReadU32(entry + 8);
                const tjs_uint32 imageOffset = ReadU32(entry + 12);
                if(imageOffset > size || imageSize > size - imageOffset)
                    continue;
                const int w = entry[0] ? entry[0] : 256;
                const int h = entry[1] ? entry[1] : 256;
                const long area = (long)w * (long)h;
                if(area > bestArea) {
                    bestArea = area;
                    best = i;
                }
            }
            if(best < 0)
                return false;

            const tjs_uint8 *entry = data + 6 + (size_t)best * 16;
            const tjs_uint32 imageSize = ReadU32(entry + 8);
            const tjs_uint32 imageOffset = ReadU32(entry + 12);
            if(type == 2) {
                hotSpotX = ReadU16(entry + 4);
                hotSpotY = ReadU16(entry + 6);
                hasHotSpot = true;
            }
            return DecodeDibImage(data + imageOffset, imageSize, out);
        }
    }

    return DecodeDibImage(data, size, out);
}

void ClampHotSpot(tTVPCursorResource &resource) {
    const tTVPCursorImage &image = resource.Steps.front().Image;
    resource.HotSpotX = std::min(std::max(resource.HotSpotX, 0), image.Width - 1);
    resource.HotSpotY =
        std::min(std::max(resource.HotSpotY, 0), image.Height - 1);
}

//---------------------------------------------------------------------------
//! Decodes an ANI container: "anih" describes the frame count, the default
//! step rate and the flags, "rate" / "seq " drive the animation and the
//! "fram" list holds icon chunks.
bool DecodeAni(const tjs_uint8 *data, size_t size, tTVPCursorResource &out) {
    tjs_uint32 declaredSteps = 0;
    tjs_uint32 defaultRate = 0;
    std::vector<tTVPCursorImage> images;
    std::vector<tjs_uint32> rates;
    std::vector<tjs_uint32> sequence;
    bool hasHotSpot = false;

    size_t pos = 12; // "RIFF" size "ACON"
    while(pos + 8 <= size) {
        const tjs_uint8 *chunk = data + pos;
        const tjs_uint32 chunkSize = ReadU32(chunk + 4);
        const size_t body = pos + 8;
        if(chunkSize > size - body)
            break; // truncated chunk: keep what was parsed so far

        if(memcmp(chunk, "anih", 4) == 0 && chunkSize >= 36) {
            declaredSteps = ReadU32(data + body + 8);
            defaultRate = ReadU32(data + body + 28);
        } else if(memcmp(chunk, "rate", 4) == 0) {
            for(size_t i = 0; i + 4 <= chunkSize; i += 4)
                rates.push_back(ReadU32(data + body + i));
        } else if(memcmp(chunk, "seq ", 4) == 0) {
            for(size_t i = 0; i + 4 <= chunkSize; i += 4)
                sequence.push_back(ReadU32(data + body + i));
        } else if(memcmp(chunk, "LIST", 4) == 0 && chunkSize >= 4 &&
                  memcmp(data + body, "fram", 4) == 0) {
            const size_t framesEnd = body + chunkSize;
            size_t inner = body + 4;
            while(inner + 8 <= framesEnd) {
                const tjs_uint32 innerSize = ReadU32(data + inner + 4);
                const size_t innerBody = inner + 8;
                if(innerSize > framesEnd - innerBody)
                    break;
                if(memcmp(data + inner, "icon", 4) == 0) {
                    tTVPCursorImage image;
                    int hotX = 0, hotY = 0;
                    bool frameHasHotSpot = false;
                    if(DecodeCursorImage(data + innerBody, innerSize, image,
                                         hotX, hotY, frameHasHotSpot)) {
                        if(frameHasHotSpot && !hasHotSpot) {
                            out.HotSpotX = hotX;
                            out.HotSpotY = hotY;
                            hasHotSpot = true;
                        }
                        images.push_back(std::move(image));
                    } else {
                        // Keep undecodable frames so that "seq " indices keep
                        // pointing at the right images.
                        images.emplace_back();
                    }
                    if(images.size() >= kMaxCursorSteps)
                        break;
                }
                inner = innerBody + innerSize + (innerSize & 1);
            }
        }

        pos = body + chunkSize + (chunkSize & 1);
    }

    if(images.empty())
        return false;

    size_t steps = declaredSteps ? declaredSteps : images.size();
    if(!sequence.empty())
        steps = sequence.size();
    steps = std::min(steps, kMaxCursorSteps);

    for(size_t i = 0; i < steps; i++) {
        const size_t frame = i < sequence.size() ? sequence[i] : i;
        if(frame >= images.size() || images[frame].Pixels.empty())
            continue;
        // ANI rates are in 1/60 s (jiffies).
        const tjs_uint32 jiffies = i < rates.size() ? rates[i] : defaultRate;
        tTVPCursorStep step;
        step.Image = images[frame];
        step.DurationMS = jiffies ? (int)((jiffies * 1000u + 30u) / 60u) : 100;
        out.Steps.push_back(std::move(step));
    }

    if(out.Steps.empty())
        return false;

    ClampHotSpot(out);
    return true;
}

} // namespace

//---------------------------------------------------------------------------
bool TVPDecodeCursorFile(const void *data, size_t size, tTVPCursorResource &out) {
    out = tTVPCursorResource();
    if(!data || size < 6)
        return false;

    const tjs_uint8 *bytes = static_cast<const tjs_uint8 *>(data);
    if(size >= 12 && memcmp(bytes, "RIFF", 4) == 0 &&
       memcmp(bytes + 8, "ACON", 4) == 0)
        return DecodeAni(bytes, size, out);

    tTVPCursorImage image;
    int hotX = 0, hotY = 0;
    bool hasHotSpot = false;
    if(!DecodeCursorImage(bytes, size, image, hotX, hotY, hasHotSpot))
        return false;

    tTVPCursorStep step;
    step.Image = std::move(image);
    out.Steps.push_back(std::move(step));
    out.HotSpotX = hasHotSpot ? hotX : 0;
    out.HotSpotY = hasHotSpot ? hotY : 0;
    ClampHotSpot(out);
    return true;
}

//---------------------------------------------------------------------------
// Loaded-cursor registry.  Cursors are shared per storage path by
// TVPGetCursor(), so a handle stays valid for the lifetime of the process and
// only the script thread touches the pool.
//---------------------------------------------------------------------------
static std::vector<tTVPCursorResource> TVPCursorPool;

tjs_int TVPCursorRegister(tTVPCursorResource resource) {
    TVPCursorPool.push_back(std::move(resource));
    return TVPCursorHandleBase + (tjs_int)TVPCursorPool.size() - 1;
}

const tTVPCursorResource *TVPCursorFind(tjs_int handle) {
    if(handle < TVPCursorHandleBase)
        return nullptr;
    const size_t index = (size_t)(handle - TVPCursorHandleBase);
    if(index >= TVPCursorPool.size())
        return nullptr;
    return &TVPCursorPool[index];
}
