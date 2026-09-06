// PlayerRenderExecute.cpp — render command build and execution
// Split from PlayerRender.cpp for maintainability.
//
#include <atomic>

#include "PlayerRenderInternal.h"
#include "MotionTraceWeb.h"
#include "PrivateMotionGLL.h"
#include "SourceCache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <unordered_set>

using namespace motion::internal;
using namespace motion::internal::render_detail;

namespace motion {
    bool Player::buildRenderCommands(tjs_int canvasWidth,
                                     tjs_int canvasHeight) {
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::motionTraceRenderBuildCommandsEnter(
            this, static_cast<int>(canvasWidth),
            static_cast<int>(canvasHeight));
#endif
        // Equivalent to sub_6D5164 @ 0x6D5178's `player+544` null gate —
        // the port has no explicit +544 mirror, so an absent runtime is
        // the canonical "no render list yet" signal.
        if(!_runtime) {
#if defined(KRKR2_WASMTIME_HEADLESS)
            detail::motionTraceRenderBuildCommandsLeave(
                this, static_cast<int>(canvasWidth),
                static_cast<int>(canvasHeight));
#endif
            return false;
        }

        _runtime->preparedRenderItemsTopLevel.clear();
        _runtime->preparedRenderItemsGroup.clear();
        const auto motionPath = _runtime->activeMotion
            ? _runtime->activeMotion->path
            : std::string{};
        for(auto &entry : _runtime->preparedRenderItems) {
            // libkrkr2.so sub_6C4E28 works in-place on the render item list
            // built by sub_6C2334. It does not blanket-clear +20/+21 or
            // +216..228: item+19==0 leaves those fields untouched, and failed
            // intersections only write item+21=0. Local execution-only state
            // is reset here; native fields were restored in 0x6C2334 setup.
            entry.builtRect = { 0, 0, 0, 0 };
            entry.leafBuilt = false;
            entry.composedBuilt = false;
            entry.executedDirect = false;

            RenderClipRect clipRect;
            std::string clipFailureReason;
            const bool drawableGate = entry.drawFlag && !entry.rawFlag16;
            if(!entry.drawFlag) {
                // libkrkr2.so sub_6C4E28 only materializes item+21 and
                // item+216..228 for item+19 entries. Ordinary direct items are
                // clipped and submitted later by sub_6C7440 from item+184..212.
                // Because this branch skips the native writer entirely, keep
                // the restored +21/+216..228 values intact.
            } else if(!drawableGate ||
                      !computeRenderClipRect(entry, canvasWidth, canvasHeight,
                                             clipRect, &clipFailureReason)) {
                entry.rawFlag21 = false;
                detail::logoChainTraceCheck(
                    motionPath, "renderItem.clip", "0x6C4E28", _clampedEvalTime,
                    fmt::format(
                        "paintBox∩viewport exp "
                        "paintBox=[{:.3f},{:.3f},{:.3f},{:.3f}] viewport={}",
                        entry.paintBox[0], entry.paintBox[1], entry.paintBox[2],
                        entry.paintBox[3],
                        entry.hasViewport
                            ? fmt::format("[{:.3f},{:.3f},{:.3f},{:.3f}]",
                                          entry.viewport[0], entry.viewport[1],
                                          entry.viewport[2], entry.viewport[3])
                            : std::string("<invalid default>")),
                    fmt::format("nodeIndex={} act=<invalid:{}>",
                                entry.nodeIndex, clipFailureReason),
                    false, "sub_6C4E28 produced an invalid local clip rect");
            } else {
                entry.rawFlag21 = true;
                entry.clipRect = { clipRect.left, clipRect.top, clipRect.right,
                                   clipRect.bottom };
                entry.dirtyRect = entry.clipRect;

                for(size_t ci = 0; ci < entry.corners.size(); ci += 2) {
                    entry.localCorners[ci] = entry.corners[ci] - 0.5f -
                        static_cast<float>(clipRect.left);
                    entry.localCorners[ci + 1] = entry.corners[ci + 1] - 0.5f -
                        static_cast<float>(clipRect.top);
                }

                entry.localMeshPoints.clear();
                entry.localMeshPoints.reserve(entry.meshPoints.size());
                for(size_t pi = 0; pi + 1 < entry.meshPoints.size(); pi += 2) {
                    entry.localMeshPoints.push_back(
                        entry.meshPoints[pi] - 0.5f -
                        static_cast<float>(clipRect.left));
                    entry.localMeshPoints.push_back(
                        entry.meshPoints[pi + 1] - 0.5f -
                        static_cast<float>(clipRect.top));
                }

                std::array<float, 8> expectedLocalCorners{};
                bool cornersOk = true;
                for(size_t ci = 0; ci < entry.corners.size(); ci += 2) {
                    expectedLocalCorners[ci] = entry.corners[ci] - 0.5f -
                        static_cast<float>(clipRect.left);
                    expectedLocalCorners[ci + 1] = entry.corners[ci + 1] -
                        0.5f - static_cast<float>(clipRect.top);
                    if(std::fabs(expectedLocalCorners[ci] -
                                 entry.localCorners[ci]) > 0.01f ||
                       std::fabs(expectedLocalCorners[ci + 1] -
                                 entry.localCorners[ci + 1]) > 0.01f) {
                        cornersOk = false;
                    }
                }
                detail::logoChainTraceCheck(
                    motionPath, "renderItem.clip", "0x6C4E28", _clampedEvalTime,
                    fmt::format("paintBox∩viewport exp=[{},{},{},{}]",
                                clipRect.left, clipRect.top, clipRect.right,
                                clipRect.bottom),
                    fmt::format("nodeIndex={} act=[{},{},{},{}]",
                                entry.nodeIndex, entry.clipRect[0],
                                entry.clipRect[1], entry.clipRect[2],
                                entry.clipRect[3]),
                    true,
                    "sub_6C4E28 clip rect diverged from expected intersection");
                detail::logoChainTraceCheck(
                    motionPath, "renderItem.localCorners", "0x6C4E28",
                    _clampedEvalTime,
                    fmt::format(
                        "corners-0.5-clipOrigin "
                        "exp=[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},"
                        "{:.3f}]",
                        expectedLocalCorners[0], expectedLocalCorners[1],
                        expectedLocalCorners[2], expectedLocalCorners[3],
                        expectedLocalCorners[4], expectedLocalCorners[5],
                        expectedLocalCorners[6], expectedLocalCorners[7]),
                    fmt::format("nodeIndex={} "
                                "act=[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}"
                                ",{:.3f},{:.3f}]",
                                entry.nodeIndex, entry.localCorners[0],
                                entry.localCorners[1], entry.localCorners[2],
                                entry.localCorners[3], entry.localCorners[4],
                                entry.localCorners[5], entry.localCorners[6],
                                entry.localCorners[7]),
                    cornersOk,
                    "sub_6C4E28 local corner translation diverged from "
                    "clip-local expectation");
            }

            persistNativeRenderItemFieldLifetimeLike_0x6C4E28(entry);
            if(entry.groupList) {
                _runtime->preparedRenderItemsGroup.push_back(&entry);
            }
            if(entry.topLevelList) {
                _runtime->preparedRenderItemsTopLevel.push_back(&entry);
            }
        }
        if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
           motionPath.find("m2logo.mtn") != std::string::npos &&
           _clampedEvalTime >= 43.0 && _clampedEvalTime <= 50.0) {
            for(size_t i = 0; i < _runtime->preparedRenderItems.size(); ++i) {
                const auto &item = _runtime->preparedRenderItems[i];
                if(!(item.nodeIndex == 14 || item.nodeIndex == 15 ||
                     item.nodeIndex == 19 ||
                     (item.nodeIndex >= 20 && item.nodeIndex <= 29))) {
                    continue;
                }
                std::fprintf(
                    stderr,
                    "SNAPCMD frame=%.3f order=%zu nodeIndex=%d source=%s "
                    "groupOnly=%d topLevel=%d groupList=%d "
                    "rawFlags=[%d,%d,%d,%d,%d,%d] parentNodeIndex=%d "
                    "hasRenderParent=%d childCount=%zu layerId=(%d,%d) "
                    "clipRect=[%d,%d,%d,%d] opacity=%d blend=%d\n",
                    _clampedEvalTime, i, item.nodeIndex,
                    item.sourceKey.empty() ? "<none>" : item.sourceKey.c_str(),
                    item.groupOnly ? 1 : 0, item.topLevelList ? 1 : 0,
                    item.groupList ? 1 : 0, item.rawFlag16 ? 1 : 0,
                    item.skipFlag0 ? 1 : 0, item.skipFlag1 ? 0 : 1,
                    item.drawFlag ? 1 : 0, item.rawFlag20 ? 1 : 0,
                    item.rawFlag21 ? 1 : 0,
                    item.parentItem ? item.parentItem->nodeIndex
                                    : item.visibleAncestorIndex,
                    item.parentItem ? 1 : 0, item.childItems.size(),
                    item.layerId, item.layerId2, item.clipRect[0],
                    item.clipRect[1], item.clipRect[2], item.clipRect[3],
                    item.opacity, item.blendMode);
            }
        }

        detail::logoChainTraceLogf(
            motionPath, "renderItem.count", "0x6C4E28", _clampedEvalTime,
            "canvas={}x{} preparedItems={} topLevelList={} groupList={}",
            canvasWidth, canvasHeight, _runtime->preparedRenderItems.size(),
            _runtime->preparedRenderItemsTopLevel.size(),
            _runtime->preparedRenderItemsGroup.size());
        const bool ok = !_runtime->preparedRenderItems.empty();
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::motionTraceRenderBuildCommandsLeave(
            this, static_cast<int>(canvasWidth),
            static_cast<int>(canvasHeight));
#endif
        return ok;
    }

    namespace {
        // The legacy per-part accurate-SLA path has been removed, so E-mote
        // must use the REF render-command graph by default. Set
        // KRKR_EMOTE_COMMAND_GRAPH=0 only for an explicit diagnostic rollback.
        const bool kRenderCommandGraphEnabled = [] {
            const char *env = std::getenv("KRKR_EMOTE_COMMAND_GRAPH");
            return !(env && env[0] != '\0' && env[0] == '0');
        }();
        const bool kRenderCommandGraphDiag = [] {
            const char *env = std::getenv("KRKR_EMOTE_MASK_DIAG");
            return env && env[0] != '\0' && env[0] != '0';
        }();

        // REF renderReuseHashCombine + renderCommandLeafReuseSignature
        // (PlayerRender.cpp 274-454). Exact float hashes are intentional:
        // quantizing animation geometry would freeze sub-pixel eye/hair
        // motion.
        inline void renderReuseHashCombine(std::size_t &seed,
                                           std::size_t value) {
            seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        }
        std::size_t renderCommandLeafReuseSignature(
            const detail::PlayerRuntime::ScopedRenderCommand &command) {
            std::size_t seed = 0x6c7440u;
            renderReuseHashCombine(seed,
                                   std::hash<int>{}(command.nodeIndex));
            renderReuseHashCombine(
                seed, std::hash<const void *>{}(command.renderScopeId));
            renderReuseHashCombine(
                seed, std::hash<int>{}(command.scopedNodeIndex));
            if(command.item != nullptr) {
                renderReuseHashCombine(
                    seed, std::hash<std::string>{}(command.item->sourceKey));
                renderReuseHashCombine(
                    seed, std::hash<bool>{}(command.hasOwnSource));
                for(const auto value : command.item->packedColors) {
                    renderReuseHashCombine(
                        seed, std::hash<std::uint32_t>{}(value));
                }
                for(const auto value : command.item->localCorners) {
                    renderReuseHashCombine(
                        seed, std::hash<float>{}(value));
                }
                for(const auto value : command.item->localMeshPoints) {
                    renderReuseHashCombine(
                        seed, std::hash<float>{}(value));
                }
            }
            renderReuseHashCombine(seed,
                                   std::hash<bool>{}(command.groupOnly));
            // Blend resolution consumes the low nibble; hashing only the high
            // nibble lets a changed alpha/add/sub operation reuse old pixels.
            renderReuseHashCombine(
                seed, std::hash<int>{}(command.blendMode & 0x0F));
            renderReuseHashCombine(
                seed, std::hash<int>{}(command.item->clipRect[2] -
                                       command.item->clipRect[0]));
            renderReuseHashCombine(
                seed, std::hash<int>{}(command.item->clipRect[3] -
                                       command.item->clipRect[1]));
            return seed;
        }
        // Prepared-item variant: the executor consumes PreparedRenderItem
        // slots, so the leaf signature hashes the same visual inputs from
        // the item itself. The scoped identity (nativeLifetimeOwner/Key)
        // keeps authored identity stable across topology changes.
        std::size_t renderCommandLeafReuseSignatureForItem(
            const detail::PlayerRuntime::PreparedRenderItem &item) {
            std::size_t seed = 0x6c7440u;
            renderReuseHashCombine(seed,
                                   std::hash<int>{}(item.nodeIndex));
            renderReuseHashCombine(
                seed, std::hash<const void *>{}(
                          static_cast<const void *>(
                              item.nativeLifetimeOwner)));
            renderReuseHashCombine(
                seed, std::hash<int>{}(item.nativeLifetimeKey));
            renderReuseHashCombine(
                seed, std::hash<std::string>{}(item.sourceKey));
            renderReuseHashCombine(
                seed, std::hash<bool>{}(item.hasOwnSource));
            renderReuseHashCombine(
                seed, std::hash<bool>{}(item.groupOnly));
            // The effective operation is selected from the low nibble.
            renderReuseHashCombine(
                seed, std::hash<int>{}(item.blendMode & 0x0F));
            renderReuseHashCombine(
                seed, std::hash<int>{}(item.clipRect[2] - item.clipRect[0]));
            renderReuseHashCombine(
                seed, std::hash<int>{}(item.clipRect[3] - item.clipRect[1]));
            for(const auto value : item.packedColors) {
                renderReuseHashCombine(
                    seed, std::hash<std::uint32_t>{}(value));
            }
            for(const auto value : item.localCorners) {
                renderReuseHashCombine(seed, std::hash<float>{}(value));
            }
            for(const auto value : item.localMeshPoints) {
                renderReuseHashCombine(seed, std::hash<float>{}(value));
            }
            return seed;
        }
    }

    bool Player::commandGraphEnabled() const {
        return kRenderCommandGraphEnabled;
    }

    bool Player::buildRenderCommandGraph(tjs_int canvasWidth,
                                         tjs_int canvasHeight) {
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::motionTraceRenderBuildCommandsEnter(
            this, static_cast<int>(canvasWidth),
            static_cast<int>(canvasHeight));
#endif
        if(!_runtime) {
#if defined(KRKR2_WASMTIME_HEADLESS)
            detail::motionTraceRenderBuildCommandsLeave(
                this, static_cast<int>(canvasWidth),
                static_cast<int>(canvasHeight));
#endif
            return false;
        }
        const auto motionPath =
            _runtime->activeMotion ? _runtime->activeMotion->path
                                   : std::string{};

        using ScopedRenderCommand =
            detail::PlayerRuntime::ScopedRenderCommand;
        using PreparedRenderItem =
            detail::PlayerRuntime::PreparedRenderItem;
        auto &commands = _runtime->renderCommands;
        commands.clear();
        _runtime->preparedRenderItemsTopLevel.clear();
        _runtime->preparedRenderItemsGroup.clear();
        RenderClipRect clipScratch;
        std::string clipReasonScratch;
        std::array<size_t, 5> cmdGraphGateRejects{};
        const auto localRenderScopeId =
            static_cast<const void *>(_runtime.get());

        // Pass 1 — sub_6C4E28-equivalent preparation, replicating the legacy
        // buildRenderCommands body exactly (gating included): every entry
        // gets clip/local-space reset, only drawable gates receive rawFlag21
        // and local-space writes, and the topLevel/group lists keep native
        // membership. The SLA path and executor consume these lists
        // regardless of skip flags, so membership must not diverge.
        for(auto &entry : _runtime->preparedRenderItems) {
            entry.builtRect = { 0, 0, 0, 0 };
            entry.leafBuilt = false;
            entry.composedBuilt = false;
            entry.executedDirect = false;
            const bool drawableGate = entry.drawFlag && !entry.rawFlag16;
            if(!entry.drawFlag) {
                // Legacy branch: local execution state resets only.
            } else if(!drawableGate ||
                      !computeRenderClipRect(entry, canvasWidth, canvasHeight,
                                             clipScratch, &clipReasonScratch)) {
                entry.rawFlag21 = false;
            } else {
                entry.rawFlag21 = true;
                entry.clipRect = { clipScratch.left, clipScratch.top,
                                   clipScratch.right, clipScratch.bottom };
                entry.dirtyRect = entry.clipRect;
                for(size_t ci = 0; ci < entry.corners.size(); ci += 2) {
                    entry.localCorners[ci] =
                        entry.corners[ci] - 0.5f -
                        static_cast<float>(clipScratch.left);
                    entry.localCorners[ci + 1] =
                        entry.corners[ci + 1] - 0.5f -
                        static_cast<float>(clipScratch.top);
                }
                entry.localMeshPoints.clear();
                entry.localMeshPoints.reserve(entry.meshPoints.size());
                for(size_t pi = 0; pi + 1 < entry.meshPoints.size(); pi += 2) {
                    entry.localMeshPoints.push_back(
                        entry.meshPoints[pi] - 0.5f -
                        static_cast<float>(clipScratch.left));
                    entry.localMeshPoints.push_back(
                        entry.meshPoints[pi + 1] - 0.5f -
                        static_cast<float>(clipScratch.top));
                }
            }
            persistNativeRenderItemFieldLifetimeLike_0x6C4E28(entry);
            if(entry.groupList) {
                _runtime->preparedRenderItemsGroup.push_back(&entry);
            }
            if(entry.topLevelList) {
                _runtime->preparedRenderItemsTopLevel.push_back(&entry);
            }
        }

        // Pass 1b — command creation under the REF 8044..8286 gate (without
        // the Yuzu title utility-layer skips, which are game-specific and
        // not ported). Zero-opacity authored mask sources stay alive through
        // the stencilMaskReferenced escape in that gate.
        for(auto &entry : _runtime->preparedRenderItems) {
            if(kRenderCommandGraphDiag) {
                if(!entry.drawFlag) {
                    ++cmdGraphGateRejects[0];
                } else if(entry.skipFlag0) {
                    ++cmdGraphGateRejects[1];
                } else if(_preview && entry.skipFlag1) {
                    ++cmdGraphGateRejects[2];
                } else if(entry.opacity <= 0 &&
                          !entry.stencilMaskReferenced) {
                    ++cmdGraphGateRejects[3];
                } else if(!entry.rawFlag21) {
                    ++cmdGraphGateRejects[4];
                    if(kRenderCommandGraphDiag) {
                        if(auto logger = LOGGER) {
                            logger->warn(
                                "emote.cmdgraph.noclip player={} "
                                "nodeIndex={} groupOnly={} "
                                "paintBox=[{:.1f},{:.1f},{:.1f},{:.1f}] "
                                "viewport={} visAnc={} scope={} scopedIdx={}",
                                static_cast<const void *>(this),
                                entry.nodeIndex, entry.groupOnly ? 1 : 0,
                                entry.paintBox[0], entry.paintBox[1],
                                entry.paintBox[2], entry.paintBox[3],
                                entry.hasViewport ? 1 : 0,
                                entry.visibleAncestorIndex,
                                entry.renderScopeId == localRenderScopeId
                                    ? "local"
                                    : "foreign",
                                entry.scopedNodeIndex);
                        }
                    }
                }
            }
            // skipFlag1 mirrors the executor's preview-only consumption
            // (sub_6C7440 gates item+18 behind the preview flag); rejecting
            // on it unconditionally would drop every non-priorDraw item in
            // a normal commercial frame.
            if(!entry.drawFlag || entry.skipFlag0 ||
               (_preview && entry.skipFlag1) ||
               (entry.opacity <= 0 && !entry.stencilMaskReferenced)) {
                continue;
            }
            if(!entry.rawFlag21) {
                // REF 8097-8119: an invalid clip drops the command — except
                // that a type-12 stencil group keeps its graph presence
                // when its authored mask inputs live in a nested Player
                // scope (S6 derives its output rect from the mask subtree
                // composite) AND a type-3 wrapper root that such groups
                // reference as a mask stays alive too (its geometry comes
                // from its own children). Without both exemptions the mask
                // group cannot resolve its inputs and the mask root cannot
                // materialize a bitmap for the mask group.
                const bool authorMaskedGroup =
                    entry.groupOnly && !entry.stencilMaskNodeIndices.empty();
                const bool maskWrapperRoot =
                    entry.groupOnly && entry.stencilMaskReferenced;
                if(!authorMaskedGroup && !maskWrapperRoot) {
                    ++cmdGraphGateRejects[4];
                    continue;
                }
            }

            ScopedRenderCommand cmd;
            cmd.item = &entry;
            cmd.nodeIndex = entry.nodeIndex;
            // (renderScopeId, scopedNodeIndex) mirrors REF scoped identity.
            // nativeLifetimeOwner/nativeLifetimeKey record the creating
            // runtime and its local node index; merged-namespace offsets
            // never rewrite the key, so the pair stays scoped-true across
            // flattening.
            cmd.renderScopeId =
                entry.nativeLifetimeOwner
                    ? static_cast<const void *>(entry.nativeLifetimeOwner)
                    : static_cast<const void *>(_runtime.get());
            cmd.scopedNodeIndex = entry.nativeLifetimeKey;
            cmd.parentRenderScopeId = entry.parentRenderScopeId;
            cmd.scopedParentNodeIndex = entry.scopedParentNodeIndex;
            cmd.outerRenderAncestorChain = entry.outerRenderAncestorChain;
            cmd.groupOnly = entry.groupOnly;
            cmd.hasOwnSource = entry.hasOwnSource;
            cmd.blendMode = entry.blendMode;
            cmd.opacity = entry.opacity;
            // item+244 composite flags; REF reads the same bits through
            // entry.updateCount for its (itemFlags & 7) classification.
            cmd.itemFlags = entry.stencilComposite;
            cmd.parentNodeIndex = entry.visibleAncestorIndex;
            cmd.stencilMaskInputs = entry.scopedStencilMaskInputs;
            if(cmd.stencilMaskInputs.empty()) {
                // Owner-less legacy references resolve inside the group's
                // own scope first, matching native sub_6C7440's walk.
                for(const int maskNodeIndex : entry.stencilMaskNodeIndices) {
                    cmd.stencilMaskInputs.push_back({ maskNodeIndex, nullptr });
                }
            }
            commands.push_back(std::move(cmd));
        }

        // Pass 2 — index maps. Scoped maps win over the merged namespace so
        // duplicate node indexes across nested players (both eyes commonly
        // use node 4/5) cannot steal each other's commands. REF resets the
        // item-level mask flag here and re-marks it only when wired.
        std::unordered_map<int, size_t> commandIndexByNode;
        std::unordered_map<const void *,
                           std::unordered_map<int, size_t>>
            commandIndexByScopedNode;
        commandIndexByNode.reserve(commands.size());
        for(size_t i = 0; i < commands.size(); ++i) {
            // Unlike REF, the item-level stencilMaskReferenced flag is NOT
            // reset here: the legacy prepare pass owns it, and clearing it
            // would unmark flattened foreign items whose lifecycle lives in
            // a child runtime's lifetime map.
            commandIndexByNode.emplace(commands[i].nodeIndex, i);
            if(commands[i].renderScopeId != nullptr &&
               commands[i].scopedNodeIndex >= 0) {
                commandIndexByScopedNode[commands[i].renderScopeId].emplace(
                    commands[i].scopedNodeIndex, i);
            }
        }
        auto findCommandIndex =
            [&](const void *scopeId, int scopedNodeIndex,
                int flattenedNodeIndex) -> size_t {
            if(scopeId != nullptr && scopedNodeIndex >= 0) {
                const auto scopeIt =
                    commandIndexByScopedNode.find(scopeId);
                if(scopeIt != commandIndexByScopedNode.end()) {
                    const auto nodeIt =
                        scopeIt->second.find(scopedNodeIndex);
                    if(nodeIt != scopeIt->second.end()) {
                        return nodeIt->second;
                    }
                }
            }
            const auto flatIt =
                commandIndexByNode.find(flattenedNodeIndex);
            return flatIt == commandIndexByNode.end()
                ? commands.size()
                : flatIt->second;
        };
        struct PairScopeHash {
            size_t operator()(
                const std::pair<const void *, int> &value) const {
                const auto h1 =
                    std::hash<const void *>{}(value.first);
                return h1 ^ (std::hash<int>{}(value.second) << 1);
            }
        };
        // REF findNearestAncestorCommandIndex (PlayerRender.cpp 8343..8424):
        // resolve the nearest command for an authored ancestor reference.
        // Scoped identity wins; a flattened child motion may point through
        // source-less transform nodes that correctly have no command, so the
        // walk climbs the local node tree before crossing the next recorded
        // Player boundary, then tries the outer ancestor chain.
        auto findNearestAncestorCommandIndex =
            [&](const void *scopeId, int scopedNodeIndex,
                int flattenedNodeIndex,
                const std::vector<
                    detail::PlayerRuntime::PreparedRenderItem::
                        RenderAncestorReference> &outerAncestorChain,
                std::unordered_map<
                    const void *, std::unordered_map<int, size_t>>
                    &commandIndexByScopedNode,
                std::unordered_map<int, size_t> &commandIndexByNode,
                std::unordered_set<std::pair<const void *, int>,
                                   PairScopeHash> &visitedScopedNodes)
            -> size_t {
            auto walkScopedAncestors =
                [&](const void *candidateScopeId,
                    int candidateScopedNodeIndex,
                    int candidateFlattenedNodeIndex) -> size_t {
                while(true) {
                    const size_t commandIndex = findCommandIndex(
                        candidateScopeId, candidateScopedNodeIndex,
                        candidateFlattenedNodeIndex);
                    if(commandIndex < commands.size()) {
                        return commandIndex;
                    }
                    if(candidateScopeId == nullptr ||
                       candidateScopedNodeIndex < 0 ||
                       !visitedScopedNodes.insert(
                           {candidateScopeId, candidateScopedNodeIndex})
                            .second) {
                        break;
                    }
                    // Climb the scope's local node tree through nodes that
                    // have no command of their own.
                    auto scopeIt =
                        commandIndexByScopedNode.find(candidateScopeId);
                    (void)scopeIt;
                    const auto *scopeRuntime = static_cast<
                        const detail::PlayerRuntime *>(candidateScopeId);
                    if(scopeRuntime == nullptr ||
                       candidateScopedNodeIndex >=
                           static_cast<int>(
                               scopeRuntime->nodes.size())) {
                        break;
                    }
                    const int nextScopedNodeIndex =
                        scopeRuntime
                            ->nodes[static_cast<size_t>(
                                candidateScopedNodeIndex)]
                            .visibleAncestorIndex;
                    if(nextScopedNodeIndex == candidateScopedNodeIndex) {
                        break;
                    }
                    candidateScopedNodeIndex = nextScopedNodeIndex;
                    candidateFlattenedNodeIndex = -1;
                }
                return commands.size();
            };

            size_t commandIndex =
                walkScopedAncestors(scopeId, scopedNodeIndex,
                                    flattenedNodeIndex);
            if(commandIndex < commands.size()) {
                return commandIndex;
            }
            for(const auto &outerAncestor : outerAncestorChain) {
                commandIndex = walkScopedAncestors(
                    outerAncestor.renderScopeId,
                    outerAncestor.scopedNodeIndex, -1);
                if(commandIndex < commands.size()) {
                    return commandIndex;
                }
            }
            return commands.size();
        };

        // Pass 3 — parent wiring (REF 8455..8604 with the 8343..8424 scoped
        // ancestor resolver). Each command walks its parent chain through
        // (parentRenderScopeId, scopedParentNodeIndex) first, then its
        // outerRenderAncestorChain, and only falls back to the merged
        // numeric namespace when no scoped identity exists. Stencil groups
        // own their whole drawable descendant subtree.
        size_t parentedCommands = 0;
        for(size_t i = 0; i < commands.size(); ++i) {
            int ancestorNodeIndex = commands[i].parentNodeIndex;
            const void *ancestorScopeId = commands[i].parentRenderScopeId;
            int ancestorScopedNodeIndex = commands[i].scopedParentNodeIndex;
            const auto *ancestorOuterChain =
                &commands[i].outerRenderAncestorChain;
            std::unordered_set<size_t> visitedAncestorCommands;
            std::unordered_set<std::pair<const void *, int>,
                               PairScopeHash>
                visitedScopedAncestors;
            while(ancestorNodeIndex >= 0 ||
                  (ancestorScopeId != nullptr &&
                   ancestorScopedNodeIndex >= 0)) {
                const size_t ancestorCommandIndex =
                    findNearestAncestorCommandIndex(
                        ancestorScopeId, ancestorScopedNodeIndex,
                        ancestorNodeIndex, *ancestorOuterChain,
                        commandIndexByScopedNode, commandIndexByNode,
                        visitedScopedAncestors);
                if(ancestorCommandIndex >= commands.size() ||
                   !visitedAncestorCommands.insert(ancestorCommandIndex)
                        .second) {
                    break;
                }
                auto &ancestorCommand = commands[ancestorCommandIndex];
                if(ancestorCommand.groupOnly) {
                    const bool isStandaloneAlphaModifier =
                        ancestorCommand.stencilMaskInputs.empty() &&
                        (ancestorCommand.itemFlags & 7) == 6;
                    // The independent difference-alpha classification stays
                    // behind the REF policy seam, which is null in the
                    // public fallback renderer this port mirrors; the
                    // pairing fields remain wired for phase 2.
                    if(!ancestorCommand.stencilMaskInputs.empty() ||
                       isStandaloneAlphaModifier) {
                        ancestorCommand.childCommandIndices.push_back(
                            static_cast<int>(i));
                        commands[i].hasRenderParent = true;
                    }
                    break;
                }
                const int nextAncestorNodeIndex =
                    ancestorCommand.parentNodeIndex;
                if(nextAncestorNodeIndex == ancestorNodeIndex &&
                   ancestorCommand.parentRenderScopeId == ancestorScopeId &&
                   ancestorCommand.scopedParentNodeIndex ==
                       ancestorScopedNodeIndex) {
                    break;
                }
                ancestorNodeIndex = nextAncestorNodeIndex;
                ancestorScopeId = ancestorCommand.parentRenderScopeId;
                ancestorScopedNodeIndex =
                    ancestorCommand.scopedParentNodeIndex;
                ancestorOuterChain = &ancestorCommand.outerRenderAncestorChain;
            }
            if(commands[i].hasRenderParent) {
                ++parentedCommands;
            }
        }

        // Pass 4 — flags-6 alpha modifiers attach to a concrete parent
        // command (REF 8719..8741), resolved through the same scoped
        // ancestor resolver. Composition-side application of the item+264
        // alpha carrier lands with phase 2; until then the legacy
        // child-as-mask fallback inside the executor still covers these
        // groups, so the wiring is recorded graph-side only.
        for(size_t i = 0; i < commands.size(); ++i) {
            auto &modifier = commands[i];
            if(!modifier.groupOnly || !modifier.stencilMaskInputs.empty() ||
               (modifier.itemFlags & 7) != 6 || modifier.parentNodeIndex < 0) {
                continue;
            }
            std::unordered_set<std::pair<const void *, int>, PairScopeHash>
                modifierVisitedScopes;
            const size_t parentCommandIndex =
                findNearestAncestorCommandIndex(
                    modifier.parentRenderScopeId,
                    modifier.scopedParentNodeIndex, modifier.parentNodeIndex,
                    modifier.outerRenderAncestorChain,
                    commandIndexByScopedNode, commandIndexByNode,
                    modifierVisitedScopes);
            if(parentCommandIndex >= commands.size() ||
               parentCommandIndex == i) {
                continue;
            }
            commands[parentCommandIndex].stencilModifierCommandIndices
                .push_back(static_cast<int>(i));
            modifier.hasRenderParent = true;
        }

        // Pass 5 — authored stencil-mask wiring (REF 8745..8772) resolved
        // scope-first. An input carrying a foreign owner must NOT fall back
        // to the merged namespace: both eyes commonly use node 4/5, and a
        // flat hit would bind the group to an unrelated mask from an outer
        // player. Owner-less legacy inputs fall back to the group's own
        // scope, matching native sub_6C7440's scoped item walk.
        size_t maskWiredCount = 0;
        size_t maskBindFailCount = 0;
        // A mask input naming a type-3 wrapper root has no command of its
        // own (wrapper roots splice their subtree instead of emitting an
        // item). Resolve such an input to the first drawable descendant
        // command inside that wrapper's subtree: collect the wrapper's
        // descendant node indexes in its owning runtime (via node
        // parentIndex chains), then scan this frame's commands for the
        // first one whose scopedNodeIndex belongs to that set. The S6
        // geometry pass then backwrites the group rect from it, and the
        // executor composes the whole subtree through the wired item.
        auto resolveMaskInputOrSubtreeRoot =
            [&](const void *ownerScope, int scopedNodeIndex,
                size_t selfIndex) -> size_t {
            const auto *ownerRuntime =
                static_cast<const detail::PlayerRuntime *>(ownerScope);
            if(ownerRuntime == nullptr || scopedNodeIndex < 0 ||
               scopedNodeIndex >=
                   static_cast<int>(ownerRuntime->nodes.size())) {
                return commands.size();
            }
            const auto &maskNode =
                ownerRuntime->nodes[static_cast<size_t>(scopedNodeIndex)];
            if(maskNode.nodeType != 3) {
                return commands.size();
            }
            std::set<int> subtreeNodes;
            for(size_t ni = 0; ni < ownerRuntime->nodes.size(); ++ni) {
                int ancestor = ownerRuntime->nodes[ni].parentIndex;
                for(int guard = 0;
                    ancestor >= 0 &&
                    ancestor < static_cast<int>(
                                   ownerRuntime->nodes.size()) &&
                    guard < 256;
                    ++guard) {
                    if(ancestor == scopedNodeIndex) {
                        subtreeNodes.insert(static_cast<int>(ni));
                        break;
                    }
                    const int next =
                        ownerRuntime->nodes[static_cast<size_t>(ancestor)]
                            .parentIndex;
                    if(next == ancestor) {
                        break;
                    }
                    ancestor = next;
                }
            }
            if(subtreeNodes.empty()) {
                return commands.size(); // signals "unresolvable"
            }
            for(size_t ci = 0; ci < commands.size(); ++ci) {
                if(ci == selfIndex) {
                    continue;
                }
                if(commands[ci].renderScopeId == ownerScope &&
                   subtreeNodes.count(commands[ci].scopedNodeIndex) != 0) {
                    if(kRenderCommandGraphDiag) {
                        if(auto logger = LOGGER) {
                            logger->warn(
                                "emote.cmdgraph.subtreehit wrapper={} "
                                "descendants={} command={} scopedIdx={}",
                                scopedNodeIndex, subtreeNodes.size(), ci,
                                commands[ci].scopedNodeIndex);
                        }
                    }
                    return ci;
                }
            }
            // Authored self-reference: the referenced wrapper's only
            // drawable descendant is the referencing group itself (body
            // stencil groups 6/16/24 clip to their own children). REF
            // leaves such an input unwired and the executor applies the
            // children-as-alpha fallback ((stencilComposite&4) with no
            // mask items), so treat it as intentionally unwired.
            return commands.size() + 1;
        };
        auto appendMaskWrapperDescendants =
            [&](const void *ownerScope, int scopedNodeIndex,
                size_t selfIndex, ScopedRenderCommand &groupCommand) {
            const auto *ownerRuntime =
                static_cast<const detail::PlayerRuntime *>(ownerScope);
            if(ownerRuntime == nullptr || scopedNodeIndex < 0 ||
               scopedNodeIndex >=
                   static_cast<int>(ownerRuntime->nodes.size())) {
                return;
            }
            if(ownerRuntime->nodes[static_cast<size_t>(scopedNodeIndex)]
                   .nodeType != 3) {
                return;
            }
            const int wrapperParentIndex =
                ownerRuntime->nodes[static_cast<size_t>(scopedNodeIndex)]
                    .parentIndex;
            std::unordered_set<int> subtreeNodes;
            for(size_t ni = 0; ni < ownerRuntime->nodes.size(); ++ni) {
                int ancestor = ownerRuntime->nodes[ni].parentIndex;
                for(int guard = 0;
                    ancestor >= 0 &&
                    ancestor < static_cast<int>(ownerRuntime->nodes.size()) &&
                    guard < 256;
                    ++guard) {
                    if(ancestor == scopedNodeIndex) {
                        subtreeNodes.insert(static_cast<int>(ni));
                        break;
                    }
                    const int next = ownerRuntime
                        ->nodes[static_cast<size_t>(ancestor)].parentIndex;
                    if(next == ancestor) {
                        break;
                    }
                    ancestor = next;
                }
            }
            for(size_t ci = 0; ci < commands.size(); ++ci) {
                if(ci == selfIndex) {
                    continue;
                }
                bool inWrapperSubtree =
                    commands[ci].renderScopeId == ownerScope &&
                    subtreeNodes.count(commands[ci].scopedNodeIndex) != 0;
                if(!inWrapperSubtree) {
                    // Foreign child entries carry the outer wrapper in their
                    // ancestor chain rather than sharing the owner's scope.
                    // This is the cross-Player case that the merged numeric
                    // namespace cannot resolve on its own.
                    inWrapperSubtree = std::any_of(
                        commands[ci].outerRenderAncestorChain.begin(),
                        commands[ci].outerRenderAncestorChain.end(),
                        [&](const auto &ancestor) {
                            return ancestor.renderScopeId == ownerScope &&
                                (ancestor.scopedNodeIndex == scopedNodeIndex ||
                                 (wrapperParentIndex >= 0 &&
                                  ancestor.scopedNodeIndex ==
                                      wrapperParentIndex));
                        });
                }
                if(!inWrapperSubtree) {
                    continue;
                }
                if(std::find(groupCommand.childCommandIndices.begin(),
                             groupCommand.childCommandIndices.end(),
                             static_cast<int>(ci)) ==
                   groupCommand.childCommandIndices.end()) {
                    groupCommand.childCommandIndices.push_back(
                        static_cast<int>(ci));
                }
                commands[ci].hasRenderParent = true;
            }
            if(kRenderCommandGraphDiag) {
                if(auto logger = LOGGER) {
                    logger->warn(
                        "emote.cmdgraph.wrapperExpand wrapper={} "
                        "subtreeNodes={} group={} children={}",
                        scopedNodeIndex, subtreeNodes.size(),
                        groupCommand.nodeIndex,
                        groupCommand.childCommandIndices.size());
                }
            }
        };
        for(size_t gi = 0; gi < commands.size(); ++gi) {
            auto &command = commands[gi];
            for(const auto &input : command.stencilMaskInputs) {
                const void *fallbackScope =
                    input.second != nullptr ? input.second
                                            : command.renderScopeId;
                // Body/face type-12 groups author their mask through the
                // containing type-3 wrapper (the same node as
                // visibleAncestor). That wrapper is intentionally omitted
                // from the drawable command list, so binding only its first
                // descendant loses the rest of the group. Expand the whole
                // wrapper subtree before attempting single-mask wiring.
                if(command.groupOnly &&
                   (input.second == nullptr ||
                    input.second == command.renderScopeId) &&
                   input.first == command.item->visibleAncestorIndex) {
                    appendMaskWrapperDescendants(fallbackScope, input.first,
                                                 gi, command);
                    continue;
                }
                size_t maskCommandIndex =
                    findCommandIndex(input.second, input.first, input.first);
                if(maskCommandIndex >= commands.size() &&
                   (input.second == nullptr ||
                    input.second == command.renderScopeId)) {
                    maskCommandIndex =
                        findCommandIndex(command.renderScopeId, input.first,
                                         -1);
                }
                if(maskCommandIndex >= commands.size() ||
                   maskCommandIndex == gi) {
                    // Wrapper-root fallback: bind the mask to the first
                    // drawable descendant of the referenced wrapper
                    // subtree. The executor follows stencilMaskItems ->
                    // childItems recursively, so the composed result covers
                    // the whole subtree either way. A return of size()+1
                    // marks an authored self-reference — intentionally
                    // unwired, not a binding failure.
                    maskCommandIndex = resolveMaskInputOrSubtreeRoot(
                        fallbackScope, input.first, gi);
                    if(maskCommandIndex == commands.size() + 1) {
                        appendMaskWrapperDescendants(fallbackScope, input.first,
                                                     gi, command);
                        continue;
                    }
                }
                if(maskCommandIndex >= commands.size() ||
                   maskCommandIndex == gi) {
                    // Distinguish the two failure directions. A mask input
                    // naming a real drawable node that this frame turns off
                    // is an EMPTY mask: the group's content must disappear
                    // (closed eyes must not keep drawing the iris). Only a
                    // structurally unresolvable reference — a type-3 wrapper
                    // that owns no render item of its own — stays permissive,
                    // because dropping a whole body region is far worse than
                    // drawing it unclipped.
                    const auto *maskOwner =
                        static_cast<const detail::PlayerRuntime *>(
                            fallbackScope);
                    if(maskOwner != nullptr && input.first >= 0 &&
                       input.first <
                           static_cast<int>(maskOwner->nodes.size()) &&
                       maskOwner->nodes[static_cast<size_t>(input.first)]
                               .nodeType != 3) {
                        command.emptyAuthoredMask = true;
                    }
                    if(kRenderCommandGraphDiag) {
                        if(auto logger = LOGGER) {
                            logger->warn(
                                "emote.cmdgraph.maskbind.fail group={} "
                                "groupScoped={} inputNode={} inputScope={} "
                                "groupScope={} player={}",
                                command.nodeIndex, command.scopedNodeIndex,
                                input.first,
                                static_cast<const void *>(input.second),
                                command.renderScopeId,
                                static_cast<const void *>(this));
                        }
                    }
                    ++maskBindFailCount;
                    continue;
                }
                command.stencilMaskCommandIndices.push_back(
                    static_cast<int>(maskCommandIndex));
                commands[maskCommandIndex].item->stencilMaskReferenced = true;
                ++maskWiredCount;
            }
        }

        // The prepared-item pass owns a few native aggregation edges that do
        // not have an authored visibleAncestor (notably the self-seeded
        // type-12 stencil groups 6/16/24).  REF keeps those edges in the
        // command graph before deriving group geometry.  Project them here;
        // otherwise the graph command exists but has no children to union,
        // leaving an invalid clip and dropping the entire face/body group.
        std::unordered_map<const PreparedRenderItem *, size_t>
            commandIndexByItem;
        commandIndexByItem.reserve(commands.size());
        for(size_t ci = 0; ci < commands.size(); ++ci) {
            commandIndexByItem.emplace(commands[ci].item, ci);
        }
        for(size_t ci = 0; ci < commands.size(); ++ci) {
            auto &command = commands[ci];
            auto appendPreparedEdge = [&](PreparedRenderItem *related,
                                          bool maskEdge) {
                if(!related) {
                    return;
                }
                const auto it = commandIndexByItem.find(related);
                if(it == commandIndexByItem.end() || it->second == ci) {
                    return;
                }
                auto &indices = maskEdge ? command.stencilMaskCommandIndices
                                         : command.childCommandIndices;
                if(std::find(indices.begin(), indices.end(),
                             static_cast<int>(it->second)) == indices.end()) {
                    indices.push_back(static_cast<int>(it->second));
                }
                if(command.groupOnly) {
                    commands[it->second].hasRenderParent = true;
                }
            };
            for(auto *child : command.item->childItems) {
                appendPreparedEdge(child, false);
            }
            for(auto *mask : command.item->stencilMaskItems) {
                appendPreparedEdge(mask, true);
            }
            if(kRenderCommandGraphDiag && command.groupOnly &&
               (command.nodeIndex == 6 || command.nodeIndex == 16 ||
                command.nodeIndex == 24)) {
                if(auto logger = LOGGER) {
                    logger->warn(
                        "emote.cmdgraph.edges node={} children={} masks={} "
                        "rawFlag21={} visibleAncestor={} scope={}",
                        command.nodeIndex, command.childCommandIndices.size(),
                        command.stencilMaskCommandIndices.size(),
                        command.item->rawFlag21 ? 1 : 0,
                        command.item->visibleAncestorIndex,
                        command.renderScopeId);
                }
            }
        }

        // S6 — mask-derived output geometry for scope-split stencil groups.
        // A type-12 group whose authored mask inputs resolve in a different
        // Player scope has no drawable of its own in this namespace (its
        // paint box stays empty), yet the executor composes it like any
        // buffered group: its output rect is the union of its mask
        // commands' clip rects. Likewise a mask wrapper-root group derives
        // its rect from its own child commands. Two rounds: children feed
        // wrapper roots first, then wrapper roots feed the stencil groups
        // that reference them.
        for(int round = 0; round < 2; ++round) {
            for(auto &command : commands) {
                auto *groupItem = command.item;
                if(groupItem->rawFlag21 || !command.groupOnly) {
                    continue;
                }
                int unionLeft = INT_MAX;
                int unionTop = INT_MAX;
                int unionRight = INT_MIN;
                int unionBottom = INT_MIN;
                auto absorb = [&](detail::PlayerRuntime::PreparedRenderItem
                                      *sourceItem) {
                    if(!sourceItem->rawFlag21) {
                        return;
                    }
                    unionLeft = std::min(unionLeft,
                                         sourceItem->clipRect[0]);
                    unionTop = std::min(unionTop, sourceItem->clipRect[1]);
                    unionRight = std::max(unionRight,
                                          sourceItem->clipRect[2]);
                    unionBottom = std::max(unionBottom,
                                           sourceItem->clipRect[3]);
                };
                for(const int maskIndex : command.stencilMaskCommandIndices) {
                    absorb(commands[maskIndex].item);
                }
                for(const int childIndex : command.childCommandIndices) {
                    absorb(commands[childIndex].item);
                }
                if(unionLeft >= unionRight || unionTop >= unionBottom) {
                    continue;
                }
                groupItem->clipRect = { unionLeft, unionTop, unionRight,
                                        unionBottom };
                groupItem->dirtyRect = groupItem->clipRect;
                groupItem->paintBox = { static_cast<float>(unionLeft),
                                        static_cast<float>(unionTop),
                                        static_cast<float>(unionRight),
                                        static_cast<float>(unionBottom) };
                groupItem->corners = { static_cast<float>(unionLeft),
                                       static_cast<float>(unionTop),
                                       static_cast<float>(unionRight),
                                       static_cast<float>(unionTop),
                                       static_cast<float>(unionRight),
                                       static_cast<float>(unionBottom),
                                       static_cast<float>(unionLeft),
                                       static_cast<float>(unionBottom) };
                groupItem->rawFlag21 = true;
            }
        }
        if(kRenderCommandGraphDiag && LOGGER) {
            for(const auto &command : commands) {
                if(!command.groupOnly ||
                   !(command.nodeIndex == 6 || command.nodeIndex == 16 ||
                     command.nodeIndex == 24)) {
                    continue;
                }
                LOGGER->warn(
                    "emote.cmdgraph.geometry node={} rawFlag21={} "
                    "clip=[{},{},{},{}] children={} masks={}",
                    command.nodeIndex, command.item->rawFlag21 ? 1 : 0,
                    command.item->clipRect[0], command.item->clipRect[1],
                    command.item->clipRect[2], command.item->clipRect[3],
                    command.childCommandIndices.size(),
                    command.stencilMaskCommandIndices.size());
            }
        }

        // Pass 6 — projection onto the PreparedRenderItem pointer channels
        // (parentItem/childItems/stencilMaskItems) that the proven executor
        // consumes. Legacy relationships produced by the prepare post-pass
        // are preserved (they own the type12 aggregation and wrapper-splice
        // semantics that the binary enforces); the graph only APPENDS the
        // scoped mask bindings the legacy numeric namespace could not
        // resolve. Flags-6 modifier edges stay graph-only until the
        // executor learns the item+264 alpha carrier (phase 2).
        const auto isPassThroughGroup =
            [](const ScopedRenderCommand &command) {
            // A group whose authored mask went empty this frame is NOT a
            // pass-through group: its children must be consumed (and then
            // dropped), not promoted to standalone unclipped draws.
            return command.groupOnly && command.item != nullptr &&
                !command.emptyAuthoredMask &&
                !command.hasOwnSource && command.item->sourceKey.empty() &&
                !command.childCommandIndices.empty() &&
                command.stencilMaskCommandIndices.empty() &&
                command.stencilModifierCommandIndices.empty() &&
                command.opacity >= 255;
        };
        auto appendUniqueItem =
            [](std::vector<detail::PlayerRuntime::PreparedRenderItem *> &list,
               detail::PlayerRuntime::PreparedRenderItem *item) {
                if(std::find(list.begin(), list.end(), item) == list.end()) {
                    list.push_back(item);
                }
            };
        for(auto &command : commands) {
            for(const int childIndex : command.childCommandIndices) {
                auto *childItem = commands[childIndex].item;
                appendUniqueItem(command.item->childItems, childItem);
                if(!isPassThroughGroup(command) &&
                   childItem->parentItem == nullptr) {
                    childItem->parentItem = command.item;
                }
            }
            for(const int maskIndex : command.stencilMaskCommandIndices) {
                auto *maskItem = commands[maskIndex].item;
                appendUniqueItem(command.item->stencilMaskItems, maskItem);
                if(!isPassThroughGroup(command) &&
                   maskItem->parentItem == nullptr) {
                    maskItem->parentItem = command.item;
                }
            }
        }

        detail::logoChainTraceLogf(
            motionPath, "renderCommand.count", "0x6C4E28",
            _clampedEvalTime,
            "canvas={}x{} preparedItems={} renderCommands={} "
            "parentedCommands={} maskWired={} maskBindFail={}",
            canvasWidth, canvasHeight,
            _runtime->preparedRenderItems.size(), commands.size(),
            parentedCommands, maskWiredCount, maskBindFailCount);
        if(kRenderCommandGraphDiag) {
            if(auto logger = LOGGER) {
                logger->warn(
                    "emote.cmdgraph.count player={} items={} commands={} "
                    "parented={} maskWired={} maskBindFail={} "
                    "rejects[noDraw,skip0,skip1,opa0,noClip]={},{},{},{},{}",
                    static_cast<const void *>(this),
                    _runtime->preparedRenderItems.size(), commands.size(),
                    parentedCommands, maskWiredCount, maskBindFailCount,
                    cmdGraphGateRejects[0], cmdGraphGateRejects[1],
                    cmdGraphGateRejects[2], cmdGraphGateRejects[3],
                    cmdGraphGateRejects[4]);
            }
        }
        const bool ok = !_runtime->preparedRenderItems.empty();
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::motionTraceRenderBuildCommandsLeave(
            this, static_cast<int>(canvasWidth),
            static_cast<int>(canvasHeight));
#endif
        return ok;
    }

    bool Player::executeLayerRenderCommands(iTJSDispatch2 *renderLayerObject,
                                             bool skipUpdate) {
        if(!renderLayerObject || !_runtime || !_runtime->activeMotion) {
            return false;
        }
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::MotionTraceRenderExecuteScope renderTrace(
            this, renderLayerObject, skipUpdate);
#endif
        const auto motionPath = _runtime->activeMotion->path;
        const bool renderProfileEnabled = [] {
            const char *env = std::getenv("KRKR_EMOTE_RENDER_PROFILE");
            return env && env[0] != '\0' && env[0] != '0';
        }();
        const auto executeStart = renderProfileEnabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};

        auto *renderLayer = resolveNativeLayer(renderLayerObject);
        if(!renderLayer) {
            renderLayer =
                resolvePrivateMotionGLLNativeLike_0x6DE24C(renderLayerObject);
        }
        iTJSDispatch2 *scratchOwner = resolveMainWindowOwnerObject();
        iTJSDispatch2 *scratchParent = resolveMainWindowPrimaryLayerObject();
        if(scratchParent && !resolveNativeLayer(scratchParent)) {
            if(auto *resolved = tryResolveLayerDispatch(
                   tTJSVariant(scratchParent, scratchParent))) {
                scratchParent = resolved;
            }
        }
        if(!scratchParent) {
            scratchParent = renderLayerObject;
        }
        if(scratchParent && !resolveNativeLayer(scratchParent)) {
            scratchParent = renderLayerObject;
        }
        detail::logoChainTraceLogf(
            motionPath, "execute.setup.pre", "0x6C7440", _clampedEvalTime,
            "renderLayer={} scratchOwner={} scratchParent={} "
            "renderLayerNative={} scratchParentNative={}",
            static_cast<const void *>(renderLayerObject),
            static_cast<const void *>(scratchOwner),
            static_cast<const void *>(scratchParent),
            static_cast<const void *>(renderLayer),
            static_cast<const void *>(resolveNativeLayer(scratchParent)));
        detail::logoChainTraceLogf(
            motionPath, "execute.begin", "0x6C7440", _clampedEvalTime,
            "renderItems={} topLevelItems={} groupItems={} renderLayer={} "
            "scratchOwner={} scratchParent={} skipUpdate={}",
            _runtime->preparedRenderItems.size(),
            _runtime->preparedRenderItemsTopLevel.size(),
            _runtime->preparedRenderItemsGroup.size(),
            static_cast<const void *>(renderLayer),
            static_cast<const void *>(scratchOwner),
            static_cast<const void *>(scratchParent), skipUpdate ? 1 : 0);
        int snapshotCopyOrder = 0;
        if(!renderLayer) {
            detail::logoChainTraceCheck(
                motionPath, "execute.setup", "0x6C7440", _clampedEvalTime,
                "renderLayer should resolve before executeLayerRenderCommands",
                fmt::format("renderLayer={}",
                            static_cast<const void *>(renderLayer)),
                false,
                "SLA/Layer backend could not resolve native layers before "
                "copy");
            return false;
        }

        using PreparedRenderItem = detail::PlayerRuntime::PreparedRenderItem;
#if defined(KRKR2_WASMTIME_HEADLESS)
        const auto recordPostDrawCandidate = [&](iTJSDispatch2 *layerObject,
                                                 const char *samplePoint) {
            detail::motionTraceRecordPostDrawLayerCandidate(this, layerObject,
                                                            samplePoint);
        };
        const auto directItemCoversRenderTarget =
            [&](const PreparedRenderItem &item) {
                if(!renderLayer)
                    return false;
                float minX = item.corners[0];
                float maxX = item.corners[0];
                float minY = item.corners[1];
                float maxY = item.corners[1];
                for(size_t i = 2; i + 1 < item.corners.size(); i += 2) {
                    minX = std::min(minX, item.corners[i]);
                    maxX = std::max(maxX, item.corners[i]);
                    minY = std::min(minY, item.corners[i + 1]);
                    maxY = std::max(maxY, item.corners[i + 1]);
                }
                return minX <= 0.0f && minY <= 0.0f &&
                    maxX >= static_cast<float>(renderLayer->GetWidth()) &&
                    maxY >= static_cast<float>(renderLayer->GetHeight());
            };
#endif

        tTJSVariant layerClassObject;
        if(!getLayerClassDispatchVariantLike_0x5CB08C(layerClassObject)) {
            detail::logoChainTraceCheck(
                motionPath, "execute.layerClass", "0x6C7440", _clampedEvalTime,
                "Layer class dispatch should resolve before operateAffine",
                "global.Layer unavailable", false,
                "sub_6C7440 could not resolve Layer class dispatch");
            return false;
        }

        struct ResolvedSourceObject {
            std::shared_ptr<tTVPBaseBitmap> bitmap;
            // Kept for the headless post-draw probes. Normal rendering no
            // longer materializes a SourceCache Layer just to obtain pixels.
            iTJSDispatch2 *layerObject = nullptr;
            tTJSNI_BaseLayer *layer = nullptr;
            iTVPBaseBitmap *image = nullptr;
            tjs_int width = 0;
            tjs_int height = 0;
        };

        auto resolveSourceObjectLike_0x6C1B70 =
            [&](const PreparedRenderItem &item) -> ResolvedSourceObject {
            ResolvedSourceObject resolved;
            if(item.sourceKey.empty() || !_runtime->sourceCacheNative) {
                return resolved;
            }

            resolved.bitmap =
                _runtime->sourceCacheNative->loadRenderSourceBitmapByName(
                    detail::widen(item.sourceKey), item.srcRef, item.blendMode,
                    item.packedColors, item.sourceMotion);
            if(!resolved.bitmap) {
                return resolved;
            }

            resolved.image = resolved.bitmap.get();
            resolved.width = static_cast<tjs_int>(resolved.bitmap->GetWidth());
            resolved.height =
                static_cast<tjs_int>(resolved.bitmap->GetHeight());

            detail::logoChainTraceLogf(
                motionPath, "execute.source", "0x6C1B70/0x6A7BA8",
                _clampedEvalTime,
                "source={} bitmap={}x{}",
                item.sourceKey, resolved.width, resolved.height);
            return resolved;
        };

        const int playerStencilType = _maskMode;
        // REF keeps E-mote output on the linear sampler for both GPU and
        // software render managers.  Nearest is visibly destructive on the
        // small eye/lash source cells and turns fractional motion into hard
        // one-pixel jumps.
        const auto emoteStretchType = stLinear;
        // KRKR_EMOTE_LEGACY_RASTER=1 (regression A/B): restore the pre-
        // 9e28714 axis-aligned leaf shortcut. The current unconditional
        // sub-pixel affine rasters every meshType=0 leaf at its authored
        // fractional phase, which softened all silhouette ramps by ~1px
        // against the Aug-29 runtime (user screenshot baseline). The legacy
        // path integer-snaps axis-aligned leaves with lround — crisp edges
        // at the cost of the 1px twitch that 9e28714 set out to fix. Env
        // default keeps the current behavior.
        static const bool legacyRaster = [] {
            const char *env = std::getenv("KRKR_EMOTE_LEGACY_RASTER");
            return env && env[0] != '\0' && env[0] != '0';
        }();
        const auto axisAlignedRectBounds =
            [](const std::array<float, 8> &corners, float xOffset,
               float yOffset, tTVPRect &out) -> bool {
            constexpr float epsilon = 0.02f;
            if(std::fabs(corners[0] - corners[6]) > epsilon ||
               std::fabs(corners[2] - corners[4]) > epsilon ||
               std::fabs(corners[1] - corners[3]) > epsilon ||
               std::fabs(corners[5] - corners[7]) > epsilon) {
                return false;
            }
            const float left = std::min(corners[0], corners[2]) + xOffset;
            const float right = std::max(corners[0], corners[2]) + xOffset;
            const float top = std::min(corners[1], corners[5]) + yOffset;
            const float bottom = std::max(corners[1], corners[5]) + yOffset;
            out = { static_cast<int>(std::lround(left)),
                    static_cast<int>(std::lround(top)),
                    static_cast<int>(std::lround(right)),
                    static_cast<int>(std::lround(bottom)) };
            return out.left < out.right && out.top < out.bottom;
        };
        auto ensurePrivateOutputLayer =
            [&](tTJSVariant &slot) -> iTJSDispatch2 * {
            iTJSDispatch2 *layerObject =
                slot.Type() == tvtObject ? slot.AsObjectNoAddRef() : nullptr;
            if(!layerObject) {
                // Construction still needs a tree owner, but command scratch
                // surfaces must not remain attached to the primary layer.
                layerObject = ensureReusableLayerObject(
                    slot, scratchOwner, nullptr,
                    static_cast<tTVPLayerType>(ltAlpha), false);
            } else if(!configureReusableLayerObject(
                          layerObject, nullptr,
                          static_cast<tTVPLayerType>(ltAlpha), false, false)) {
                return nullptr;
            }
            if(auto *layer = resolveNativeLayer(layerObject);
               layer && layer->GetParent()) {
                layer->SetParent(nullptr);
            }
            return layerObject;
        };
        auto ensureLeafItemLayer =
            [&](PreparedRenderItem &item) -> iTJSDispatch2 * {
            if(kRenderCommandGraphEnabled) {
                // REF command buffers are private scratch surfaces. Keeping
                // them under the user-facing primary layer makes every
                // SetSize/Update invalidate the whole layer tree and turns a
                // 60 Hz motion into an O(parts × tree) recomposite. Reuse
                // the item slot under the current private SLA target instead.
                return ensurePrivateOutputLayer(item.leafLayer);
            }
            const tjs_int stateLayerId = item.layerId;
            if(stateLayerId == 0) {
                return ensureReusableLayerObject(
                    item.leafLayer, scratchOwner, scratchParent,
                    static_cast<tTVPLayerType>(ltAlpha), false);
            }

            auto &state = _runtime->renderLayerStates[stateLayerId];
            if(!state.initialized) {
                state.layerId = stateLayerId;
                state.absolute = _runtime->nextLayerAbsolute++;
                state.hitThreshold = 256;
                state.initialized = true;
                if(item.nodeIndex >= 0 &&
                   item.nodeIndex < static_cast<int>(_runtime->nodes.size())) {
                    const auto &node = _runtime->nodes[item.nodeIndex];
                    state.layerGetter =
                        getLayerGetter(detail::widen(node.layerName));
                }
            }

            auto *layerObject = ensureReusableLayerObject(
                state.layerObject, scratchOwner, scratchParent,
                static_cast<tTVPLayerType>(ltAlpha), false);
            if(!layerObject) {
                return nullptr;
            }
            item.rawFlag20 = true;
            persistNativeRenderItemFieldLifetimeLike_0x6C4E28(item);

            setObjectIntProperty(layerObject, TJS_W("absolute"),
                                 state.absolute);
            setObjectIntProperty(layerObject, TJS_W("hitThreshold"),
                                 state.hitThreshold);

            state.clipRect = { static_cast<float>(item.clipRect[0]),
                               static_cast<float>(item.clipRect[1]),
                               static_cast<float>(item.clipRect[2]),
                               static_cast<float>(item.clipRect[3]) };
            state.worldRect = { item.corners[0], item.corners[1],
                                item.corners[4], item.corners[5] };
            state.localRect = { item.localCorners[0], item.localCorners[1],
                                item.localCorners[4], item.localCorners[5] };
            state.packedColors = item.packedColors;
            state.isDirty = true;

            item.leafLayer = state.layerObject;
            return layerObject;
        };
        auto ensureComposedItemLayer =
            [&](PreparedRenderItem &item) -> iTJSDispatch2 * {
            if(kRenderCommandGraphEnabled) {
                return ensurePrivateOutputLayer(item.composedLayer);
            }
            return ensureReusableLayerObject(
                item.composedLayer, scratchOwner, scratchParent,
                static_cast<tTVPLayerType>(ltAlpha), false);
        };
        tTJSVariant graphScratchLayerSlot;
        iTJSDispatch2 *graphScratchLayerObject = nullptr;
        tTJSNI_BaseLayer *graphScratchLayer = nullptr;
        if(kRenderCommandGraphEnabled) {
            graphScratchLayerObject =
                ensurePrivateOutputLayer(graphScratchLayerSlot);
            graphScratchLayer = resolveNativeLayer(graphScratchLayerObject);
            if(!graphScratchLayerObject || !graphScratchLayer ||
               !prepareLayerForRender(
                   graphScratchLayerObject,
                   static_cast<int>(renderLayer->GetWidth()),
                   static_cast<int>(renderLayer->GetHeight()), 0x00000000)) {
                graphScratchLayerObject = nullptr;
                graphScratchLayer = nullptr;
            }
        }
        auto renderItemSourceToLayer =
            [&](PreparedRenderItem &item, iTJSDispatch2 *targetLayerObject,
                tTJSNI_BaseLayer *targetLayer, iTVPBaseBitmap *srcImage,
                const tTVPRect &sourceRect, const char *branch) -> bool {
            if(!targetLayerObject || !targetLayer) {
                return false;
            }
            const int clipWidth = item.clipRect[2] - item.clipRect[0];
            const int clipHeight = item.clipRect[3] - item.clipRect[1];
            if(clipWidth <= 0 || clipHeight <= 0) {
                return false;
            }
            if(!prepareLayerForRender(targetLayerObject, clipWidth, clipHeight,
                                      0x00000000)) {
                return false;
            }
            if(!srcImage || srcImage->GetWidth() <= 0 ||
               srcImage->GetHeight() <= 0) {
                return true;
            }
            if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
               motionPath.find("m2logo.mtn") != std::string::npos &&
               _clampedEvalTime >= 30.0 && _clampedEvalTime <= 50.0) {
                std::fprintf(
                    stderr,
                    "SNAPGEOM phase=leafSource frame=%.3f nodeIndex=%d "
                    "source=%s meshType=%d layerSize=%dx%d "
                    "sourceRect=[%d,%d,%d,%d] clipRect=[%d,%d,%d,%d] "
                    "worldCorners=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] "
                    "localCorners=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]\n",
                    _clampedEvalTime, item.nodeIndex,
                    item.sourceKey.empty() ? "<none>" : item.sourceKey.c_str(),
                    item.meshType, clipWidth, clipHeight, sourceRect.left,
                    sourceRect.top, sourceRect.right, sourceRect.bottom,
                    item.clipRect[0], item.clipRect[1], item.clipRect[2],
                    item.clipRect[3], item.corners[0], item.corners[1],
                    item.corners[2], item.corners[3], item.corners[4],
                    item.corners[5], item.corners[6], item.corners[7],
                    item.localCorners[0], item.localCorners[1],
                    item.localCorners[2], item.localCorners[3],
                    item.localCorners[4], item.localCorners[5],
                    item.localCorners[6], item.localCorners[7]);
            }
            const bool meshAsAffine = item.meshType == 1 &&
                item.meshDivX <= 2 && item.meshDivY <= 2;
            if(item.meshType == 0 || meshAsAffine) {
                if(legacyRaster && item.meshType == 0) {
                    tTVPRect destinationRect;
                    if(axisAlignedRectBounds(item.localCorners, 0.5f, 0.5f,
                                             destinationRect)) {
                        targetLayer->StretchCopy(destinationRect, srcImage,
                                                 sourceRect, emoteStretchType);
                        return true;
                    }
                }
                // Affine only.  Stretch takes an integer destination rect, so
                // an axis-aligned shortcut quantised the sub-pixel sway E-mote
                // authors per component: axis-aligned parts snapped a whole
                // pixel as their position crossed .5 while rotated neighbours
                // moved smoothly, and a part oscillating across the alignment
                // epsilon swapped rasterisers every frame.  AffineBlt is
                // sub-pixel accurate in both axes (LayerBitmapIntf.cpp adjusts
                // the source start by the fractional part of each edge).
                const auto localPts =
                    buildAffineTrianglePoints(item.localCorners, 0.0f, 0.0f);
                targetLayer->AffineCopy(localPts.data(), srcImage, sourceRect,
                                        emoteStretchType, _clearEnabled);
#if defined(KRKR2_WASMTIME_HEADLESS)
                recordPostDrawCandidate(
                    targetLayerObject,
                    "Player::executeLayerRenderCommands.item.afterAffineCopy");
#endif
            } else {
                if(item.localMeshPoints.empty() || item.meshDivX < 2 ||
                   item.meshDivY < 2) {
                    return false;
                }
                auto localMeshPoints =
                    buildMeshPoints(item.localMeshPoints, 0.0f, 0.0f);
                if(item.meshType == 1 || item.meshType == 2) {
                    targetLayer->MeshCopy(localMeshPoints.data(), item.meshDivX,
                                          item.meshDivY, srcImage, sourceRect,
                                          emoteStretchType, _clearEnabled);
#if defined(KRKR2_WASMTIME_HEADLESS)
                    recordPostDrawCandidate(
                        targetLayerObject,
                        "Player::executeLayerRenderCommands.item."
                        "afterMeshCopy");
#endif
                } else {
                    return false;
                }
            }
            detail::logoChainTraceLogf(
                motionPath, "execute.layerSource", "0x6C7440", _clampedEvalTime,
                "branch={} nodeIndex={} clipRect=[{},{},{},{}] layer={}x{} "
                "clearEnabled={}",
                branch, item.nodeIndex, item.clipRect[0], item.clipRect[1],
                item.clipRect[2], item.clipRect[3], clipWidth, clipHeight,
                _clearEnabled ? 1 : 0);
            return true;
        };
#if defined(KRKR2_WASMTIME_HEADLESS)
        auto renderAccurateSlaPostDrawCandidateLike_0x6C9CA8 =
            [&](PreparedRenderItem &item, const ResolvedSourceObject &source,
                const tTVPRect &sourceRect) -> bool {
            if(!detail::motionTraceIsAccurateSlaRenderActive() ||
               !renderLayer || !source.image) {
                return false;
            }

            // libkrkr2.so sub_6C9CA8 clips item+184..196 to the target
            // Layer, then sizes the tracked Layer to right-left/bottom-top
            // before calling affineCopy/meshCopy/bezierPatchCopy on it.
            float clipLeft = std::max(item.paintBox[0], 0.0f);
            float clipTop = std::max(item.paintBox[1], 0.0f);
            float clipRight = std::min(
                item.paintBox[2], static_cast<float>(renderLayer->GetWidth()));
            float clipBottom = std::min(
                item.paintBox[3], static_cast<float>(renderLayer->GetHeight()));
            if(!item.corners.empty()) {
                float minX = item.corners[0];
                float maxX = item.corners[0];
                float minY = item.corners[1];
                float maxY = item.corners[1];
                for(size_t i = 2; i + 1 < item.corners.size(); i += 2) {
                    minX = std::min(minX, item.corners[i]);
                    maxX = std::max(maxX, item.corners[i]);
                    minY = std::min(minY, item.corners[i + 1]);
                    maxY = std::max(maxY, item.corners[i + 1]);
                }
                clipLeft = std::max(clipLeft, std::floor(minX));
                clipTop = std::max(clipTop, std::floor(minY));
                clipRight = std::min(clipRight, std::ceil(maxX));
                clipBottom = std::min(clipBottom, std::ceil(maxY));
            }
            if(clipRight <= clipLeft || clipBottom <= clipTop) {
                return false;
            }
            const int clipWidth = static_cast<int>(clipRight - clipLeft);
            const int clipHeight = static_cast<int>(clipBottom - clipTop);
            // libkrkr2.so sub_6C9CA8 sizes the SLA item layer and invokes
            // affineCopy(clear=1) before this pass writes the final layer
            // type, so the checkpoint helper must start from the ltAlpha
            // transparent-white neutral color, not a stale reused type color.
            int layerWidth = clipWidth;
            int layerHeight = clipHeight;
            if(layerWidth <= 0 || layerHeight <= 0) {
                return false;
            }

            iTJSDispatch2 *candidateLayerObject = ensureLeafItemLayer(item);
            auto *candidateLayer = resolveNativeLayer(candidateLayerObject);
            if(!candidateLayerObject || !candidateLayer ||
               !prepareLayerForRender(candidateLayerObject, layerWidth,
                                      layerHeight, 0x00FFFFFFu)) {
                return false;
            }

            const float offsetX = -0.5f - clipLeft;
            const float offsetY = -0.5f - clipTop;
            if(item.meshType == 0) {
                const auto localPts =
                    buildAffineTrianglePoints(item.corners, offsetX, offsetY);
                candidateLayer->AffineCopy(localPts.data(), source.image,
                                           sourceRect, emoteStretchType, true);
                recordPostDrawCandidate(candidateLayerObject,
                                        "Player::executeLayerRenderCommands."
                                        "accurateSla.item.afterAffineCopy");
                return true;
            }
            if(item.meshPoints.empty() || item.meshDivX < 2 ||
               item.meshDivY < 2) {
                return false;
            }
            auto localMeshPoints =
                buildMeshPoints(item.meshPoints, offsetX, offsetY);
            if(item.meshType == 1 || item.meshType == 2) {
                candidateLayer->MeshCopy(localMeshPoints.data(), item.meshDivX,
                                         item.meshDivY, source.image,
                                         sourceRect, emoteStretchType, true);
                recordPostDrawCandidate(candidateLayerObject,
                                        "Player::executeLayerRenderCommands."
                                        "accurateSla.item.afterMeshCopy");
                return true;
            }
            return false;
        };
#endif
        auto chooseItemOutputLayerObject =
            [&](PreparedRenderItem &item) -> iTJSDispatch2 * {
            // REF selects a composed output whenever the command actually
            // has one. Gating this on stencilComposite drops ordinary wrapper
            // groups whose children are colour-composited without a mask.
            if(item.composedBuilt && item.composedLayer.Type() == tvtObject) {
                return item.composedLayer.AsObjectNoAddRef();
            }
            if(item.leafBuilt && item.leafLayer.Type() == tvtObject) {
                return item.leafLayer.AsObjectNoAddRef();
            }
            if(item.composedLayer.Type() == tvtObject) {
                return item.composedLayer.AsObjectNoAddRef();
            }
            return nullptr;
        };
        auto computeTargetLayerClipLike_0x6C7440 =
            [&](const PreparedRenderItem &item, RenderClipRect &outRect,
                bool &hasViewportClip) -> bool {
            hasViewportClip = false;
            if(item.hasViewport && item.viewport[2] >= item.viewport[0] &&
               item.viewport[3] >= item.viewport[1]) {
                const float clipLeft =
                    std::max(item.paintBox[0], floorf(item.viewport[0]));
                const float clipTop =
                    std::max(item.paintBox[1], floorf(item.viewport[1]));
                const float clipRight =
                    std::min(item.paintBox[2], ceilf(item.viewport[2]));
                const float clipBottom =
                    std::min(item.paintBox[3], ceilf(item.viewport[3]));
                if(clipLeft > clipRight || clipTop > clipBottom) {
                    return false;
                }

                const int left = static_cast<int>(clipLeft);
                const int top = static_cast<int>(clipTop);
                const int width = static_cast<int>(clipRight - clipLeft);
                const int height = static_cast<int>(clipBottom - clipTop);
                outRect = {
                    left,
                    top,
                    left + width,
                    top + height,
                };
                hasViewportClip = true;
                return true;
            }

            auto *clipTarget = graphScratchLayer ? graphScratchLayer : renderLayer;
            outRect = {
                0,
                0,
                clipTarget ? static_cast<int>(clipTarget->GetWidth()) : 0,
                clipTarget ? static_cast<int>(clipTarget->GetHeight()) : 0,
            };
            return true;
        };
        // The previous frame leaves the target clip reset. Avoid touching the
        // primary layer here on the graph path: ResetClip invalidates its
        // exposed-region tree and costs tens of milliseconds even when no
        // command has a viewport clip.
        bool targetLayerClipIsFull = kRenderCommandGraphEnabled;
        auto applyTargetLayerClipLike_0x6C7440 =
            [&](const PreparedRenderItem &item,
                RenderClipRect &outRect) -> bool {
            bool hasViewportClip = false;
            if(!computeTargetLayerClipLike_0x6C7440(item, outRect,
                                                    hasViewportClip)) {
                return false;
            }

            // libkrkr2.so Player_renderToCanvas_guess @ 0x6C77C4..0x6C78DC:
            // set target Layer clip before both direct and composed output. The
            // later operateAffine call still receives the full source rect.
            auto *clipTarget = graphScratchLayer ? graphScratchLayer : renderLayer;
            if(hasViewportClip) {
                clipTarget->SetClip(outRect.left, outRect.top,
                                    outRect.right - outRect.left,
                                    outRect.bottom - outRect.top);
                targetLayerClipIsFull = false;
            } else if(!kRenderCommandGraphEnabled || !targetLayerClipIsFull) {
                clipTarget->ResetClip();
                targetLayerClipIsFull = true;
            }

            const auto &actualClip = clipTarget->GetClip();
            outRect = {
                actualClip.left,
                actualClip.top,
                actualClip.right,
                actualClip.bottom,
            };
            return true;
        };

        const bool commandOutputCacheEnabled =
            detail::isEmoteLikeMotion(*_runtime) &&
            !_runtime->renderCommands.empty() &&
            [] {
                const char *env = std::getenv("KRKR_EMOTE_DISABLE_OUTPUT_CACHE");
                return !(env && env[0] != '\0' && env[0] != '0');
            }();
        const std::uint64_t commandCacheGeneration = commandOutputCacheEnabled
            ? ++_runtime->emoteCommandOutputCacheGeneration
            : 0;
        auto renderItemOutputSignature =
            [&](auto &&self, const PreparedRenderItem *item,
                std::unordered_set<const PreparedRenderItem *> &visiting)
            -> std::size_t {
            if(!item) {
                return 0;
            }
            if(!visiting.insert(item).second) {
                return 0x9e3779b9u;
            }
            std::size_t seed = renderCommandLeafReuseSignatureForItem(*item);
            renderReuseHashCombine(seed, std::hash<int>{}(item->opacity));
            renderReuseHashCombine(seed,
                                   std::hash<int>{}(item->stencilComposite));
            renderReuseHashCombine(seed, std::hash<bool>{}(item->skipFlag0));
            renderReuseHashCombine(seed, std::hash<bool>{}(item->rawFlag16));
            renderReuseHashCombine(seed,
                                   std::hash<bool>{}(item->groupOnly));
            renderReuseHashCombine(seed,
                                   std::hash<bool>{}(item->parentItem != nullptr));
            if(item->parentItem) {
                renderReuseHashCombine(
                    seed, std::hash<const void *>{}(
                              static_cast<const void *>(
                                  item->parentItem->nativeLifetimeOwner)));
                renderReuseHashCombine(
                    seed, std::hash<int>{}(item->parentItem->nativeLifetimeKey));
            }
            renderReuseHashCombine(seed,
                                   std::hash<std::size_t>{}(item->childItems.size()));
            renderReuseHashCombine(
                seed, std::hash<std::size_t>{}(item->stencilMaskItems.size()));
            for(const auto *child : item->childItems) {
                renderReuseHashCombine(seed, self(self, child, visiting));
            }
            for(const auto *mask : item->stencilMaskItems) {
                renderReuseHashCombine(seed, self(self, mask, visiting));
            }
            visiting.erase(item);
            return seed;
        };

        auto buildItemOutput = [&](auto &&self,
                                   PreparedRenderItem *itemPtr) -> bool {
            if(!itemPtr) {
                return false;
            }
            auto &item = *itemPtr;
            if(item.executedDirect || item.leafBuilt || item.composedBuilt) {
                return true;
            }
            const bool hasChildren = !item.childItems.empty() ||
                !item.stencilMaskItems.empty();
            // Command output cache (REF 10089-10203): identify a render item
            // by its owning runtime and local node index. Prepared-item
            // addresses are not stable across vector rebuilds/reallocation.
            detail::PlayerRuntime::EmoteCommandOutputCacheEntry
                *commandCacheEntry = nullptr;
            std::size_t itemLeafSignature = 0;
            std::size_t itemOutputSignature = 0;
            if(commandOutputCacheEnabled) {
                const auto cacheKey = fmt::format(
                    "command:{}:{}",
                    static_cast<const void *>(item.nativeLifetimeOwner),
                    item.nativeLifetimeKey);
                auto [cacheIt, cacheInserted] =
                    _runtime->emoteCommandOutputCache.try_emplace(cacheKey);
                (void)cacheInserted;
                commandCacheEntry = &cacheIt->second;
                commandCacheEntry->lastUseGeneration =
                    commandCacheGeneration;
                // Even on a signature miss the retained objects are useful
                // scratch buffers; prepareLayerForRender overwrites them
                // before the new output is exposed.
                item.leafLayer = commandCacheEntry->leafLayer;
                item.composedLayer = commandCacheEntry->composedLayer;
                itemLeafSignature = renderCommandLeafReuseSignatureForItem(
                    item);
                if(hasChildren) {
                    std::unordered_set<const PreparedRenderItem *> visiting;
                    itemOutputSignature =
                        renderItemOutputSignature(renderItemOutputSignature,
                                                  &item, visiting);
                } else {
                    itemOutputSignature = itemLeafSignature;
                }
                if(commandCacheEntry->outputValid &&
                   commandCacheEntry->outputSignature == itemOutputSignature) {
                    iTJSDispatch2 *cachedOutput =
                        commandCacheEntry->composedBuilt &&
                                commandCacheEntry->composedLayer.Type() == tvtObject
                            ? commandCacheEntry->composedLayer.AsObjectNoAddRef()
                            : (commandCacheEntry->leafBuilt &&
                                       commandCacheEntry->leafLayer.Type() == tvtObject
                                   ? commandCacheEntry->leafLayer.AsObjectNoAddRef()
                                   : nullptr);
                    if(cachedOutput && resolveNativeLayer(cachedOutput)) {
                        item.leafBuilt = commandCacheEntry->leafBuilt;
                        item.composedBuilt = commandCacheEntry->composedBuilt;
                        item.builtRect = item.clipRect;
                        ++_runtime->emoteCommandOutputCacheHits;
                        return true;
                    }
                    commandCacheEntry->outputValid = false;
                }
                if(commandCacheEntry->leafValid &&
                   commandCacheEntry->leafSignature == itemLeafSignature &&
                   item.leafLayer.Type() == tvtObject &&
                   resolveNativeLayer(
                       item.leafLayer.AsObjectNoAddRef())) {
                    item.leafBuilt = true;
                    item.builtRect = item.clipRect;
                    ++_runtime->emoteCommandLeafCacheHits;
                    if(!hasChildren) {
                        return true;
                    }
                } else {
                    item.leafBuilt = false;
                }
            }
            item.composedBuilt = false;
            const bool useDirectRenderPath =
                shouldUseDirectRenderPathLike_0x6C7440(item, _clearEnabled) &&
                !hasChildren && item.parentItem == nullptr && !item.skipFlag0 &&
                !item.rawFlag16 && !(_preview && item.skipFlag1) &&
                item.opacity > 0;

            const int clipWidth = item.clipRect[2] - item.clipRect[0];
            const int clipHeight = item.clipRect[3] - item.clipRect[1];
            if(!useDirectRenderPath) {
                if(item.rawFlag21 && (clipWidth <= 0 || clipHeight <= 0)) {
                    return false;
                }
                if(!item.rawFlag21) {
                    return false;
                }
            }

            auto source = resolveSourceObjectLike_0x6C1B70(item);
            const bool hasSourceBitmap =
                source.bitmap && source.width > 0 && source.height > 0;
            if(!hasSourceBitmap && item.childItems.empty()) {
                detail::logoChainTraceCheck(
                    motionPath, "execute.source", "0x6C7440", _clampedEvalTime,
                                "resolved source bitmap should exist with "
                                "positive image size",
                    fmt::format("nodeIndex={} source={} image={}x{}",
                                item.nodeIndex, item.sourceKey, source.width,
                                source.height),
                    false,
                    "sub_6C1B70 could not resolve a drawable source object");
                return false;
            }

            const tTVPRect sourceRect(0, 0, hasSourceBitmap ? source.width : 0,
                                      hasSourceBitmap ? source.height : 0);
            if(hasSourceBitmap) {
                detail::logoChainTraceCheck(
                    motionPath, "execute.srcRect", "0x6C7440", _clampedEvalTime,
                    fmt::format("full texture rect exp=[0,0,{},{}]",
                                source.width, source.height),
                    fmt::format("nodeIndex={} act=[{},{},{},{}]",
                                item.nodeIndex, sourceRect.left, sourceRect.top,
                                sourceRect.right, sourceRect.bottom),
                    true,
                    "sub_6C7440 source rect was not the full texture bounds");
            }

            if(useDirectRenderPath) {
                RenderClipRect directTargetRect;
                bool hasViewportClip = false;
                if(!computeTargetLayerClipLike_0x6C7440(item, directTargetRect,
                                                        hasViewportClip)) {
                    return false;
                }
                item.executedDirect = true;
                item.builtRect = {
                    directTargetRect.left,
                    directTargetRect.top,
                    directTargetRect.right,
                    directTargetRect.bottom,
                };
                return true;
            }
            if(!item.rawFlag21) {
                return false;
            }

            if(hasSourceBitmap) {
                iTJSDispatch2 *leafLayerObject = ensureLeafItemLayer(item);
                auto *leafLayer = resolveNativeLayer(leafLayerObject);
                if(!leafLayerObject || !leafLayer) {
                    detail::logoChainTraceCheck(
                        motionPath, "execute.workLayer", "0x6C7440",
                        _clampedEvalTime,
                        "leaf layer should resolve for buffered item path",
                        fmt::format("nodeIndex={} leafLayer={}", item.nodeIndex,
                                    static_cast<const void *>(leafLayer)),
                        false,
                        "sub_6C7440 could not allocate the per-item leaf layer");
                    return false;
                }

                if(!renderItemSourceToLayer(item, leafLayerObject, leafLayer,
                                            source.image, sourceRect,
                                            "item.leaf.affineCopy")) {
                    return false;
                }
                item.leafBuilt = true;
            } else {
                // Type-12/wrapper commands often carry no source of their own;
                // their output is the composition of child commands. Avoid
                // allocating and clearing a throwaway blank leaf surface.
                item.leafBuilt = false;
            }
            item.builtRect = item.clipRect;

            bool hasBuiltChildren = false;
            for(auto *childItem : item.childItems) {
                hasBuiltChildren = self(self, childItem) || hasBuiltChildren;
            }
            for(auto *maskItem : item.stencilMaskItems) {
                hasBuiltChildren = self(self, maskItem) || hasBuiltChildren;
            }

            if(!hasBuiltChildren) {
                return true;
            }

            iTJSDispatch2 *composedLayerObject = ensureComposedItemLayer(item);
            auto *composedLayer = resolveNativeLayer(composedLayerObject);
            if(!composedLayerObject || !composedLayer) {
                detail::logoChainTraceCheck(
                    motionPath, "execute.workLayer", "0x6C7440",
                    _clampedEvalTime,
                    "composed layer should resolve for parent item path",
                    fmt::format("nodeIndex={} composedLayer={}", item.nodeIndex,
                                static_cast<const void *>(composedLayer)),
                    false,
                    "sub_6C7440 could not allocate the composed output layer");
                return false;
            }

            if(!prepareLayerForRender(composedLayerObject, clipWidth,
                                      clipHeight, 0x00000000)) {
                return false;
            }
            if(item.leafBuilt) {
                const auto localRect = localRectFromItem(item);
                auto *leafLayer = resolveNativeLayer(
                    item.leafLayer.Type() == tvtObject
                        ? item.leafLayer.AsObjectNoAddRef()
                        : nullptr);
                if(!leafLayer || !leafLayer->GetMainImage()) {
                    return false;
                }
                composedLayer->CopyRect(0, 0, leafLayer->GetMainImage(),
                                        nullptr, localRect);
            }

            for(auto *childPtr : item.childItems) {
                if(!childPtr) {
                    continue;
                }
                auto &child = *childPtr;
                if(!child.rawFlag21 || child.rawFlag16) {
                    continue;
                }
                auto *childOutputLayerObject =
                    chooseItemOutputLayerObject(child);
                auto *childOutputLayer =
                    resolveNativeLayer(childOutputLayerObject);
                if(!childOutputLayerObject || !childOutputLayer) {
                    continue;
                }
                const auto childLocalRect = localRectFromItem(child);
                const auto childBlendMode =
                    resolveBlendOperationModeLike_0x6C7440(child.blendMode);
                const auto childOpacity =
                    static_cast<tjs_int>(std::clamp(child.opacity, 0, 255));
                if(childOpacity <= 0) {
                    continue;
                }
                composedLayer->OperateRect(
                    child.builtRect[0] - item.clipRect[0],
                    child.builtRect[1] - item.clipRect[1],
                    childOutputLayer->GetMainImage(), childLocalRect,
                    childBlendMode, childOpacity);
            }

            // A type-12 composite first unions its authored mask surfaces with
            // op-5, then applies that union to the colour group with flags&3.
            // Applying the group's full value (normally 0x5) directly to the
            // colour output would add alpha instead of cropping it.
            std::vector<MotionCompositeMaskSurface> compositeMaskSurfaces;
            compositeMaskSurfaces.reserve(item.stencilMaskItems.size());
            for(auto *maskPtr : item.stencilMaskItems) {
                if(!maskPtr || !maskPtr->rawFlag21 || maskPtr->rawFlag16) {
                    continue;
                }
                auto *maskLayerObject = chooseItemOutputLayerObject(*maskPtr);
                auto *maskLayer = resolveNativeLayer(maskLayerObject);
                if(!maskLayerObject || !maskLayer ||
                   !maskLayer->GetMainImage()) {
                    continue;
                }
                const int maskWidth =
                    maskPtr->builtRect[2] - maskPtr->builtRect[0];
                const int maskHeight =
                    maskPtr->builtRect[3] - maskPtr->builtRect[1];
                if(maskWidth <= 0 || maskHeight <= 0) {
                    continue;
                }
                compositeMaskSurfaces.push_back(
                    { maskLayerObject, maskPtr->builtRect[0],
                      maskPtr->builtRect[1], maskWidth, maskHeight,
                      maskPtr->stencilComposite, maskPtr->nodeIndex });
            }
            const int compositeMaskOperation = item.stencilComposite & 3;
            if((item.stencilComposite & 4) != 0 &&
               !compositeMaskSurfaces.empty() &&
               (compositeMaskOperation == 1 ||
                compositeMaskOperation == 2)) {
                if(compositeMaskSurfaces.size() == 1) {
                    // REF applies a single op-5 mask across the whole group,
                    // not only across the mask bitmap's own rectangle. This
                    // clears the iris/eyelid pixels outside the aperture and
                    // is what keeps the upper lash on the eye boundary.
                    const auto &surface = compositeMaskSurfaces.front();
                    applyMotionAlphaMaskLike_0x6AF104(
                        composedLayerObject, 0, 0, surface.layerObject,
                        item.clipRect[0] - surface.worldLeft,
                        item.clipRect[1] - surface.worldTop, clipWidth,
                        clipHeight, 64, playerStencilType,
                        compositeMaskOperation, motionPath, _clampedEvalTime,
                        item.nodeIndex, surface.nodeIndex);
                } else {
                    applyMotionCompositeMasksLike_0x6AF104(
                        composedLayerObject, item.clipRect[0], item.clipRect[1],
                        clipWidth, clipHeight, compositeMaskSurfaces, 64,
                        playerStencilType, item.stencilComposite, motionPath,
                        _clampedEvalTime, item.nodeIndex);
                }
            } else {
                for(const auto &surface : compositeMaskSurfaces) {
                    applyMotionAlphaMaskLike_0x6AF104(
                        composedLayerObject,
                        surface.worldLeft - item.clipRect[0],
                        surface.worldTop - item.clipRect[1],
                        surface.layerObject, 0, 0, surface.width,
                        surface.height, 64, playerStencilType,
                        surface.itemFlags & 3, motionPath, _clampedEvalTime,
                        item.nodeIndex, surface.nodeIndex);
                }
            }

            item.composedBuilt = true;
            // rememberCommandOutput (REF 10177-10199): retain the layer
            // objects and refresh signatures so the next frame with an
            // identical leaf signature can skip the raster.
            if(commandCacheEntry != nullptr) {
                commandCacheEntry->leafLayer = item.leafLayer;
                commandCacheEntry->composedLayer = item.composedLayer;
                commandCacheEntry->leafSignature = itemLeafSignature;
                commandCacheEntry->outputSignature = itemOutputSignature;
                commandCacheEntry->leafBuilt = item.leafBuilt;
                commandCacheEntry->composedBuilt = item.composedBuilt;
                commandCacheEntry->leafValid = item.leafBuilt;
                commandCacheEntry->outputValid =
                    item.leafBuilt || item.composedBuilt;
                commandCacheEntry->lastUseGeneration =
                    commandCacheGeneration;
            }
            return true;
        };

        // The REF executor walks the render-command graph, not the legacy
        // prepared top-level list. Synthetic type-12 stencil groups are
        // intentionally emitted only in the auxiliary group list
        // (topLevelList=false); using preparedRenderItemsTopLevel therefore
        // drops the group and its children before buildItemOutput can
        // compose them. That is the intermittent whole-face/body loss seen
        // when an action switches a group between direct and masked output.
        std::vector<PreparedRenderItem *> executionItems;
        if(kRenderCommandGraphEnabled && !_runtime->renderCommands.empty()) {
            executionItems.reserve(_runtime->renderCommands.size());
            const auto isPassThroughGroup =
                [](const auto &command) {
                return command.groupOnly && command.item != nullptr &&
                    !command.emptyAuthoredMask &&
                    !command.hasOwnSource &&
                    command.item->sourceKey.empty() &&
                    command.childCommandIndices.size() > 0 &&
                    command.stencilMaskCommandIndices.empty() &&
                    command.stencilModifierCommandIndices.empty() &&
                    command.opacity >= 255;
            };
            std::vector<bool> consumedByParent(
                _runtime->renderCommands.size(), false);
            for(const auto &command : _runtime->renderCommands) {
                if(isPassThroughGroup(command)) {
                    continue;
                }
                for(const int childIndex : command.childCommandIndices) {
                    if(childIndex >= 0 &&
                       childIndex <
                           static_cast<int>(consumedByParent.size())) {
                        consumedByParent[static_cast<size_t>(childIndex)] =
                            true;
                    }
                }
                for(const int maskIndex : command.stencilMaskCommandIndices) {
                    if(maskIndex >= 0 &&
                       maskIndex < static_cast<int>(consumedByParent.size())) {
                        consumedByParent[static_cast<size_t>(maskIndex)] =
                            true;
                    }
                }
            }
            for(size_t commandIndex = 0;
                commandIndex < _runtime->renderCommands.size();
                ++commandIndex) {
                const auto &command = _runtime->renderCommands[commandIndex];
                if(!command.item || command.alphaMaskOnly ||
                   command.emptyAuthoredMask ||
                   isPassThroughGroup(command) ||
                   (command.hasRenderParent &&
                    consumedByParent[commandIndex])) {
                    continue;
                }
                executionItems.push_back(command.item);
            }
        } else {
            executionItems = _runtime->preparedRenderItemsTopLevel;
        }
        // KRKR_EMOTE_GEOM_DUMP=1: per-draw final world geometry of every
        // executed item. This is the only way to tell a per-component
        // oscillation apart from a whole-sprite one: the series for a single
        // node either alternates between two values every frame or it does
        // not. Emits one line per executed item per draw; diagnostic only.
        {
            static const bool geomDump = [] {
                const char *env = std::getenv("KRKR_EMOTE_GEOM_DUMP");
                return env && env[0] != '\0' && env[0] != '0';
            }();
            if(geomDump) {
                static std::atomic<unsigned> drawSerial{ 0 };
                const unsigned serial = drawSerial.fetch_add(1);
                for(const auto *dumpItem : executionItems) {
                    if(!dumpItem) {
                        continue;
                    }
                    LOGGER->warn(
                        "emote.geomdump draw={} player={} node={} scope={} "
                        "scoped={} src='{}' x={:.4f} y={:.4f} "
                        "x2={:.4f} y2={:.4f} mesh={} opa={}",
                        serial, static_cast<const void *>(this),
                        dumpItem->nodeIndex,
                        static_cast<const void *>(dumpItem->renderScopeId),
                        dumpItem->scopedNodeIndex, dumpItem->sourceKey,
                        dumpItem->corners[0], dumpItem->corners[1],
                        dumpItem->corners[4], dumpItem->corners[5],
                        dumpItem->meshType, dumpItem->opacity);
                }
            }
        }
        const auto cacheHitsBefore = _runtime->emoteCommandOutputCacheHits;
        const auto leafCacheHitsBefore =
            _runtime->emoteCommandLeafCacheHits;
        std::size_t outputBuilt = 0;
        std::size_t outputBuildFailed = 0;
        std::size_t directOutputs = 0;
        std::size_t bufferedOutputs = 0;
        bool outputCopyException = false;
        auto *drawTargetLayer = graphScratchLayer ? graphScratchLayer : renderLayer;
        double outputBuildMs = 0.0;
        double outputCopyMs = 0.0;
        double outputClipMs = 0.0;
        double presentationCopyMs = 0.0;

        for(auto *itemPtr : executionItems) {
            if(!itemPtr) {
                continue;
            }
            auto &item = *itemPtr;

            const auto blendMode =
                resolveBlendOperationModeLike_0x6C7440(item.blendMode);
            const auto effectiveColor = unpackPackedRgba(item.packedColors[0]);
            const auto opa =
                static_cast<tjs_int>(std::clamp(item.opacity, 0, 255));
            if(opa <= 0) {
                continue;
            }

            // libkrkr2.so 0x6C7440 reads item+17/item+16 first, then updates
            // target Layer clip, and only then applies the preview item+18
            // gate.
            if(item.skipFlag0) {
                continue;
            }
            if(item.rawFlag16) {
                continue;
            }
            RenderClipRect targetLayerClip;
            const auto itemClipStart = renderProfileEnabled
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if(!applyTargetLayerClipLike_0x6C7440(item, targetLayerClip)) {
                if(renderProfileEnabled) {
                    outputClipMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - itemClipStart)
                        .count();
                }
                continue;
            }
            if(renderProfileEnabled) {
                outputClipMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - itemClipStart)
                    .count();
            }
            detail::logoChainTraceLogf(
                motionPath, "execute.setClip", "0x6C7440", _clampedEvalTime,
                "nodeIndex={} targetClip=[{},{},{},{}]", item.nodeIndex,
                targetLayerClip.left, targetLayerClip.top,
                targetLayerClip.right, targetLayerClip.bottom);
            if(_preview && item.skipFlag1) {
                continue;
            }
            if(!kRenderCommandGraphEnabled && item.parentItem) {
                continue;
            }
            const auto itemBuildStart = renderProfileEnabled
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if(!buildItemOutput(buildItemOutput, &item)) {
                if(renderProfileEnabled) {
                    outputBuildMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - itemBuildStart)
                        .count();
                }
                ++outputBuildFailed;
                continue;
            }
            if(renderProfileEnabled) {
                outputBuildMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - itemBuildStart)
                    .count();
            }
            ++outputBuilt;
            if(item.executedDirect) {
                ++directOutputs;
            } else {
                ++bufferedOutputs;
            }

            const auto itemCopyStart = renderProfileEnabled
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            try {
                if(item.executedDirect) {
                    auto source = resolveSourceObjectLike_0x6C1B70(item);
                    if(!source.bitmap || source.width <= 0 ||
                       source.height <= 0) {
                        continue;
                    }
                    const tTVPRect sourceRect(0, 0, source.width,
                                              source.height);
                    std::string branch("direct.operateAffine");
#if defined(KRKR2_WASMTIME_HEADLESS)
                    const auto emitDirectProbe =
                        [&](const char *samplePoint, const char *phase,
                            const char *executionMethod = "native-direct-call",
                            iTJSDispatch2 *sourceArgObject = nullptr,
                            tTJSNI_BaseLayer *sourceArgLayer = nullptr,
                            const char *sourceArgClass = nullptr) {
                            emitDirectExecuteDiagnostics(
                                this, samplePoint, phase, branch.c_str(),
                                executionMethod, item, renderLayer,
                                std::shared_ptr<tTVPBaseBitmap>{},
                                sourceArgObject, sourceArgLayer, sourceArgClass,
                                blendMode, opa, emoteStretchType);
                    };
#endif
                    const bool meshAsAffine = item.meshType == 1 &&
                        item.meshDivX <= 2 && item.meshDivY <= 2;
                    if(item.meshType == 0 || meshAsAffine) {
                        if(legacyRaster && item.meshType == 0) {
                            tTVPRect destinationRect;
                            if(axisAlignedRectBounds(item.corners, 0.0f, 0.0f,
                                                     destinationRect)) {
                                drawTargetLayer->OperateStretch(
                                    destinationRect, source.bitmap.get(),
                                    sourceRect, blendMode, opa,
                                    emoteStretchType);
                                continue;
                            }
                        }
                        // Affine only — see the leaf path above: an integer
                        // OperateStretch rect quantises per-component
                        // sub-pixel motion into visible twitching.
                        const auto worldPts = buildAffineTrianglePoints(
                            item.corners, -0.5f, -0.5f);
#if defined(KRKR2_WASMTIME_HEADLESS)
                        TVPResetSoftwareAffineDiagnosticsForWasmtime();
                        emitDirectProbe("Player::executeLayerRenderCommands."
                                        "direct.beforeOperateAffine",
                                        "before", "native-operateAffine");
#endif
                        operateAffineBitmapWithoutLayerUpdate(
                            drawTargetLayer, worldPts.data(), source.bitmap.get(),
                            sourceRect, blendMode, opa, emoteStretchType);
#if defined(KRKR2_WASMTIME_HEADLESS)
                        if(detail::motionTraceIsAccurateSlaRenderActive()) {
                            if(!renderAccurateSlaPostDrawCandidateLike_0x6C9CA8(
                                   item, source, sourceRect)) {
                                recordPostDrawCandidate(
                                    directItemCoversRenderTarget(item)
                                        ? renderLayerObject
                                        : (source.layerObject
                                               ? source.layerObject
                                               : renderLayerObject),
                                    "Player::executeLayerRenderCommands.direct."
                                    "afterOperateAffine."
                                    "accurateSlaCandidateFallback");
                            }
                        }
                        emitDirectProbe("Player::executeLayerRenderCommands."
                                        "direct.afterOperateAffine",
                                        "after", "native-operateAffine");
#endif
                    } else {
                        if(item.meshPoints.empty() || item.meshDivX < 2 ||
                           item.meshDivY < 2) {
                            continue;
                        }
                        auto worldMeshPoints =
                            buildMeshPoints(item.meshPoints, -0.5f, -0.5f);
                        if(item.meshType == 1 || item.meshType == 2) {
                            branch = "direct.operateMesh";
#if defined(KRKR2_WASMTIME_HEADLESS)
                            emitDirectProbe(
                                "Player::executeLayerRenderCommands.direct."
                                "beforeOperateMesh",
                                "before");
#endif
                            drawTargetLayer->OperateMesh(
                                worldMeshPoints.data(), item.meshDivX,
                                item.meshDivY, source.image, sourceRect,
                                blendMode, opa, emoteStretchType, _clearEnabled);
#if defined(KRKR2_WASMTIME_HEADLESS)
                            emitDirectProbe(
                                "Player::executeLayerRenderCommands.direct."
                                "afterOperateMesh",
                                "after");
#endif
                        } else {
                            continue;
                        }
                    }
                    detail::logoChainTraceLogf(
                        motionPath, "execute.copy", "0x6C7440",
                        _clampedEvalTime,
                        "branch={} nodeIndex={} clipRect=[{},{},{},{}] "
                        "dirtyRect=[{},{},{},{}] blendMode={} opacity={} "
                        "packedColor=[0x{:08x},0x{:08x},0x{:08x},0x{:08x}] "
                        "effectiveColor=[{},{},{},{}] visibleAncestorIndex={} "
                        "clearEnabled={} renderPath=direct workLayer=0x0 "
                        "renderLayer={}x{}",
                        branch, item.nodeIndex, item.clipRect[0],
                        item.clipRect[1], item.clipRect[2], item.clipRect[3],
                        item.dirtyRect[0], item.dirtyRect[1], item.dirtyRect[2],
                        item.dirtyRect[3], item.blendMode, opa,
                        item.packedColors[0], item.packedColors[1],
                        item.packedColors[2], item.packedColors[3],
                        effectiveColor[0], effectiveColor[1], effectiveColor[2],
                        effectiveColor[3], item.visibleAncestorIndex,
                        _clearEnabled ? 1 : 0, renderLayer->GetWidth(),
                        renderLayer->GetHeight());
                    if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
                       motionPath.find("m2logo.mtn") != std::string::npos &&
                       _clampedEvalTime >= 30.0 && _clampedEvalTime <= 50.0) {
                        std::fprintf(
                            stderr,
                            "SNAPCOPY order=%d frame=%.3f nodeIndex=%d "
                            "source=%s branch=%s clipRect=[%d,%d,%d,%d] "
                            "opacity=%d blend=%d\n",
                            snapshotCopyOrder++, _clampedEvalTime,
                            item.nodeIndex,
                            item.sourceKey.empty() ? "<none>"
                                                   : item.sourceKey.c_str(),
                            branch.c_str(), item.clipRect[0], item.clipRect[1],
                            item.clipRect[2], item.clipRect[3], opa,
                            item.blendMode);
                    }
                    continue;
                }

                auto *outputLayerObject = chooseItemOutputLayerObject(item);
                auto *outputLayer = resolveNativeLayer(outputLayerObject);
                if(!outputLayerObject || !outputLayer) {
                    continue;
                }

                const auto localRect = localRectFromItem(item);
                drawTargetLayer->OperateRect(
                    item.clipRect[0], item.clipRect[1],
                    outputLayer->GetMainImage(), localRect, blendMode, opa);
                detail::logoChainTraceLogf(
                    motionPath, "execute.copy", "0x6C7440", _clampedEvalTime,
                    "branch={} nodeIndex={} clipRect=[{},{},{},{}] "
                    "dirtyRect=[{},{},{},{}] blendMode={} opacity={} "
                    "packedColor=[0x{:08x},0x{:08x},0x{:08x},0x{:08x}] "
                    "effectiveColor=[{},{},{},{}] visibleAncestorIndex={} "
                    "clearEnabled={} renderPath=buffered outputLayer={}x{} "
                    "renderLayer={}x{} childCount={} phase={}",
                    item.composedBuilt ? "buffered.operateRect.composed"
                                       : "buffered.operateRect.leaf",
                    item.nodeIndex, item.clipRect[0], item.clipRect[1],
                    item.clipRect[2], item.clipRect[3], item.dirtyRect[0],
                    item.dirtyRect[1], item.dirtyRect[2], item.dirtyRect[3],
                    item.blendMode, opa, item.packedColors[0],
                    item.packedColors[1], item.packedColors[2],
                    item.packedColors[3], effectiveColor[0], effectiveColor[1],
                    effectiveColor[2], effectiveColor[3],
                    item.visibleAncestorIndex, _clearEnabled ? 1 : 0,
                    localRect.get_width(), localRect.get_height(),
                    renderLayer->GetWidth(), renderLayer->GetHeight(),
                    item.childItems.size(), 0);
                if(detail::logoSnapshotMarkEnabledForPath(motionPath) &&
                   motionPath.find("m2logo.mtn") != std::string::npos &&
                   _clampedEvalTime >= 30.0 && _clampedEvalTime <= 50.0) {
                    const char *snapBranch = item.composedBuilt
                        ? "buffered.operateRect.composed"
                        : "buffered.operateRect.leaf";
                    std::fprintf(
                        stderr,
                        "SNAPCOPY order=%d frame=%.3f nodeIndex=%d source=%s "
                        "branch=%s clipRect=[%d,%d,%d,%d] opacity=%d blend=%d "
                        "childCount=%zu phase=%d\n",
                        snapshotCopyOrder++, _clampedEvalTime, item.nodeIndex,
                        item.sourceKey.empty() ? "<none>"
                                               : item.sourceKey.c_str(),
                        snapBranch, item.clipRect[0], item.clipRect[1],
                        item.clipRect[2], item.clipRect[3], opa, item.blendMode,
                        item.childItems.size(), 0);
                }
            } catch(const eTJS &error) {
                outputCopyException = true;
                ++outputBuildFailed;
                if(LOGGER) {
                    LOGGER->error(
                        "emote.execute copy failed node={} source={} error={}",
                        item.nodeIndex, item.sourceKey,
                        error.getMessage().AsStdString());
                }
            } catch(const std::exception &error) {
                outputCopyException = true;
                ++outputBuildFailed;
                if(LOGGER) {
                    LOGGER->error(
                        "emote.execute copy failed node={} source={} error={}",
                        item.nodeIndex, item.sourceKey, error.what());
                }
            } catch(...) {
                outputCopyException = true;
                ++outputBuildFailed;
                if(LOGGER) {
                    LOGGER->error(
                        "emote.execute copy failed node={} source={} error=unknown",
                        item.nodeIndex, item.sourceKey);
                }
            }
            if(renderProfileEnabled) {
                outputCopyMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - itemCopyStart)
                    .count();
            }
        }

        // libkrkr2.so Player_renderToCanvas_guess @ 0x6C8FCC resets the target
        // Layer clip once the top-level render-item walk is complete.
        const auto finalClipResetStart = renderProfileEnabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if(graphScratchLayer) {
            const auto presentationCopyStart = renderProfileEnabled
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const auto &targetClip = renderLayer->GetClip();
            if(targetClip.left != 0 || targetClip.top != 0 ||
               targetClip.right != static_cast<tjs_int>(renderLayer->GetWidth()) ||
               targetClip.bottom != static_cast<tjs_int>(renderLayer->GetHeight())) {
                renderLayer->ResetClip();
            }
            const tTVPRect scratchRect(
                0, 0, static_cast<tjs_int>(graphScratchLayer->GetWidth()),
                static_cast<tjs_int>(graphScratchLayer->GetHeight()));
            renderLayer->OperateRect(0, 0, graphScratchLayer->GetMainImage(),
                                     scratchRect, omAlpha, 255);
            if(renderProfileEnabled) {
                presentationCopyMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - presentationCopyStart)
                        .count();
            }
            targetLayerClipIsFull = true;
        }
        if(!kRenderCommandGraphEnabled || !targetLayerClipIsFull) {
            renderLayer->ResetClip();
            targetLayerClipIsFull = true;
        }
        const double finalClipResetMs = renderProfileEnabled
            ? std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - finalClipResetStart)
                  .count()
            : 0.0;
        const auto updateStart = renderProfileEnabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if(!skipUpdate) {
            renderLayer->Update(false);
            detail::logoChainTraceLogf(
                motionPath, "execute.update", "0x6C7440", _clampedEvalTime,
                "renderLayer.Update(false) size={}x{}", renderLayer->GetWidth(),
                renderLayer->GetHeight());
        }
        const double updateMs = renderProfileEnabled
            ? std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - updateStart)
                  .count()
            : 0.0;
        // REF emote command output cache GC (11151-11173): every 120
        // generations evict entries unused for 240 generations, then trim
        // to a 512-entry cap.
        if(_runtime->emoteCommandOutputCacheGeneration > 0 &&
           (_runtime->emoteCommandOutputCacheGeneration % 120u) == 0u) {
            auto &cache = _runtime->emoteCommandOutputCache;
            for(auto it = cache.begin(); it != cache.end();) {
                if(it->second.lastUseGeneration + 240u <
                   _runtime->emoteCommandOutputCacheGeneration) {
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }
            while(cache.size() > 512u) {
                auto oldest = cache.begin();
                for(auto it = std::next(cache.begin()); it != cache.end();
                    ++it) {
                    if(it->second.lastUseGeneration <
                       oldest->second.lastUseGeneration) {
                        oldest = it;
                    }
                }
                cache.erase(oldest);
            }
        }
        if(kRenderCommandGraphDiag && LOGGER &&
           _runtime->emoteCommandOutputCacheHits +
                   _runtime->emoteCommandLeafCacheHits >
               0) {
            LOGGER->warn(
                "emote.exec.cache player={} generation={} hits={} leafHits={} "
                "entries={}",
                static_cast<const void *>(this),
                _runtime->emoteCommandOutputCacheGeneration,
                _runtime->emoteCommandOutputCacheHits,
                _runtime->emoteCommandLeafCacheHits,
                _runtime->emoteCommandOutputCache.size());
        }
        if(renderProfileEnabled && LOGGER) {
            const double executeMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - executeStart)
                    .count();
            LOGGER->info(
                "emote.render.profile path={} ms={:.3f} clipMs={:.3f} "
                "buildMs={:.3f} copyMs={:.3f} presentationMs={:.3f} updateMs={:.3f} finalResetMs={:.3f} items={} built={} failed={} direct={} buffered={} scratch={} "
                "cacheHits={} leafHits={} cacheEntries={}",
                motionPath, executeMs, outputClipMs, outputBuildMs, outputCopyMs,
                presentationCopyMs, updateMs, finalClipResetMs,
                executionItems.size(), outputBuilt, outputBuildFailed,
                directOutputs, bufferedOutputs,
                graphScratchLayer ? 1 : 0,
                _runtime->emoteCommandOutputCacheHits - cacheHitsBefore,
                _runtime->emoteCommandLeafCacheHits - leafCacheHitsBefore,
                _runtime->emoteCommandOutputCache.size());
        }
#if defined(KRKR2_WASMTIME_HEADLESS)
        renderTrace.setResult(!outputCopyException);
#endif
        return !outputCopyException;
    }

} // namespace motion
