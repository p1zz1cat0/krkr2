#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace motion {
    namespace detail {

        // E-mote 零件是高分辨率插画网格，不是像素画。Affine/Mesh 复制必须走
        // 双线性，否则旋转和缩放后立绘边缘会呈锯齿。数值对齐
        // tTVPBBStretchType::stFastLinear。
        constexpr int kEmoteRasterStretchType = 1;

        inline bool differenceTrackOwnsLabel(
            bool playing, int flags, double blendRatio,
            const std::string &label, const std::string &trackLabel,
            bool instantVariable) {
            return playing && (flags & 2) != 0 && blendRatio != 0.0 &&
                !label.empty() && !instantVariable && trackLabel == label;
        }

        // C01（REF emotemotion 单树递归）：是否并入父 bounds/render graph
        // 只看结构事实——该节点是否拥有 child Player（nodeType==3，由
        // buildNodeTree 按 PSB motion 引用结构创建）。source 前缀与日文
        // label allowlist（motion/face_parts/…、■目L 等 15 项）是症状驱动
        // 的启发式，已删除：未命中名字的合法附件此前会完成 update 却永不
        // 进入渲染图。
        inline bool shouldMergeEmoteBoundedChild(int nodeType,
                                                 const std::string &source,
                                                 const std::string &layerName) {
            (void)source;
            (void)layerName;
            return nodeType == 3;
        }

        inline bool clipLabelMatchesRequest(const std::string &label,
                                            const std::string &requested) {
            if(label == requested) {
                return true;
            }
            return !requested.empty() && label.size() > requested.size() &&
                label.compare(0, requested.size(), requested) == 0 &&
                label[requested.size()] == '(';
        }

        struct MultiCacheCandidate {
            std::string chara;
            std::string motion;
            std::uint64_t loadGeneration = 0;
            bool mostRecentlyLoaded = false;
            bool hasSnapshot = false;
        };

        inline std::size_t selectMultiCacheCandidate(
            const std::vector<MultiCacheCandidate> &candidates,
            const std::string &requestedChara,
            const std::string &requestedMotion) {
            std::size_t selected = std::numeric_limits<std::size_t>::max();
            bool bestIsMostRecentlyLoaded = false;
            std::uint64_t bestGeneration = 0;
            for(std::size_t index = 0; index < candidates.size(); ++index) {
                const auto &candidate = candidates[index];
                if(!candidate.hasSnapshot || candidate.chara.empty() ||
                   candidate.motion.empty() ||
                   (!requestedChara.empty() &&
                    candidate.chara != requestedChara) ||
                   (!requestedMotion.empty() &&
                    candidate.motion != requestedMotion)) {
                    continue;
                }

                if(selected == std::numeric_limits<std::size_t>::max() ||
                   (candidate.mostRecentlyLoaded &&
                    !bestIsMostRecentlyLoaded) ||
                   (candidate.mostRecentlyLoaded ==
                        bestIsMostRecentlyLoaded &&
                    candidate.loadGeneration > bestGeneration)) {
                    selected = index;
                    bestIsMostRecentlyLoaded = candidate.mostRecentlyLoaded;
                    bestGeneration = candidate.loadGeneration;
                }
            }
            return selected;
        }

        inline bool shouldAppendPrivateMotionGLLItem(bool hasTexture,
                                                     int width, int height) {
            return hasTexture && width > 0 && height > 0;
        }

        // E-mote's authored masks are antialiased alpha composites. The
        // private GPU queue can only represent those masks as a binary
        // stencil, while the layer-command renderer preserves the source
        // alpha and applies Player::_maskMode. Keep ordinary Motion players
        // on the cheaper queue; only an E-mote-like runtime in Alpha mode
        // needs the command path.
        inline bool shouldUseContinuousEmoteMask(bool emoteLike,
                                                  int maskMode) {
            return emoteLike && maskMode != 0;
        }

        inline std::uint8_t applyMotionMaskAlpha(
            std::uint8_t destinationAlpha, std::uint8_t sourceAlpha,
            int itemFlags, int maskMode, int threshold) {
            const int src = static_cast<int>(sourceAlpha);
            const int dst = static_cast<int>(destinationAlpha);
            if(maskMode == 0) {
                switch(itemFlags) {
                    case 1:
                        return src < threshold ? 0 : destinationAlpha;
                    case 2:
                        return src >= threshold ? 0 : destinationAlpha;
                    case 5:
                    case 6:
                        return src >= threshold ? 255 : destinationAlpha;
                    default:
                        return destinationAlpha;
                }
            }

            switch(itemFlags) {
                case 1:
                    return static_cast<std::uint8_t>((dst * src) / 255);
                case 2:
                    return static_cast<std::uint8_t>(((255 - src) * dst) /
                                                     255);
                case 5:
                case 6:
                    return static_cast<std::uint8_t>(
                        src + (((255 - src) * dst) / 255));
                default:
                    return destinationAlpha;
            }
        }

        inline std::uint8_t unionMotionMaskAlpha(
            std::uint8_t accumulatedAlpha, std::uint8_t sourceAlpha,
            int maskMode, int threshold) {
            if(maskMode == 0) {
                return sourceAlpha >= threshold ? 255 : accumulatedAlpha;
            }
            const int accumulated = static_cast<int>(accumulatedAlpha);
            const int source = static_cast<int>(sourceAlpha);
            return static_cast<std::uint8_t>(
                accumulated + (((255 - accumulated) * source) / 255));
        }

        // 一个 mask surface 与目标矩形的交集，输出目标局部坐标的 dst 位置与
        // surface 局部坐标的 src 位置。覆盖判定与 CPU 逐像素版一致：
        // surface 像素被采用当且仅当 sourceX/Y 落在 surface 尺寸内且目标
        // 坐标落在目标矩形内。无交集返回 false。
        struct MotionMaskSurfaceRect {
            int dstLeft = 0;
            int dstTop = 0;
            int srcLeft = 0;
            int srcTop = 0;
            int width = 0;
            int height = 0;
        };

        inline bool motionMaskSurfaceRect(int dstWorldLeft, int dstWorldTop,
                                          int dstWidth, int dstHeight,
                                          int surfaceWorldLeft,
                                          int surfaceWorldTop,
                                          int surfaceWidth, int surfaceHeight,
                                          MotionMaskSurfaceRect &out) {
            const int left = std::max(0, surfaceWorldLeft - dstWorldLeft);
            const int top = std::max(0, surfaceWorldTop - dstWorldTop);
            const int right = std::min(
                dstWidth, surfaceWorldLeft + surfaceWidth - dstWorldLeft);
            const int bottom = std::min(
                dstHeight, surfaceWorldTop + surfaceHeight - dstWorldTop);
            if(left >= right || top >= bottom) {
                return false;
            }
            out.dstLeft = left;
            out.dstTop = top;
            out.srcLeft = left + dstWorldLeft - surfaceWorldLeft;
            out.srcTop = top + dstWorldTop - surfaceWorldTop;
            out.width = right - left;
            out.height = bottom - top;
            return true;
        }

        inline std::uint8_t applyMotionCompositeMaskAlpha(
            std::uint8_t destinationAlpha, std::uint8_t unionAlpha,
            int compositeFlags, int maskMode, int threshold) {
            return applyMotionMaskAlpha(destinationAlpha, unionAlpha,
                                        compositeFlags & 3, maskMode,
                                        threshold);
        }

        inline std::string renderSourceCacheIdentity(
            const std::string &motionPath, const std::string &sourceKey) {
            return motionPath + '\n' + sourceKey;
        }

        // G02（REF EmoteNode.cpp useBezierMesh = isNeedBp || 祖先 type==1）：
        // 是否保留 Bezier/mesh 变形只看 mesh 数据本身——参数化节点、帧
        // authored bp、祖先 mesh surface 任一成立即保留。素材命名
        // （face_parts/face_ 字符串分流）不是格式语义，已删除。
        inline bool nodeKeepsEmoteDeformation(bool parameterized,
                                              bool hasFrameBp,
                                              bool hasMeshAncestor) {
            return parameterized || hasFrameBp || hasMeshAncestor;
        }

        struct MeshDivisionPlan {
            int divX = 2;
            int divY = 2;
            bool useAffineGrid = true;
        };

        inline MeshDivisionPlan
        planEmoteMeshDivision(int authoredDivision, double meshDivisionRatio,
                              bool hasUnitBp, bool unitBpNearIdentity,
                              bool keepDeformation, double clipW,
                              double clipH) {
            // G01（REF）：meshDivision 只 clamp 到 1..50，再按 icon 宽高
            // 分配 divX/divY。删除 cap=20 与「unit-bp 折成 2x2 affine」
            // 折叠——authored density 是像素契约，性能旋钮是 TJS
            // meshDivisionRatio（ratio 缩放 authoredDivision）。非 name、
            // 非 unit-bp 驱动。hasUnitBp/unitBpNearIdentity 仅为调用方
            // 兼容保留，不再参与判定。
            constexpr int kMeshDivHardCap = 50;

            const double ratio = (std::isfinite(meshDivisionRatio) &&
                                  meshDivisionRatio > 0.0)
                ? meshDivisionRatio
                : 1.0;
            int divTotal = static_cast<int>(std::lround(
                static_cast<double>(std::max(authoredDivision, 0)) * ratio));
            if(divTotal < 1) {
                divTotal = 4;
            }
            if(divTotal > kMeshDivHardCap) {
                divTotal = kMeshDivHardCap;
            }

            MeshDivisionPlan plan;
            plan.useAffineGrid = !keepDeformation;
            (void)hasUnitBp;
            (void)unitBpNearIdentity;
            if(plan.useAffineGrid) {
                plan.divX = 2;
                plan.divY = 2;
                return plan;
            }

            const double width = clipW > 0.0 ? clipW : 1.0;
            const double height = clipH > 0.0 ? clipH : 1.0;
            plan.divX = static_cast<int>(static_cast<double>(divTotal) * width /
                                         (width + height)) +
                1;
            plan.divY = divTotal - plan.divX + 2;
            if(plan.divX < 2) {
                plan.divX = 2;
            }
            if(plan.divY < 2) {
                plan.divY = 2;
            }
            return plan;
        }

        // Unit-space 4×4 Bezier rest pose. sdl3 emoteframe default bp and
        // authored "bp": null rest keys (NEKOPARA 胴体同期UD time=30) both
        // mean identity, not "no mesh channel".
        inline constexpr std::array<double, 32> kUnitMeshBezierPoints = {
            0.0,       0.0,       1.0 / 3.0, 0.0,       2.0 / 3.0, 0.0,
            1.0,       0.0,       0.0,       1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0,
            2.0 / 3.0, 1.0 / 3.0, 1.0,       1.0 / 3.0, 0.0,       2.0 / 3.0,
            1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 1.0,       2.0 / 3.0,
            0.0,       1.0,       1.0 / 3.0, 1.0,       2.0 / 3.0, 1.0,
            1.0,       1.0
        };

        inline void
        fillUnitMeshBezierIfEmpty(std::vector<double> &points) {
            if(points.empty()) {
                points.assign(kUnitMeshBezierPoints.begin(),
                              kUnitMeshBezierPoints.end());
            }
        }

        inline bool
        isExactUnitMeshBezier(const std::vector<double> &points) {
            if(points.size() != 32) {
                return true;
            }
            for(size_t index = 0; index < 32; ++index) {
                if(std::fabs(points[index] - kUnitMeshBezierPoints[index]) >
                   1.0e-6) {
                    return false;
                }
            }
            return true;
        }

        inline void
        lerpMeshBezierPoints(std::vector<double> &dst,
                             const std::vector<double> &other, double t) {
            const bool dstOk = dst.size() == 32;
            const bool otherOk = other.size() == 32;
            if(!dstOk && !otherOk) {
                return;
            }
            std::array<double, 32> aPts = kUnitMeshBezierPoints;
            std::array<double, 32> bPts = kUnitMeshBezierPoints;
            if(dstOk) {
                std::copy(dst.begin(), dst.end(), aPts.begin());
            }
            if(otherOk) {
                std::copy(other.begin(), other.end(), bPts.begin());
            }
            dst.resize(32);
            const double u = 1.0 - t;
            for(size_t index = 0; index < 32; ++index) {
                dst[index] = aPts[index] * u + bPts[index] * t;
            }
        }

        inline std::array<float, 8> quadCornersToMeshPoints(
            const std::array<float, 8> &corners) {
            // MeshCopy 2x2 is row-major: TL, TR, BL, BR. MotionNode corners
            // are TL, TR, BR, BL.
            return { corners[0], corners[1], corners[2], corners[3],
                     corners[6], corners[7], corners[4], corners[5] };
        }

        inline bool boundsAreUsable(double minX, double minY, double maxX,
                                    double maxY) {
            return std::isfinite(minX) && std::isfinite(minY) &&
                std::isfinite(maxX) && std::isfinite(maxY) && maxX > minX &&
                maxY > minY && std::fabs(minX) < 1.0e307 &&
                std::fabs(minY) < 1.0e307 && std::fabs(maxX) < 1.0e307 &&
                std::fabs(maxY) < 1.0e307;
        }

        inline bool pointInAabb(double x, double y, double minX, double minY,
                                double maxX, double maxY) {
            return boundsAreUsable(minX, minY, maxX, maxY) && x >= minX &&
                x <= maxX && y >= minY && y <= maxY;
        }

    } // namespace detail
} // namespace motion
