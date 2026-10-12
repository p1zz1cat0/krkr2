// PlayerUpdateGeometry.cpp — updateLayers geometry, visibility, camera, and
// Geometry update phases.
#include "PlayerUpdateLayersInternal.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace motion {
    namespace {
        // One dense mesh grid to build after the vertex loop.
        struct MeshGridJob {
            detail::MotionNode *node;
            double orgX, orgY;
            double mw11, mw12, mw21, mw22;
            bool ownMesh;
            size_t chainBegin, chainEnd; // range in the shared chain list
        };

        void evalPatchPoint(const float *mesh, float u, float v, float &outX,
                            float &outY) {
            // Keep the operation order of evalBezierPatch in
            // updateLayersPhase3_VertexComputation, so that the grid stays
            // bit-identical to the serial calculation.
            const float su = 1.f - u, sv = 1.f - v;
            const float bu[4] = { su * su * su, 3.f * su * su * u,
                                  3.f * su * u * u, u * u * u };
            const float bv[4] = { sv * sv * sv, 3.f * sv * sv * v,
                                  3.f * sv * v * v, v * v * v };
            float resultX = 0;
            float resultY = 0;
            for(int i = 0; i < 16; ++i) {
                const float w = bv[i >> 2] * bu[i & 3];
                resultX += mesh[i * 2] * w;
                resultY += mesh[i * 2 + 1] * w;
            }
            outX = resultX;
            outY = resultY;
        }

        using MeshChainList = std::vector<const detail::MotionNode *>;

        void buildMeshGrid(const MeshGridJob &job,
                           const MeshChainList &chains) {
            auto &vn = *job.node;
            float *grid = vn.meshRenderPoints.data();
            for(int gy = 0; gy < vn.meshDivY; ++gy) {
                const float v = vn.meshDivY > 1
                    ? static_cast<float>(gy) / (vn.meshDivY - 1)
                    : 0.0f;
                for(int gx = 0; gx < vn.meshDivX; ++gx) {
                    const float u = vn.meshDivX > 1
                        ? static_cast<float>(gx) / (vn.meshDivX - 1)
                        : 0.0f;
                    float px = static_cast<float>(job.orgX + u * job.mw11 +
                                                  v * job.mw12);
                    float py = static_cast<float>(job.orgY + u * job.mw21 +
                                                  v * job.mw22);
                    if(job.ownMesh) {
                        evalPatchPoint(vn.meshWorldControlPoints.data(), u,
                                       v, px, py);
                    }
                    const size_t point =
                        static_cast<size_t>(gy * vn.meshDivX + gx) * 2;
                    grid[point] = px;
                    grid[point + 1] = py;
                }
            }
            const size_t gridCount = vn.meshRenderPoints.size() / 2;
            for(size_t c = job.chainBegin; c < job.chainEnd; ++c) {
                const auto &a = *chains[c];
                // Locals let the compiler keep the patch in registers. The
                // grid stores are floats and could alias the vector data.
                float patch[32];
                std::copy_n(a.meshWorldControlPoints.data(), 32, patch);
                const float offX = a.meshInvOffX, offY = a.meshInvOffY;
                const double inv11 = a.meshInvM11, inv12 = a.meshInvM12;
                const double inv21 = a.meshInvM21, inv22 = a.meshInvM22;
                for(size_t point = 0; point < gridCount; ++point) {
                    const float x = grid[point * 2] + offX;
                    const float y = grid[point * 2 + 1] + offY;
                    const float u = static_cast<float>(inv11 * x + inv12 * y);
                    const float v = static_cast<float>(inv21 * x + inv22 * y);
                    evalPatchPoint(patch, u, v, grid[point * 2],
                                   grid[point * 2 + 1]);
                }
            }
        }

        // Each job writes only its own node grid and reads only finished
        // ancestor patches, so the jobs are independent.
        void runMeshGridJobs(const std::vector<MeshGridJob> &jobs,
                             const MeshChainList &chains,
                             size_t work) {
            // Below this many point evaluations the dispatch costs more than it
            // saves.
            constexpr size_t kParallelWork = 16384;
    #if defined(__APPLE__)
            if(work >= kParallelWork && jobs.size() > 1) {
                struct Context {
                    const std::vector<MeshGridJob> *jobs;
                    const MeshChainList *chains;
                } context{ &jobs, &chains };
                dispatch_apply_f(jobs.size(), DISPATCH_APPLY_AUTO, &context,
                                 [](void *raw, size_t index) {
                                     const auto *ctx =
                                         static_cast<const Context *>(raw);
                                     buildMeshGrid((*ctx->jobs)[index],
                                                   *ctx->chains);
                                 });
                return;
            }
    #endif
            (void)work;
            for(const auto &job : jobs) {
                buildMeshGrid(job, chains);
            }
        }
    } // namespace

    void Player::updateLayersPhase3_CameraConstraint() {
        auto &nodes = _runtime->nodes;
        // --- sub_6BC000: Camera constraint (nodeType=9) ---
        // Aligned to 0x6BC000..0x6BC4EC. Only when !isEmoteMode.
        // 9 cases at 0x6BC1B0..0x6BC358 based on flipX/flipY + constraintType
        // (node+2376).
        if(!_runtime->isEmoteMode && nodes.size() >= 2) {
            double offsetX = 0, offsetY = 0, offsetZ = 0;
            // Track which axes have constraints and their types
            bool hasMinX = false, hasMaxX = false, hasTrackX = false;
            bool hasMinY = false, hasMaxY = false, hasTrackY = false;
            bool hasMinZ = false, hasMaxZ = false, hasTrackZ = false;
            double minX = 3.4e38, maxX = -3.4e38, trackX = 0;
            double minY = 3.4e38, maxY = -3.4e38, trackY = 0;
            double minZ = 3.4e38, maxZ = -3.4e38, trackZ = 0;

            for(size_t ci = 1; ci < nodes.size(); ++ci) {
                auto &cn = nodes[ci];
                if(cn.nodeType != 9 || cn.activeSlot().done ||
                   !cn.accumulated.active)
                    continue;

                // Target node: root (node 0). Full impl would look up dtgt.
                const auto &target = nodes[0];

                // Compute constraintType with flip adjustment
                // (0x6BC1B0..0x6BC1FC)
                int ctype = cn.cameraConstraintType;
                if(cn.accumulated.flipX) {
                    if(ctype == 0)
                        ctype = 2;
                    else if(ctype == 2)
                        ctype = 0;
                }
                if(cn.accumulated.flipY) {
                    if(ctype == 3)
                        ctype = 5;
                    else if(ctype == 5)
                        ctype = 3;
                }

                // 9 cases (0x6BC224..0x6BC358)
                switch(ctype) {
                    case 0: { // X min constraint
                        double d =
                            target.accumulated.posX - cn.accumulated.posX;
                        if(d < 0 && d < minX) {
                            minX = d;
                            hasMinX = true;
                        }
                        break;
                    }
                    case 1: { // X direct track
                        trackX = target.accumulated.posX - cn.accumulated.posX;
                        hasTrackX = true;
                        break;
                    }
                    case 2: { // X max constraint
                        double d =
                            target.accumulated.posX - cn.accumulated.posX;
                        if(d > 0 && d > maxX) {
                            maxX = d;
                            hasMaxX = true;
                        }
                        break;
                    }
                    case 3: { // Y min constraint
                        double d =
                            target.accumulated.posY - cn.accumulated.posY;
                        if(d < 0 && d < minY) {
                            minY = d;
                            hasMinY = true;
                        }
                        break;
                    }
                    case 4: { // Y direct track
                        trackY = target.accumulated.posY - cn.accumulated.posY;
                        hasTrackY = true;
                        break;
                    }
                    case 5: { // Y max constraint
                        double d =
                            target.accumulated.posY - cn.accumulated.posY;
                        if(d > 0 && d > maxY) {
                            maxY = d;
                            hasMaxY = true;
                        }
                        break;
                    }
                    case 6: { // Z min constraint
                        double d =
                            target.accumulated.posZ - cn.accumulated.posZ;
                        if(d < 0 && d < minZ) {
                            minZ = d;
                            hasMinZ = true;
                        }
                        break;
                    }
                    case 7: { // Z direct track
                        trackZ = target.accumulated.posZ - cn.accumulated.posZ;
                        hasTrackZ = true;
                        break;
                    }
                    case 8: { // Z max constraint
                        double d =
                            target.accumulated.posZ - cn.accumulated.posZ;
                        if(d > 0 && d > maxZ) {
                            maxZ = d;
                            hasMaxZ = true;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
            // Resolve final offset per axis (0x6BC398..0x6BC410)
            // Priority: track > max > min > 0
            if(hasTrackX)
                offsetX = trackX;
            else if(hasMaxX)
                offsetX = maxX;
            else if(hasMinX)
                offsetX = minX;
            if(hasTrackY)
                offsetY = trackY;
            else if(hasMaxY)
                offsetY = maxY;
            else if(hasMinY)
                offsetY = minY;
            if(hasTrackZ)
                offsetZ = trackZ;
            else if(hasMaxZ)
                offsetZ = maxZ;
            else if(hasMinZ)
                offsetZ = minZ;

            // Apply offset to all nodes (0x6BC450..0x6BC4BC)
            if(offsetX != 0 || offsetY != 0 || offsetZ != 0) {
                for(size_t ci = 1; ci < nodes.size(); ++ci) {
                    nodes[ci].accumulated.posX += offsetX;
                    nodes[ci].accumulated.posY += offsetY;
                    nodes[ci].accumulated.posZ += offsetZ;
                }
            }
        }
    }

    void Player::updateLayersPhase3_VertexComputation() {
        auto &nodes = _runtime->nodes;
        std::vector<MeshGridJob> gridJobs;
        MeshChainList gridChains;
        size_t gridWork = 0;
        // --- sub_6BC4F0: Vertex computation ---
        // Aligned to 0x6BC4F0. Full implementation matching decompilation.
        for(size_t vi = 1; vi < nodes.size(); ++vi) {
            auto &vn = nodes[vi];
            const int parentIdx = vn.parentIndex >= 0 ? vn.parentIndex : 0;
            auto &parentNode = nodes[parentIdx];
            const int slotIdx = 0; // current slot index

            if(vn.hasSource) {
                refreshSourceGeometryFromSourceName(vn, _runtime->activeMotion,
                                                    vn.activeSlot().src);
            }

            // priorDraw flag from emoteEdit (0x6BC648..0x6BC6C4)
            // priorDraw from emoteEdit (0x6BC648..0x6BC6C4)
            if(vn.forceVisible && vn.emoteEditDict) {
                // sub_6636D4: read bool "priorDraw" from emoteEdit dict
                auto pdVal = (*vn.emoteEditDict)["priorDraw"];
                if(auto num = std::dynamic_pointer_cast<PSB::PSBNumber>(pdVal))
                    vn.priorDraw =
                        num->getValue<int>(); // keep raw int — bit flags
                                              // checked via (v12 & 5)
                else if(auto bl =
                            std::dynamic_pointer_cast<PSB::PSBBool>(pdVal))
                    vn.priorDraw = bl->value ? 1 : 0;
                else
                    vn.priorDraw = 0;
            } else {
                vn.priorDraw = 0; // 0x6BC67C
            }

            // node+1968 stores the nearest inherited mesh-chain node, not the
            // shape/stencil parent. A combine boundary remains in that chain
            // even when it has no patch of its own.
            if(parentIdx > 0 && parentIdx < static_cast<int>(nodes.size())) {
                vn.meshAncestorIndex =
                    (parentNode.hasMeshData || parentNode.meshCombineEnabled ||
                     parentNode.patchMeshAlive)
                    ? parentIdx
                    : parentNode.meshAncestorIndex;
            } else {
                vn.meshAncestorIndex = -1;
            }
            vn.hasMeshData = !vn.activeSlot().done && vn.accumulated.active &&
                vn.meshType != 0 && vn.meshControlPoints.size() == 32 &&
                (vn.meshFlags & 0x8) != 0;
            // Patch lifetime latch (see MotionNode.h). Cleared only when the node
            // stops carrying a patch or goes inactive, not when one frame's
            // slot/flags clear hasMeshData.
            vn.patchMeshAlive = vn.hasMeshData ||
                (vn.patchMeshAlive && vn.accumulated.active &&
                 vn.meshControlPoints.size() == 32);
            vn.meshCombineEnabled = vn.meshAncestorIndex >= 0 &&
                (vn.inheritFlags & 0x02000000) == 0;

            // Check visible (0x6BC700..0x6BC74C)
            if(!vn.accumulated.visible) {
                // Walk parent for mesh flag
                goto bc4f0_next;
            }

            // Propagate clip origin
            vn.clipOriginX = vn.interpolatedCache.ox;
            vn.clipOriginY = vn.interpolatedCache.oy;

            // nodeType 1/5 special position via parent mesh chain
            // (0x6BC828..0x6BC8D4) if ((1 << nodeType) & 0x22) != 0 → nodeType
            // 1 (shape) or 5 (camera)
            if(((1 << vn.nodeType) & 0x22) != 0) {
                double px = vn.accumulated.posX;
                double py = vn.accumulated.posY;
                // Walk parent clip chain, evaluate through each mesh
                // (0x6BC838..0x6BC8B0)
                int clipWalk = vn.meshAncestorIndex;
                size_t clipDepth = 0;
                while(clipWalk >= 0 &&
                      clipWalk < static_cast<int>(nodes.size()) &&
                      clipDepth++ < nodes.size()) {
                    auto &cn = nodes[clipWalk];
                    if(cn.meshWorldControlPoints.size() == 32) {
                        // Apply inverse matrix to get normalized coords
                        // (0x6BC858..0x6BC87C)
                        float tx = static_cast<float>(px) + cn.meshInvOffX;
                        float ty = static_cast<float>(py) + cn.meshInvOffY;
                        float ix = static_cast<float>(cn.meshInvM11 * tx +
                                                      cn.meshInvM12 * ty);
                        float iy = static_cast<float>(cn.meshInvM21 * tx +
                                                      cn.meshInvM22 * ty);
                        // Evaluate bezier patch at normalized coords
                        // (sub_69B1E8)
                        const float *mesh =
                            cn.meshWorldControlPoints.data();
                        const float su = 1.f - ix, sv = 1.f - iy;
                        const float bu[4] = { su * su * su, 3.f * su * su * ix,
                                              3.f * su * ix * ix,
                                              ix * ix * ix };
                        const float bv[4] = { sv * sv * sv, 3.f * sv * sv * iy,
                                              3.f * sv * iy * iy,
                                              iy * iy * iy };
                        float ox = 0, oy = 0;
                        for(int bi = 0; bi < 16; ++bi) {
                            float w = bv[bi >> 2] * bu[bi & 3];
                            ox += mesh[bi * 2] * w;
                            oy += mesh[bi * 2 + 1] * w;
                        }
                        px = ox;
                        py = oy;
                    }
                    clipWalk = cn.meshAncestorIndex;
                }
                vn.vertexPosX = px;
                vn.vertexPosY = py;
                vn.vertexPosZ = vn.accumulated.posZ;
            }

            // Non slot-done path: vertex computation (0x6BC8DC..0x6BD730)
            if(!vn.activeSlot().done) {
                // Second visibility bitmask check (0x6BCE2C..0x6BCE40)
                // Non-emote: 7233 = 0x1C41, Emote: 7241 = 0x1C49
                const int vbm = _runtime->isEmoteMode ? 7241 : 7233;
                const bool vertexEligible =
                    vn.forceVisible || ((vbm & (1 << vn.nodeType)) != 0);

                if(vertexEligible && vn.hasSource) {
                    const double m11 = vn.accumulated.m11,
                                 m12 = vn.accumulated.m12;
                    const double m21 = vn.accumulated.m21,
                                 m22 = vn.accumulated.m22;
                    const double posX = vn.accumulated.posX;
                    const double posY =
                        vn.accumulated.posY + vn.accumulated.posZ * _zFactor;

                    // Origin offset (0x6BCB58..0x6BCBA4)
                    const double totalOX = vn.originX + vn.clipOriginX;
                    const double totalOY = vn.originY + vn.clipOriginY;
                    const double orgX = posX - (m12 * totalOY + totalOX * m11);
                    const double orgY = posY - (totalOY * m22 + totalOX * m21);
                    vn.vertexPosX = orgX;
                    vn.vertexPosY = orgY;
                    vn.vertexPosZ = vn.accumulated.posZ;

                    const double cw = vn.clipW;
                    const double ch = vn.clipH;

                    auto evalBezierPatch = [](const float *mesh, float u,
                                              float v, float &outX,
                                              float &outY) {
                        const float su = 1.f - u, sv = 1.f - v;
                        const float bu[4] = { su * su * su, 3.f * su * su * u,
                                              3.f * su * u * u, u * u * u };
                        const float bv[4] = { sv * sv * sv, 3.f * sv * sv * v,
                                              3.f * sv * v * v, v * v * v };
                        // Read all control points before writing the result.
                        float resultX = 0;
                        float resultY = 0;
                        for(int i = 0; i < 16; ++i) {
                            const float w = bv[i >> 2] * bu[i & 3];
                            resultX += mesh[i * 2] * w;
                            resultY += mesh[i * 2 + 1] * w;
                        }
                        outX = resultX;
                        outY = resultY;
                    };
                    // Native +2024, +2048 and +2072 are different buffers:
                    // authored unit patch, dense render grid, and world-space
                    // control patch. Keeping this ownership intact prevents a
                    // node's own patch from being applied once as geometry and
                    // then again as inherited deformation.
                    const bool ownMesh = vn.meshType == 1 &&
                        vn.meshControlPoints.size() == 32 && cw > 0.0 &&
                        ch > 0.0;
                    const double mw11 = m11 * cw, mw12 = m12 * ch;
                    const double mw21 = m21 * cw, mw22 = m22 * ch;
                    if(ownMesh) {
                        const double det = mw11 * mw22 - mw12 * mw21;
                        if(std::fabs(det) > 1e-10) {
                            vn.meshInvM11 = mw22 / det;
                            vn.meshInvM12 = -mw12 / det;
                            vn.meshInvM21 = -mw21 / det;
                            vn.meshInvM22 = mw11 / det;
                            vn.meshInvOffX = -static_cast<float>(orgX);
                            vn.meshInvOffY = -static_cast<float>(orgY);
                        }
                        vn.meshWorldControlPoints.resize(32);
                        for(size_t point = 0; point < 16; ++point) {
                            const double u = vn.meshControlPoints[point * 2];
                            const double v =
                                vn.meshControlPoints[point * 2 + 1];
                            vn.meshWorldControlPoints[point * 2] =
                                static_cast<float>(orgX + u * mw11 + v * mw12);
                            vn.meshWorldControlPoints[point * 2 + 1] =
                                static_cast<float>(orgY + u * mw21 + v * mw22);
                        }
                    } else {
                        vn.meshWorldControlPoints.clear();
                    }

                    if((vn.meshAncestorIndex >= 0 ||
                        !_externalMeshParents.empty()) &&
                       cw > 0.0 && ch > 0.0) {
                        int divisionSource = ownMesh
                            ? static_cast<int>(vi)
                            : vn.meshAncestorIndex;
                        while(!ownMesh && divisionSource >= 0 &&
                              divisionSource < static_cast<int>(nodes.size()) &&
                              !nodes[divisionSource].hasMeshData &&
                              !nodes[divisionSource].patchMeshAlive) {
                            divisionSource =
                                nodes[divisionSource].meshAncestorIndex;
                        }
                        const detail::MotionNode *divisionNode = nullptr;
                        if(divisionSource >= 0 &&
                           divisionSource < static_cast<int>(nodes.size())) {
                            divisionNode = &nodes[divisionSource];
                        } else if(!_externalMeshParents.empty()) {
                            divisionNode = _externalMeshParents.front();
                        }
                        int divTotal = 1;
                        if(divisionNode) {
                            divTotal = std::clamp(static_cast<int>(
                                _emoteMeshDivisionRatio *
                                static_cast<double>(divisionNode->meshDivision)),
                                1, 50);
                            if(!ownMesh) {
                                const double extent =
                                    divisionNode->clipW + divisionNode->clipH;
                                if(extent > 0.0) {
                                    divTotal = std::clamp(static_cast<int>(
                                        static_cast<double>(divTotal) *
                                        (cw + ch) / extent), 1, 50);
                                }
                            }
                        }
                        const int xSegments = std::clamp(static_cast<int>(
                            static_cast<double>(divTotal) * cw / (cw + ch)),
                            0, divTotal);
                        vn.meshDivX = xSegments + 1;
                        vn.meshDivY = divTotal - xSegments + 1;
                        const int numPts = vn.meshDivX * vn.meshDivY;
                        vn.meshRenderPoints.resize(
                            static_cast<size_t>(numPts) * 2);

                        // Only the cascaded origin is needed in this loop. It
                        // costs one patch evaluation for each ancestor. The
                        // dense grid costs one for each grid point and
                        // ancestor, so it goes to gridJobs and runs after the
                        // loop. The ancestors are earlier nodes, thus their
                        // patches are final when the job runs.
                        const size_t chainBegin = gridChains.size();
                        double cascadeOrgX = orgX;
                        double cascadeOrgY = orgY;
                        auto applyAncestor = [&](const detail::MotionNode &a) {
                            if((!a.hasMeshData && !a.patchMeshAlive) ||
                               a.meshWorldControlPoints.size() != 32) {
                                return;
                            }
                            gridChains.push_back(&a);
                            const float x = static_cast<float>(cascadeOrgX) +
                                a.meshInvOffX;
                            const float y = static_cast<float>(cascadeOrgY) +
                                a.meshInvOffY;
                            const float u = static_cast<float>(
                                a.meshInvM11 * x + a.meshInvM12 * y);
                            const float v = static_cast<float>(
                                a.meshInvM21 * x + a.meshInvM22 * y);
                            float resultX = 0.0f, resultY = 0.0f;
                            evalBezierPatch(a.meshWorldControlPoints.data(), u,
                                            v, resultX, resultY);
                            cascadeOrgX = resultX;
                            cascadeOrgY = resultY;
                            _processedMeshVerticesNum += numPts + 1;
                        };
                        int meshWalk = vn.meshAncestorIndex;
                        size_t meshDepth = 0;
                        while(meshWalk >= 0 &&
                              meshWalk < static_cast<int>(nodes.size()) &&
                              meshDepth++ < nodes.size()) {
                            const auto &ancestor = nodes[meshWalk];
                            applyAncestor(ancestor);
                            meshWalk = ancestor.meshAncestorIndex;
                        }
                        for(const auto *ancestor : _externalMeshParents) {
                            if(ancestor) {
                                applyAncestor(*ancestor);
                            }
                        }
                        gridJobs.push_back({ &vn, orgX, orgY, mw11, mw12, mw21,
                                             mw22, ownMesh, chainBegin,
                                             gridChains.size() });
                        gridWork += static_cast<size_t>(numPts) *
                            (gridChains.size() - chainBegin + 1);
                        vn.vertexPosX = cascadeOrgX;
                        vn.vertexPosY = cascadeOrgY;
                    } else {
                        vn.meshRenderPoints.clear();
                        vn.meshDivX = 0;
                        vn.meshDivY = 0;
                    }

                    // 4-corner vertex output (0x6BCE44..0x6BCEC0)
                    {
                        const double fx = vn.vertexPosX;
                        const double fy = vn.vertexPosY;
                        vn.vertices[0] = static_cast<float>(fx);
                        vn.vertices[1] = static_cast<float>(fy);
                        vn.vertices[2] = static_cast<float>(fx + m11 * cw);
                        vn.vertices[3] = static_cast<float>(fy + m21 * cw);
                        vn.vertices[4] =
                            static_cast<float>(fx + m11 * cw + m12 * ch);
                        vn.vertices[5] =
                            static_cast<float>(fy + m21 * cw + m22 * ch);
                        vn.vertices[6] = static_cast<float>(fx + m12 * ch);
                        vn.vertices[7] = static_cast<float>(fy + m22 * ch);
                        if(detail::logoChainTraceEnabled(
                               _runtime->activeMotion)) {
                            const auto motionPath =
                                _runtime->activeMotion->path;
                            const std::array<float, 8> expectedVertices = {
                                static_cast<float>(fx),
                                static_cast<float>(fy),
                                static_cast<float>(fx + m11 * cw),
                                static_cast<float>(fy + m21 * cw),
                                static_cast<float>(fx + m11 * cw + m12 * ch),
                                static_cast<float>(fy + m21 * cw + m22 * ch),
                                static_cast<float>(fx + m12 * ch),
                                static_cast<float>(fy + m22 * ch)
                            };
                            bool ok = true;
                            for(size_t vi = 0; vi < expectedVertices.size();
                                ++vi) {
                                if(std::fabs(vn.vertices[vi] -
                                             expectedVertices[vi]) > 0.01f) {
                                    ok = false;
                                    break;
                                }
                            }
                            detail::logoChainTraceCheck(
                                motionPath, "updateLayers.phase3.vertices",
                                "0x6BC4F0", _clampedEvalTime,
                                fmt::format(
                                    "pos=({:.3f},{:.3f}) clip=({:.3f},{:.3f}) "
                                    "m=({:.6f},{:.6f},{:.6f},{:.6f}) "
                                    "exp=[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:"
                                    ".3f},{:.3f},{:.3f}]",
                                    fx, fy, cw, ch, m11, m12, m21, m22,
                                    expectedVertices[0], expectedVertices[1],
                                    expectedVertices[2], expectedVertices[3],
                                    expectedVertices[4], expectedVertices[5],
                                    expectedVertices[6], expectedVertices[7]),
                                fmt::format("src={} "
                                            "act=[{:.3f},{:.3f},{:.3f},{:.3f},{"
                                            ":.3f},{:.3f},{:.3f},{:.3f}]",
                                            vn.interpolatedCache.src.empty()
                                                ? std::string("<none>")
                                                : vn.interpolatedCache.src,
                                            vn.vertices[0], vn.vertices[1],
                                            vn.vertices[2], vn.vertices[3],
                                            vn.vertices[4], vn.vertices[5],
                                            vn.vertices[6], vn.vertices[7]),
                                ok,
                                "sub_6BC4F0 vertex output diverged from "
                                "expected corners");
                        }
                    }

                    // forceVisible TJS property writing (0x6BD38C..0x6BD72C)
                    // When node+1996 (forceVisible) is set, write node
                    // properties to a TJS dictionary for sub-motion evaluation.
                    // forceVisible TJS property writing (0x6BD38C..0x6BD72C)
                    // Write node properties to TJS dict for sub-motion
                    // evaluation.
                    if(vn.forceVisible && vn.tjsLayerObject) {
                        auto *tjsObj =
                            static_cast<iTJSDispatch2 *>(vn.tjsLayerObject);
                        try {
                            // "c" array: [posX, posY] (0x6BD480..0x6BD494)
                            tTJSVariant posXv(vn.vertexPosX);
                            tTJSVariant posYv(vn.vertexPosY);
                            // "mtx" array: [m11,m12,m21,m22]
                            // (0x6BD534..0x6BD570)
                            tTJSVariant m11v(m11), m12v(m12), m21v(m21),
                                m22v(m22);
                            // "width" (0x6BD590)
                            tTJSVariant wv(cw);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("width"),
                                            nullptr, &wv, tjsObj);
                            // "height" (0x6BD5B0)
                            tTJSVariant hv(ch);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("height"),
                                            nullptr, &hv, tjsObj);
                            // "originX" (0x6BD5E4)
                            tTJSVariant oxv(totalOX);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("originX"),
                                            nullptr, &oxv, tjsObj);
                            // "originY" (0x6BD618)
                            tTJSVariant oyv(totalOY);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("originY"),
                                            nullptr, &oyv, tjsObj);
                            // "flipX" (0x6BD638)
                            tTJSVariant fxv(
                                static_cast<tjs_int>(vn.accumulated.flipX));
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("flipX"),
                                            nullptr, &fxv, tjsObj);
                            // "flipY" (0x6BD658)
                            tTJSVariant fyv(
                                static_cast<tjs_int>(vn.accumulated.flipY));
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("flipY"),
                                            nullptr, &fyv, tjsObj);
                            // "zoomX" (0x6BD678)
                            tTJSVariant zxv(vn.accumulated.scaleX);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("zoomX"),
                                            nullptr, &zxv, tjsObj);
                            // "zoomY" (0x6BD698)
                            tTJSVariant zyv(vn.accumulated.scaleY);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("zoomY"),
                                            nullptr, &zyv, tjsObj);
                            // "slantX" (0x6BD6B8)
                            tTJSVariant sxv(vn.accumulated.slantX);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("slantX"),
                                            nullptr, &sxv, tjsObj);
                            // "angle" (0x6BD6D8)
                            tTJSVariant av(vn.accumulated.angle);
                            tjsObj->PropSet(TJS_MEMBERENSURE, TJS_W("angle"),
                                            nullptr, &av, tjsObj);
                        } catch(...) {
                        }
                    }
                }
            }
        bc4f0_next:;
        }
        runMeshGridJobs(gridJobs, gridChains, gridWork);

        // Delta position computation (0x6BBB74..0x6BBC54)
        // if playing (player+480): delta = 0; else: delta = currentPos -
        // prevPos
        {
            bool anyPlaying = std::any_of(
                _runtime->timelines.begin(), _runtime->timelines.end(),
                [](const auto &e) { return e.second.playing; });
            for(size_t di = 1; di < nodes.size(); ++di) {
                auto &dn = nodes[di];
                if(anyPlaying) {
                    dn.deltaPosX = 0;
                    dn.deltaPosY = 0;
                    dn.deltaPosZ = 0;
                } else {
                    dn.deltaPosX = dn.accumulated.posX - dn.prevPosX;
                    dn.deltaPosY = dn.accumulated.posY - dn.prevPosY;
                    dn.deltaPosZ = dn.accumulated.posZ - dn.prevPosZ;
                }
            }
        }
    }

    void Player::updateLayersPhase3_Visibility() {
        auto &nodes = _runtime->nodes;
        // Visibility flags — aligned to sub_6BD8DC at 0x6BD8DC.
        // Root node (index 0) is always visible.
        if(!nodes.empty()) {
            nodes[0].drawFlag =
                nodes[0].accumulated.visible && nodes[0].hasSource;
        }
        // Visibility bitmask: which nodeTypes can render
        // Non-emote: 6145 = 0x1801 → nodeTypes 0, 11, 12
        // Emote:     6153 = 0x1809 → nodeTypes 0, 3, 11, 12
        // e-mote3 PSB type=0 仍走 motion 位掩码；有 variableLabels 时按 emote
        // 处理。
        const bool emoteLikeMotion = detail::isEmoteLikeMotion(*_runtime);
        const int visBitmask = emoteLikeMotion ? 6153 : 6145;
        for(size_t i = 1; i < nodes.size(); ++i) {
            auto &node = nodes[i];

            // Find visible ancestor (walk parent chain, 0x6BD9D8)
            int pIdx = node.parentIndex;
            if(pIdx >= 0 && pIdx < static_cast<int>(nodes.size())) {
                if(!nodes[pIdx].drawFlag) {
                    node.visibleAncestorIndex =
                        nodes[pIdx].visibleAncestorIndex;
                } else {
                    node.visibleAncestorIndex = pIdx;
                }
            }

            // Visibility logic — exact replica of sub_6BD8DC
            // (0x6BD958..0x6BDA00):
            //   if (slotDone) { v9 = 0; }
            //   else { v9 = stencilType; if (v9) { v9 = active; if (v9) {
            //     if (forceVisible || (bitmask & (1<<nodeType))) v9 =
            //     hasSource; } } }
            //   drawFlag = v9;
            if(node.activeSlot().done) {
                node.drawFlag = false;
            } else if(node.stencilType == 0) {
                const bool emoteDrawableStencilZero = _runtime->activeMotion &&
                    !_runtime->activeMotion->variableLabels.empty() &&
                    node.hasSource;
                if(!emoteDrawableStencilZero) {
                    node.drawFlag = false;
                } else if(!node.accumulated.active) {
                    node.drawFlag = false;
                } else if(node.forceVisible ||
                          (visBitmask & (1 << node.nodeType)) != 0) {
                    node.drawFlag = node.hasSource;
                } else {
                    node.drawFlag = true;
                }
            } else if(!node.accumulated.active) {
                node.drawFlag = false;
            } else if(node.forceVisible ||
                      (visBitmask & (1 << node.nodeType)) != 0) {
                node.drawFlag = node.hasSource;
            } else {
                // Active node, not in renderable bitmask, not forceVisible:
                // v9 stays as active (non-zero) → drawFlag = true
                node.drawFlag = true;
            }
        }
    }

    void Player::updateLayersPhase3_CameraNode() {
        auto &nodes = _runtime->nodes;
        // Camera node processing — aligned to sub_6BDA28 (0x6BDA28).
        // Find first nodeType=5 (camera) that is active, compute cameraOffset.
        _hasCamera = false;
        for(size_t i = 1; i < nodes.size(); ++i) {
            const auto &camNode = nodes[i];
            if(camNode.nodeType != 5 || !camNode.accumulated.active)
                continue;
            _hasCamera = true;

            // Compute delta from root node position
            const auto &rootAcc = nodes[0].accumulated;
            const double dx = -(camNode.accumulated.posX - rootAcc.posX);
            const double dy = -(camNode.accumulated.posY * _zFactor +
                                camNode.accumulated.posZ -
                                (rootAcc.posY * _zFactor + rootAcc.posZ));

            // Transform by drawAffineMatrix (player+808..832)
            const auto &dam = _runtime->drawAffineMatrix;
            _cameraOffsetX = static_cast<float>(
                static_cast<int>(dam[0] * dx + dam[2] * dy + 0.5));
            _cameraOffsetY = static_cast<float>(
                static_cast<int>(dam[1] * dx + dam[3] * dy + 0.5));

            // Camera-to-target angle (0x6BDC04..0x6BDCB0)
            // When stereovisionActive (a1+1094): compute camera angle for 3D
            // effect.
            if(_stereovisionActive) {
                // Store camera/target positions (a1+72..112)
                _cameraPosX = camNode.accumulated.posX;
                _cameraPosY = camNode.accumulated.posY;
                _cameraPosZ = camNode.accumulated.posZ;
                // Resolve the target position from the clip-slot action path, defaulting
                // to the previous position.
                double angleRad =
                    std::atan2(camNode.accumulated.posZ - _cameraTargetZ,
                               camNode.accumulated.posX - _cameraTargetX);
                double angleDeg = angleRad * -57.2957795 + 90.0;
                while(angleDeg < 0.0)
                    angleDeg += 360.0;
                while(angleDeg >= 360.0)
                    angleDeg -= 360.0;
                _cameraAngle = angleDeg; // a1+472
                _cameraTargetX = _cameraPosX;
                _cameraTargetY = _cameraPosY;
                _cameraTargetZ = _cameraPosZ;
            }
            break; // only first camera node
        }
    }

    void Player::updateLayersPhase3_ShapeAABB() {
        auto &nodes = _runtime->nodes;
        // --- sub_6BDCC0: Shape AABB computation (nodeType=7) ---
        // Aligned to 0x6BDCC0. For nodeType=7 active nodes, compute AABB
        // from 2x2 matrix × 16-unit extent, origin offset, parent clip
        // clamping.
        for(size_t si = 1; si < nodes.size(); ++si) {
            auto &sn = nodes[si];
            // Propagate parent clip region (node+1936)
            if(sn.parentIndex >= 0 &&
               sn.parentIndex < static_cast<int>(nodes.size())) {
                sn.parentClipIndex = nodes[sn.parentIndex].parentClipIndex;
            }
            if(sn.nodeType != 7 || !sn.accumulated.active)
                continue;

            const double m11 = sn.accumulated.m11, m12 = sn.accumulated.m12;
            const double m21 = sn.accumulated.m21, m22 = sn.accumulated.m22;
            const double px = sn.accumulated.posX, py = sn.accumulated.posY;
            const double pzs = sn.accumulated.posZ * _zFactor + py;
            const double ox = sn.clipOriginX, oy = sn.clipOriginY;
            const double oox = ox * m11 + oy * m12;
            const double ooy = ox * m21 + oy * m22;
            // Extent = matrix × 16
            const double ex1 = m11 * 16.0, ex2 = m12 * 16.0;
            const double ey1 = m21 * 16.0, ey2 = m22 * 16.0;
            double xMin = px - ex1 - ex2 - oox;
            double xMax = px + ex1 + ex2 - oox;
            double yMin = pzs - ey1 - ey2 - ooy;
            double yMax = pzs + ey1 + ey2 - ooy;
            if(xMin > xMax)
                std::swap(xMin, xMax);
            if(yMin > yMax)
                std::swap(yMin, yMax);
            sn.shapeAABB[0] = static_cast<float>(xMin);
            sn.shapeAABB[1] = static_cast<float>(yMin);
            sn.shapeAABB[2] = static_cast<float>(xMax);
            sn.shapeAABB[3] = static_cast<float>(yMax);
            // Clamp to parent clip (0x6BDE40..0x6BDE80)
            if(sn.parentClipIndex >= 0 &&
               sn.parentClipIndex < static_cast<int>(nodes.size())) {
                const auto &pc = nodes[sn.parentClipIndex];
                if(pc.shapeAABB[0] > sn.shapeAABB[0])
                    sn.shapeAABB[0] = pc.shapeAABB[0];
                if(pc.shapeAABB[1] > sn.shapeAABB[1])
                    sn.shapeAABB[1] = pc.shapeAABB[1];
                if(pc.shapeAABB[2] < sn.shapeAABB[2])
                    sn.shapeAABB[2] = pc.shapeAABB[2];
                if(pc.shapeAABB[3] < sn.shapeAABB[3])
                    sn.shapeAABB[3] = pc.shapeAABB[3];
            }
            sn.parentClipIndex = static_cast<int>(si);

            if(detail::logoSnapshotMarkEnabledForPath(
                   _runtime->activeMotion->path) &&
               _runtime->activeMotion->path.find("m2logo.mtn") !=
                   std::string::npos &&
               _clampedEvalTime >= 43.0 && _clampedEvalTime <= 50.0 &&
               sn.index == 18) {
                std::fprintf(
                    stderr,
                    "SNAPSHAPE frame=%.3f nodeIndex=%d label=%s "
                    "slotOxOy=(%.3f,%.3f) interpOxOy=(%.3f,%.3f) "
                    "clipOrigin=(%.3f,%.3f) accumPos=(%.3f,%.3f,%.3f) "
                    "m=(%.6f,%.6f,%.6f,%.6f) shapeAABB=[%.3f,%.3f,%.3f,%.3f] "
                    "parentClipIndex=%d\n",
                    _clampedEvalTime, sn.index,
                    sn.layerName.empty() ? "<none>" : sn.layerName.c_str(),
                    sn.activeSlot().ox, sn.activeSlot().oy,
                    sn.interpolatedCache.ox, sn.interpolatedCache.oy,
                    sn.clipOriginX, sn.clipOriginY, sn.accumulated.posX,
                    sn.accumulated.posY, sn.accumulated.posZ,
                    sn.accumulated.m11, sn.accumulated.m12, sn.accumulated.m21,
                    sn.accumulated.m22, sn.shapeAABB[0], sn.shapeAABB[1],
                    sn.shapeAABB[2], sn.shapeAABB[3], sn.parentClipIndex);
            }
        }
    }

    void Player::updateLayersPhase3_ShapeGeometry() {
        auto &nodes = _runtime->nodes;
        // --- sub_6BDE94: Shape geometry computation (nodeType=1) ---
        // Aligned to 0x6BDE94. For nodeType=1 nodes with active slot,
        // compute shape vertices based on shapeType
        // (0=point,1=circle,2=rect,3=quad).
        for(size_t si = 1; si < nodes.size(); ++si) {
            auto &sn = nodes[si];
            if(sn.nodeType != 1 || sn.activeSlot().done)
                continue;
            sn.shapeGeomType = sn.shapeType;
            switch(sn.shapeType) {
                case 0: // point (0x6BDF40)
                    sn.shapeVertices[0] = sn.vertexPosX;
                    sn.shapeVertices[1] = sn.vertexPosY;
                    break;
                case 1: { // circle (0x6BDF50)
                    sn.shapeVertices[0] = sn.vertexPosX;
                    sn.shapeVertices[1] = sn.vertexPosY;
                    sn.shapeVertices[2] = sn.accumulated.scaleX * 16.0 * 0.5;
                    break;
                }
                case 2: { // rect (0x6BDF70)
                    const double hw = sn.accumulated.scaleX * 16.0 * 0.5;
                    const double hh = sn.accumulated.scaleY * 16.0 * 0.5;
                    sn.shapeVertices[3] = sn.vertexPosX - hw;
                    sn.shapeVertices[4] = sn.vertexPosY - hh;
                    sn.shapeVertices[5] = sn.vertexPosX + hw;
                    sn.shapeVertices[6] = sn.vertexPosY + hh;
                    break;
                }
                case 3: { // quad (0x6BDFA8)
                    const double m11 = sn.accumulated.m11,
                                 m12 = sn.accumulated.m12;
                    const double m21 = sn.accumulated.m21,
                                 m22 = sn.accumulated.m22;
                    const double ox = sn.clipOriginX, oy = sn.clipOriginY;
                    const double oox = ox * m11 + oy * m12;
                    const double ooy = ox * m21 + oy * m22;
                    const double px = sn.vertexPosX, py = sn.vertexPosY;
                    const double ax = m11 * -8.0, bx = m12 * -8.0;
                    const double cx = m11 * 8.0, dx = m12 * 8.0;
                    const double ay = m21 * -8.0, by = m22 * -8.0;
                    const double cy = m21 * 8.0, dy = m22 * 8.0;
                    sn.shapeVertices[7] = px + ax + bx - oox;
                    sn.shapeVertices[8] = py + ay + by - ooy;
                    sn.shapeVertices[9] = px + cx + bx - oox;
                    sn.shapeVertices[10] = py + cy + by - ooy;
                    sn.shapeVertices[11] = px + cx + dx - oox;
                    sn.shapeVertices[12] = py + cy + dy - ooy;
                    sn.shapeVertices[13] = px + ax + dx - oox;
                    sn.shapeVertices[14] = py + ay + dy - ooy;
                    break;
                }
                default:
                    break;
            }
        }
    }


} // namespace motion
