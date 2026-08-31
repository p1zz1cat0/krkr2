// PlayerRenderInternal.cpp — shared render helpers moved from PlayerRender.cpp
// Split from PlayerRender.cpp for maintainability.
//
#include "PlayerRenderInternal.h"
#include "ConfigManager/IndividualConfigManager.h"
#include "MotionTraceWeb.h"
#include "RenderManager.h"
#include "ThreadIntf.h"

using namespace motion::internal;

#if defined(KRKR2_WASMTIME_HEADLESS)
extern "C" const char *TVPGetSoftwareAffinePathForWasmtime();
extern "C" const char *TVPGetSoftwareAffineRendererForWasmtime();
extern "C" int TVPGetSoftwareAffineAlphaBlendDReadyForWasmtime();
extern "C" int TVPGetSoftwareAffineTempFirstPixelValidForWasmtime();
extern "C" unsigned int TVPGetSoftwareAffineTempFirstPixelForWasmtime();
extern "C" int TVPGetSoftwareAffineTargetFirstPixelBeforeValidForWasmtime();
extern "C" unsigned int TVPGetSoftwareAffineTargetFirstPixelBeforeForWasmtime();
extern "C" int TVPGetSoftwareAffineTargetFirstPixelAfterValidForWasmtime();
extern "C" unsigned int TVPGetSoftwareAffineTargetFirstPixelAfterForWasmtime();
extern "C" int TVPGetSoftwareAffineAlphaBlendDProbeValidForWasmtime();
extern "C" unsigned int TVPGetSoftwareAffineAlphaBlendDProbePixelForWasmtime();
extern "C" int TVPGetSoftwareAffineAlphaBlendDCProbeValidForWasmtime();
extern "C" unsigned int TVPGetSoftwareAffineAlphaBlendDCProbePixelForWasmtime();
extern "C" int TVPGetSoftwareAffineAlphaBlendDPointsToCForWasmtime();
extern "C" int TVPGetSoftwareAffineRenderMethodOpacityForWasmtime();
extern "C" const char *TVPGetSoftwareAffineRenderMethodBranchForWasmtime();
#endif

namespace motion::internal::render_detail {

    tTJSNI_BaseLayer *resolveNativeLayer(iTJSDispatch2 *layerObject);

    bool getLayerClassDispatchVariantLike_0x5CB08C(tTJSVariant &layerClassVar) {
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

    tjs_error callLayerOperateAffineLike_0x6C7440(
        const tTJSVariant &layerClassObject, iTJSDispatch2 *renderLayerObject,
        const tTVPPointD *points, const tTJSVariant &sourceObject,
        const tTVPRect &sourceRect, tTVPBlendOperationMode blendMode,
        tjs_int opacity, tTVPBBStretchType type) {
        if(!renderLayerObject || !points ||
           layerClassObject.Type() != tvtObject ||
           !layerClassObject.AsObjectNoAddRef()) {
            return TJS_E_FAIL;
        }
        if(sourceObject.Type() != tvtObject ||
           !sourceObject.AsObjectNoAddRef()) {
            return TJS_E_NATIVECLASSCRASH;
        }

        tTJSVariant sourceArg(sourceObject);
        tTJSVariant srcLeft(sourceRect.left);
        tTJSVariant srcTop(sourceRect.top);
        tTJSVariant srcWidth(sourceRect.get_width());
        tTJSVariant srcHeight(sourceRect.get_height());
        tTJSVariant useAffineMatrix(false);
        tTJSVariant x0(points[0].x);
        tTJSVariant y0(points[0].y);
        tTJSVariant x1(points[1].x);
        tTJSVariant y1(points[1].y);
        tTJSVariant x2(points[2].x);
        tTJSVariant y2(points[2].y);
        tTJSVariant mode(static_cast<tjs_int32>(blendMode));
        tTJSVariant opa(static_cast<tjs_int32>(opacity));
        tTJSVariant stretchType(static_cast<tjs_int32>(type));

        tTJSVariant *args[] = {
            &sourceArg, &srcLeft,   &srcTop,
            &srcWidth,  &srcHeight, &useAffineMatrix,
            &x0,        &y0,        &x1,
            &y1,        &x2,        &y2,
            &mode,      &opa,       &stretchType,
        };

        static tjs_uint32 operateAffineHint = 0;
        // libkrkr2.so 0x6C7440 dispatches through the Layer class object and
        // passes the render layer only as objthis.
        return layerClassObject.AsObjectNoAddRef()->FuncCall(
            0, TJS_W("operateAffine"), &operateAffineHint, nullptr, 15, args,
            renderLayerObject);
    }

    std::array<int, 4> unpackPackedRgba(std::uint32_t packedColor) {
        return {
            static_cast<int>(packedColor & 0xFFu),
            static_cast<int>((packedColor >> 8) & 0xFFu),
            static_cast<int>((packedColor >> 16) & 0xFFu),
            static_cast<int>((packedColor >> 24) & 0xFFu),
        };
    }

    iTJSDispatch2 *
    resolvePrimaryLayerObject(iTJSDispatch2 *layerTreeOwnerObject) {
        if(!layerTreeOwnerObject) {
            return nullptr;
        }

        tTJSVariant ownerVar(layerTreeOwnerObject, layerTreeOwnerObject);
        tTJSVariant primaryVar;
        if(!getObjectProperty(ownerVar, TJS_W("primaryLayer"), primaryVar) ||
           primaryVar.Type() != tvtObject || !primaryVar.AsObjectNoAddRef()) {
            return nullptr;
        }

        if(auto *resolved = tryResolveLayerDispatch(primaryVar)) {
            return resolved;
        }
        return primaryVar.AsObjectNoAddRef();
    }

    iTJSDispatch2 *resolveMainWindowOwnerObject() {
        if(!TVPMainWindow) {
            return nullptr;
        }
        auto *owner = TVPMainWindow->GetOwnerNoAddRef();
        if(owner) {
            return owner;
        }

        iTJSDispatch2 *global = TVPGetScriptDispatch();
        if(!global) {
            return nullptr;
        }

        tTJSVariant windowClassVar;
        tTJSVariant mainWindowVar;
        iTJSDispatch2 *resolved = nullptr;
        if(TJS_SUCCEEDED(global->PropGet(0, TJS_W("Window"), nullptr,
                                         &windowClassVar, global)) &&
           windowClassVar.Type() == tvtObject &&
           windowClassVar.AsObjectNoAddRef() &&
           TJS_SUCCEEDED(windowClassVar.AsObjectNoAddRef()->PropGet(
               0, TJS_W("mainWindow"), nullptr, &mainWindowVar,
               windowClassVar.AsObjectNoAddRef())) &&
           mainWindowVar.Type() == tvtObject &&
           mainWindowVar.AsObjectNoAddRef()) {
            resolved = mainWindowVar.AsObjectNoAddRef();
        }

        global->Release();
        return resolved;
    }

    iTJSDispatch2 *resolveMainWindowPrimaryLayerObject() {
        return resolvePrimaryLayerObject(resolveMainWindowOwnerObject());
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

    // Try to resolve a source image path for the given source name in the
    // motion snapshot. Uses the same candidate generation logic as
    // loadMotionSourceImage but without OpenCV.
    ttstr
    resolveMotionSourcePath(const motion::detail::MotionSnapshot &snapshot,
                            const std::string &source) {
        if(source.empty() || isMotionCrossReference(source)) {
            return {};
        }

        std::vector<ttstr> candidates;
        const auto sourcePath = motion::detail::widen(source);
        const auto lastSlash = source.rfind('/');
        const auto baseName = (lastSlash != std::string::npos)
            ? source.substr(lastSlash + 1)
            : source;

        const auto appendCandidatesForSnapshot =
            [&](const motion::detail::MotionSnapshot &snap) {
                candidates.push_back(sourcePath);
                pushGraphicCandidates(candidates, sourcePath);
                motion::detail::appendEmbeddedSourceCandidates(snap, source,
                                                               candidates);
                for(const auto &alias : snap.resourceAliases) {
                    const auto embeddedBase = ttstr{ TJS_W("psb://") } +
                        motion::detail::widen(alias) + TJS_W("/") + sourcePath;
                    pushGraphicCandidates(candidates, embeddedBase);
                }
                for(const auto &[resPath, _] : snap.resourcesByPath) {
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

    iTJSDispatch2 *createLayerObject(iTJSDispatch2 *layerTreeOwnerObject,
                                     iTJSDispatch2 *parentLayerObject) {
        if(!layerTreeOwnerObject) {
            return nullptr;
        }

        tTJSVariant layerClassVar;
        iTJSDispatch2 *created = nullptr;
        const bool haveLayerClass =
            getLayerClassDispatchVariantLike_0x5CB08C(layerClassVar);
        if(haveLayerClass) {
            tTJSVariant ownerVar(layerTreeOwnerObject, layerTreeOwnerObject);
            tTJSVariant parentVar = parentLayerObject
                ? tTJSVariant(parentLayerObject, parentLayerObject)
                : tTJSVariant();
            tTJSVariant *args[] = { &ownerVar, &parentVar };
            if(TJS_FAILED(layerClassVar.AsObjectNoAddRef()->CreateNew(
                   0, nullptr, nullptr, &created, 2, args,
                   layerClassVar.AsObjectNoAddRef()))) {
                created = nullptr;
            }
        }

        return created;
    }

    bool configureReusableLayerObject(iTJSDispatch2 *layerObject,
                                      iTJSDispatch2 *parentLayerObject,
                                      tTVPLayerType layerType, bool visible,
                                      bool absoluteOrderMode) {
        auto *layer = resolveNativeLayer(layerObject);
        if(!layer) {
            return false;
        }

        if(parentLayerObject) {
            if(auto *parentLayer = resolveNativeLayer(parentLayerObject);
               parentLayer && layer->GetParent() != parentLayer) {
                layer->SetParent(parentLayer);
            }
        }

        layer->SetType(layerType);
        layer->SetAbsoluteOrderMode(absoluteOrderMode);
        layer->SetVisible(visible);
        return true;
    }

    iTJSDispatch2 *ensureReusableLayerObject(
        tTJSVariant &slot, iTJSDispatch2 *layerTreeOwnerObject,
        iTJSDispatch2 *parentLayerObject, tTVPLayerType layerType, bool visible,
        bool absoluteOrderMode) {
        if(!parentLayerObject && layerTreeOwnerObject) {
            parentLayerObject = resolvePrimaryLayerObject(layerTreeOwnerObject);
        }

        iTJSDispatch2 *layerObject =
            slot.Type() == tvtObject ? slot.AsObjectNoAddRef() : nullptr;
        if(!layerObject) {
            if(!layerTreeOwnerObject) {
                return nullptr;
            }
            layerObject =
                createLayerObject(layerTreeOwnerObject, parentLayerObject);
            if(!layerObject) {
                return nullptr;
            }
            slot = tTJSVariant(layerObject, layerObject);
            layerObject->Release();
            layerObject = slot.AsObjectNoAddRef();
        }

        if(!configureReusableLayerObject(layerObject, parentLayerObject,
                                         layerType, visible,
                                         absoluteOrderMode)) {
            return nullptr;
        }
        return layerObject;
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

    bool queryLayerCanvasSize(iTJSDispatch2 *layerObject, int &width,
                              int &height) {
        width = 0;
        height = 0;
        if(auto *layer = resolveNativeLayer(layerObject)) {
            width = static_cast<int>(layer->GetWidth());
            height = static_cast<int>(layer->GetHeight());
            if(width <= 0 || height <= 0) {
                width = static_cast<int>(layer->GetImageWidth());
                height = static_cast<int>(layer->GetImageHeight());
            }
        }
        return width > 0 && height > 0;
    }

    bool setObjectIntProperty(iTJSDispatch2 *object, const tjs_char *name,
                              tjs_int value) {
        if(!object) {
            return false;
        }
        tTJSVariant var(value);
        return TJS_SUCCEEDED(
            object->PropSet(TJS_MEMBERENSURE, name, nullptr, &var, object));
    }

    bool prepareLayerForRender(iTJSDispatch2 *layerObject, int width,
                               int height, tjs_uint32 clearColor) {
        auto *layer = resolveNativeLayer(layerObject);
        if(!layer || width <= 0 || height <= 0) {
            return false;
        }

        if(!layer->GetHasImage()) {
            layer->SetHasImage(true);
        }
        layer->SetImageSize(static_cast<tjs_uint>(width),
                            static_cast<tjs_uint>(height));
        layer->SetSize(width, height);
        layer->SetClip(0, 0, width, height);
        tTVPRect rect(0, 0, width, height);
        layer->FillRect(rect, clearColor);
        return true;
    }

    std::string summarizeLayerChildren(tTJSNI_BaseLayer *layer,
                                       int maxChildren) {
        if(!layer) {
            return "<null-layer>";
        }
        int visibleChildren = 0;
        const auto totalChildren = static_cast<int>(layer->GetCount());
        for(int i = 0; i < totalChildren; ++i) {
            auto *child = layer->GetChildren(i);
            if(child && child->GetVisible() && child->GetOpacity() != 0) {
                ++visibleChildren;
            }
        }
        std::string summary = fmt::format(
            "children={} visibleChildren={} selfVisible={} opacity={}",
            totalChildren, visibleChildren, layer->GetVisible() ? 1 : 0,
            layer->GetOpacity());
        const int count = std::min(totalChildren, maxChildren);
        for(int i = 0; i < count; ++i) {
            auto *child = layer->GetChildren(i);
            if(!child) {
                continue;
            }
            summary += fmt::format(
                " | [{}] ptr={} name={} vis={} nodeVis={} opacity={} "
                "size={}x{}",
                i, static_cast<const void *>(child),
                child->GetName().AsStdString(), child->GetVisible() ? 1 : 0,
                child->GetNodeVisible() ? 1 : 0, child->GetOpacity(),
                child->GetWidth(), child->GetHeight());
        }
        return summary;
    }

    tTVPBlendOperationMode
    resolveBlendOperationModeLike_0x6C7440(int rawBlendMode) {
        // libkrkr2.so 0x6C7440 does not pass the raw item blend flag through to
        // operateRect. It first maps the low 4 bits to the final TVP blend
        // operation mode: 1->0xE, 2/5->0xF, 3->0x10, 4->0x11, and the raw 0 /
        // default path ultimately composites with mode 2 in the common case.
        switch(rawBlendMode & 0x0F) {
            case 1:
                return omPsAdditive; // 0xE
            case 2:
            case 5:
                return omPsSubtractive; // 0xF
            case 3:
                return omPsMultiplicative; // 0x10
            case 4:
                return omPsScreen; // 0x11
            case 0:
            default:
                return omAlpha; // 0x2
        }
    }

    bool operateAffineBitmapWithoutLayerUpdate(
        tTJSNI_BaseLayer *target, const tTVPPointD *points,
        iTVPBaseBitmap *source, const tTVPRect &sourceRect,
        tTVPBlendOperationMode mode, tjs_int opacity,
        tTVPBBStretchType type) {
        if(!target || !target->GetMainImage() || !points || !source ||
           opacity <= 0) {
            return false;
        }

        tTVPBBBltMethod method;
        switch(mode) {
            case omPsNormal: method = bmPsNormal; break;
            case omPsAdditive: method = bmPsAdditive; break;
            case omPsSubtractive: method = bmPsSubtractive; break;
            case omPsMultiplicative: method = bmPsMultiplicative; break;
            case omPsScreen: method = bmPsScreen; break;
            case omPsOverlay: method = bmPsOverlay; break;
            case omPsHardLight: method = bmPsHardLight; break;
            case omPsSoftLight: method = bmPsSoftLight; break;
            case omPsColorDodge: method = bmPsColorDodge; break;
            case omPsColorDodge5: method = bmPsColorDodge5; break;
            case omPsColorBurn: method = bmPsColorBurn; break;
            case omPsLighten: method = bmPsLighten; break;
            case omPsDarken: method = bmPsDarken; break;
            case omPsDifference: method = bmPsDifference; break;
            case omPsDifference5: method = bmPsDifference5; break;
            case omPsExclusion: method = bmPsExclusion; break;
            case omAdditive: method = bmAdd; break;
            case omSubtractive: method = bmSub; break;
            case omMultiplicative: method = bmMul; break;
            case omDodge: method = bmDodge; break;
            case omDarken: method = bmDarken; break;
            case omLighten: method = bmLighten; break;
            case omScreen: method = bmScreen; break;
            case omAlpha:
                method = target->GetFace() == dfOpaque
                    ? bmAlpha
                    : (target->GetFace() == dfAddAlpha
                           ? bmAlphaOnAddAlpha
                           : bmAlphaOnAlpha);
                break;
            case omAddAlpha:
                method = target->GetFace() == dfOpaque
                    ? bmAddAlpha
                    : (target->GetFace() == dfAddAlpha
                           ? bmAddAlphaOnAddAlpha
                           : bmAddAlphaOnAlpha);
                break;
            case omOpaque:
                method = target->GetFace() == dfOpaque
                    ? bmCopy
                    : (target->GetFace() == dfAddAlpha
                           ? bmCopyOnAddAlpha
                           : bmCopyOnAlpha);
                break;
            default:
                return false;
        }

        tTVPRect updateRect;
        return target->GetMainImage()->AffineBlt(
            target->GetClip(), source, sourceRect, points, method, opacity,
            &updateRect, target->GetHoldAlpha(), type);
    }

    bool shouldUseDirectRenderPathLike_0x6C7440(
        const motion::detail::PlayerRuntime::PreparedRenderItem &item,
        bool clearEnabled) {
        const unsigned lowNibble =
            static_cast<unsigned>(item.blendMode) & 0x0Fu;
        // The native direct/buffered decision follows item+264: the prepared
        // render item's actual parent pointer. visibleAncestorIndex belongs
        // to the motion transform/visibility tree and can remain populated
        // for ordinary source layers that were never attached to a composite
        // render item. Treating that logical ancestor as a render parent
        // suppresses those layers before their affine copy is submitted.
        return !clearEnabled && item.parentItem == nullptr &&
            (lowNibble == 0u || lowNibble > 5u);
    }

    std::array<tTVPPointD, 3>
    buildAffineTrianglePoints(const std::array<float, 8> &corners,
                              float xOffset, float yOffset) {
        return { {
            { static_cast<double>(corners[0] + xOffset),
              static_cast<double>(corners[1] + yOffset) },
            { static_cast<double>(corners[2] + xOffset),
              static_cast<double>(corners[3] + yOffset) },
            { static_cast<double>(corners[6] + xOffset),
              static_cast<double>(corners[7] + yOffset) },
        } };
    }

    std::vector<tTVPPointD> buildMeshPoints(const std::vector<float> &points,
                                            float xOffset, float yOffset) {
        std::vector<tTVPPointD> result;
        result.reserve(points.size() / 2u);
        for(size_t i = 0; i + 1 < points.size(); i += 2) {
            result.push_back({
                static_cast<double>(points[i] + xOffset),
                static_cast<double>(points[i + 1] + yOffset),
            });
        }
        return result;
    }

    motion::D3DAdaptor *
    ensureSharedD3DAdaptor(iTJSDispatch2 *targetLayerObject) {
        (void)targetLayerObject;
        if(!TVPMainWindow) {
            return nullptr;
        }
        iTJSDispatch2 *windowObject = TVPMainWindow->GetOwnerNoAddRef();
        if(!windowObject) {
            return nullptr;
        }

        static std::unique_ptr<motion::D3DAdaptor> s_sharedAdaptor;
        if(!s_sharedAdaptor) {
            s_sharedAdaptor = std::make_unique<motion::D3DAdaptor>();
            // Player_drawCompat @ 0x6D5FB8 constructs the shared adaptor from
            // the main Window size, not from the destination Layer size.
            const int width = static_cast<int>(TVPMainWindow->GetWidth());
            const int height = static_cast<int>(TVPMainWindow->GetHeight());
            const auto halfLike_0x6D5FB8 = [](int value) {
                return (value >= 0 ? value : value + 1) >> 1;
            };
            tTJSVariant window(windowObject, windowObject);
            s_sharedAdaptor->initializeLike_0x6ADB10(window, width, height,
                                                     halfLike_0x6D5FB8(width),
                                                     halfLike_0x6D5FB8(height));
        }
        s_sharedAdaptor->setVisible(true);
        return s_sharedAdaptor.get();
    }

    bool computeRenderClipRect(
        const motion::detail::PlayerRuntime::PreparedRenderItem &entry,
        int canvasWidth, int canvasHeight, RenderClipRect &out,
        std::string *failureReason) {
        // REF intersects the paint box with the target canvas before creating
        // the command's scratch surface. Keeping off-screen coordinates here
        // makes a 1280x720 character allocate 1700px-tall buffers and then
        // throw most of the pixels away during the final copy.
        float clipLeft = std::max(0.0f, entry.paintBox[0]);
        float clipTop = std::max(0.0f, entry.paintBox[1]);
        float clipRight = std::min(static_cast<float>(canvasWidth),
                                   entry.paintBox[2]);
        float clipBottom = std::min(static_cast<float>(canvasHeight),
                                    entry.paintBox[3]);

        if(entry.hasViewport && entry.viewport[2] >= entry.viewport[0] &&
           entry.viewport[3] >= entry.viewport[1]) {
            clipLeft = std::max(clipLeft, floorf(entry.viewport[0]));
            clipTop = std::max(clipTop, floorf(entry.viewport[1]));
            clipRight = std::min(clipRight, ceilf(entry.viewport[2]));
            clipBottom = std::min(clipBottom, ceilf(entry.viewport[3]));
        }

        if(!(clipLeft < clipRight && clipTop < clipBottom)) {
            if(failureReason) {
                *failureReason = fmt::format(
                    "invalid_intersection "
                    "paintBox=[{:.3f},{:.3f},{:.3f},{:.3f}] viewport={}",
                    entry.paintBox[0], entry.paintBox[1], entry.paintBox[2],
                    entry.paintBox[3],
                    entry.hasViewport
                        ? fmt::format("[{:.3f},{:.3f},{:.3f},{:.3f}]",
                                      entry.viewport[0], entry.viewport[1],
                                      entry.viewport[2], entry.viewport[3])
                        : std::string("<invalid default>"));
            }
            return false;
        }

        // REF truncates the already canvas-clamped floating bounds.  Mixing
        // floor/ceil here makes an animated edge resize its scratch origin by
        // one pixel as it crosses an integer, which shows up as horizontal
        // seams and left/right jitter on face parts.
        out.left = static_cast<int>(clipLeft);
        out.top = static_cast<int>(clipTop);
        out.right = static_cast<int>(clipRight);
        out.bottom = static_cast<int>(clipBottom);
        if(failureReason) {
            failureReason->clear();
        }
        return out.left < out.right && out.top < out.bottom;
    }

    bool isAccurateSlaRenderEnabled() {
        auto *renderManager = TVPGetRenderManager();
        if(renderManager && renderManager->IsSoftware()) {
            return true;
        }
        auto *config = IndividualConfigManager::GetInstance();
        if(!config) {
            return false;
        }
        return config->GetValue<bool>("ogl_accurate_render", false);
    }

    tTVPRect localRectFromItem(
        const motion::detail::PlayerRuntime::PreparedRenderItem &item) {
        return tTVPRect(0, 0, item.clipRect[2] - item.clipRect[0],
                        item.clipRect[3] - item.clipRect[1]);
    }

    void persistNativeRenderItemFieldLifetimeLike_0x6C4E28(
        motion::detail::PlayerRuntime::PreparedRenderItem &item) {
        auto *owner = item.nativeLifetimeOwner;
        if(!owner) {
            return;
        }
        auto &state =
            owner->renderItemNativeFieldLifetimeByNode[item.nativeLifetimeKey];
        state.rawFlag20 = item.rawFlag20;
        state.rawFlag21 = item.rawFlag21;
        state.clipRect = item.clipRect;
        state.dirtyRect = item.dirtyRect;
        state.localCorners = item.localCorners;
        state.localMeshPoints = item.localMeshPoints;
    }

    bool clearLayerAlphaOutsideRect(tTJSNI_BaseLayer *layer,
                                    const tTVPRect &outerRect,
                                    const tTVPRect &innerRect) {
        if(!layer || !layer->GetMainImage()) {
            return false;
        }
        auto *bmp = layer->GetMainImage();
        if(outerRect.left >= outerRect.right ||
           outerRect.top >= outerRect.bottom) {
            return true;
        }

        auto clearMask = [&](const tTVPRect &rect) {
            if(rect.left < rect.right && rect.top < rect.bottom) {
                bmp->FillMask(rect, 0);
            }
        };

        clearMask(tTVPRect(outerRect.left, outerRect.top, innerRect.left,
                           outerRect.bottom));
        clearMask(tTVPRect(innerRect.right, outerRect.top, outerRect.right,
                           outerRect.bottom));
        clearMask(tTVPRect(
            std::max(outerRect.left, innerRect.left), outerRect.top,
            std::min(outerRect.right, innerRect.right), innerRect.top));
        clearMask(tTVPRect(
            std::max(outerRect.left, innerRect.left), innerRect.bottom,
            std::min(outerRect.right, innerRect.right), outerRect.bottom));
        return true;
    }

    int motionMaskThreadCount(int width, int height) {
        const auto pixelCount = static_cast<std::int64_t>(width) * height;
        if(pixelCount < 128 * 1024) {
            return 1;
        }
        return std::max(1, std::min(TVPGetThreadNum(), height));
    }

    bool applyMotionAlphaMaskLike_0x6AF104(
        iTJSDispatch2 *dstLayerObject, int dstX, int dstY,
        iTJSDispatch2 *srcLayerObject, int srcX, int srcY, int width,
        int height, int threshold, int playerStencilType, int itemFlags,
        const std::string &motionPath, double frameTime, int dstNodeIndex,
        int srcNodeIndex) {
        auto *dstLayer = resolveNativeLayer(dstLayerObject);
        auto *srcLayer = resolveNativeLayer(srcLayerObject);
        if(!dstLayer || !srcLayer || !dstLayer->GetHasImage() ||
           !srcLayer->GetHasImage() || !dstLayer->GetMainImage() ||
           !srcLayer->GetMainImage()) {
            return false;
        }

        auto *dstBmp = dstLayer->GetMainImage();
        auto *srcBmp = srcLayer->GetMainImage();
        const auto &dstClip = dstLayer->GetClip();
        const int dstImageWidth = static_cast<int>(dstLayer->GetImageWidth());
        const int dstImageHeight = static_cast<int>(dstLayer->GetImageHeight());
        const int srcImageWidth = static_cast<int>(srcLayer->GetImageWidth());
        const int srcImageHeight = static_cast<int>(srcLayer->GetImageHeight());

        const int requestedLeft = dstX;
        const int requestedTop = dstY;
        const int requestedRight = dstX + width;
        const int requestedBottom = dstY + height;

        if(dstClip.left > dstX) {
            srcX += dstClip.left - dstX;
            width -= dstClip.left - dstX;
            dstX = dstClip.left;
        }
        if(dstClip.top > dstY) {
            srcY += dstClip.top - dstY;
            height -= dstClip.top - dstY;
            dstY = dstClip.top;
        }
        if(srcX < 0) {
            dstX -= srcX;
            width += srcX;
            srcX = 0;
        }
        if(srcY < 0) {
            dstY -= srcY;
            height += srcY;
            srcY = 0;
        }

        const int dstLimitRight = std::min(dstClip.right, dstImageWidth);
        const int dstLimitBottom = std::min(dstClip.bottom, dstImageHeight);
        if(dstX + width > dstLimitRight) {
            width = dstLimitRight - dstX;
        }
        if(dstY + height > dstLimitBottom) {
            height = dstLimitBottom - dstY;
        }
        if(srcX + width > srcImageWidth) {
            width = srcImageWidth - srcX;
        }
        if(srcY + height > srcImageHeight) {
            height = srcImageHeight - srcY;
        }

        const tTVPRect requestedRect(std::max(requestedLeft, dstClip.left),
                                     std::max(requestedTop, dstClip.top),
                                     std::min(requestedRight, dstLimitRight),
                                     std::min(requestedBottom, dstLimitBottom));
        const tTVPRect overlapRect(dstX, dstY, dstX + width, dstY + height);

        if((itemFlags & 3) == 1) {
            clearLayerAlphaOutsideRect(dstLayer, requestedRect, overlapRect);
        }

        if(width <= 0 || height <= 0) {
            return true;
        }

        auto *dstBase = static_cast<std::uint8_t *>(
            dstBmp->GetScanLineForWrite(0));
        const auto *srcBase = static_cast<const std::uint8_t *>(
            srcBmp->GetScanLine(0));
        const int dstPitch = dstBmp->GetPitchBytes();
        const int srcPitch = srcBmp->GetPitchBytes();
        const int threadCount = motionMaskThreadCount(width, height);
        TVPExecThreadTask(threadCount, [&](int taskIndex) {
            const int y0 = height * taskIndex / threadCount;
            const int y1 = height * (taskIndex + 1) / threadCount;
            for(int y = y0; y < y1; ++y) {
                auto *dstRow = dstBase + (dstY + y) * dstPitch + dstX * 4;
                const auto *srcRow = srcBase + (srcY + y) * srcPitch + srcX * 4;
                for(int x = 0; x < width; ++x) {
                    auto *dstPixel = dstRow + x * 4;
                    const auto *srcPixel = srcRow + x * 4;
                    dstPixel[3] = detail::applyMotionMaskAlpha(
                        dstPixel[3], srcPixel[3], itemFlags, playerStencilType,
                        threshold);
                }
            }
        });

        motion::detail::logoChainTraceLogf(
            motionPath, "execute.mask", "0x6AF104", frameTime,
            "dstNode={} srcNode={} itemFlags={} playerStencilType={} "
            "threshold={} requested=[{},{},{},{}] overlap=[{},{},{},{}]",
            dstNodeIndex, srcNodeIndex, itemFlags, playerStencilType, threshold,
            requestedRect.left, requestedRect.top, requestedRect.right,
            requestedRect.bottom, overlapRect.left, overlapRect.top,
            overlapRect.right, overlapRect.bottom);
        return true;
    }

    bool applyMotionCompositeMasksLike_0x6AF104(
        iTJSDispatch2 *dstLayerObject, int dstWorldLeft, int dstWorldTop,
        int width, int height,
        const std::vector<MotionCompositeMaskSurface> &surfaces, int threshold,
        int playerStencilType, int compositeFlags,
        const std::string &motionPath, double frameTime, int dstNodeIndex) {
        auto *dstLayer = resolveNativeLayer(dstLayerObject);
        if(!dstLayer || !dstLayer->GetHasImage() ||
           !dstLayer->GetMainImage() || surfaces.empty() ||
           ((compositeFlags & 3) != 1 && (compositeFlags & 3) != 2)) {
            return false;
        }

        struct ResolvedSurface {
            const tTVPBaseTexture *bitmap = nullptr;
            int worldLeft = 0;
            int worldTop = 0;
            int width = 0;
            int height = 0;
        };
        std::vector<ResolvedSurface> resolved;
        resolved.reserve(surfaces.size());
        for(const auto &surface : surfaces) {
            auto *layer = resolveNativeLayer(surface.layerObject);
            auto *bitmap = layer ? layer->GetMainImage() : nullptr;
            if(!layer || !layer->GetHasImage() || !bitmap ||
               surface.width <= 0 || surface.height <= 0) {
                continue;
            }
            resolved.push_back({ bitmap, surface.worldLeft, surface.worldTop,
                                 std::min(surface.width,
                                          static_cast<int>(bitmap->GetWidth())),
                                 std::min(surface.height,
                                          static_cast<int>(bitmap->GetHeight())) });
        }
        if(resolved.empty()) {
            return false;
        }

        auto *dstBitmap = dstLayer->GetMainImage();
        width = std::min(width, static_cast<int>(dstBitmap->GetWidth()));
        height = std::min(height, static_cast<int>(dstBitmap->GetHeight()));
        if(width <= 0 || height <= 0) {
            return false;
        }

        auto *dstBase = static_cast<std::uint8_t *>(
            dstBitmap->GetScanLineForWrite(0));
        const int dstPitch = dstBitmap->GetPitchBytes();
        const int threadCount = motionMaskThreadCount(width, height);
        TVPExecThreadTask(threadCount, [&](int taskIndex) {
            const int y0 = height * taskIndex / threadCount;
            const int y1 = height * (taskIndex + 1) / threadCount;
            std::vector<const std::uint8_t *> sourceRows(resolved.size());
            for(int y = y0; y < y1; ++y) {
                const int worldY = dstWorldTop + y;
                for(size_t surfaceIndex = 0; surfaceIndex < resolved.size();
                    ++surfaceIndex) {
                    const auto &surface = resolved[surfaceIndex];
                    const int sourceY = worldY - surface.worldTop;
                    if(sourceY >= 0 && sourceY < surface.height) {
                        sourceRows[surfaceIndex] =
                            static_cast<const std::uint8_t *>(
                                surface.bitmap->GetScanLine(sourceY));
                    } else {
                        sourceRows[surfaceIndex] = nullptr;
                    }
                }
                auto *dstRow = dstBase + y * dstPitch;
                for(int x = 0; x < width; ++x) {
                    const int worldX = dstWorldLeft + x;
                    std::uint8_t unionAlpha = 0;
                    for(size_t surfaceIndex = 0;
                        surfaceIndex < resolved.size(); ++surfaceIndex) {
                        const auto &surface = resolved[surfaceIndex];
                        const int sourceX = worldX - surface.worldLeft;
                        const auto *sourceRow = sourceRows[surfaceIndex];
                        if(!sourceRow || sourceX < 0 ||
                           sourceX >= surface.width) {
                            continue;
                        }
                        unionAlpha = detail::unionMotionMaskAlpha(
                            unionAlpha, sourceRow[sourceX * 4 + 3],
                            playerStencilType, threshold);
                        if(unionAlpha == 255) {
                            break;
                        }
                    }
                    auto &dstAlpha = dstRow[x * 4 + 3];
                    dstAlpha = detail::applyMotionCompositeMaskAlpha(
                        dstAlpha, unionAlpha, compositeFlags, playerStencilType,
                        threshold);
                }
            }
        });

        detail::logoChainTraceLogf(
            motionPath, "execute.compositeMask", "0x6AF104", frameTime,
            "dstNode={} flags={} operation={} surfaces={} world=[{},{},{},{}]",
            dstNodeIndex, compositeFlags, compositeFlags & 3, resolved.size(),
            dstWorldLeft, dstWorldTop, dstWorldLeft + width,
            dstWorldTop + height);
        return true;
    }

#if defined(KRKR2_WASMTIME_HEADLESS)
    struct FirstPixelProbe {
        bool ok = false;
        std::uint32_t bgra = 0;
        int b = 0;
        int g = 0;
        int r = 0;
        int a = 0;
        int x = 0;
        int y = 0;
    };

    FirstPixelProbe readPixelForDiagnostics(const iTVPBaseBitmap *bitmap, int x,
                                            int y) {
        FirstPixelProbe probe;
        probe.x = x;
        probe.y = y;
        if(!bitmap || bitmap->GetWidth() <= 0 || bitmap->GetHeight() <= 0) {
            return probe;
        }
        if(x < 0 || y < 0 || x >= static_cast<int>(bitmap->GetWidth()) ||
           y >= static_cast<int>(bitmap->GetHeight())) {
            return probe;
        }
        const auto *row = static_cast<const std::uint8_t *>(
            bitmap->GetScanLine(static_cast<tjs_uint>(y)));
        if(!row) {
            return probe;
        }
        std::memcpy(&probe.bgra, row + static_cast<size_t>(x) * 4u,
                    sizeof(probe.bgra));
        probe.b = static_cast<int>(probe.bgra & 0xffu);
        probe.g = static_cast<int>((probe.bgra >> 8) & 0xffu);
        probe.r = static_cast<int>((probe.bgra >> 16) & 0xffu);
        probe.a = static_cast<int>((probe.bgra >> 24) & 0xffu);
        probe.ok = true;
        return probe;
    }

    FirstPixelProbe readFirstPixelForDiagnostics(const iTVPBaseBitmap *bitmap) {
        return readPixelForDiagnostics(bitmap, 0, 0);
    }

    void appendPointerJson(std::string &out, const char *name,
                           const void *ptr) {
        out += ",\"";
        out += name;
        out += "\":";
        if(ptr) {
            out += "\"";
            out += fmt::format("{}", ptr);
            out += "\"";
        } else {
            out += "null";
        }
    }

    void appendPixelProbeJson(std::string &out, const char *name,
                              const FirstPixelProbe &probe) {
        out += fmt::format(",\"{}\":{{\"ok\":{},\"x\":{},\"y\":{},\"bgra\":"
                           "\"0x{:08x}\",\"b\":{},\"g\":{},\"r\":{},\"a\":{}}}",
                           name, probe.ok ? "true" : "false", probe.x, probe.y,
                           probe.bgra, probe.b, probe.g, probe.r, probe.a);
    }

    void appendPixelSamplesJson(std::string &out, const char *name,
                                const std::vector<FirstPixelProbe> &samples) {
        out += ",\"";
        out += name;
        out += "\":[";
        for(size_t i = 0; i < samples.size(); ++i) {
            const auto &probe = samples[i];
            if(i != 0) {
                out += ",";
            }
            out += fmt::format("{{\"ok\":{},\"x\":{},\"y\":{},\"bgra\":\"0x{:"
                               "08x}\",\"b\":{},\"g\":{},\"r\":{},\"a\":{}}}",
                               probe.ok ? "true" : "false", probe.x, probe.y,
                               probe.bgra, probe.b, probe.g, probe.r, probe.a);
        }
        out += "]";
    }

    template <size_t N>
    void appendFloatArrayJson(std::string &out, const char *name,
                              const std::array<float, N> &values) {
        out += ",\"";
        out += name;
        out += "\":[";
        for(size_t i = 0; i < values.size(); ++i) {
            if(i != 0) {
                out += ",";
            }
            out += fmt::format("{:.9g}", values[i]);
        }
        out += "]";
    }

    void appendPointArrayJson(std::string &out, const char *name,
                              const std::array<tTVPPointD, 3> &points) {
        out += ",\"";
        out += name;
        out += "\":[";
        for(size_t i = 0; i < points.size(); ++i) {
            if(i != 0) {
                out += ",";
            }
            out += fmt::format("[{:.17g},{:.17g}]", points[i].x, points[i].y);
        }
        out += "]";
    }

    const char *bltMethodNameForDiagnostics(tTVPBBBltMethod method) {
        switch(method) {
            case bmCopy:
                return "bmCopy";
            case bmCopyOnAlpha:
                return "bmCopyOnAlpha";
            case bmAlpha:
                return "bmAlpha";
            case bmAlphaOnAlpha:
                return "bmAlphaOnAlpha";
            case bmAddAlphaOnAlpha:
                return "bmAddAlphaOnAlpha";
            case bmAlphaOnAddAlpha:
                return "bmAlphaOnAddAlpha";
            case bmCopyOnAddAlpha:
                return "bmCopyOnAddAlpha";
            default:
                return "other";
        }
    }

    void emitDirectExecuteDiagnostics(
        motion::Player *player, const char *samplePoint, const char *probePhase,
        const char *branch, const char *executionMethod,
        const motion::detail::PlayerRuntime::PreparedRenderItem &item,
        tTJSNI_BaseLayer *renderLayer,
        const std::shared_ptr<tTVPBaseBitmap> &srcBmp,
        iTJSDispatch2 *sourceArgObject, tTJSNI_BaseLayer *sourceArgLayer,
        const char *sourceArgClass, tTVPBlendOperationMode blendMode,
        tjs_int opacity, tTVPBBStretchType type) {
        tTVPBBBltMethod bltMethod = bmCopy;
        const bool bltMethodOk = renderLayer &&
            renderLayer->ResolveBltMethodForDiagnostics(bltMethod, blendMode);
        const iTVPBaseBitmap *sourceImage = sourceArgLayer
            ? static_cast<const iTVPBaseBitmap *>(
                  sourceArgLayer->GetMainImage())
            : static_cast<const iTVPBaseBitmap *>(srcBmp.get());
        auto *targetImage = renderLayer ? renderLayer->GetMainImage() : nullptr;
        const auto sourcePixel = readFirstPixelForDiagnostics(sourceImage);
        const auto targetPixel = readFirstPixelForDiagnostics(targetImage);
        const auto affinePointArgs =
            buildAffineTrianglePoints(item.corners, -0.5f, -0.5f);
        std::vector<FirstPixelProbe> sourcePixelSamples;
        for(const auto &[x, y] : {
                std::pair<int, int>{ 0, 0 },
                std::pair<int, int>{ 1, 42 },
                std::pair<int, int>{ 2, 42 },
                std::pair<int, int>{ 3, 42 },
                std::pair<int, int>{ 1, 43 },
                std::pair<int, int>{ 2, 43 },
                std::pair<int, int>{ 3, 43 },
                std::pair<int, int>{ 1, 49 },
                std::pair<int, int>{ 3, 49 },
                std::pair<int, int>{ 1, 50 },
                std::pair<int, int>{ 3, 50 },
            }) {
            sourcePixelSamples.push_back(
                readPixelForDiagnostics(sourceImage, x, y));
        }
        std::vector<FirstPixelProbe> targetPixelSamples;
        for(const auto &[x, y] : {
                std::pair<int, int>{ 725, 693 },
                std::pair<int, int>{ 725, 694 },
                std::pair<int, int>{ 725, 695 },
                std::pair<int, int>{ 725, 696 },
                std::pair<int, int>{ 725, 697 },
                std::pair<int, int>{ 726, 700 },
                std::pair<int, int>{ 726, 701 },
            }) {
            targetPixelSamples.push_back(
                readPixelForDiagnostics(targetImage, x, y));
        }

        std::string payload;
        payload += fmt::format(
            "\"probePhase\":\"{}\",\"branch\":\"{}\","
            "\"executionMethod\":\"{}\",\"nodeIndex\":{},"
            "\"meshType\":{},\"blendMode\":{},\"opacity\":{},\"stretchType\":{}"
            ","
            "\"targetFace\":{},\"targetDrawFace\":{},\"targetHoldAlpha\":{},"
            "\"resolvedBltMethodOk\":{},\"resolvedBltMethod\":{},"
            "\"resolvedBltMethodName\":\"{}\"",
            probePhase ? probePhase : "", branch ? branch : "",
            executionMethod ? executionMethod : "", item.nodeIndex,
            item.meshType, static_cast<int>(blendMode), opacity,
            static_cast<int>(type),
            renderLayer ? static_cast<int>(renderLayer->GetFace()) : -1,
            renderLayer
                ? static_cast<int>(renderLayer->GetDrawFaceForDiagnostics())
                : -1,
            renderLayer && renderLayer->GetHoldAlpha() ? 1 : 0,
            bltMethodOk ? "true" : "false",
            bltMethodOk ? static_cast<int>(bltMethod) : -1,
            bltMethodOk ? bltMethodNameForDiagnostics(bltMethod)
                        : "unresolved");
        appendPointerJson(payload, "renderLayer", renderLayer);
        appendPointerJson(payload, "targetImage", targetImage);
        appendPointerJson(payload, "sourceBitmap", srcBmp.get());
        appendPointerJson(payload, "sourceObject", sourceArgObject);
        appendPointerJson(payload, "sourceNativeLayer", sourceArgLayer);
        appendPointerJson(payload, "sourceImage", sourceImage);
        payload +=
            fmt::format(",\"sourceArgClass\":\"{}\"",
                        sourceArgClass ? sourceArgClass
                                       : (sourceArgLayer ? "Layer" : "Bitmap"));
        payload += fmt::format(
            ",\"sourceSize\":[{},{}],\"targetSize\":[{},{}]",
            sourceImage ? static_cast<int>(sourceImage->GetWidth()) : 0,
            sourceImage ? static_cast<int>(sourceImage->GetHeight()) : 0,
            renderLayer ? static_cast<int>(renderLayer->GetWidth()) : 0,
            renderLayer ? static_cast<int>(renderLayer->GetHeight()) : 0);
        appendFloatArrayJson(payload, "renderItemCorners", item.corners);
        appendPointArrayJson(payload, "operateAffinePointArgs",
                             affinePointArgs);
        payload += fmt::format(
            ",\"softwareAffinePath\":\"{}\","
            "\"softwareAffineRenderer\":\"{}\","
            "\"softwareAffineAlphaBlendDReady\":{},"
            "\"softwareAffineTempFirstPixelValid\":{},"
            "\"softwareAffineTempFirstPixel\":\"0x{:08x}\","
            "\"softwareAffineTargetFirstPixelBeforeValid\":{},"
            "\"softwareAffineTargetFirstPixelBefore\":\"0x{:08x}\","
            "\"softwareAffineTargetFirstPixelAfterValid\":{},"
            "\"softwareAffineTargetFirstPixelAfter\":\"0x{:08x}\","
            "\"softwareAffineAlphaBlendDProbeValid\":{},"
            "\"softwareAffineAlphaBlendDProbePixel\":\"0x{:08x}\","
            "\"softwareAffineAlphaBlendDCProbeValid\":{},"
            "\"softwareAffineAlphaBlendDCProbePixel\":\"0x{:08x}\","
            "\"softwareAffineAlphaBlendDPointsToC\":{},"
            "\"softwareAffineRenderMethodOpacity\":{},"
            "\"softwareAffineRenderMethodBranch\":\"{}\"",
            TVPGetSoftwareAffinePathForWasmtime(),
            TVPGetSoftwareAffineRendererForWasmtime(),
            TVPGetSoftwareAffineAlphaBlendDReadyForWasmtime() ? "true"
                                                              : "false",
            TVPGetSoftwareAffineTempFirstPixelValidForWasmtime() ? "true"
                                                                 : "false",
            TVPGetSoftwareAffineTempFirstPixelForWasmtime(),
            TVPGetSoftwareAffineTargetFirstPixelBeforeValidForWasmtime()
                ? "true"
                : "false",
            TVPGetSoftwareAffineTargetFirstPixelBeforeForWasmtime(),
            TVPGetSoftwareAffineTargetFirstPixelAfterValidForWasmtime()
                ? "true"
                : "false",
            TVPGetSoftwareAffineTargetFirstPixelAfterForWasmtime(),
            TVPGetSoftwareAffineAlphaBlendDProbeValidForWasmtime() ? "true"
                                                                   : "false",
            TVPGetSoftwareAffineAlphaBlendDProbePixelForWasmtime(),
            TVPGetSoftwareAffineAlphaBlendDCProbeValidForWasmtime() ? "true"
                                                                    : "false",
            TVPGetSoftwareAffineAlphaBlendDCProbePixelForWasmtime(),
            TVPGetSoftwareAffineAlphaBlendDPointsToCForWasmtime() ? "true"
                                                                  : "false",
            TVPGetSoftwareAffineRenderMethodOpacityForWasmtime(),
            TVPGetSoftwareAffineRenderMethodBranchForWasmtime());
        appendPixelProbeJson(payload, "sourceFirstPixel", sourcePixel);
        appendPixelProbeJson(payload, "targetFirstPixel", targetPixel);
        appendPixelSamplesJson(payload, "sourcePixelSamples",
                               sourcePixelSamples);
        appendPixelSamplesJson(payload, "targetPixelSamples",
                               targetPixelSamples);
        motion::detail::motionTraceRenderDirectExecuteProbe(player, samplePoint,
                                                            payload.c_str());
    }
#endif

} // namespace motion::internal::render_detail
