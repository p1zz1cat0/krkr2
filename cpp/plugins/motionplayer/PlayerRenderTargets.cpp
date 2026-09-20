// PlayerRenderTargets.cpp — Layer/SLA/D3D render targets and post-draw update
// Split from PlayerRender.cpp for maintainability.
//
#include "PlayerRenderInternal.h"
#include "EmoteCompatInternal.h"
#include "MotionTraceWeb.h"
#include "PrivateMotionGLL.h"
#include "RenderManager.h"
#include "SourceCache.h"
#include <spdlog/spdlog.h>
#include "ncbind.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <GLES2/gl2.h>
#elif !defined(KRKR2_WASMTIME_HEADLESS)
#include "ogl/ogl_common.h"
#endif

using namespace motion::internal;
using namespace motion::internal::render_detail;

namespace motion {
    namespace {
        using PreparedRenderItem = detail::PlayerRuntime::PreparedRenderItem;

        std::array<tTVPPointD, 6> makeTextureQuad(double w, double h) {
            return { {
                { 0.0, 0.0 },
                { w, 0.0 },
                { 0.0, h },
                { w, 0.0 },
                { 0.0, h },
                { w, h },
            } };
        }

        std::array<tTVPPointD, 6>
        makeAffineTargetQuad(const PreparedRenderItem &item, double xOffset,
                             double yOffset) {
            return { {
                { item.corners[0] + xOffset, item.corners[1] + yOffset },
                { item.corners[2] + xOffset, item.corners[3] + yOffset },
                { item.corners[6] + xOffset, item.corners[7] + yOffset },
                { item.corners[2] + xOffset, item.corners[3] + yOffset },
                { item.corners[6] + xOffset, item.corners[7] + yOffset },
                { item.corners[4] + xOffset, item.corners[5] + yOffset },
            } };
        }

        std::vector<tTVPPointD>
        tessellateBezierPatch(const std::vector<float> &controlPoints, int divx,
                              int divy, double xOffset, double yOffset) {
            std::vector<tTVPPointD> out;
            if(controlPoints.size() < 32u || divx < 2 || divy < 2) {
                return out;
            }
            const auto cubicBlend = [](double p0, double p1, double p2,
                                       double p3, double t) {
                const double mt = 1.0 - t;
                return mt * mt * mt * p0 + 3.0 * mt * mt * t * p1 +
                    3.0 * mt * t * t * p2 + t * t * t * p3;
            };
            const auto samplePatch = [&](double u, double v) {
                tTVPPointD curve[4];
                for(int row = 0; row < 4; ++row) {
                    const size_t base = static_cast<size_t>(row) * 8u;
                    curve[row].x = cubicBlend(
                        controlPoints[base + 0], controlPoints[base + 2],
                        controlPoints[base + 4], controlPoints[base + 6], u);
                    curve[row].y = cubicBlend(
                        controlPoints[base + 1], controlPoints[base + 3],
                        controlPoints[base + 5], controlPoints[base + 7], u);
                }
                return tTVPPointD{
                    cubicBlend(curve[0].x, curve[1].x, curve[2].x, curve[3].x,
                               v) +
                        xOffset,
                    cubicBlend(curve[0].y, curve[1].y, curve[2].y, curve[3].y,
                               v) +
                        yOffset,
                };
            };
            out.reserve(static_cast<size_t>(divx) * static_cast<size_t>(divy));
            for(int y = 0; y < divy; ++y) {
                const double v =
                    static_cast<double>(y) / static_cast<double>(divy - 1);
                for(int x = 0; x < divx; ++x) {
                    const double u =
                        static_cast<double>(x) / static_cast<double>(divx - 1);
                    out.push_back(samplePatch(u, v));
                }
            }
            return out;
        }

        std::vector<tTVPPointD>
        buildOffsetMeshPoints(const std::vector<float> &points, double xOffset,
                              double yOffset) {
            std::vector<tTVPPointD> out;
            out.reserve(points.size() / 2u);
            for(size_t i = 0; i + 1 < points.size(); i += 2) {
                out.push_back({ points[i] + xOffset, points[i + 1] + yOffset });
            }
            return out;
        }

        bool shouldQueuePrivateMotionGLLRenderItemLike_0x6DE738(
            const PreparedRenderItem &item, bool preview) {
            if((item.blendMode & 0xF) == 6 || item.skipFlag0 ||
               item.rawFlag16 || item.opacity == 0) {
                return false;
            }
            if(preview && !item.skipFlag1) {
                return false;
            }
            return !item.sourceKey.empty();
        }

        bool isEmoteLikeRuntime(const detail::PlayerRuntime *runtime) {
            return runtime && runtime->activeMotion &&
                (runtime->isEmoteMode || !runtime->parameterEntries.empty() ||
                 !runtime->activeMotion->variableLabels.empty());
        }

        int privateMotionGLLOpacityLike_0x6DE738(const PreparedRenderItem &item,
                                                 bool preview) {
            int opacity = item.opacity;
            if(preview) {
                opacity = opacity >= 0 ? opacity / 2 : (opacity + 1) / 2;
            }
            return opacity;
        }

        void populatePrivateMotionGLLPointsLike_0x6DE738(
            const PreparedRenderItem &item,
            PrivateMotionGLLRenderItemInputLike_0x6DE738 &queueItem) {
            if(item.meshType == 0) {
                queueItem.points = {
                    { item.corners[0], item.corners[1] },
                    { item.corners[2], item.corners[3] },
                    { item.corners[6], item.corners[7] },
                };
                return;
            }

            queueItem.points.reserve(item.meshPoints.size() / 2u);
            for(size_t i = 0; i + 1 < item.meshPoints.size(); i += 2) {
                queueItem.points.push_back(
                    { item.meshPoints[i], item.meshPoints[i + 1] });
            }
        }

        unsigned int d3dPackedColorWithOpacity(const PreparedRenderItem &item,
                                               int opacity) {
            const auto base = item.packedColors[0];
            const auto rgb =
                base == 0xFF808080u ? 0x00FFFFFFu : (base & 0x00FFFFFFu);
            return rgb | (static_cast<unsigned int>(opacity) << 24u);
        }

        tTVPBBBltMethod softwareMethodForD3DBlend(int blendLowNibble) {
            // Motion D3D target is an alpha buffer cleared to A=0.
            // Plain bmAlpha / AlphaBlend does not write destination alpha, so
            // capture ends up as RGB-with-A=0 and the layer is fully invisible.
            // Use the "_d" (dest-alpha) variants so A is composed correctly.
            switch(blendLowNibble) {
                case 1:
                    return bmPsAdditive;
                case 2:
                case 5:
                    return bmPsSubtractive;
                case 3:
                    return bmPsMultiplicative;
                case 4:
                    return bmPsScreen;
                default:
                    return bmAlphaOnAlpha;
            }
        }

        tTVPLayerType accurateSlaLayerTypeLike_0x6C9CA8(int rawBlendMode) {
            switch(rawBlendMode & 0x0F) {
                case 1:
                    return ltPsAdditive;
                case 2:
                case 5:
                    return ltPsSubtractive;
                case 3:
                    return ltPsMultiplicative;
                case 4:
                    return ltPsScreen;
                default:
                    return ltAlpha;
            }
        }

        bool shouldRenderAccurateSlaItemLike_0x6C9CA8(
            const PreparedRenderItem &item) {
            return !item.skipFlag0 && !item.rawFlag16 && item.opacity != 0 &&
                !item.sourceKey.empty();
        }

        bool computeAccurateSlaClipLike_0x6C9CA8(const PreparedRenderItem &item,
                                                 int canvasWidth,
                                                 int canvasHeight,
                                                 RenderClipRect &out) {
            float clipLeft = std::max(item.paintBox[0], 0.0f);
            float clipTop = std::max(item.paintBox[1], 0.0f);
            float clipRight =
                std::min(item.paintBox[2], static_cast<float>(canvasWidth));
            float clipBottom =
                std::min(item.paintBox[3], static_cast<float>(canvasHeight));

            if(item.hasViewport && item.viewport[2] >= item.viewport[0] &&
               item.viewport[3] >= item.viewport[1]) {
                clipLeft = std::max(
                    clipLeft, static_cast<float>(std::floor(item.viewport[0])));
                clipTop = std::max(
                    clipTop, static_cast<float>(std::floor(item.viewport[1])));
                clipRight = std::min(
                    clipRight, static_cast<float>(std::ceil(item.viewport[2])));
                clipBottom =
                    std::min(clipBottom,
                             static_cast<float>(std::ceil(item.viewport[3])));
            }

            if(!(clipLeft < clipRight && clipTop < clipBottom)) {
                return false;
            }

            out.left = static_cast<int>(clipLeft);
            out.top = static_cast<int>(clipTop);
            out.right = static_cast<int>(clipRight);
            out.bottom = static_cast<int>(clipBottom);
            return out.left < out.right && out.top < out.bottom;
        }

        iTJSDispatch2 *
        resolveLayerWindowObjectLike_0x6CE19C(iTJSDispatch2 *targetLayerObject,
                                              tTJSVariant &ownerStorage) {
            ownerStorage.Clear();
            if(!targetLayerObject) {
                return nullptr;
            }
            if(TJS_FAILED(targetLayerObject->PropGet(0, TJS_W("window"),
                                                     nullptr, &ownerStorage,
                                                     targetLayerObject))) {
                return nullptr;
            }
            return ownerStorage.Type() == tvtObject
                ? ownerStorage.AsObjectNoAddRef()
                : nullptr;
        }

        bool setLayerSizeLike_0x6CE19C(iTJSDispatch2 *layerObject, int width,
                                       int height) {
            if(!layerObject || width <= 0 || height <= 0) {
                return false;
            }
            tTJSVariant widthArg(width);
            tTJSVariant heightArg(height);
            tTJSVariant *args[] = { &widthArg, &heightArg };
            return TJS_SUCCEEDED(layerObject->FuncCall(
                0, TJS_W("setSize"), nullptr, nullptr, 2, args, layerObject));
        }

        bool piledCopyLayerLike_0x6CE938(iTJSDispatch2 *destinationObject,
                                         iTJSDispatch2 *sourceObject, int width,
                                         int height) {
            auto *destination = resolveNativeLayer(destinationObject);
            auto *source = resolveNativeLayer(sourceObject);
            if(!destination || !source || width <= 0 || height <= 0) {
                return false;
            }
            const tTVPRect sourceRect(0, 0, width, height);
            destination->PiledCopy(0, 0, source, sourceRect);
            return true;
        }

        iTJSDispatch2 *ensureAccurateSlaStateLayerLike_0x6C6B48(
            tTJSVariant &slot, iTJSDispatch2 *layerTreeOwnerObject,
            iTJSDispatch2 *parentLayerObject, tTVPLayerType layerType) {
            if(!parentLayerObject && layerTreeOwnerObject) {
                parentLayerObject =
                    resolvePrimaryLayerObject(layerTreeOwnerObject);
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
            layer->SetType(layerType);
            layer->SetAbsoluteOrderMode(false);
            return layerObject;
        }

        const char *gpuMethodNameForD3DBlend(int blendLowNibble,
                                             bool alphaOpAdd, bool alphaTest) {
            switch(blendLowNibble) {
                case 1:
                    return alphaTest ? "PsAddBlend_color_AlphaTest"
                                     : "PsAddBlend_color";
                case 2:
                case 5:
                    return alphaTest ? "PsSubBlend_color_AlphaTest"
                                     : "PsSubBlend_color";
                case 3:
                    return alphaTest ? "PsMulBlend_color_AlphaTest"
                                     : "PsMulBlend_color";
                case 4:
                    return alphaTest ? "PsScreenBlend_color_AlphaTest"
                                     : "PsScreenBlend_color";
                default:
                    if(alphaOpAdd) {
                        return alphaTest ? "AlphaBlend_color_a_AlphaTest"
                                         : "AlphaBlend_color_a";
                    }
                    return alphaTest ? "AlphaBlend_color_AlphaTest"
                                     : "AlphaBlend_color";
            }
        }

        iTVPRenderMethod *selectD3DRenderMethod(int blendLowNibble,
                                                unsigned int color,
                                                bool alphaOpAdd, bool alphaTest,
                                                int opacity) {
            auto *mgr = TVPGetRenderManager();
            if(mgr->IsSoftware()) {
                return mgr->GetRenderMethod(
                    opacity, false, softwareMethodForD3DBlend(blendLowNibble));
            }

            auto *method = mgr->GetRenderMethod(gpuMethodNameForD3DBlend(
                blendLowNibble, alphaOpAdd, alphaTest));
            if(!method) {
                return nullptr;
            }
            const int colorId = method->EnumParameterID("color");
            method->SetParameterColor4B(colorId, color);
            if(alphaTest) {
                const int thresholdId =
                    method->EnumParameterID("alpha_threshold");
                method->SetParameterOpa(thresholdId, 64);
            }
            return method;
        }

        bool markStencilMaskChainLike_0x6ADFBC(PreparedRenderItem *item,
                                               std::uint8_t ref) {
            if(!item) {
                return false;
            }
            bool hasDrawableMaskTarget = false;
            for(auto *ancestor = item; ancestor;
                ancestor = ancestor->parentItem) {
                ancestor->stencilMaskRef = ref;
                if(!ancestor->rawFlag16 && !ancestor->skipFlag0 &&
                   ancestor->opacity != 0) {
                    hasDrawableMaskTarget = true;
                }
                for(auto *child : ancestor->childItems) {
                    if(!child || child == ancestor) {
                        continue;
                    }
                    child->stencilMaskRef = ref;
                    if(!child->rawFlag16 && !child->skipFlag0 &&
                       child->opacity != 0) {
                        hasDrawableMaskTarget = true;
                    }
                }
            }
            return hasDrawableMaskTarget;
        }

        int
        assignStencilRefsLike_0x6ADFBC(std::vector<PreparedRenderItem> &items) {
            for(auto &item : items) {
                item.stencilMaskRef = 0;
                item.stencilWriteRef = 0;
            }

            int stencilCount = 0;
            for(auto &item : items) {
                if((item.blendMode & 0xF) == 6 || !item.drawFlag ||
                   item.rawFlag16 || item.opacity == 0 || !item.parentItem) {
                    continue;
                }
                if(stencilCount >= 255) {
                    break;
                }
                const auto ref = static_cast<std::uint8_t>(++stencilCount);
                item.stencilWriteRef = ref;
                if(!markStencilMaskChainLike_0x6ADFBC(item.parentItem, ref)) {
                    item.stencilWriteRef = 0;
                }
            }
            return stencilCount;
        }

        void beginD3DStencilIfNeeded(iTVPTexture2D *target, bool enabled) {
            if(!enabled) {
                return;
            }
            auto *mgr = TVPGetRenderManager();
            mgr->SetRenderTarget(target);
            mgr->BeginStencil(target);
#if !defined(KRKR2_WASMTIME_HEADLESS)
            if(!mgr->IsSoftware()) {
                glDisable(GL_DEPTH_TEST);
                glStencilMask(255);
                glClearStencil(0);
                glClear(GL_STENCIL_BUFFER_BIT);
                glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
                glDepthMask(GL_FALSE);
                glDisable(GL_STENCIL_TEST);
            }
#endif
        }

        void applyD3DStencilState(const PreparedRenderItem &item,
                                  bool enabled) {
            if(!enabled) {
                return;
            }
#if !defined(KRKR2_WASMTIME_HEADLESS)
            auto *mgr = TVPGetRenderManager();
            if(mgr->IsSoftware()) {
                return;
            }
            const auto maskRef = item.stencilMaskRef;
            const auto writeRef = item.stencilWriteRef;
            if(writeRef) {
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(GL_LEQUAL, writeRef, 255);
                if(maskRef) {
                    glStencilMask(maskRef);
                    glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
                } else {
                    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                }
            } else if(maskRef) {
                glEnable(GL_STENCIL_TEST);
                glStencilMask(maskRef);
                glStencilFunc(GL_ALWAYS, maskRef, 255);
                glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
            } else {
                glDisable(GL_STENCIL_TEST);
            }
#endif
        }

        void endD3DStencilIfNeeded(bool enabled) {
            if(!enabled) {
                return;
            }
#if !defined(KRKR2_WASMTIME_HEADLESS)
            if(!TVPGetRenderManager()->IsSoftware()) {
                glDepthMask(GL_TRUE);
            }
#endif
            TVPGetRenderManager()->EndStencil();
        }

        bool operateD3DAffine(iTVPRenderMethod *method, iTVPTexture2D *target,
                              const tTVPRect &targetRect,
                              const PreparedRenderItem &item,
                              iTVPTexture2D *sourceTexture) {
            auto dst = makeAffineTargetQuad(item, 0.5, 0.5);
            auto src = makeTextureQuad(sourceTexture->GetWidth(),
                                       sourceTexture->GetHeight());
            tRenderTexQuadArray::Element srcTex[] = {
                tRenderTexQuadArray::Element(sourceTexture, src.data())
            };
            TVPGetRenderManager()->OperateTriangles(
                method, 2, target, target, targetRect, dst.data(),
                tRenderTexQuadArray(srcTex));
            return true;
        }

        bool operateD3DMesh(iTVPRenderMethod *method, iTVPTexture2D *target,
                            const tTVPRect &targetRect,
                            const PreparedRenderItem &item,
                            iTVPTexture2D *sourceTexture,
                            const std::vector<tTVPPointD> &meshPoints) {
            if(item.meshDivX < 2 || item.meshDivY < 2 ||
               meshPoints.size() < static_cast<size_t>(item.meshDivX) *
                       static_cast<size_t>(item.meshDivY)) {
                return false;
            }

            const double srcW = sourceTexture->GetWidth();
            const double srcH = sourceTexture->GetHeight();
            for(int y = 0; y < item.meshDivY - 1; ++y) {
                const double v0 = static_cast<double>(y) /
                    static_cast<double>(item.meshDivY - 1);
                const double v1 = static_cast<double>(y + 1) /
                    static_cast<double>(item.meshDivY - 1);
                for(int x = 0; x < item.meshDivX - 1; ++x) {
                    const double u0 = static_cast<double>(x) /
                        static_cast<double>(item.meshDivX - 1);
                    const double u1 = static_cast<double>(x + 1) /
                        static_cast<double>(item.meshDivX - 1);
                    const auto &p0 = meshPoints[y * item.meshDivX + x];
                    const auto &p1 = meshPoints[y * item.meshDivX + x + 1];
                    const auto &p2 = meshPoints[(y + 1) * item.meshDivX + x];
                    const auto &p3 =
                        meshPoints[(y + 1) * item.meshDivX + x + 1];
                    std::array<tTVPPointD, 6> dst{ {
                        p0,
                        p1,
                        p2,
                        p1,
                        p2,
                        p3,
                    } };
                    // 共享边 UV 连续：右/下边界与邻格左/上边界一致（统一
                    // floor），避免原 floor/ceil 混用在非整数边界处产生
                    // 1px 采样缝隙（脸部网格黑线）。退化时保底 1px 源宽度，
                    // 邻格共享边仍一致，不产生重叠（alpha 不双倍混合）。
                    const double texU0 = std::floor(srcW * u0);
                    const double texU1 =
                        std::max(texU0 + 1.0, std::floor(srcW * u1));
                    const double texV0 = std::floor(srcH * v0);
                    const double texV1 =
                        std::max(texV0 + 1.0, std::floor(srcH * v1));
                    std::array<tTVPPointD, 6> src{ {
                        { texU0, texV0 },
                        { texU1, texV0 },
                        { texU0, texV1 },
                        { texU1, texV0 },
                        { texU0, texV1 },
                        { texU1, texV1 },
                    } };
                    tRenderTexQuadArray::Element srcTex[] = {
                        tRenderTexQuadArray::Element(sourceTexture, src.data())
                    };
                    TVPGetRenderManager()->OperateTriangles(
                        method, 2, target, target, targetRect, dst.data(),
                        tRenderTexQuadArray(srcTex));
                }
            }
            return true;
        }

        bool
        shouldSkipD3DRenderItemLike_0x6ADFBC(const PreparedRenderItem &item,
                                             bool preview) {
            if((item.blendMode & 0xF) == 6) {
                return true;
            }
            if(item.skipFlag0 || item.rawFlag16) {
                return true;
            }
            return preview && item.skipFlag1;
        }
    } // namespace

    bool Player::renderViaSharedD3DAdaptor(iTJSDispatch2 *targetLayerObject) {
        if(!targetLayerObject) {
            return false;
        }

        auto *resolvedTarget = targetLayerObject;
        tTJSVariant wrapper(targetLayerObject, targetLayerObject);
        if(auto *resolved = tryResolveLayerDispatch(wrapper)) {
            resolvedTarget = resolved;
        }

        auto *targetLayer = resolveNativeLayer(resolvedTarget);
        if(!targetLayer) {
            return false;
        }

        auto *sharedAdaptor = ensureSharedD3DAdaptor(resolvedTarget);
        if(!sharedAdaptor) {
            return false;
        }

        if(!renderToD3DAdaptor(sharedAdaptor)) {
            return false;
        }

        if(sharedAdaptor->getWidth() > 0 && sharedAdaptor->getHeight() > 0) {
            targetLayer->SetSize(sharedAdaptor->getWidth(),
                                 sharedAdaptor->getHeight());
        }
        targetLayer->SetVisible(true);

        tTJSVariant targetVar(resolvedTarget, resolvedTarget);
        tTJSVariant *args[] = { &targetVar };
        if(TJS_FAILED(
               sharedAdaptor->captureCanvas(nullptr, 1, args, nullptr))) {
            return false;
        }

        targetLayer->Update(false);
        _runtime->lastCanvas = tTJSVariant(resolvedTarget, resolvedTarget);
        return true;
    }


    iTJSDispatch2 *Player::resolveSeparateLayerRenderTarget(
        SeparateLayerAdaptor *sla, int &canvasWidth, int &canvasHeight) {
        canvasWidth = 0;
        canvasHeight = 0;
        const auto motionPath = _runtime && _runtime->activeMotion
            ? _runtime->activeMotion->path
            : std::string{};
        auto traceResolveFailure = [&](const char *reason,
                                       const tTJSVariant &target,
                                       iTJSDispatch2 *targetLayerObject) {
            iTJSDispatch2 *targetObject = nullptr;
            iTJSDispatch2 *targetObjThis = nullptr;
            if(target.Type() == tvtObject && target.AsObjectNoAddRef()) {
                const auto closure = target.AsObjectClosureNoAddRef();
                targetObject = closure.Object;
                targetObjThis = closure.ObjThis;
            }
            detail::logoChainTraceLogf(
                motionPath, "sla.resolveTarget.fail", "0x6D5948",
                _clampedEvalTime,
                "reason={} targetType={} targetObject={} targetObjThis={} "
                "targetLayer={} canvas={}x{}",
                reason ? reason : "<unknown>", static_cast<int>(target.Type()),
                static_cast<const void *>(targetObject),
                static_cast<const void *>(targetObjThis),
                static_cast<const void *>(targetLayerObject), canvasWidth,
                canvasHeight);
        };
        if(!sla) {
            return nullptr;
        }

        const auto originalOwnerLayer = sla->getOwnerVariant();
        const auto originalTargetLayer = sla->getTargetLayer();

        // libkrkr2.so Player_ResolveSLATarget @ 0x6D5948 constructs
        // PrivateMotionGLL(owner, targetLayer) from SLA+0 and SLA+20, then
        // stores it in SLA+40. targetLayer remains the original SLA+20
        // variant; only this local variable is reduced like sub_A7A050.
        iTJSDispatch2 *targetLayerObject =
            tryResolveLayerDispatch(originalTargetLayer);
        if(!targetLayerObject) {
            traceResolveFailure("no-target-layer", originalTargetLayer,
                                targetLayerObject);
            return nullptr;
        }

        if(!queryLayerCanvasSize(targetLayerObject, canvasWidth,
                                 canvasHeight)) {
            traceResolveFailure("no-canvas-size", originalTargetLayer,
                                targetLayerObject);
            return nullptr;
        }

        iTJSDispatch2 *renderTarget = ensurePrivateMotionGLLLike_0x6D5948(
            *sla, originalOwnerLayer, originalTargetLayer, targetLayerObject,
            canvasWidth, canvasHeight);
        if(!renderTarget) {
            traceResolveFailure("ensure-private-target-failed",
                                originalTargetLayer, targetLayerObject);
            return nullptr;
        }

        return renderTarget;
    }

    bool Player::renderMotionFrameToTarget(iTJSDispatch2 *renderTargetObject,
                                           tjs_int canvasWidth,
                                           tjs_int canvasHeight,
                                           const char *traceFunc) {
        if(!renderTargetObject || canvasWidth <= 0 || canvasHeight <= 0) {
            return false;
        }
        auto *renderLayer =
            resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTargetObject);
        if(!renderLayer) {
            return false;
        }

        clearPrivateMotionGLLRenderQueueLike_0x6DE738(renderTargetObject);
        const auto motionPath = _runtime && _runtime->activeMotion
            ? _runtime->activeMotion->path
            : std::string{};
        detail::logoChainTraceLogf(
            motionPath, "sla.renderMotionFrame", "0x6DE738", _clampedEvalTime,
            "target={} canvas={}x{} route={}",
            static_cast<const void *>(renderTargetObject), canvasWidth,
            canvasHeight, traceFunc ? traceFunc : "0x6DE738");
        if(commandGraphEnabled()) {
            buildRenderCommandGraph(canvasWidth, canvasHeight);
        } else {
            buildRenderCommands(canvasWidth, canvasHeight);
        }
        std::size_t missingTextures = 0;
        if(_runtime && _runtime->sourceCacheNative) {
            for(const auto &item : _runtime->preparedRenderItems) {
                if(!shouldQueuePrivateMotionGLLRenderItemLike_0x6DE738(
                       item, _preview)) {
                    continue;
                }
                auto *sourceTexture =
                    _runtime->sourceCacheNative->loadRenderSourceTextureByName(
                        detail::widen(item.sourceKey), item.srcRef,
                        item.blendMode, item.packedColors,
                        item.sourceMotion);
                if(!detail::shouldAppendPrivateMotionGLLItem(
                       sourceTexture != nullptr,
                       sourceTexture ? sourceTexture->GetWidth() : 0,
                       sourceTexture ? sourceTexture->GetHeight() : 0)) {
                    ++missingTextures;
                    continue;
                }
                PrivateMotionGLLRenderItemInputLike_0x6DE738 queueItem;
                queueItem.opacity =
                    privateMotionGLLOpacityLike_0x6DE738(item, _preview);
                queueItem.stencilMaskRef = item.stencilMaskRef;
                queueItem.stencilWriteRef = item.stencilWriteRef;
                queueItem.blendMode = item.blendMode;
                queueItem.geometryType = item.meshType;
                queueItem.meshDivX = item.meshDivX;
                queueItem.meshDivY = item.meshDivY;
                queueItem.packedColors = item.packedColors;
                queueItem.sourceRect = {
                    0,
                    0,
                    static_cast<std::int32_t>(sourceTexture->GetWidth()),
                    static_cast<std::int32_t>(sourceTexture->GetHeight()),
                };
                queueItem.sourceTexture = sourceTexture;
                populatePrivateMotionGLLPointsLike_0x6DE738(item, queueItem);
                appendPrivateMotionGLLRenderItemLike_0x6DE738(
                    renderTargetObject, queueItem);
            }
            detail::logoChainTraceLogf(
                motionPath, "sla.renderMotionFrame.queue", "0x6DE738",
                _clampedEvalTime, "queuedItems={} missingTextures={}",
                privateMotionGLLRenderQueueSizeLike_0x6DE738(
                    renderTargetObject),
                missingTextures);
        }
        // Player_DrawSLA @ 0x6D5658 calls Player_RenderMotionFrame @ 0x6DE738
        // only to populate the private +824 queue; Layer_UpdateRect @ 0x800F4C
        // later dispatches __Private_Motion_GLLayer::Draw_GPU @ 0x6DD56C.
        return true;
    }

    bool Player::renderToD3DAdaptor(D3DAdaptor *adaptor) {
        if(!adaptor || adaptor->getWidth() <= 0 || adaptor->getHeight() <= 0) {
            return false;
        }
        // Guard against recursion: D3D capture can re-enter drawCompat.
        static bool s_inRenderToD3D = false;
        if(s_inRenderToD3D)
            return false;
        s_inRenderToD3D = true;
        struct Guard {
            ~Guard() { s_inRenderToD3D = false; }
        } guard;

        ensureMotionLoaded();
        if(!_runtime->activeMotion)
            return false;
        const auto motionPath = _runtime->activeMotion->path;
        detail::logoChainTraceLogf(
            motionPath, "draw.d3d", "0x6D5B90", _clampedEvalTime,
            "adaptorSize={}x{} route=D3DAdaptor_renderFromPlayer",
            adaptor->getWidth(), adaptor->getHeight());

        prepareRenderItems();
        applyPreparedRenderItemTranslateOffsets();
        return renderFromPlayerLike_0x6ADE24(adaptor);
    }

    bool Player::renderAlphaMaskedD3DLike_0x6ADFBC(D3DAdaptor *adaptor) {
        if(!adaptor || !_runtime || !_runtime->activeMotion) {
            return false;
        }

        iTJSDispatch2 *windowObject = adaptor->getWindowObject();
        if(!windowObject) {
            windowObject = resolveMainWindowOwnerObject();
        }
        iTJSDispatch2 *primaryLayerObject =
            resolvePrimaryLayerObject(windowObject);
        if(!windowObject || !primaryLayerObject) {
            return false;
        }

        iTJSDispatch2 *renderLayerObject = ensureReusableLayerObject(
            _runtime->internalRenderLayer, windowObject, primaryLayerObject,
            static_cast<tTVPLayerType>(ltAlpha), false);
        if(!renderLayerObject ||
           !prepareLayerForRender(renderLayerObject, adaptor->getWidth(),
                                  adaptor->getHeight(), 0x00000000)) {
            return false;
        }
        const bool renderCommandsBuilt = commandGraphEnabled()
            ? buildRenderCommandGraph(adaptor->getWidth(),
                                      adaptor->getHeight())
            : buildRenderCommands(adaptor->getWidth(),
                                  adaptor->getHeight());
        if(!renderCommandsBuilt ||
           !executeLayerRenderCommands(renderLayerObject, true)) {
            return false;
        }

        auto *renderLayer = resolveNativeLayer(renderLayerObject);
        auto *image = renderLayer ? renderLayer->GetMainImage() : nullptr;
        auto *sourceTexture = image ? image->GetTexture() : nullptr;
        return adaptor->copyTextureFrom(sourceTexture);
    }

    bool Player::renderFromPlayerLike_0x6ADE24(D3DAdaptor *adaptor) {
        if(!adaptor || adaptor->getWidth() <= 0 || adaptor->getHeight() <= 0) {
            return false;
        }
        // libkrkr2.so D3DAdaptor_renderFromPlayer @ 0x6ADE24 gates the whole
        // GPU texture pipeline on adaptor+21 canvasCaptureEnabled.
        // Yoghourt: default is now true; keep the gate but log skips.
        if(!adaptor->getCanvasCaptureEnabled()) {
            if(auto logger = spdlog::get("plugin")) {
                logger->warn(
                    "renderFromPlayerLike: canvasCaptureEnabled=false, "
                    "skipping D3D draw (blank motion output)");
            }
            return true;
        }
        if(!adaptor->ensureTargetTexture()) {
            return false;
        }
        if(adaptor->getClearEnabled()) {
            adaptor->clearTargetTexture();
        }
        const bool emoteLike = isEmoteLikeRuntime(_runtime.get());
        const bool continuousMask =
            detail::shouldUseContinuousEmoteMask(emoteLike, _maskMode);
        if(continuousMask && renderAlphaMaskedD3DLike_0x6ADFBC(adaptor)) {
            detail::logoChainTraceLogf(
                _runtime->activeMotion->path, "draw.d3d.alphaMask",
                "0x6ADFBC", _clampedEvalTime,
                "route=executeLayerRenderCommands maskMode={} emoteLike=1",
                _maskMode);
            return true;
        }
        if(continuousMask) {
            if(auto logger = spdlog::get("plugin")) {
                logger->warn(
                    "D3D alpha-mask command route failed; falling back to "
                    "binary stencil path");
            }
        }
        return renderItemsToD3DTextureLike_0x6ADFBC(adaptor);
    }

    bool Player::renderItemsToD3DTextureLike_0x6ADFBC(D3DAdaptor *adaptor) {
        if(!adaptor || !_runtime || !_runtime->activeMotion ||
           !_runtime->sourceCacheNative) {
            return false;
        }
        auto *targetTexture = adaptor->targetTexture();
        if(!targetTexture) {
            return false;
        }

        const int width = adaptor->getWidth();
        const int height = adaptor->getHeight();
        const tTVPRect targetRect(0, 0, width, height);
        if(commandGraphEnabled()) {
            buildRenderCommandGraph(width, height);
        } else {
            buildRenderCommands(width, height);
        }

        int stencilRefs = 0;
        if(!_preview) {
            // Mirrors 0x6ADFBC's non-preview prepass: clear item+22/+23,
            // assign stencil refs, then operate on the freshly clipped items.
            stencilRefs =
                assignStencilRefsLike_0x6ADFBC(_runtime->preparedRenderItems);
        }
        const bool stencilEnabled = stencilRefs > 0;
        beginD3DStencilIfNeeded(targetTexture, stencilEnabled);
        struct StencilGuard {
            bool enabled;
            ~StencilGuard() { endD3DStencilIfNeeded(enabled); }
        } stencilGuard{ stencilEnabled };

        const auto motionPath = _runtime->activeMotion->path;
        detail::logoChainTraceLogf(motionPath, "draw.d3d.renderItemsToTexture",
                                   "0x6ADFBC", _clampedEvalTime,
                                   "target={} targetRect=[0,0,{},{}] items={} "
                                   "preview={} stencilRefs={}",
                                   static_cast<const void *>(targetTexture),
                                   width, height,
                                   _runtime->preparedRenderItems.size(),
                                   _preview ? 1 : 0, stencilRefs);

        int skippedSkip = 0, skippedOpacity = 0, skippedEmptyKey = 0,
            skippedTexture = 0, skippedMethod = 0, drawn = 0;
        for(auto &item : _runtime->preparedRenderItems) {
            if(shouldSkipD3DRenderItemLike_0x6ADFBC(item, _preview)) {
                ++skippedSkip;
                continue;
            }

            int opacity = item.opacity;
            if(_preview) {
                opacity = opacity >= 0 ? opacity / 2 : (opacity + 1) / 2;
            }
            opacity = std::clamp(opacity, 0, 255);
            if(opacity <= 0 && item.stencilMaskRef == 0) {
                ++skippedOpacity;
                continue;
            }
            if(item.sourceKey.empty()) {
                ++skippedEmptyKey;
                continue;
            }

            auto *sourceTexture =
                _runtime->sourceCacheNative->loadRenderSourceTextureByName(
                    detail::widen(item.sourceKey), item.srcRef, item.blendMode,
                    item.packedColors, item.sourceMotion);
            if(!sourceTexture || sourceTexture->GetWidth() <= 0 ||
               sourceTexture->GetHeight() <= 0) {
                ++skippedTexture;
                if(auto logger = spdlog::get("plugin"); logger &&
                   skippedTexture <= 8) {
                    logger->warn(
                        "D3D item texture miss key={} opacity={} blend={:#x}",
                        item.sourceKey, opacity, item.blendMode);
                }
                continue;
            }

            applyD3DStencilState(item, stencilEnabled);
            auto *method = selectD3DRenderMethod(
                item.blendMode & 0xF, d3dPackedColorWithOpacity(item, opacity),
                adaptor->getAlphaOpAdd(), item.stencilMaskRef != 0, opacity);
            if(!method) {
                ++skippedMethod;
                continue;
            }

            // 2×2 mesh is a single quad — draw as affine (sharper edges,
            // far cheaper than OperateTriangles grid).
            const bool meshAsAffine = item.meshType == 1 &&
                item.meshDivX <= 2 && item.meshDivY <= 2 &&
                !item.corners.empty();
            if(item.meshType == 0 || meshAsAffine) {
                operateD3DAffine(method, targetTexture, targetRect, item,
                                 sourceTexture);
            } else if(item.meshType == 1 || item.meshType == 2) {
                const auto meshPoints =
                    buildOffsetMeshPoints(item.meshPoints, 0.5, 0.5);
                operateD3DMesh(method, targetTexture, targetRect, item,
                               sourceTexture, meshPoints);
            }
            ++drawn;
        }

        if(auto logger = spdlog::get("plugin")) {
            logger->warn(
                "D3D renderItems items={} drawn={} skip={} opacity={} "
                "emptyKey={} texMiss={} methodMiss={} {}x{}",
                _runtime->preparedRenderItems.size(), drawn, skippedSkip,
                skippedOpacity, skippedEmptyKey, skippedTexture, skippedMethod,
                width, height);
        }

        return true;
    }

    bool
    Player::renderToCanvasLike_0x6C7440(tTJSVariant *target,
                                        bool willCallUpdateLayerAfterDraw) {
        if(!target) {
            return false;
        }

        ensureMotionLoaded();
        if(!_runtime || !_runtime->activeMotion) {
            return false;
        }
        const auto motionPath = _runtime->activeMotion->path;

        iTJSDispatch2 *resolvedLayerObject = tryResolveLayerDispatch(*target);
        if(!resolvedLayerObject && target->Type() == tvtObject) {
            resolvedLayerObject = target->AsObjectNoAddRef();
        }
        if(!resolvedLayerObject) {
            detail::logoChainTraceCheck(
                motionPath, "draw.renderToCanvas", "0x6C7440", _clampedEvalTime,
                "target variant should resolve to a Layer object",
                "target did not resolve", false,
                "Player_renderToCanvas_guess could not resolve target variant");
            return false;
        }

        int canvasWidth = 0;
        int canvasHeight = 0;
        if(!queryLayerCanvasSize(resolvedLayerObject, canvasWidth,
                                 canvasHeight) &&
           _runtime->activeMotion) {
            canvasWidth = static_cast<int>(_runtime->activeMotion->width);
            canvasHeight = static_cast<int>(_runtime->activeMotion->height);
        }
        if(canvasWidth <= 0 || canvasHeight <= 0) {
            return false;
        }

        const bool useInternalRenderLayer =
            _needsInternalAssignImages && willCallUpdateLayerAfterDraw;
        detail::logoChainTraceLogf(
            motionPath, "draw.renderToCanvas", "0x6C7440", _clampedEvalTime,
            "targetLayerCanvas={}x{} willCallUpdateLayerAfterDraw={} "
            "needsInternalAssignImages={} useInternalRenderLayer={}",
            canvasWidth, canvasHeight, willCallUpdateLayerAfterDraw ? 1 : 0,
            _needsInternalAssignImages ? 1 : 0, useInternalRenderLayer ? 1 : 0);

        iTJSDispatch2 *renderLayerObject = resolvedLayerObject;
        if(useInternalRenderLayer) {
            renderLayerObject = ensureReusableLayerObject(
                _runtime->internalRenderLayer, resolveMainWindowOwnerObject(),
                resolvedLayerObject, static_cast<tTVPLayerType>(ltAlpha),
                false);
        }
        if(renderLayerObject != resolvedLayerObject) {
            if(!prepareLayerForRender(renderLayerObject, canvasWidth,
                                      canvasHeight, 0x00000000)) {
                return false;
            }
        } else if(auto *targetLayer = resolveNativeLayer(resolvedLayerObject)) {
            if(targetLayer->GetWidth() != canvasWidth ||
               targetLayer->GetHeight() != canvasHeight) {
                targetLayer->SetSize(canvasWidth, canvasHeight);
            }
        } else {
            return false;
        }

        if(commandGraphEnabled()) {
            buildRenderCommandGraph(canvasWidth, canvasHeight);
        } else {
            buildRenderCommands(canvasWidth, canvasHeight);
        }
        if(!executeLayerRenderCommands(renderLayerObject, true)) {
            return false;
        }

        _runtime->lastCanvas =
            tTJSVariant(resolvedLayerObject, resolvedLayerObject);
        detail::logoChainTraceSummary(
            motionPath, "renderToCanvasLike_0x6C7440", _clampedEvalTime,
            useInternalRenderLayer ? "internalRenderLayer=1"
                                   : "internalRenderLayer=0");
        return true;
    }

    bool Player::renderToLayer(iTJSDispatch2 *layerObject, bool skipUpdate) {
        if(!layerObject) {
            return false;
        }

        ensureMotionLoaded();
        if(!_runtime || !_runtime->activeMotion) {
            return false;
        }
        const auto motionPath = _runtime->activeMotion->path;

        tTJSVariant target(layerObject, layerObject);
        iTJSDispatch2 *resolvedLayerObject = layerObject;
        if(auto *resolved = tryResolveLayerDispatch(target)) {
            resolvedLayerObject = resolved;
        }

        int canvasWidth = 0;
        int canvasHeight = 0;
        if(!queryLayerCanvasSize(resolvedLayerObject, canvasWidth,
                                 canvasHeight) &&
           _runtime->activeMotion) {
            canvasWidth = static_cast<int>(_runtime->activeMotion->width);
            canvasHeight = static_cast<int>(_runtime->activeMotion->height);
        }
        if(canvasWidth <= 0 || canvasHeight <= 0) {
            return false;
        }
        detail::logoChainTraceLogf(
            motionPath, "draw.layer", "0x6C7440/0x6CE7D8", _clampedEvalTime,
            "targetLayerCanvas={}x{} skipUpdate={} "
            "needsInternalAssignImages={}",
            canvasWidth, canvasHeight, skipUpdate ? 1 : 0,
            _needsInternalAssignImages ? 1 : 0);

        prepareRenderItems();
        applyPreparedRenderItemTranslateOffsets();

        const bool needsInternalAssignBeforeRender =
            _needsInternalAssignImages && !skipUpdate;
        if(!renderToCanvasLike_0x6C7440(&target, !skipUpdate)) {
            return false;
        }

        if(!skipUpdate) {
            if(needsInternalAssignBeforeRender) {
                updateLayerAfterDrawLike_0x6CE7D8(&target);
            } else if(auto *layer = resolveNativeLayer(resolvedLayerObject)) {
                if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
                   motionPath.find("m2logo.mtn") != std::string::npos &&
                   _clampedEvalTime >= 30.0 && _clampedEvalTime <= 50.0) {
                    std::fprintf(stderr,
                                 "SNAPLAYER phase=beforeUpdate frame=%.3f %s\n",
                                 _clampedEvalTime,
                                 summarizeLayerChildren(layer).c_str());
                }
                layer->Update(false);
                detail::logoChainTraceLogf(
                    motionPath, "post.layer", "0x6CE7D8", _clampedEvalTime,
                    "targetLayer.Update(false) size={}x{}", layer->GetWidth(),
                    layer->GetHeight());
                if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
                   motionPath.find("m2logo.mtn") != std::string::npos &&
                   _clampedEvalTime >= 30.0 && _clampedEvalTime <= 50.0) {
                    std::fprintf(stderr,
                                 "SNAPLAYER phase=afterUpdate frame=%.3f %s\n",
                                 _clampedEvalTime,
                                 summarizeLayerChildren(layer).c_str());
                }
            }
        }

        detail::logoChainTraceSummary(
            motionPath, "renderToLayer", _clampedEvalTime,
            skipUpdate ? "skipUpdate=1" : "skipUpdate=0");
        return true;
    }

    bool Player::renderToSeparateLayerAdaptor(iTJSDispatch2 *slaObject) {
        if(!slaObject || !_runtime) {
            return false;
        }

        SeparateLayerAdaptor *sla =
            ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(
                slaObject, false);
        if(!sla) {
            return false;
        }
        iTJSDispatch2 *ownerLayer =
            tryResolveLayerDispatch(sla->getOwnerVariant());

        ensureMotionLoaded();
        if(!_runtime->activeMotion) {
            return false;
        }
        const auto motionPath = _runtime->activeMotion->path;

        int canvasWidth = 0;
        int canvasHeight = 0;
        const bool emoteLike = isEmoteLikeRuntime(_runtime.get());
        const bool continuousMask =
            detail::shouldUseContinuousEmoteMask(emoteLike, _maskMode);
        iTJSDispatch2 *targetLayerObject =
            tryResolveLayerDispatch(sla->getTargetLayer());
        const bool accurateSla = isAccurateSlaRenderEnabled();
        // Both SLA modes need the private Motion GLL target. Accurate mode
        // used to bypass this because its deleted legacy path painted part
        // Layers directly under targetLayerObject; the REF executor instead
        // always renders into SLA+40 and only publishes it afterwards.
        iTJSDispatch2 *renderTarget =
            resolveSeparateLayerRenderTarget(sla, canvasWidth, canvasHeight);
        if(!targetLayerObject) {
            targetLayerObject = tryResolveLayerDispatch(sla->getTargetLayer());
        }
        if(!renderTarget) {
            detail::logoChainTraceSummary(
                motionPath, "renderToSeparateLayerAdaptor", _clampedEvalTime,
                "fail=resolveSeparateLayerRenderTarget");
            return false;
        }
        detail::logoChainTraceLogf(
            motionPath, "draw.sla", "0x6D5658", _clampedEvalTime,
            "ownerLayer={} targetCanvas={}x{} accurate={} route={}",
            static_cast<const void *>(ownerLayer), canvasWidth, canvasHeight,
            accurateSla ? 1 : 0,
            accurateSla ? "0x6C9CA8 -> 0x6CE938"
                        : "Player_RenderMotionFrame -> Layer_UpdateRect");
        detail::logoChainTraceLogf(
            motionPath, "draw.sla.mask", "0x6D5658", _clampedEvalTime,
            "emoteLike={} maskMode={} continuousMask={} accurate={}",
            emoteLike ? 1 : 0, _maskMode, continuousMask ? 1 : 0,
            accurateSla ? 1 : 0);
        detail::logoChainTraceLogf(
            motionPath, "sla.resolveTarget", "0x6D5948", _clampedEvalTime,
            "targetLayer={} privateTarget={} absolute={} canvas={}x{} "
            "targetName={} targetType={} targetFace={} targetChildren={} "
            "targetParent={} targetParentType={} "
            "privateType={} privateFace={} privateParent={} "
            "privateParentType={}",
            static_cast<const void *>(targetLayerObject),
            static_cast<const void *>(renderTarget), sla->getAbsolute(),
            canvasWidth, canvasHeight,
            resolveNativeLayer(targetLayerObject)
                ? resolveNativeLayer(targetLayerObject)->GetName().AsStdString()
                : std::string("<not-layer>"),
            resolveNativeLayer(targetLayerObject)
                ? static_cast<int>(
                      resolveNativeLayer(targetLayerObject)->GetType())
                : -1,
            resolveNativeLayer(targetLayerObject)
                ? static_cast<int>(
                      resolveNativeLayer(targetLayerObject)->GetFace())
                : -1,
            resolveNativeLayer(targetLayerObject)
                ? static_cast<int>(
                      resolveNativeLayer(targetLayerObject)->GetCount())
                : -1,
            resolveNativeLayer(targetLayerObject)
                ? static_cast<const void *>(
                      resolveNativeLayer(targetLayerObject)->GetParent())
                : nullptr,
            resolveNativeLayer(targetLayerObject) &&
                    resolveNativeLayer(targetLayerObject)->GetParent()
                ? static_cast<int>(resolveNativeLayer(targetLayerObject)
                                       ->GetParent()
                                       ->GetType())
                : -1,
            resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                ? static_cast<int>(
                      resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                          ->GetType())
                : -1,
            resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                ? static_cast<int>(
                      resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                          ->GetFace())
                : -1,
            resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                ? static_cast<const void *>(
                      resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                          ->GetParent())
                : nullptr,
            resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget) &&
                    resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                        ->GetParent()
                ? static_cast<int>(
                      resolvePrivateMotionGLLNativeLike_0x6DE24C(renderTarget)
                          ->GetParent()
                          ->GetType())
                : -1);

        prepareRenderItems();
        applyPreparedRenderItemTranslateOffsets();

#if defined(KRKR2_WASMTIME_HEADLESS)
        struct AccurateSlaRenderTraceScope {
            Player *player = nullptr;
            iTJSDispatch2 *target = nullptr;
            bool active = false;
            AccurateSlaRenderTraceScope(Player *p, iTJSDispatch2 *t,
                                        bool enabled) :
                player(p), target(t), active(enabled) {
                if(active) {
                    detail::motionTraceBeginAccurateSlaRender(player, target);
                }
            }
            ~AccurateSlaRenderTraceScope() {
                if(active) {
                    detail::motionTraceEndAccurateSlaRender(player, target);
                }
            }
        } accurateSlaRenderTrace{ this, renderTarget, accurateSla };
#endif

        if(accurateSla) {
#if defined(KRKR2_WASMTIME_HEADLESS)
            detail::MotionTraceRenderExecuteScope renderTrace(
                this, targetLayerObject, false);
#endif
            // Phase-3 architecture alignment (REF PlayerRender.cpp
            // 13433-13440): REF's accurate-SLA mode has NO per-part
            // rasterization — its main draw IS the command-graph executor
            // (renderMotionFrame 0x6DE738 -> executeLayerRenderCommands)
            // and the "after draw" step is just a target Update. TGT grew a
            // self-built per-part SLA branch that bypassed the executor
            // entirely, so every render-chain fix landed on a path the
            // commercial game never draws through.
            //
            // 0x6C9CA8 equivalent (REF 13433-13440): accurate mode draws
            // through the command-graph executor into the SLA private
            // target (an alpha layer) — never the user-facing
            // targetLayerObject, which may be a ltBinder without a main
            // image. The first step is the transparent clear (REF 11283):
            // without it every frame paints over the previous frame's
            // pixels — full-body ghosting. The REF after-draw step is just
            // a target Update.
            iTJSDispatch2 *slaRenderTarget = renderTarget;
            if(!slaRenderTarget) {
                detail::logoChainTraceSummary(
                    motionPath, "renderToSeparateLayerAdaptor",
                    _clampedEvalTime, "fail=slaPrivateTarget");
                return false;
            }
            auto *privateLayer =
                resolvePrivateMotionGLLNativeLike_0x6DE24C(slaRenderTarget);
            if(!privateLayer || canvasWidth <= 0 || canvasHeight <= 0) {
                detail::logoChainTraceSummary(
                    motionPath, "renderToSeparateLayerAdaptor",
                    _clampedEvalTime, "fail=slaGraphClear");
                return false;
            }
            privateLayer->SetHasImage(true);
            privateLayer->SetImageSize(static_cast<tjs_uint>(canvasWidth),
                                       static_cast<tjs_uint>(canvasHeight));
            privateLayer->SetSize(canvasWidth, canvasHeight);
            privateLayer->SetClip(0, 0, canvasWidth, canvasHeight);
            privateLayer->FillRect(tTVPRect(0, 0, canvasWidth, canvasHeight),
                                   0x00000000);
            if(commandGraphEnabled()) {
                buildRenderCommandGraph(canvasWidth, canvasHeight);
            } else {
                buildRenderCommands(canvasWidth, canvasHeight);
            }
            if(!executeLayerRenderCommands(slaRenderTarget, true)) {
                detail::logoChainTraceSummary(
                    motionPath, "renderToSeparateLayerAdaptor",
                    _clampedEvalTime, "fail=executeLayerRenderCommands");
                return false;
            }
            {
                auto *renderLayer = resolveNativeLayer(slaRenderTarget);
                if(!renderLayer) {
                    return false;
                }
                renderLayer->Update(false);
            }
            detail::logoChainTraceLogf(
                motionPath, "sla.accurate.end", "0x6CE938", _clampedEvalTime,
                "target={}", static_cast<const void *>(targetLayerObject));
#if defined(KRKR2_WASMTIME_HEADLESS)
            renderTrace.setResult(true);
#endif
        } else if(!renderMotionFrameToTarget(renderTarget, canvasWidth,
                                             canvasHeight, "0x6DE738")) {
            detail::logoChainTraceSummary(
                motionPath, "renderToSeparateLayerAdaptor", _clampedEvalTime,
                "fail=renderMotionFrameToTarget");
            return false;
        } else if(auto *renderLayer =
                      resolvePrivateMotionGLLNativeLike_0x6DE24C(
                          renderTarget)) {
            renderLayer->Update(false);
            detail::logoChainTraceLogf(
                motionPath, "sla.updateRect", "0x800F4C", _clampedEvalTime,
                "renderTarget.Update(false) size={}x{} ownerLayer={}",
                renderLayer->GetWidth(), renderLayer->GetHeight(),
                static_cast<const void *>(ownerLayer));
        } else {
            detail::logoChainTraceCheck(
                motionPath, "sla.updateRect", "0x800F4C", _clampedEvalTime,
                "renderTarget should expose a native layer for Update(false)",
                "renderTarget native layer missing", false,
                "Player_RenderMotionFrame finished but SLA target lacked a "
                "native layer");
        }

        iTJSDispatch2 *lastCanvasObject =
            ownerLayer ? ownerLayer : renderTarget;
        _runtime->lastCanvas = tTJSVariant(lastCanvasObject, lastCanvasObject);
        detail::logoChainTraceSummary(
            motionPath, "renderToSeparateLayerAdaptor", _clampedEvalTime,
            accurateSla ? "accurate=1" : "accurate=0");
        return true;
    }

    bool Player::updateLayerAfterDrawLike_0x6CE7D8(tTJSVariant *target) {
#if defined(KRKR2_WASMTIME_HEADLESS)
        iTJSDispatch2 *rawProbeLayerObject =
            target ? tryResolveLayerDispatch(*target) : nullptr;
        if(!rawProbeLayerObject && target && target->Type() == tvtObject) {
            rawProbeLayerObject = target->AsObjectNoAddRef();
        }
        detail::motionTraceRenderImageCheckpoint(
            this, rawProbeLayerObject, "updateLayerAfterDraw_pre",
            "Player::updateLayerAfterDraw_0x6CE7D8.enter.after-target-resolve");
        detail::motionTraceLayerRawProbe(this, rawProbeLayerObject,
                                         "updateLayerAfterDraw_0x6CE7D8.enter");
        struct UpdateLayerAfterDrawTraceLeave {
            Player *player;
            iTJSDispatch2 *layerObject;
            ~UpdateLayerAfterDrawTraceLeave() {
                detail::motionTraceRenderImageCheckpoint(
                    player, layerObject, "updateLayerAfterDraw_post",
                    "Player::updateLayerAfterDraw_0x6CE7D8.leave.before-"
                    "return");
                detail::motionTraceLayerRawProbe(
                    player, layerObject, "updateLayerAfterDraw_0x6CE7D8.leave");
            }
        } updateLayerAfterDrawTraceLeave{ this, rawProbeLayerObject };
#endif
        if(!_needsInternalAssignImages) {
            return true;
        }
        const auto motionPath = _runtime && _runtime->activeMotion
            ? _runtime->activeMotion->path
            : std::string{};

        _needsInternalAssignImages = false;
        if(!target || !_runtime) {
            return false;
        }

        iTJSDispatch2 *renderLayerObject =
            _runtime->internalRenderLayer.Type() == tvtObject
            ? _runtime->internalRenderLayer.AsObjectNoAddRef()
            : nullptr;
        if(!renderLayerObject) {
            return false;
        }

        try {
            tTJSVariant targetVar;
            targetVar = *target;
            tTJSVariant *args[] = { &targetVar };
            const bool ok = TJS_SUCCEEDED(renderLayerObject->FuncCall(
                0, TJS_W("assignImages"), nullptr, nullptr, 1, args,
                renderLayerObject));
#if defined(KRKR2_WASMTIME_HEADLESS)
            if(ok) {
                detail::motionTraceRecordPostDrawLayerCandidate(
                    this, renderLayerObject,
                    "Player::updateLayerAfterDraw_0x6CE7D8.afterAssignImages");
            }
#endif
            detail::logoChainTraceCheck(
                motionPath, "post.assignImages", "0x6CE7D8", _clampedEvalTime,
                "internal render layer assignImages(original target variant)",
                ok ? "assignImages(target)" : "assignImages(failed)", ok,
                "sub_6CE7D8 failed to assign internal render layer to target");
            return ok;
        } catch(...) {
            detail::logoChainTraceCheck(
                motionPath, "post.assignImages", "0x6CE7D8", _clampedEvalTime,
                "internal render layer assignImages(original target variant)",
                "assignImages(threw)", false,
                "sub_6CE7D8 threw while assigning internal render layer");
            return false;
        }
    }

    bool Player::updateLayerAfterDraw(iTJSDispatch2 *targetLayerObject) {
        if(!targetLayerObject) {
            return !_needsInternalAssignImages;
        }
        tTJSVariant target(targetLayerObject, targetLayerObject);
        return updateLayerAfterDrawLike_0x6CE7D8(&target);
    }

    bool Player::updateAccurateSLAAfterDraw(iTJSDispatch2 *targetLayerObject) {
        if(!targetLayerObject) {
            return false;
        }
        const auto motionPath = _runtime && _runtime->activeMotion
            ? _runtime->activeMotion->path
            : std::string{};
        const auto postStart = std::chrono::steady_clock::now();

        if(!_needsInternalAssignImages) {
            detail::logoChainTraceLogf(motionPath, "post.sla.accurate",
                                       "0x6CE938", _clampedEvalTime,
                                       "needsInternalAssignImages=0");
        }

        int canvasWidth = 0;
        int canvasHeight = 0;
        if(!queryLayerCanvasSize(targetLayerObject, canvasWidth,
                                 canvasHeight)) {
            detail::logoChainTraceCheck(
                motionPath, "post.sla.accurate", "0x6CE938", _clampedEvalTime,
                "target layer width/height should be readable before piledCopy",
                "target-size=0", false,
                "sub_6CE938 failed to query target Layer width/height");
            return false;
        }

        tTJSVariant ownerStorage;
        iTJSDispatch2 *layerTreeOwner = resolveLayerWindowObjectLike_0x6CE19C(
            targetLayerObject, ownerStorage);
        if(!layerTreeOwner) {
            layerTreeOwner = resolveMainWindowOwnerObject();
        }

        iTJSDispatch2 *internalLayerObject = ensureReusableLayerObject(
            _runtime->internalRenderLayer, layerTreeOwner, targetLayerObject,
            static_cast<tTVPLayerType>(ltAlpha), false);
        if(!internalLayerObject ||
           !setLayerSizeLike_0x6CE19C(internalLayerObject, canvasWidth,
                                      canvasHeight)) {
            detail::logoChainTraceCheck(
                motionPath, "post.sla.accurate", "0x6CE19C/0x6CE938",
                _clampedEvalTime,
                "player+696 internal Layer should exist and match target size",
                "internal-layer-setup-failed", false,
                "sub_6CE19C failed to create/size the internal render layer");
            return false;
        }

        // The internal render layer is created invisible and nothing in the
        // accurate-SLA path reads it back: the part layers composite directly
        // into the target layer, and the ordinary-path assignImages flow does
        // not run here. Copying the whole composed canvas into an invisible,
        // unread layer was therefore dead work that cost one full-canvas
        // composite plus copy per portrait per frame in the software
        // renderer. Skip it while the layer stays hidden.
        if(auto *internalLayer = resolveNativeLayer(internalLayerObject);
           internalLayer && !internalLayer->GetVisible()) {
            detail::logoChainTraceLogf(motionPath, "post.sla.accurate.skip",
                                       "0x6CE938", _clampedEvalTime,
                                       "hidden-internal-layer");
            // Rate-limited composite probe: one piledCopy per ~3s measures
            // what the engine pays to composite all part children into the
            // target canvas. The copy itself is discarded (layer invisible).
            // The game can legitimately draw while the target layer has no
            // image yet (mid-transition), and PiledCopy throws on either
            // side lacking an image — the probe must never propagate that.
            static auto lastCompositeProbe =
                std::chrono::steady_clock::now();
            const auto probeNow = std::chrono::steady_clock::now();
            if(probeNow - lastCompositeProbe > std::chrono::seconds(3)) {
                lastCompositeProbe = probeNow;
                auto *targetLayer = resolveNativeLayer(targetLayerObject);
                if(targetLayer && targetLayer->GetHasImage() &&
                   targetLayer->GetMainImage() && internalLayer &&
                   internalLayer->GetHasImage() &&
                   internalLayer->GetMainImage()) {
                    const auto probeStart = std::chrono::steady_clock::now();
                    try {
                        piledCopyLayerLike_0x6CE938(internalLayerObject,
                                                    targetLayerObject,
                                                    canvasWidth, canvasHeight);
                    } catch(...) {
                        // Probe only; ignore engine-state surprises.
                    }
                    const double compositeMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - probeStart)
                            .count();
                    if(auto logger = spdlog::get("plugin")) {
                        logger->info(
                            "sla.accurate.composite.ms path={} ms={:.1f} "
                            "canvas={}x{}",
                            motionPath, compositeMs, canvasWidth,
                            canvasHeight);
                    }
                }
            }
            return true;
        }

        try {
            const bool ok = piledCopyLayerLike_0x6CE938(
                internalLayerObject, targetLayerObject, canvasWidth,
                canvasHeight);
            const double postMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - postStart)
                    .count();
            if(postMs > 10.0) {
                static auto lastPostLog = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if(now - lastPostLog > std::chrono::seconds(2)) {
                    lastPostLog = now;
                    if(auto logger = spdlog::get("plugin")) {
                        logger->warn(
                            "sla.accurate.post.slow path={} postMs={:.1f} "
                            "canvas={}x{}",
                            motionPath, postMs, canvasWidth, canvasHeight);
                    }
                }
            }
            detail::logoChainTraceCheck(
                motionPath, "post.sla.accurate", "0x6CE938", _clampedEvalTime,
                fmt::format("internalLayer.piledCopy(0,0,target,0,0,{},{})",
                            canvasWidth, canvasHeight),
                ok ? "piledCopy" : "piledCopy-failed", ok,
                "sub_6CE938 accurate SLA post-copy diverged");
            return ok;
        } catch(...) {
            detail::logoChainTraceCheck(
                motionPath, "post.sla.accurate", "0x6CE938", _clampedEvalTime,
                "internalLayer.piledCopy should not throw", "piledCopy-threw",
                false, "sub_6CE938 threw while copying target into player+696");
            return false;
        }
    }

} // namespace motion
