#include "SourceCache.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_set>
#include <vector>

#include <cocos2d.h>
#include "BitmapIntf.h"
#include "GraphicsLoaderIntf.h"
#include "EmoteCompatInternal.h"
#include "LayerBitmapIntf.h"
#include "LayerIntf.h"
#include "PlayerInternal.h"
#include "RenderManager.h"
#include "ResourceManager.h"
#include "ScriptMgnIntf.h"
#include "StorageIntf.h"
#include <spdlog/spdlog.h>

namespace {

    bool getObjectProperty(const tTJSVariant &object, const tjs_char *name,
                           tTJSVariant &out) {
        if(object.Type() != tvtObject || !object.AsObjectNoAddRef()) {
            return false;
        }
        return TJS_SUCCEEDED(object.AsObjectNoAddRef()->PropGet(
            0, name, nullptr, &out, object.AsObjectNoAddRef()));
    }

    std::optional<ttstr> sourceNameFromVariant(const tTJSVariant &value) {
        if(value.Type() == tvtVoid) {
            return std::nullopt;
        }
        if(value.Type() == tvtObject && value.AsObjectNoAddRef()) {
            for(const auto *name : { TJS_W("src"), TJS_W("key") }) {
                tTJSVariant prop;
                if(getObjectProperty(value, name, prop) &&
                   prop.Type() != tvtVoid) {
                    return ttstr(prop);
                }
            }
            return std::nullopt;
        }
        return ttstr(value);
    }

    bool packedColorsAreDefault(std::uint32_t c0, std::uint32_t c1,
                                std::uint32_t c2, std::uint32_t c3) {
        return c0 == 0xFF808080u && c1 == 0xFF808080u && c2 == 0xFF808080u &&
            c3 == 0xFF808080u;
    }

    bool packedColorsAreOpaqueWhite(std::uint32_t c0, std::uint32_t c1,
                                    std::uint32_t c2, std::uint32_t c3) {
        return (c0 & c1 & c2 & c3) == 0xFFFFFFFFu;
    }

    std::array<int, 4> unpackPackedRgba(std::uint32_t packedColor) {
        return {
            static_cast<int>(packedColor & 0xFFu),
            static_cast<int>((packedColor >> 8) & 0xFFu),
            static_cast<int>((packedColor >> 16) & 0xFFu),
            static_cast<int>((packedColor >> 24) & 0xFFu),
        };
    }

    void copyAndApplyPackedCornerTintLike_0x6A7518(
        const tTVPBaseBitmap &source, tTVPBaseBitmap &destination,
        const std::array<std::uint32_t, 4> &packedColors, bool halfAlphaBlend) {
        const auto c0 = packedColors[0];
        const auto c1 = packedColors[1];
        const auto c2 = packedColors[2];
        const auto c3 = packedColors[3];
        if(packedColorsAreDefault(c0, c1, c2, c3) ||
           packedColorsAreOpaqueWhite(c0, c1, c2, c3)) {
            return;
        }

        const auto topLeft = unpackPackedRgba(c0);
        const auto topRight = unpackPackedRgba(c1);
        const auto bottomRight = unpackPackedRgba(c2);
        const auto bottomLeft = unpackPackedRgba(c3);
        const int width = static_cast<int>(source.GetWidth());
        const int height = static_cast<int>(source.GetHeight());
        if(width <= 0 || height <= 0) {
            return;
        }

        const int colorDivisor = halfAlphaBlend ? 128 : 255;
        const bool uniform = c0 == c1 && c1 == c2 && c2 == c3;
        if(uniform) {
            const auto tint = topLeft;
            for(int y = 0; y < height; ++y) {
                const auto *srcRow = static_cast<const std::uint8_t *>(
                    source.GetScanLine(static_cast<tjs_uint>(y)));
                auto *dstRow = static_cast<std::uint8_t *>(
                    destination.GetScanLineForWrite(static_cast<tjs_uint>(y)));
                for(int x = 0; x < width; ++x) {
                    const auto *src = srcRow + static_cast<size_t>(x) * 4u;
                    auto *dst = dstRow + static_cast<size_t>(x) * 4u;
                    dst[2] = static_cast<std::uint8_t>(std::min(
                        255, tint[0] * static_cast<int>(src[2]) /
                            colorDivisor));
                    dst[1] = static_cast<std::uint8_t>(std::min(
                        255, tint[1] * static_cast<int>(src[1]) /
                            colorDivisor));
                    dst[0] = static_cast<std::uint8_t>(std::min(
                        255, tint[2] * static_cast<int>(src[0]) /
                            colorDivisor));
                    dst[3] = static_cast<std::uint8_t>(std::min(
                        255, tint[3] * static_cast<int>(src[3]) /
                            colorDivisor));
                }
            }
            return;
        }

        const int spanX = std::max(width - 1, 1);
        const int spanY = std::max(height - 1, 1);
        const auto lerpChannel = [](int a, int b, int pos, int span) -> int {
            if(span <= 0) {
                return a;
            }
            return a + (pos * (b - a)) / span;
        };

        for(int y = 0; y < height; ++y) {
            const auto *srcRow = static_cast<const std::uint8_t *>(
                source.GetScanLine(static_cast<tjs_uint>(y)));
            auto *row = static_cast<std::uint8_t *>(
                destination.GetScanLineForWrite(static_cast<tjs_uint>(y)));
            const int rowLeftR =
                lerpChannel(topLeft[0], bottomLeft[0], y, spanY);
            const int rowLeftG =
                lerpChannel(topLeft[1], bottomLeft[1], y, spanY);
            const int rowLeftB =
                lerpChannel(topLeft[2], bottomLeft[2], y, spanY);
            const int rowLeftA =
                lerpChannel(topLeft[3], bottomLeft[3], y, spanY);
            const int rowRightR =
                lerpChannel(topRight[0], bottomRight[0], y, spanY);
            const int rowRightG =
                lerpChannel(topRight[1], bottomRight[1], y, spanY);
            const int rowRightB =
                lerpChannel(topRight[2], bottomRight[2], y, spanY);
            const int rowRightA =
                lerpChannel(topRight[3], bottomRight[3], y, spanY);

            for(int x = 0; x < width; ++x) {
                auto *dst = row + static_cast<size_t>(x) * 4u;
                const auto *src = srcRow + static_cast<size_t>(x) * 4u;
                const int tintR = lerpChannel(rowLeftR, rowRightR, x, spanX);
                const int tintG = lerpChannel(rowLeftG, rowRightG, x, spanX);
                const int tintB = lerpChannel(rowLeftB, rowRightB, x, spanX);
                const int tintA = lerpChannel(rowLeftA, rowRightA, x, spanX);
                dst[2] = static_cast<std::uint8_t>(std::min(
                    255, tintR * static_cast<int>(src[2]) / colorDivisor));
                dst[1] = static_cast<std::uint8_t>(std::min(
                    255, tintG * static_cast<int>(src[1]) / colorDivisor));
                dst[0] = static_cast<std::uint8_t>(std::min(
                    255, tintB * static_cast<int>(src[0]) / colorDivisor));
                dst[3] = static_cast<std::uint8_t>(std::min(
                    255, tintA * static_cast<int>(src[3]) / colorDivisor));
            }
        }
    }

    void pushGraphicCandidates(std::vector<ttstr> &candidates,
                               const ttstr &base) {
        if(base.IsEmpty()) {
            return;
        }

        candidates.push_back(base);
        const auto raw = motion::detail::narrow(base);
        if(raw.find('.') != std::string::npos) {
            return;
        }

        static const char *exts[] = { ".png", ".webp", ".jpg",  ".jpeg",
                                      ".bmp", ".tlg",  ".pimg", ".psb" };
        for(const auto *ext : exts) {
            candidates.emplace_back(base + ttstr{ ext });
        }
    }

    ttstr resolveMotionSourcePathLike_0x6948E8(
        const motion::detail::MotionSnapshot &snapshot,
        const std::string &source) {
        if(source.empty() || motion::internal::isMotionCrossReference(source)) {
            return {};
        }

        std::vector<ttstr> candidates;
        const auto sourcePath = motion::detail::widen(source);
        const auto lastSlash = source.rfind('/');
        const auto baseName = lastSlash == std::string::npos
            ? source
            : source.substr(lastSlash + 1);

        const auto appendCandidatesForSnapshot =
            [&](const motion::detail::MotionSnapshot &snap) {
                pushGraphicCandidates(candidates, sourcePath);
                motion::detail::appendEmbeddedSourceCandidates(snap, source,
                                                               candidates);
                for(const auto &alias : snap.resourceAliases) {
                    const auto embeddedBase = ttstr{ TJS_W("psb://") } +
                        motion::detail::widen(alias) + TJS_W("/") + sourcePath;
                    pushGraphicCandidates(candidates, embeddedBase);
                }
                for(const auto &[resPath, ignored] : snap.resourcesByPath) {
                    (void)ignored;
                    const auto targetSuffix = "/" + baseName + "/pixel";
                    if(resPath.size() >= targetSuffix.size() &&
                       resPath.compare(resPath.size() - targetSuffix.size(),
                                       targetSuffix.size(),
                                       targetSuffix) == 0) {
                        for(const auto &alias : snap.resourceAliases) {
                            const auto psbPath = ttstr{ TJS_W("psb://") } +
                                motion::detail::widen(alias) + TJS_W("/") +
                                motion::detail::widen(resPath);
                            pushGraphicCandidates(candidates, psbPath);
                        }
                    }
                }
            };

        appendCandidatesForSnapshot(snapshot);
        for(const auto &attached : snapshot.attachedSnapshots) {
            if(attached) {
                appendCandidatesForSnapshot(*attached);
            }
        }

        std::unordered_set<std::string> seen;
        for(const auto &candidate : candidates) {
            const auto candidateKey = motion::detail::narrow(candidate);
            if(!seen.insert(candidateKey).second || candidate.IsEmpty()) {
                continue;
            }
            if(candidateKey.rfind("psb://", 0) == 0) {
                if(TVPIsExistentStorage(candidate)) {
                    return candidate;
                }
                continue;
            }
            if(const auto placed = TVPGetPlacedPath(candidate);
               !placed.IsEmpty()) {
                return placed;
            }
        }
        return {};
    }

    std::shared_ptr<tTVPBaseBitmap> loadGraphicBitmap(const ttstr &path) {
        if(path.IsEmpty()) {
            return nullptr;
        }

        ttstr loadPath = path;
        const auto pathString = motion::detail::narrow(path);
        if(pathString.rfind('.') == std::string::npos ||
           pathString.rfind('.') < pathString.rfind('/')) {
            loadPath = path + TJS_W(".png");
        }

        try {
            auto bmp = std::make_shared<tTVPBaseBitmap>(1, 1, 32);
            TVPLoadGraphic(bmp.get(), loadPath, TVP_clNone, 0, 0, glmNormal,
                           nullptr, nullptr);
            if(bmp->GetWidth() > 0 && bmp->GetHeight() > 0) {
                return bmp;
            }
        } catch(...) {
        }
        return nullptr;
    }

    std::shared_ptr<tTVPBaseBitmap>
    loadPsbBitmap(const motion::detail::MotionSnapshot &snapshot,
                  const std::string &sourceKey) {
        int width = 0;
        int height = 0;
        double originX = 0.0;
        double originY = 0.0;
        std::vector<std::uint8_t> decodedPixels;
        bool decodedPixelsAreBgra = false;
        const auto *resource = motion::internal::findPSBResourceBySourceName(
            snapshot, sourceKey, width, height, decodedPixels, originX, originY,
            &decodedPixelsAreBgra);
        if(!resource || width <= 0 || height <= 0 || resource->data.empty()) {
            if(auto logger = spdlog::get("plugin")) {
                logger->warn(
                    "loadPsbBitmap miss key={} resource={} size={}x{} data={}",
                    sourceKey, resource != nullptr, width, height,
                    resource ? resource->data.size() : 0);
            }
            return nullptr;
        }

        const auto &pixelData =
            decodedPixels.empty() ? resource->data : decodedPixels;
        auto bmp = std::make_shared<tTVPBaseBitmap>(
            static_cast<tjs_uint>(width), static_cast<tjs_uint>(height), 32);
        tTVPRect fillRect(0, 0, width, height);
        bmp->Fill(fillRect, 0x00000000);
        const auto *src = pixelData.data();
        for(int y = 0; y < height; ++y) {
            auto *row = static_cast<std::uint8_t *>(
                bmp->GetScanLineForWrite(static_cast<tjs_uint>(y)));
            for(int x = 0; x < width; ++x) {
                const size_t sourceIndex =
                    (static_cast<size_t>(y) * width + x) * 4u;
                if(sourceIndex + 3 >= pixelData.size()) {
                    break;
                }
                auto *dst = row + static_cast<size_t>(x) * 4u;
                if(decodedPixelsAreBgra) {
                    dst[0] = src[sourceIndex + 0];
                    dst[1] = src[sourceIndex + 1];
                    dst[2] = src[sourceIndex + 2];
                } else {
                    dst[0] = src[sourceIndex + 2];
                    dst[1] = src[sourceIndex + 1];
                    dst[2] = src[sourceIndex + 0];
                }
                dst[3] = src[sourceIndex + 3];
            }
        }
        return bmp;
    }

    tTJSNI_BaseLayer *resolveNativeLayer(iTJSDispatch2 *layerObject) {
        if(!layerObject) {
            return nullptr;
        }
        tTJSNI_BaseLayer *layer = nullptr;
        if(TJS_FAILED(layerObject->NativeInstanceSupport(
               TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
               reinterpret_cast<iTJSNativeInstance **>(&layer))) ||
           !layer) {
            return nullptr;
        }
        return layer;
    }

    bool getLayerClassVariant(tTJSVariant &layerClassVar) {
        iTJSDispatch2 *global = TVPGetScriptDispatch();
        if(!global) {
            return false;
        }
        const bool ok =
            TJS_SUCCEEDED(global->PropGet(0, TJS_W("Layer"), nullptr,
                                          &layerClassVar, global)) &&
            layerClassVar.Type() == tvtObject &&
            layerClassVar.AsObjectNoAddRef();
        global->Release();
        return ok;
    }

    iTJSDispatch2 *createLayerObject(const tTJSVariant &owner,
                                     iTJSDispatch2 *parentLayerObject) {
        if(owner.Type() != tvtObject || !owner.AsObjectNoAddRef()) {
            return nullptr;
        }

        tTJSVariant layerClassVar;
        if(!getLayerClassVariant(layerClassVar)) {
            return nullptr;
        }

        iTJSDispatch2 *created = nullptr;
        tTJSVariant ownerArg(owner);
        tTJSVariant parentArg = parentLayerObject
            ? tTJSVariant(parentLayerObject, parentLayerObject)
            : tTJSVariant();
        tTJSVariant *args[] = { &ownerArg, &parentArg };
        if(TJS_FAILED(layerClassVar.AsObjectNoAddRef()->CreateNew(
               0, nullptr, nullptr, &created, 2, args,
               layerClassVar.AsObjectNoAddRef()))) {
            return nullptr;
        }
        return created;
    }

    iTJSDispatch2 *ensureLayerObject(tTJSVariant &slot,
                                     const tTJSVariant &owner,
                                     iTJSDispatch2 *parentLayerObject,
                                     bool visible) {
        iTJSDispatch2 *layerObject =
            slot.Type() == tvtObject ? slot.AsObjectNoAddRef() : nullptr;
        if(!layerObject) {
            layerObject = createLayerObject(owner, parentLayerObject);
            if(!layerObject) {
                return nullptr;
            }
            slot = tTJSVariant(layerObject, layerObject);
            layerObject->Release();
            layerObject = slot.AsObjectNoAddRef();
        }

        auto *layer = resolveNativeLayer(layerObject);
        if(!layer) {
            return nullptr;
        }
        if(parentLayerObject) {
            if(auto *parentLayer = resolveNativeLayer(parentLayerObject);
               parentLayer && layer->GetParent() != parentLayer) {
                layer->SetParent(parentLayer);
            }
        }
        layer->SetType(ltAlpha);
        layer->SetVisible(visible);
        layer->SetAbsoluteOrderMode(false);
        return layerObject;
    }

    // Motion resources are decoded into tTVPBaseBitmap, whose backing texture
    // belongs to the software render manager. AssignMainImageWithUpdate would
    // share that texture with tTVPBaseTexture while the destination Layer
    // subsequently dispatches through the OpenGL render manager. OpenGL then
    // treats the software texture as tTVPOGLTexture2D and may call through an
    // incompatible vtable during operateAffine.
    //
    // Keep the renderer boundary explicit: let the Layer allocate its native
    // render-target texture, then upload the decoded pixels into it.
    bool assignBitmapToLayerLike_0x6948E8(tTJSNI_BaseLayer *sourceLayer,
                                          const iTVPBaseBitmap &src) {
        if(!sourceLayer || src.GetWidth() <= 0 || src.GetHeight() <= 0) {
            return false;
        }
        const auto pitch = src.GetPitchBytes();
        const auto *pixels = src.GetScanLine(0);
        if(!pixels || pitch <= 0) {
            return false;
        }
        if(!sourceLayer->GetHasImage()) {
            sourceLayer->SetHasImage(true);
        }
        sourceLayer->SetType(ltAlpha);
        sourceLayer->SetSize(src.GetWidth(), src.GetHeight());
        auto *target = sourceLayer->GetMainImage();
        if(!target) {
            return false;
        }
        target->Update(pixels, static_cast<unsigned int>(pitch), 0, 0,
                       static_cast<int>(src.GetWidth()),
                       static_cast<int>(src.GetHeight()));
        sourceLayer->SetClip(0, 0, src.GetWidth(), src.GetHeight());
        return true;
    }

} // namespace

namespace motion {

    SourceCache::SourceCache() = default;

    SourceCache::SourceCache(tTJSVariant owner, tjs_int layerType) {
        setLayerOwner(std::move(owner), layerType);
    }

    SourceCache::~SourceCache() { clearCache(); }

    void SourceCache::bindRuntime(detail::PlayerRuntime *runtime,
                                  ResourceManager *resourceManager) {
        _runtime = runtime;
        _resourceManager = resourceManager;
    }

    void SourceCache::setSelfObject(tTJSVariant selfObject) {
        _selfObject = std::move(selfObject);
    }

    void SourceCache::setLayerOwner(tTJSVariant owner, tjs_int layerType) {
        _owner = std::move(owner);
        _layerType = layerType;
        _primaryLayer.Clear();

        if(_owner.Type() == tvtObject && _owner.AsObjectNoAddRef()) {
            tTJSVariant primary;
            if(getObjectProperty(_owner, TJS_W("primaryLayer"), primary) &&
               primary.Type() == tvtObject && primary.AsObjectNoAddRef()) {
                _primaryLayer = primary;
            } else if(resolveNativeLayer(_owner.AsObjectNoAddRef())) {
                _primaryLayer = _owner;
            }
        }

        iTJSDispatch2 *parentLayer = _primaryLayer.Type() == tvtObject
            ? _primaryLayer.AsObjectNoAddRef()
            : nullptr;
        const tTJSVariant &layerOwner = _owner;
        if(layerOwner.Type() == tvtObject) {
            ensureLayerObject(_bufLayer, layerOwner, parentLayer, false);
        }
    }

    tTJSVariant SourceCache::loadSource(tTJSVariant keyOrSource,
                                        tTJSVariant currentSource) {
        auto name = sourceNameFromVariant(keyOrSource);
        if(!name || name->IsEmpty()) {
            name = sourceNameFromVariant(currentSource);
        }
        if(!name || name->IsEmpty()) {
            return {};
        }
        return loadSourceByName(*name, currentSource);
    }

    tTJSVariant
    SourceCache::loadSourceByName(const ttstr &name,
                                  const tTJSVariant &currentSource) {
        const auto key = detail::narrow(name);
        if(key.empty()) {
            return {};
        }

        if(auto *entry = findEntryByKey(key)) {
            if(entry->sourceObject.Type() != tvtVoid) {
                return entry->sourceObject;
            }
            return entry->rawSource;
        }

        std::string resolvedKey;
        auto rawSource = currentSource.Type() != tvtVoid
            ? currentSource
            : loadRawSourceVariant(name, resolvedKey);
        Entry entry;
        entry.key = key;
        entry.resolvedKey = resolvedKey.empty() ? key : resolvedKey;
        entry.rawSource = rawSource;
        _entries.push_front(std::move(entry));
        return rawSource;
    }

    tTJSVariant SourceCache::loadRenderSourceByName(
        const ttstr &name, const tTJSVariant &currentSource, int blendMode,
        const std::array<std::uint32_t, 4> &packedColors,
        iTJSDispatch2 *layerTreeOwnerObject, iTJSDispatch2 *parentLayerObject,
        const std::shared_ptr<detail::MotionSnapshot> &sourceMotion) {
        const auto key = detail::narrow(name);
        if(key.empty()) {
            return {};
        }

        if(layerTreeOwnerObject &&
           (_owner.Type() != tvtObject || _primaryLayer.Type() != tvtObject)) {
            setLayerOwner(
                tTJSVariant(layerTreeOwnerObject, layerTreeOwnerObject),
                _layerType);
        }
        if(parentLayerObject && _primaryLayer.Type() != tvtObject) {
            _primaryLayer = tTJSVariant(parentLayerObject, parentLayerObject);
        }

        // Cached entries must not re-resolve the raw source every frame.
        // Animated scenes keep ~180 cross-PSB references permanently
        // unresolvable; retrying loadRawSourceVariant (candidate walk +
        // storage lookups + module load) for each of them every frame was a
        // large hidden cost. Reuse the cached rawSource/resolvedKey and let
        // ensureEntryBackingBitmap's backingLoadAttempted guard make known
        // failures a cheap return.
        const auto effectiveMotion = sourceMotion
            ? sourceMotion
            : (_runtime ? _runtime->activeMotion : nullptr);
        const auto sourceIdentity = detail::renderSourceCacheIdentity(
            effectiveMotion ? effectiveMotion->path : std::string{}, key);
        std::string resolvedKey;
        tTJSVariant rawSource;
        Entry *entryPtr =
            findRenderEntry(key, blendMode, sourceIdentity);
        if(entryPtr) {
            if(entryPtr->packedColors == packedColors &&
               entryPtr->sourceObject.Type() == tvtObject &&
               entryPtr->sourceObject.AsObjectNoAddRef()) {
                return entryPtr->sourceObject;
            }
            resolvedKey = entryPtr->resolvedKey;
            rawSource = entryPtr->rawSource;
        } else {
            rawSource = currentSource.Type() != tvtVoid
                ? currentSource
                : loadRawSourceVariant(name, resolvedKey);
            entryPtr = &ensureEntry(key, resolvedKey.empty() ? key : resolvedKey,
                                    blendMode, packedColors, effectiveMotion,
                                    sourceIdentity);
            entryPtr->rawSource = rawSource;
        }
        auto &entry = *entryPtr;

        if(!ensureEntryBackingBitmap(entry, key, blendMode, packedColors,
                                     effectiveMotion)) {
            return entry.rawSource;
        }

        iTJSDispatch2 *parentLayer = parentLayerObject
            ? parentLayerObject
            : (_primaryLayer.Type() == tvtObject
                   ? _primaryLayer.AsObjectNoAddRef()
                   : nullptr);
        const tTJSVariant owner = _owner.Type() == tvtObject
            ? _owner
            : (layerTreeOwnerObject
                   ? tTJSVariant(layerTreeOwnerObject, layerTreeOwnerObject)
                   : tTJSVariant());
        auto *sourceLayerObject =
            ensureLayerObject(entry.sourceObject, owner, parentLayer, false);
        auto *sourceLayer = resolveNativeLayer(sourceLayerObject);
        if(!sourceLayerObject || !sourceLayer || !entry.backingBitmap ||
           !assignBitmapToLayerLike_0x6948E8(sourceLayer,
                                             *entry.backingBitmap)) {
            entry.sourceObject.Clear();
            return entry.rawSource;
        }

        return entry.sourceObject;
    }

    std::shared_ptr<tTVPBaseBitmap>
    SourceCache::loadRenderSourceBitmapByName(
        const ttstr &name, const tTJSVariant &currentSource, int blendMode,
        const std::array<std::uint32_t, 4> &packedColors,
        const std::shared_ptr<detail::MotionSnapshot> &sourceMotion) {
        const auto key = detail::narrow(name);
        if(key.empty()) {
            return nullptr;
        }

        // Keep the same source-identity and backing-bitmap cache as the Layer
        // API, but stop before ensureLayerObject/assignBitmapToLayer.  This is
        // the hot path used by the render-command executor.
        const auto effectiveMotion = sourceMotion
            ? sourceMotion
            : (_runtime ? _runtime->activeMotion : nullptr);
        const auto sourceIdentity = detail::renderSourceCacheIdentity(
            effectiveMotion ? effectiveMotion->path : std::string{}, key);
        std::string resolvedKey;
        tTJSVariant rawSource;
        Entry *entryPtr = findRenderEntry(key, blendMode, sourceIdentity);
        if(entryPtr) {
            resolvedKey = entryPtr->resolvedKey;
            rawSource = entryPtr->rawSource;
        } else {
            rawSource = currentSource.Type() != tvtVoid
                ? currentSource
                : loadRawSourceVariant(name, resolvedKey);
            entryPtr = &ensureEntry(key, resolvedKey.empty() ? key : resolvedKey,
                                    blendMode, packedColors, effectiveMotion,
                                    sourceIdentity);
            entryPtr->rawSource = rawSource;
        }

        auto &entry = *entryPtr;
        if(!ensureEntryBackingBitmap(entry, key, blendMode, packedColors,
                                     effectiveMotion)) {
            return nullptr;
        }
        return entry.backingBitmap;
    }

    iTVPTexture2D *SourceCache::loadRenderSourceTextureByName(
        const ttstr &name, const tTJSVariant &currentSource, int blendMode,
        const std::array<std::uint32_t, 4> &packedColors,
        const std::shared_ptr<detail::MotionSnapshot> &sourceMotion) {
        const auto key = detail::narrow(name);
        if(key.empty()) {
            return nullptr;
        }

        const auto effectiveMotion = sourceMotion
            ? sourceMotion
            : (_runtime ? _runtime->activeMotion : nullptr);
        const auto sourceIdentity = detail::renderSourceCacheIdentity(
            effectiveMotion ? effectiveMotion->path : std::string{}, key);
        std::string resolvedKey;
        tTJSVariant rawSource;
        Entry *entryPtr =
            findRenderEntry(key, blendMode, sourceIdentity);
        if(entryPtr) {
            if(entryPtr->packedColors == packedColors &&
               entryPtr->sourceTexture) {
                return entryPtr->sourceTexture;
            }
            resolvedKey = entryPtr->resolvedKey;
            rawSource = entryPtr->rawSource;
        } else {
            rawSource = currentSource.Type() != tvtVoid
                ? currentSource
                : loadRawSourceVariant(name, resolvedKey);
            entryPtr = &ensureEntry(key, resolvedKey.empty() ? key : resolvedKey,
                                    blendMode, packedColors, effectiveMotion,
                                    sourceIdentity);
            entryPtr->rawSource = rawSource;
        }
        auto &entry = *entryPtr;

        if(!ensureEntryBackingBitmap(entry, key, blendMode, packedColors,
                                     effectiveMotion)) {
            return nullptr;
        }
        if(entry.sourceTexture) {
            return entry.sourceTexture;
        }

        const auto width = entry.backingBitmap->GetWidth();
        const auto height = entry.backingBitmap->GetHeight();
        const auto pitch = entry.backingBitmap->GetPitchBytes();
        const auto *pixels = entry.backingBitmap->GetScanLine(0);
        if(!pixels || pitch <= 0 || width <= 0 || height <= 0) {
            return nullptr;
        }

        // D3DAdaptor_renderFromPlayer @ 0x6ADE24 passes a source texture
        // getter into 0x6ADFBC, so this path returns texture data directly
        // instead of materializing an intermediate SourceCache Layer.
        entry.sourceTexture = TVPGetRenderManager()->CreateTexture2D(
            pixels, pitch, width, height,
            entry.backingBitmap->Is8BPP() ? TVPTextureFormat::Gray
                                          : TVPTextureFormat::RGBA,
            RENDER_CREATE_TEXTURE_FLAG_ANY);
        // E-mote parts are painted illustration meshes. Nearest sampling
        // turns rotated/scaled silhouettes into stair-steps.
        if(entry.sourceTexture) {
            if(auto *adapter =
                   entry.sourceTexture->GetAdapterTexture(nullptr)) {
                adapter->setTexParameters(cocos2d::Texture2D::TexParams{
                    GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE,
                    GL_CLAMP_TO_EDGE });
            }
        }
        return entry.sourceTexture;
    }

    tTJSVariant SourceCache::findSource(ttstr name) {
        return loadSourceByName(name, {});
    }

    void SourceCache::clearCache() {
        for(auto &entry : _entries) {
            if(entry.sourceObject.Type() == tvtObject &&
               entry.sourceObject.AsObjectNoAddRef()) {
                if(auto *layer = resolveNativeLayer(
                       entry.sourceObject.AsObjectNoAddRef())) {
                    layer->SetHasImage(false);
                }
            }
            releaseEntryTexture(entry);
        }
        _entries.clear();
    }

    void SourceCache::eraseSource(ttstr name) {
        const auto key = detail::narrow(name);
        if(key.empty()) {
            return;
        }

        for(auto it = _entries.begin(); it != _entries.end();) {
            if(it->key == key || it->resolvedKey == key) {
                releaseEntryTexture(*it);
                it = _entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    tTJSVariant SourceCache::getBufLayer() const { return _bufLayer; }

    std::size_t SourceCache::size() const { return _entries.size(); }

    const SourceCache::Entry *SourceCache::findEntry(
        const std::string &key, int blendMode,
        const std::array<std::uint32_t, 4> &packedColors) const {
        for(const auto &entry : _entries) {
            if((entry.key == key || entry.resolvedKey == key) &&
               entry.blendMode == blendMode &&
               entry.packedColors == packedColors) {
                return &entry;
            }
        }
        return nullptr;
    }

    SourceCache::Entry *
    SourceCache::findEntry(const std::string &key, int blendMode,
                           const std::array<std::uint32_t, 4> &packedColors) {
        for(auto it = _entries.begin(); it != _entries.end(); ++it) {
            if((it->key == key || it->resolvedKey == key) &&
               it->blendMode == blendMode && it->packedColors == packedColors) {
                _entries.splice(_entries.begin(), _entries, it);
                return &_entries.front();
            }
        }
        return nullptr;
    }

    SourceCache::Entry *SourceCache::findRenderEntry(
        const std::string &key, int blendMode,
        const std::string &sourceIdentity) {
        for(auto it = _entries.begin(); it != _entries.end(); ++it) {
            if((it->key == key || it->resolvedKey == key) &&
               it->blendMode == blendMode &&
               it->sourceIdentity == sourceIdentity) {
                _entries.splice(_entries.begin(), _entries, it);
                return &_entries.front();
            }
        }
        return nullptr;
    }

    SourceCache::Entry *SourceCache::findEntryByKey(const std::string &key) {
        for(auto it = _entries.begin(); it != _entries.end(); ++it) {
            if(it->key == key || it->resolvedKey == key) {
                _entries.splice(_entries.begin(), _entries, it);
                return &_entries.front();
            }
        }
        return nullptr;
    }

    SourceCache::Entry &
    SourceCache::ensureEntry(const std::string &key,
                             const std::string &resolvedKey, int blendMode,
                             const std::array<std::uint32_t, 4> &packedColors,
                             const std::shared_ptr<detail::MotionSnapshot>
                                 &sourceMotion,
                             const std::string &sourceIdentity) {
        if(auto *entry =
               findRenderEntry(key, blendMode, sourceIdentity)) {
            return *entry;
        }

        Entry entry;
        entry.key = key;
        entry.resolvedKey = resolvedKey.empty() ? key : resolvedKey;
        entry.blendMode = blendMode;
        entry.sourceMotion = sourceMotion;
        entry.sourceIdentity = sourceIdentity;
        _entries.push_front(std::move(entry));
        return _entries.front();
    }

    bool SourceCache::ensureEntryBackingBitmap(
        Entry &entry, const std::string &key, int blendMode,
        const std::array<std::uint32_t, 4> &packedColors,
        const std::shared_ptr<detail::MotionSnapshot> &sourceMotion) {
        const auto *activeMotion = sourceMotion.get();
        if(entry.sourceMotion.get() != activeMotion) {
            releaseEntryTexture(entry);
            entry.baseBitmap.reset();
            entry.backingBitmap.reset();
            entry.sourceMotion = sourceMotion;
            entry.backingLoadAttempted = false;
        }

        const bool colorsChanged = entry.packedColors != packedColors;
        if(entry.backingBitmap && !colorsChanged) {
            return entry.backingBitmap->GetWidth() > 0 &&
                entry.backingBitmap->GetHeight() > 0;
        }

        if(!entry.baseBitmap && activeMotion) {
            if(entry.backingLoadAttempted) {
                return false;
            }
            entry.backingLoadAttempted = true;
            const auto loadStart = std::chrono::steady_clock::now();
            const auto path = resolveMotionSourcePathLike_0x6948E8(
                *activeMotion, key);
            entry.baseBitmap = loadGraphicBitmap(path);
            if(!entry.baseBitmap) {
                entry.baseBitmap = loadPsbBitmap(*activeMotion, key);
            }
            const double loadMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - loadStart)
                    .count();
            if(loadMs > 100.0) {
                if(auto logger = spdlog::get("plugin")) {
                    logger->warn("SourceCache decode slow key={} ms={:.1f}",
                                 key, loadMs);
                }
            }
        }
        if(!entry.baseBitmap || entry.baseBitmap->GetWidth() <= 0 ||
           entry.baseBitmap->GetHeight() <= 0) {
            return false;
        }

        if(colorsChanged) {
            releaseEntryTexture(entry);
        }
        entry.packedColors = packedColors;
        const bool useHalfAlphaTint = (blendMode & 0xF0) == 0x10;
        const bool needsTint =
            !packedColorsAreDefault(packedColors[0], packedColors[1],
                                    packedColors[2], packedColors[3]) &&
            !packedColorsAreOpaqueWhite(packedColors[0], packedColors[1],
                                        packedColors[2], packedColors[3]);
        if(needsTint) {
            if(!entry.backingBitmap ||
               entry.backingBitmap == entry.baseBitmap ||
               entry.backingBitmap->GetWidth() !=
                   entry.baseBitmap->GetWidth() ||
               entry.backingBitmap->GetHeight() !=
                   entry.baseBitmap->GetHeight()) {
                entry.backingBitmap = std::make_shared<tTVPBaseBitmap>(
                    static_cast<tjs_uint>(entry.baseBitmap->GetWidth()),
                    static_cast<tjs_uint>(entry.baseBitmap->GetHeight()), 32);
            }
            // Animated tint changes used to allocate a fresh full-size bitmap,
            // copy the source, then walk every pixel a second time. Reuse one
            // derivative and combine source copy plus tint in a single pass so
            // long-running character scenes do not churn hundreds of MB.
            copyAndApplyPackedCornerTintLike_0x6A7518(
                *entry.baseBitmap, *entry.backingBitmap, packedColors,
                useHalfAlphaTint);
        } else {
            entry.backingBitmap = entry.baseBitmap;
        }
        return true;
    }

    void SourceCache::releaseEntryTexture(Entry &entry) {
        if(entry.sourceTexture) {
            entry.sourceTexture->Release();
            entry.sourceTexture = nullptr;
        }
    }

    tTJSVariant
    SourceCache::loadRawSourceVariant(const ttstr &name,
                                      std::string &resolvedKey) const {
        resolvedKey.clear();
        if(!_runtime || !_resourceManager) {
            return {};
        }

        ttstr resolved;
        if(!detail::resolveExistingPath(
               internal::buildSourceCandidates(*_runtime, name), resolved)) {
            return {};
        }

        resolvedKey = detail::narrow(resolved);
        return _resourceManager->load(resolved);
    }

} // namespace motion
