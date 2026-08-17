#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace motion {
    namespace detail {

        // E-mote 零件是高分辨率插画网格，不是像素画。Affine/Mesh 复制必须走
        // 双线性，否则旋转和缩放后立绘边缘会呈锯齿。数值对齐
        // tTVPBBStretchType::stFastLinear。
        constexpr int kEmoteRasterStretchType = 1;

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
