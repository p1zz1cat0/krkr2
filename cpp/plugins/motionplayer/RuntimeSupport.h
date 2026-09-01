//
// Internal helpers for motionplayer/emoteplayer runtime state.
//
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "EmoteCompatInternal.h"
#include "tjs.h"
#include "psbfile/PSBFile.h"
#include "MotionNode.h"

namespace motion {
    class SourceCache;
}

namespace motion::detail {

    struct VariableFrameInfo {
        std::string label;
        double value = 0.0;
    };

    struct VariableControllerBinding {
        int type = -1;
        int index = -1;
        std::string source;
        std::string role;
    };

    struct SelectorControlOption {
        std::string label;
        double offValue = 0.0;
        double onValue = 0.0;
    };

    struct SelectorControlBinding {
        std::string label;
        std::vector<SelectorControlOption> options;
    };

    struct FixedControllerOutputBinding {
        std::string label;
        int type = -1;
        int index = -1;
        std::string role;
    };

    struct ClampControlBinding {
        int type = 0;
        std::string varLr;
        std::string varUd;
        double minValue = 0.0;
        double maxValue = 0.0;
    };

    struct TimelineControlFrame {
        double time = 0.0;
        bool isTypeZero = true;
        float value = 0.0f;
        double easingWeight = 1.0;
    };

    struct TimelineControlTrack {
        std::string label;
        // Aligned to libkrkr2.so sub_66FC5C byte at track+8:
        // set when label is present in instantVariableList (player+0x4F8).
        bool instantVariable = false;
        std::vector<TimelineControlFrame> frames;
    };

    struct TimelineControlBinding {
        std::string label;
        double loopBegin = -1.0;
        double loopEnd = -1.0;
        double lastTime = -1.0;
        std::vector<TimelineControlTrack> tracks;
    };

    struct TimelineControlKeyframe {
        float value = 0.0f;
        float duration = 0.0f;
        float weight = 1.0f;
    };

    struct TimelineControlAnimatorState {
        std::deque<TimelineControlKeyframe> queue;
        bool active = false;
        float currentValue = 0.0f;
        float startValue = 0.0f;
        float targetValue = 0.0f;
        float progress = 1.0f;
        float duration = 0.0f;
        float weight = 1.0f;
    };

    // Runtime-owned parameter entry. Aligned to libkrkr2.so's 56-byte
    // Player+384 parameter table populated inside Player_initNonEmoteMotion
    // (0x6B365C) via sub_6B1718 / sub_6B202C.
    struct MotionParameterEntry {
        std::string id;
        bool discretization = false;
        double rangeBegin = 0.0;
        double rangeEnd = 0.0;
        double rangeScale = 1.0;
        double value = 0.0;
        int mode = 0;
        // Authored motion-timeline subdivision.  distinct from the parameter
        // range; selector/gradient parameters use their own value count for
        // quantization (see parameterizedClipTime).
        double division = 0.0;
    };

    struct MotionClip {
        std::string label;
        std::string owner;
        bool loop = false;
        double loopTime = -1.0; // from PSB; >=0 means loop restart point
        double totalFrames = 0.0;
        // Primary layer storage — PSB array order, duplicates preserved.
        // Aligned to libkrkr2.so Player_buildNodeTree (0x6B51F0) reading
        // "layer" from Player+528 as a TJS Array iterated by index.
        std::vector<std::shared_ptr<const PSB::PSBDictionary>> layerList;
        std::vector<std::string> sourceCandidates;
        // Raw PSB objects retained for Player_initNonEmoteMotion (0x6B365C).
        // The parameter table is intentionally not cached here; it is rebuilt
        // on each player init to mirror libkrkr2.so ownership/lifetime.
        std::shared_ptr<const PSB::PSBDictionary> motionObject;
        std::shared_ptr<const PSB::PSBDictionary> contentObject;
        // Faces such as 目L/眉L carry no node-level parameterize; their layer
        // timelines are indexed by the motion's own parameter axis.  This
        // clip-level index points into PlayerRuntime::parameterEntries and is
        // used by parameterizedClipTime in Phase2 (aligned to Aether REF
        // MotionClip::defaultParameterIndex).
        int defaultParameterIndex = -1;
    };

    // Map an authored parameter value onto the motion timeline's frame axis
    // (REF transToTick: division × (raw − rangeBegin)/(rangeEnd − rangeBegin)).
    // The tick axis is the parameter division; clip.totalFrames−1 is only the
    // fallback when the entry has no division.  The result is a timeline-domain
    // time: frame selection (frameSelectionTimeLike_0x6B7E44) and child
    // crossfade parentTime both compare it against authored frame times.
    inline double parameterizedClipTime(const MotionClip &clip,
                                        const MotionParameterEntry &parameter,
                                        double value) {
        const double range = parameter.rangeEnd - parameter.rangeBegin;
        if(std::abs(range) <= 0.0000001) {
            return 0.0;
        }

        const double minimum =
            std::min(parameter.rangeBegin, parameter.rangeEnd);
        const double maximum =
            std::max(parameter.rangeBegin, parameter.rangeEnd);
        double normalized =
            (std::clamp(value, minimum, maximum) - parameter.rangeBegin) /
            range;

        // Discrete parameters select authored input values, while `division`
        // describes the motion timeline's subdivisions.  Those counts can be
        // different: selector parameters quantize by their own value count.
        if(parameter.discretization) {
            const double rangeMagnitude = std::abs(range);
            const double integerSteps = std::round(rangeMagnitude);
            const double selectorSteps =
                integerSteps >= 1.0 &&
                        std::abs(rangeMagnitude - integerSteps) <= 0.0000001
                    ? integerSteps
                    : parameter.division;
            if(selectorSteps > 0.0) {
                normalized = std::round(normalized * selectorSteps) /
                             selectorSteps;
            }
        }
        normalized = std::clamp(normalized, 0.0, 1.0);

        // F01（REF transToTick）：tick 轴 = 参数表 division，而不是
        // clip.totalFrames−1。totalFrames 的 lastTime 是“最后帧时间”，
        // 两者只在素材恰好对齐时相等；以 totalFrames 优先会在
        // lastTime 未写/长 clip 上把参数轴拉偏。无 division 时再回退
        // totalFrames−1 保底。
        // 与 REF 的偏差：REF transToTick 不做 clamp，这里归一化后保留
        // [0,1]，超范围写入落到端点帧，防止越界 tick 进入选帧。
        // F01-①（2026-09-01 定域）：本函数的返回值即原 DLL 参数对象 +0x28
        // 处的存储值（normalized tick），消费点见 PlayerUpdateChildMotion
        // 的 crossfade/case-3 注释——两边同域，无需换算。
        const double timelineEnd = parameter.division > 0.0
            ? parameter.division
            : std::max(0.0, clip.totalFrames - 1.0);
        return normalized * timelineEnd;
    }

    struct TimelineState {
        std::string label;
        int flags = 0;
        bool playing = false;
        bool loop = false;
        double loopTime =
            -1.0; // from PSB; >=0 means loop, <0 means stop at end
        double totalFrames = 0.0;
        double currentTime = 0.0;
        double blendRatio = 1.0;
        bool wasPlaying = false; // for edge detection in dispatchEvents
        bool controlInitialized = false;
        double controlLastAppliedTime = 0.0;
        std::vector<int> controlFrameCursor;
        std::vector<float> controlTrackValues;
        std::vector<TimelineControlAnimatorState> controlTrackAnimators;
        TimelineControlAnimatorState blendAnimator;
        bool blendAutoStop = false;
    };

    inline bool differenceTimelineOwnsLabel(
        const TimelineState &state, const TimelineControlBinding &binding,
        const std::string &label) {
        return std::any_of(
            binding.tracks.begin(), binding.tracks.end(),
            [&state, &label](const TimelineControlTrack &track) {
                return differenceTrackOwnsLabel(
                    state.playing, state.flags, state.blendRatio, label,
                    track.label, track.instantVariable);
            });
    }

    // Aligned to libkrkr2.so Player_dispatchEvents (0x6C4490):
    // type=0: onAction(param1, param2), type=1: onSync()
    struct MotionEvent {
        int type = 0;
        std::string param1;
        std::string param2;
    };

    // PSB root "screenSize" — the E-mote logical world boundary.  Aligned to
    // sdl3-ref emotefile::_screenSize / EmotePlayer::ResetDrawArea: the
    // progress limit area and render basis, NOT the Kirikiri layer size.
    // coord NaN→-origin / Inf→(width|height)-origin sentinels are resolved
    // against this boundary (MOTIONPLAYER_TEXTURE_WORLD_COORDS.md §3.3).
    struct ScreenSize {
        double originX = 0.0;
        double originY = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    // F07（REF emotenode::progress 的 emotelimit）：节点收到的有效区域。
    // 有尺寸的父节点（icon/blank，clipW/H/originX/Y 已解析）用它自己的
    // 矩形；motion/layout/clip 等无尺寸父节点把收到的区域原样下发；
    // root 收 PSB logicalScreen。NaN/Inf 坐标哨兵（evaluateTimelineLike）
    // 以该区域换算贴边值——区域不同，贴边像素不同。
    inline ScreenSize effectiveNodeLimLike_REF(const ScreenSize &parentLim,
                                               double parentWidth,
                                               double parentHeight,
                                               double parentOriginX,
                                               double parentOriginY) {
        if(parentWidth > 0.0 && parentHeight > 0.0) {
            return ScreenSize{parentOriginX, parentOriginY, parentWidth,
                              parentHeight};
        }
        return parentLim;
    }

    struct MotionSnapshot {
        std::string path;
        std::shared_ptr<PSB::PSBFile> file;
        std::shared_ptr<const PSB::PSBDictionary> root;
        ScreenSize screenSize;
        std::unordered_map<std::string, std::shared_ptr<const PSB::PSBResource>>
            resourcesByPath;
        tTJSVariant moduleValue;
        std::vector<std::string> mainTimelineLabels;
        std::vector<std::string> diffTimelineLabels;
        std::vector<std::string> variableLabels;
        std::unordered_map<std::string, bool> loopTimelines;
        std::unordered_map<std::string, double> timelineLoopTimes;
        std::unordered_map<std::string, double> timelineTotalFrames;
        std::unordered_map<std::string, std::pair<double, double>>
            variableRanges;
        std::unordered_map<std::string, std::vector<VariableFrameInfo>>
            variableFrames;
        std::unordered_map<std::string, VariableControllerBinding>
            controllerBindings;
        std::unordered_set<std::string> instantVariableLabels;
        std::unordered_map<std::string, SelectorControlBinding>
            selectorControls;
        std::vector<FixedControllerOutputBinding> fixedControllerOutputs;
        std::vector<ClampControlBinding> clampControls;
        std::vector<std::string> mirrorVariableMatchList;
        // Primary layer storage — PSB array order, duplicates preserved.
        // Aligned to libkrkr2.so Player_buildNodeTree (0x6B51F0) which reads
        // the "layer" TJS Array from Player+528 and iterates by index.
        std::vector<std::shared_ptr<const PSB::PSBDictionary>> layerList;
        std::vector<std::string> sourceCandidates;
        // Primary clip storage — PSB priority[] order preserved.
        // Aligned to libkrkr2.so Player+548 (motion.priority TJSArray stored at
        // 0x6B37D0) + Player+616 (priority[currentIndex].content at 0x6B38FC).
        // Duplicate clip labels are allowed (index-addressable) but the
        // auxiliary label→index map below resolves name-based lookups using
        // last-wins semantics to mirror Player+24 labelMap behaviour.
        std::vector<MotionClip> clipList;
        std::unordered_map<std::string, int> clipIndexByLabel;
        std::unordered_map<std::string, TimelineControlBinding>
            timelineControlByLabel;
        std::vector<std::string> resourceAliases;
        double width = 0.0;
        double height = 0.0;
        // 参考 sdl3/emotefile::_attach（不编译）: MultiCache 复合 PSB 交叉引用
        std::vector<std::shared_ptr<MotionSnapshot>> attachedSnapshots;
    };

    // Aligned to libkrkr2.so Player+1296 std::vector<LabelEntry> written by
    // Player_initVariables (0x6CD750). Each entry is 160 bytes in the binary
    // with these observed writes (offsets relative to entry base):
    //   +0   ttstr name   — from entry["scope"], split by ':' and take the
    //                       right half; empty when no scope / no colon.
    //   +24  ttstr label  — from entry["label"].
    //   +68  u8  flag68=1 — observed default (semantics not yet reversed).
    //   +124 u8  flag124=1 — observed default (semantics not yet reversed).
    // Read paths in the binary have not been fully traced; the struct exists
    // so the eager initialisation sequence can land without drifting further.
    struct VariableLabelEntry {
        ttstr name;
        ttstr label;
        bool flag68 = true;
        bool flag124 = true;
    };

    struct PlayerRuntime {
        std::unordered_map<std::string, std::shared_ptr<MotionSnapshot>>
            motionsByKey;
        // Aligned to libkrkr2.so player+656: SourceCache object variant.
        motion::SourceCache *sourceCacheNative = nullptr;
        tTJSVariant sourceCacheObject;
        std::shared_ptr<MotionSnapshot> activeMotion;
        // Active motion's logical world boundary (PSB root screenSize).
        // Zeroed when no motion is active; consumed by frame evaluation
        // (coord NaN/Inf sentinel fix) and upcoming render alignment.
        ScreenSize logicalScreen;
        // F07-①: wrapper-provided root region. For nested child Players this
        // carries the wrapper node's rectangle (REF: emotemotion::progress
        // receives the wrapper node's originX/originY/width/height, not the
        // file's root screenSize). Zeroed = fall back to logicalScreen.
        ScreenSize wrapperLim;
        // Incremented for every motion activation/deactivation. Controller
        // candidates bind to this generation so a failed init after a motion
        // switch cannot drive the new node tree with stale state.
        std::uint64_t motionGeneration = 0;
        // EYE_DIAG: per-instance serial for log attribution.
        std::uint64_t diagPlayerId = 0;
        std::unordered_map<std::string, TimelineState> timelines;
        std::vector<std::string> playingTimelineLabels;
        std::unordered_map<std::string, tjs_int> layerIdsByName;
        std::unordered_map<tjs_int, std::string> layerNamesById;
        tjs_int nextLayerAbsolute = 1;
        struct LayerRenderState {
            tjs_int layerId = 0;
            bool clipEnabled = true;
            bool initialized = false;
            bool isDirty = false;
            tjs_int absolute = 0;
            tjs_int hitThreshold = 256;
            tTJSVariant layerObject;
            tTJSVariant layerGetter;
            std::array<float, 4> clipRect{ 0.f, 0.f, 0.f, 0.f };
            std::array<float, 4> worldRect{ 0.f, 0.f, 0.f, 0.f };
            std::array<float, 4> localRect{ 0.f, 0.f, 0.f, 0.f };
            std::array<std::uint32_t, 4> packedColors{ 0xFF808080u, 0xFF808080u,
                                                       0xFF808080u,
                                                       0xFF808080u };
        };
        std::unordered_map<tjs_int, LayerRenderState> renderLayerStates;
        std::vector<tTJSVariant> backgrounds;
        std::vector<tTJSVariant> captions;
        std::unordered_map<std::string, bool> disabledSelectorTargets;
        tTJSVariant lastCanvas;
        tTJSVariant lastViewParam;
        // One-shot scene-entry evidence for the accurate-SLA path: the first
        // completed render per Player logs its item/raster counts regardless
        // of the slow-frame threshold.
        bool slaFirstRenderLogged = false;
        // Rolling per-frame statistics for the accurate-SLA path; emitted as
        // "sla.accurate.stats" every ~3s so animated scenes report their
        // average/max frame cost instead of only slow-frame spikes.
        double slaStatsWindowStart = 0.0;
        int slaStatsFrames = 0;
        double slaStatsMsSum = 0.0;
        double slaStatsMsMax = 0.0;
        double slaStatsRendered = 0.0;
        double slaStatsChanged = 0.0;
        // Rolling per-frame statistics for the progress path (progressMsLike).
        double slaProgressWindowStart = 0.0;
        int slaProgressFrames = 0;
        double slaProgressMsSum = 0.0;
        double slaProgressMsMax = 0.0;
        // Aligned to libkrkr2.so player+696: internal render layer consumed by
        // sub_6CE7D8 / sub_6CE938 style post-draw update.
        tTJSVariant internalRenderLayer;
        // Reusable work layer for sub_6C4E28-style per-item local clipping.
        tTJSVariant scratchWorkLayer;
        std::array<double, 6> drawAffineMatrix{ 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
        tjs_int nextLayerId = 1;
        tjs_int clearColor = 0;
        tjs_int width = 0;
        tjs_int height = 0;
        int alphaOpCounter = 0;
        bool resizable = false;
        bool flip = false;
        bool visible = true;
        double opacity = 1.0;
        double slant = 0.0;
        double zoom = 1.0;
        std::vector<MotionEvent> pendingEvents;
        std::vector<MotionParameterEntry> parameterEntries;
        std::unordered_map<std::string, size_t> parameterEntryById;
        MotionParameterEntry defaultParameterEntry;
        MotionParameterEntry *defaultParameterEntryPtr = nullptr;
        int defaultParameterEntryIndex = -1;
        const MotionClip *activeClip = nullptr;
        // Persistent node tree for updateLayers pipeline. Aligned to
        // libkrkr2.so Player+200 (std::deque of MotionNode). The constructor
        // creates index 0 as the root node; loaded layer trees append real
        // nodes at indices [1,end) during Player_buildNodeTree (0x6B51F0).
        std::deque<MotionNode> nodes;
        // Values inherited from the owning Player's evaluated controller
        // table. Keep them separate from the child's local neutral values so
        // a nested face Player cannot republish its seed pose over a parent
        // action on the next update pass.
        std::unordered_map<std::string, double> inheritedVariableInputs;
        // Layer evaluation overlays persistent, evaluated and inherited
        // variables on every tick (aligned to Aether REF
        // RuntimeSupport.h effectiveVariableScratch).  Retain the union's
        // nodes between ticks so stable E-mote variable tables update values
        // in place instead of allocating two temporary unordered_maps per
        // frame.  The generation excludes labels which disappeared this tick
        // without requiring the scratch map itself to be cleared.
        struct EffectiveVariableScratchEntry {
            double value = 0.0;
            std::uint64_t generation = 0;
            bool hasRoutingNode = false;
        };
        std::unordered_map<std::string, EffectiveVariableScratchEntry>
            effectiveVariableScratch;
        std::uint64_t effectiveVariableScratchGeneration = 0;

        std::uint64_t beginEffectiveVariableScratch() {
            ++effectiveVariableScratchGeneration;
            if(effectiveVariableScratchGeneration == 0) {
                effectiveVariableScratch.clear();
                effectiveVariableScratchGeneration = 1;
            }
            return effectiveVariableScratchGeneration;
        }

        void setEffectiveVariableScratch(
            const std::string &label, double value) {
            auto [it, inserted] = effectiveVariableScratch.try_emplace(label);
            (void)inserted;
            it->second.value = value;
            it->second.generation = effectiveVariableScratchGeneration;
            it->second.hasRoutingNode = false;
        }
        // Aligned to libkrkr2.so Player+1296 std::vector<LabelEntry>.
        // Populated eagerly by Player_initVariables (0x6CD750) right after
        // buildNodeTree on the play / setMotion path.
        std::vector<VariableLabelEntry> variableLabelEntries;
        // Node label → index map. Aligned to binary's std::map<ttstr,int> at
        // player+24. Populated during recursive build with last-write-wins
        // assignment, queried by sub_6F2228 equivalent.
        std::map<std::string, int> nodeLabelMap;

        // Native render-item fields from the anonymous 0x1B0 item built by
        // libkrkr2.so 0x6C2334 and consumed in-place by 0x6C4E28 / 0x6C7440.
        // These fields intentionally keep the native write lifecycle: +21 and
        // +216..228 are not blanket-cleared every frame.
        struct NativeRenderItemFields {
            bool rawFlag16 = false; // original item +16 = node+201
            bool skipFlag0 =
                false; // original render item +17 (0x6C2334 / 0x6C7440)
            bool skipFlag1 =
                false; // original render item +18 (0x6C2334 / 0x6C7440)
            bool drawFlag = false; // original render item +19
            bool rawFlag20 = false; // original item +20, set by sub_6C4E28
                                    // requireLayerId path
            bool rawFlag21 = false; // original item +21, drawable clip valid
                                    // after sub_6C4E28
            std::uint8_t stencilMaskRef = 0; // original item +22
            std::uint8_t stencilWriteRef = 0; // original item +23
            std::array<float, 4> paintBox{ 0.f, 0.f, 0.f,
                                           0.f }; // item+184..196
            std::array<float, 4> viewport{ 1.f, 1.f, -1.f,
                                           -1.f }; // item+200..212
            std::array<int, 4> clipRect{ 0, 0, 0, 0 }; // item+216..228
            std::array<int, 4> dirtyRect{ 0, 0, 0, 0 };
            int opacity = 255; // item+232
            // item+244 in libkrkr2.so sub_6C2334 @ 0x6C2A90 — stencil/composite
            // flags copied from node.stencilType; consumed by sub_6C7440 alpha
            // mask path `(item+244 & 4)` / `(item+244 & 3)==1`.
            int stencilComposite = 0;
        };

        struct RenderItemNativeFieldLifetime {
            bool rawFlag20 = false;
            bool rawFlag21 = false;
            std::array<int, 4> clipRect{ 0, 0, 0, 0 };
            std::array<int, 4> dirtyRect{ 0, 0, 0, 0 };
            std::array<float, 8> localCorners{};
            std::vector<float> localMeshPoints;
        };

        struct PreparedRenderItem : NativeRenderItemFields {
            int nodeIndex = 0;
            tTJSVariant srcRef;
            std::string sourceKey;
            // Flattened child keys are local to the snapshot that owns the
            // child, not necessarily to the root player's active motion.
            std::shared_ptr<MotionSnapshot> sourceMotion;
            bool hasOwnSource = false;
            bool groupOnly = false;
            bool topLevelList = true;
            bool groupList = false;
            bool selfSeedChildList = false;
            PlayerRuntime *nativeLifetimeOwner = nullptr;
            int nativeLifetimeKey = 0;
            // Scoped render identity (REF renderScopeId port). nodeIndex is
            // rewritten into the merged numeric namespace during foreign
            // flattening; these fields keep the creating runtime pointer and
            // its local index so cross-Player references resolve against the
            // authoring scope instead of the merged one. The pair must never
            // be offset by merge logic (same invariant as nativeLifetimeKey).
            const void *renderScopeId = nullptr;
            int scopedNodeIndex = -1;
            // Scoped identity of the authored render parent. Zero/negative
            // means "no scoped parent recorded yet". Child roots that get
            // externally re-attached keep their original scoped parent here
            // and record the containing ancestor in outerRenderAncestorChain.
            const void *parentRenderScopeId = nullptr;
            int scopedParentNodeIndex = -1;
            // Ancestor chain crossing Player boundaries: each reference is
            // the next outer composition candidate after the local ancestor
            // walk bottoms out in a foreign scope.
            struct RenderAncestorReference {
                const void *renderScopeId = nullptr;
                int scopedNodeIndex = -1;
            };
            std::vector<RenderAncestorReference> outerRenderAncestorChain;
            double sortKey = 0.0;
            int blendMode = 16;
            tTJSVariant contextVariant; // original item +248 (player+1012 copy)
            std::array<float, 8> corners{};
            std::array<float, 8> localCorners{};
            std::array<std::uint32_t, 4> packedColors{ 0xFF808080u, 0xFF808080u,
                                                       0xFF808080u,
                                                       0xFF808080u };
            bool hasViewport = false;
            int coordinateMode = 0;
            int objTriPriority = 0;
            int visibleAncestorIndex = -1;
            bool stencilMaskReferenced = false;
            // Resolved authored mask inputs for a type-12 composite. These
            // are kept separate from visibleAncestorIndex because a mask
            // layer is an alpha input, not necessarily the render parent of
            // the colour item it clips.
            std::vector<int> stencilMaskNodeIndices;
            // Owning-scope identity parallel to stencilMaskNodeIndices. The
            // referenced index stays valid inside its owning runtime's
            // namespace; foreign flattening must not offset it again. Same
            // length as stencilMaskNodeIndices on every appending path.
            std::vector<std::pair<int, const void *>>
                scopedStencilMaskInputs;
            int meshDivX = 0;
            int meshDivY = 0;
            int meshType = 0;
            std::vector<float> meshPoints;
            std::vector<float> localMeshPoints;
            int layerId = 0;
            int layerId2 = 0;
            PreparedRenderItem *parentItem =
                nullptr; // semantic mapping of item +264
            std::vector<PreparedRenderItem *>
                childItems; // semantic mapping of item +24
            std::vector<PreparedRenderItem *>
                stencilMaskItems; // dedicated item+304-like mask inputs
            tTJSVariant leafLayer; // item+304 variant
            tTJSVariant composedLayer; // item+324 variant
            std::array<int, 4> builtRect{ 0, 0, 0, 0 };
            bool leafBuilt = false;
            bool composedBuilt = false;
            bool executedDirect = false;
        };
        std::vector<PreparedRenderItem> preparedRenderItems; // player+936/944
        // REF nested-child prepared-item reuse gate. A child Player whose
        // layer state generation and inherited draw affine are unchanged
        // since its last prepare can hand back the previous entry list
        // instead of rebuilding it every parent frame. The top-level player
        // always advances its generation, so only nested children hit the
        // fast path.
        std::uint64_t layerStateGeneration = 0;
        std::uint64_t preparedLayerStateGeneration = 0;
        std::array<double, 6> preparedDrawAffineMatrix{ 1.0, 0.0, 0.0, 1.0,
                                                        0.0, 0.0 };
        bool preparedRenderItemsValid = false;
        // Draw-space AABB captured by the normal render preparation path.
        // Hit testing reads this snapshot instead of rebuilding render items.
        std::array<double, 4> lastPreparedDrawBounds{};
        bool hasLastPreparedDrawBounds = false;
        // Native-shaped a2/a3 split passed through sub_6C2334 -> sub_6C4E28
        // -> sub_6C7440. Both lists point directly into preparedRenderItems.
        std::vector<PreparedRenderItem *> preparedRenderItemsTopLevel;
        std::vector<PreparedRenderItem *> preparedRenderItemsGroup;
        std::unordered_map<int, RenderItemNativeFieldLifetime>
            renderItemNativeFieldLifetimeByNode;

        // REF render command graph (AetherKiri buildRenderCommands,
        // PlayerRender.cpp 8034..8793): a topology-only projection of one
        // PreparedRenderItem plus explicit render edges. Execution state
        // keeps living in item so both execution paths share the same
        // drawing primitives and native field lifetimes.
        struct ScopedRenderCommand {
            PreparedRenderItem *item = nullptr;
            // Merged/flattened key within this frame's prepared list.
            int nodeIndex = -1;
            // Owning runtime identity + its local node index (the port of
            // REF renderScopeId/scopedNodeIndex). Authored node references
            // resolve through these instead of the merged namespace so two
            // nested players that both use node 4/5 cannot steal each
            // other's mask layers.
            const void *renderScopeId = nullptr;
            int scopedNodeIndex = -1;
            // Scoped identity of the authored render parent (mirrors the
            // PreparedRenderItem fields of the same name).
            const void *parentRenderScopeId = nullptr;
            int scopedParentNodeIndex = -1;
            std::vector<detail::PlayerRuntime::PreparedRenderItem::
                            RenderAncestorReference>
                outerRenderAncestorChain;
            bool groupOnly = false;
            bool hasOwnSource = false;
            int blendMode = 16;
            int opacity = 255;
            // item+244 composite flags (node.stencilType copy); gates the
            // standalone alpha-modifier / difference-mask classification.
            int itemFlags = 0;
            int parentNodeIndex = -1; // authored visibleAncestorIndex
            // Authored mask inputs with their owning scopes, resolved in the
            // graph builder into stencilMaskCommandIndices.
            std::vector<std::pair<int, const void *>> stencilMaskInputs;
            bool hasRenderParent = false;
            bool alphaMaskOnly = false;
            bool implicitVisibleStencilGroup = false;
            std::vector<int> childCommandIndices;
            std::vector<int> stencilModifierCommandIndices;
            std::vector<int> stencilMaskCommandIndices;
            std::vector<int> differenceAlphaMaskSourceCommandIndices;
            std::vector<int> differenceAlphaMaskGroupCommandIndices;
            std::vector<int> differenceAlphaMaskInputCommandIndices;
            int differenceAlphaMaskOperation = 0;
        };
        std::vector<ScopedRenderCommand> renderCommands;

        // REF emote command output cache (RuntimeSupport.h 423-441). Layer
        // objects retained across frames keyed by stable command-list slot;
        // signatures decide whether pixels may be reused, so a topology
        // change reuses allocations without reusing pixels. GC: entries
        // unused for 240 generations are evicted every 120 generations,
        // capacity capped at 512.
        struct EmoteCommandOutputCacheEntry {
            std::size_t leafSignature = 0;
            std::size_t outputSignature = 0;
            std::size_t maskSignature = 0;
            tTJSVariant leafLayer;
            tTJSVariant composedLayer;
            tTJSVariant maskLayer;
            tTJSVariant unionMaskLayer;
            bool leafValid = false;
            bool outputValid = false;
            bool maskValid = false;
            bool leafBuilt = false;
            bool composedBuilt = false;
            std::uint64_t lastUseGeneration = 0;
        };
        std::unordered_map<std::string, EmoteCommandOutputCacheEntry>
            emoteCommandOutputCache;
        std::uint64_t emoteCommandOutputCacheGeneration = 0;
        std::uint64_t emoteCommandOutputCacheHits = 0;
        std::uint64_t emoteCommandLeafCacheHits = 0;

        // Legacy local scratch for old diagnostics. libkrkr2.so player+384 is
        // the parameter table initialized by Player_initNonEmoteMotion
        // (0x6B365C), not per-node storage; node+8 resolves into
        // parameterEntries via MotionNode::parameterizeIndex.
        struct PerNodeEvalData {
            double
                padding[5] = {}; // offsets 0-39 (unused in our current scope)
            double evalTime = 0.0;
            int dirtyFlag = 0;
            // F07: effective emotelimit this node received for the current
            // frame (parent rect or inherited region; root = logicalScreen).
            // Written parent-first in Phase2 before the node's own eval, so a
            // child's sentinel resolves against the parent's current rect.
            ScreenSize evalLim;
        };
        std::vector<PerNodeEvalData> perNodeEvalData;
        // Aligned to libkrkr2.so Player_playImpl (0x6B2284):
        // PSB root "type" field: 0=non-emote (motion), 1=emote
        bool isEmoteMode = false;
        bool emoteDiagLogged = false;
        bool emoteFirstEvalDiagLogged = false;
        std::string emoteDiagMotionPath;
        std::string emoteDiagLoggedClip;
        std::string cachedParameterMotionPath;
        // F02: parameter-table ownership is (motion path, active clip).
        // Cached separately so replaying another clip of the same motion
        // file rebuilds the table instead of reusing the previous clip's
        // id/division/range axis.
        std::string cachedParameterClipLabel;
    };

    // e-mote3 PSB type=0 但含 variableList；行为对齐 sdl3 _varList.size()>0
    // 分支。
    inline bool isEmoteLikeMotion(const PlayerRuntime &runtime) {
        return runtime.isEmoteMode ||
            (runtime.activeMotion &&
             !runtime.activeMotion->variableLabels.empty());
    }

    // Timeline loop wrap aligned to libkrkr2.so Player_progress_inner
    // (0x6C106C). Returns false when playback should stop. Guards degenerate
    // loopTime >= totalFrames configs that would otherwise spin forever.
    inline bool wrapTimelineCurrentTime(double &currentTime, double totalFrames,
                                        double loopTime) {
        if(totalFrames <= 0.0 || currentTime < totalFrames) {
            return true;
        }
        if(loopTime < 0.0) {
            currentTime = totalFrames;
            return false;
        }
        if(loopTime >= totalFrames) {
            currentTime = totalFrames;
            return false;
        }
        int guard = 0;
        while(currentTime >= totalFrames && ++guard < 1024) {
            currentTime = currentTime + loopTime - totalFrames;
        }
        if(currentTime >= totalFrames) {
            currentTime = totalFrames;
            return false;
        }
        return true;
    }

    inline double wrapTimelineCurrentTimeValue(double time, double totalFrames,
                                               double loopEnd) {
        if(totalFrames <= 0.0 || time < totalFrames) {
            return time;
        }
        if(loopEnd < 0.0 || loopEnd >= totalFrames) {
            return totalFrames;
        }
        int guard = 0;
        while(time >= totalFrames && ++guard < 1024) {
            time = time - totalFrames + loopEnd;
        }
        return time >= totalFrames ? totalFrames : time;
    }

    void ensureRootNodeLike_0x6CED30(PlayerRuntime &runtime);
    void resetNodeTreeKeepRootLike_0x6B56F8(PlayerRuntime &runtime);
    std::shared_ptr<PlayerRuntime> makePlayerRuntime();

    std::string narrow(const ttstr &value);
    ttstr widen(const std::string &value);

    std::vector<ttstr> buildMotionLookupCandidates(const ttstr &name);
    bool resolveExistingPath(const std::vector<ttstr> &candidates,
                             ttstr &resolved);
    void appendEmbeddedSourceCandidates(const MotionSnapshot &snapshot,
                                        const std::string &source,
                                        std::vector<ttstr> &candidates);

    // Resolve PSB layer/node reference: inline dict or index into root
    // layerList (参考 sdl3 emotenode children / emotemotion layer[]).
    std::shared_ptr<const PSB::PSBDictionary> resolveLayerDictionaryReference(
        const std::shared_ptr<PSB::IPSBValue> &item,
        const std::vector<std::shared_ptr<const PSB::PSBDictionary>>
            &layerList);

    // MultiCache composite: merge attached PSB resources into primary snapshot
    // (参考 sdl3 emotefile::_attach resource lookup).
    void mergeAttachedSnapshotResources(MotionSnapshot &primary,
                                        const MotionSnapshot &attached);

    std::shared_ptr<MotionSnapshot> loadMotionSnapshot(const ttstr &path,
                                                       tjs_int decryptSeed);
    tTJSVariant loadPSBVariant(const ttstr &path, tjs_int decryptSeed);

    void
    registerModuleSnapshot(const tTJSVariant &module,
                           const std::shared_ptr<MotionSnapshot> &snapshot);
    std::shared_ptr<MotionSnapshot>
    lookupModuleSnapshot(const tTJSVariant &module);
    void unregisterModuleSnapshot(const tTJSVariant &module);
    void clearModuleSnapshots();

    tTJSVariant makeArray(const std::vector<tTJSVariant> &items);
    tTJSVariant makeDictionary(
        const std::vector<std::pair<std::string, tTJSVariant>> &entries);
    std::vector<tTJSVariant>
    stringsToVariants(const std::vector<std::string> &values);

    void
    primeTimelineStates(std::unordered_map<std::string, TimelineState> &states,
                        const MotionSnapshot &snapshot);
    void stepTimelines(std::unordered_map<std::string, TimelineState> &states,
                       double dt, std::vector<MotionEvent> *events = nullptr);

    bool logoChainTraceEnabled();
    bool logoChainTraceEnabledForPath(const std::string &motionPath);
    bool logoChainTraceEnabled(const std::shared_ptr<MotionSnapshot> &snapshot);
    bool logoSnapshotMarkEnabled();
    bool logoSnapshotMarkEnabledForPath(const std::string &motionPath);
    void resetLogoChainTraceSession(const std::string &motionPath);
    void logoChainTraceLog(const std::string &motionPath, const char *stage,
                           const char *func, double frameTime,
                           const std::string &message);
    void logoChainTraceCheck(const std::string &motionPath, const char *stage,
                             const char *func, double frameTime,
                             const std::string &expected,
                             const std::string &actual, bool ok,
                             const std::string &likelyRootCause = {});
    void logoChainTraceSummary(const std::string &motionPath, const char *func,
                               double frameTime, const std::string &note = {});

    template <typename... Args>
    inline void
    logoChainTraceLogf(const std::string &motionPath, const char *stage,
                       const char *func, double frameTime,
                       fmt::format_string<Args...> format, Args &&...args) {
        if(!logoChainTraceEnabledForPath(motionPath)) {
            return;
        }
        logoChainTraceLog(motionPath, stage, func, frameTime,
                          fmt::format(format, std::forward<Args>(args)...));
    }

    // Scan PSB layer tree for action/sync events between prevTime and newTime.
    // Aligned to libkrkr2.so: updateLayers queues events during tree
    // evaluation.
    void scanLayerActions(const MotionSnapshot &snapshot, double prevTime,
                          double newTime, std::vector<MotionEvent> &events);

} // namespace motion::detail
