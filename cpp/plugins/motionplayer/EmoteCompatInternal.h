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

        inline bool shouldMergeEmoteBoundedChild(int nodeType,
                                                 const std::string &source,
                                                 const std::string &layerName) {
            if(nodeType != 3) {
                return false;
            }
            if(source.find("motion/face_parts/") != std::string::npos ||
               source.find("motion/head_parts/") != std::string::npos ||
               source.find("motion/body_parts/") != std::string::npos) {
                return true;
            }

            static constexpr std::array<std::string_view, 15> kBoundedLabels = {
                "■目L",       "■目R",       "■眉L",     "■眉R",   "■口",
                "■鼻",        "■頬",        "口_種類",  "瞳L",    "瞳R",
                "涙L",        "涙R",        "頭部変形基礎", "全身変形基礎",
                "下半身変形基礎",
            };
            return std::find(kBoundedLabels.begin(), kBoundedLabels.end(),
                             std::string_view(layerName)) !=
                kBoundedLabels.end();
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

        inline std::string renderSourceCacheIdentity(
            const std::string &motionPath, const std::string &sourceKey) {
            return motionPath + '\n' + sourceKey;
        }

        inline bool sourceKeepsEmoteDeformation(const std::string &source,
                                                int parameterizeIndex) {
            if(parameterizeIndex >= 0) {
                return true;
            }
            return source.find("face_parts") != std::string::npos ||
                source.find("face_") != std::string::npos;
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
            // NEKOPARA 素材常见 meshDivision=20。先前硬帽 8 会把五官网格打成
            // 粗块，看起来像低分辨率锯齿。
            constexpr int kMeshDivCap = 20;
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
            if(divTotal > kMeshDivCap) {
                divTotal = kMeshDivCap;
            }

            MeshDivisionPlan plan;
            plan.useAffineGrid =
                !keepDeformation && (!hasUnitBp || unitBpNearIdentity);
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
