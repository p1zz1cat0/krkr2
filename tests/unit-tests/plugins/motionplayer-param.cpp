//
// F01 parameter-axis contract fixtures (REF transToTick single track).
// parameterizedClipTime is the only parameterize→tick mapping; these cases
// pin the division axis, the totalFrames−1 fallback, range clamping and
// selector quantization so the axis cannot silently flip back to a raw-frame
// or totalFrames-first contract.
//
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "motionplayer/EmoteCompatInternal.h"
#include "motionplayer/PlayerInternal.h"
#include "motionplayer/RuntimeSupport.h"

namespace {
    const std::string kUnlistedSource = "motion/tail_parts/ohagi";
    const std::string kUnlistedLabel = "しっぽ";
} // namespace

namespace {
    motion::detail::MotionParameterEntry
    entry(double rangeBegin, double rangeEnd, double division,
          bool discretization = false) {
        motion::detail::MotionParameterEntry parameter;
        parameter.id = "face_eye_open";
        parameter.rangeBegin = rangeBegin;
        parameter.rangeEnd = rangeEnd;
        parameter.division = division;
        parameter.discretization = discretization;
        return parameter;
    }

    motion::detail::MotionClip clip(double totalFrames) {
        motion::detail::MotionClip value;
        value.label = "全体構造";
        value.totalFrames = totalFrames;
        return value;
    }
} // namespace

TEST_CASE("parameterizedClipTime uses the parameter division as tick axis") {
    // REF transToTick: division × (raw − rangeBegin)/(rangeEnd − rangeBegin).
    const auto parameter = entry(-10.0, 50.0, 60.0);
    const auto motion = clip(61.0);
    // raw 0 → normalized 10/60 → tick 10: neither raw frame 0 nor range max.
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 0.0) ==
            Catch::Approx(10.0));
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, -10.0) ==
            Catch::Approx(0.0));
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 50.0) ==
            Catch::Approx(60.0));
}

TEST_CASE("parameterizedClipTime clamps out-of-range writes to endpoints") {
    const auto parameter = entry(-10.0, 50.0, 60.0);
    const auto motion = clip(61.0);
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 100.0) ==
            Catch::Approx(60.0));
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, -100.0) ==
            Catch::Approx(0.0));
}

TEST_CASE("parameterizedClipTime falls back to totalFrames-1 without division") {
    auto parameter = entry(0.0, 50.0, 0.0);
    const auto motion = clip(61.0);
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 25.0) ==
            Catch::Approx(0.5 * 60.0));
    // A negative division is not a usable axis; take the same fallback.
    parameter.division = -3.0;
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 25.0) ==
            Catch::Approx(0.5 * 60.0));
}

TEST_CASE("parameterizedClipTime quantizes selector parameters") {
    // Selector parameters quantize by their own integer value count before
    // mapping onto the tick axis (see parameterizedClipTime).
    const auto parameter = entry(0.0, 4.0, 8.0, true);
    const auto motion = clip(9.0);
    // raw 2.6 → normalized 0.65 → quantized 3/4 → tick 6.
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 2.6) ==
            Catch::Approx(6.0));
}

TEST_CASE("parameterizedClipTime maps degenerate ranges to tick 0") {
    const auto parameter = entry(5.0, 5.0, 8.0);
    const auto motion = clip(9.0);
    REQUIRE(motion::detail::parameterizedClipTime(motion, parameter, 5.0) ==
            0.0);
}

TEST_CASE("parameterizedClipTime handles reversed ranges symmetrically") {
    // rangeEnd < rangeBegin must map the same relative position to the same
    // tick as the ascending form.
    const auto ascending = entry(0.0, 50.0, 60.0);
    const auto descending = entry(50.0, 0.0, 60.0);
    const auto motion = clip(61.0);
    REQUIRE(motion::detail::parameterizedClipTime(motion, descending, 25.0) ==
            motion::detail::parameterizedClipTime(motion, ascending, 25.0));
}

TEST_CASE("effectiveNodeLim uses a sized parent rect and passes through otherwise") {
    const motion::detail::ScreenSize inherited{0.0, 0.0, 1280.0, 720.0};
    // Sized parent (icon/blank parsed to clipW/H/originX/Y): its own rect.
    const auto blank = motion::detail::effectiveNodeLimLike_REF(
        inherited, 155.0, 136.0, 77.0, 68.0);
    REQUIRE(blank.originX == Catch::Approx(77.0));
    REQUIRE(blank.width == Catch::Approx(155.0));
    // Unsized parent (motion/layout/clip): the parent's own lim unchanged.
    const auto passthrough = motion::detail::effectiveNodeLimLike_REF(
        inherited, 0.0, 0.0, 77.0, 68.0);
    REQUIRE(passthrough.width == Catch::Approx(1280.0));
    REQUIRE(passthrough.height == Catch::Approx(720.0));
    // Half-sized (width only) still counts as unsized.
    REQUIRE(motion::detail::effectiveNodeLimLike_REF(inherited, 155.0, 0.0,
                                                     77.0, 68.0).width ==
            Catch::Approx(1280.0));
}

TEST_CASE("evaluateTimelineLike resolves NaN/Inf sentinels against the node lim") {
    // F07: nested nodes resolve edge-pinning keyframes against their own
    // effective region, not the root logical screen.
    motion::detail::MotionNode node;
    auto &active = node.activeSlot();
    active.frameIndex = 0;
    active.done = true; // sentinel applies before the done early-return
    active.x = std::numeric_limits<double>::quiet_NaN();
    active.y = std::numeric_limits<double>::infinity();
    // blank/155:136:77:68
    const motion::detail::ScreenSize lim{77.0, 68.0, 155.0, 136.0};
    motion::internal::evaluateTimelineLike_0x699AE4(node, false, 0.0, false,
                                                    lim);
    REQUIRE(active.x == Catch::Approx(-77.0));          // region left edge
    REQUIRE(active.y == Catch::Approx(136.0 - 68.0));   // region bottom edge
    // -Inf maps to the same edge as +Inf: REF's isinf check is sign-blind
    // (EmoteNode.cpp 442-445), and no evidence supports diverging from it.
    motion::detail::MotionNode node2;
    auto &slot2 = node2.activeSlot();
    slot2.frameIndex = 0;
    slot2.done = true;
    slot2.x = -std::numeric_limits<double>::infinity();
    slot2.y = 12.5;
    motion::internal::evaluateTimelineLike_0x699AE4(node2, false, 0.0, false,
                                                    lim);
    REQUIRE(slot2.x == Catch::Approx(155.0 - 77.0)); // right edge, sign-blind
    REQUIRE(slot2.y == Catch::Approx(12.5));
}

TEST_CASE("evaluateTimelineLike keeps coords when the node lim is unsized") {
    motion::detail::MotionNode node;
    auto &active = node.activeSlot();
    active.frameIndex = 0;
    active.done = true;
    active.x = std::numeric_limits<double>::quiet_NaN();
    active.y = std::numeric_limits<double>::infinity();
    const motion::detail::ScreenSize unsized{0.0, 0.0, 0.0, 0.0};
    motion::internal::evaluateTimelineLike_0x699AE4(node, false, 0.0, false,
                                                    unsized);
    REQUIRE(std::isnan(active.x));
    REQUIRE(std::isinf(active.y));
}

TEST_CASE("shouldMergeEmoteBoundedChild decides on node structure only") {
    // C01: any node owning a child player merges, regardless of source or
    // layer naming; the old prefix/allowlist heuristics are gone.
    REQUIRE(motion::detail::shouldMergeEmoteBoundedChild(
        3, kUnlistedSource, kUnlistedLabel));
    REQUIRE(motion::detail::shouldMergeEmoteBoundedChild(
        3, "motion/face_parts/eye", "■目L"));
    REQUIRE_FALSE(motion::detail::shouldMergeEmoteBoundedChild(
        0, kUnlistedSource, kUnlistedLabel));
    REQUIRE_FALSE(motion::detail::shouldMergeEmoteBoundedChild(
        2, "motion/face_parts/eye", "■目L"));
    REQUIRE_FALSE(motion::detail::shouldMergeEmoteBoundedChild(
        4, kUnlistedSource, kUnlistedLabel));
}

TEST_CASE("nodeKeepsEmoteDeformation follows mesh data, not names") {
    // G02: keep on parameterized / frame-authored bp / mesh ancestor.
    REQUIRE(motion::detail::nodeKeepsEmoteDeformation(true, false, false));
    REQUIRE(motion::detail::nodeKeepsEmoteDeformation(false, true, false));
    REQUIRE(motion::detail::nodeKeepsEmoteDeformation(false, false, true));
    // No mesh data anywhere: affine fallback even for face-ish naming —
    // naming is no longer consulted.
    REQUIRE_FALSE(
        motion::detail::nodeKeepsEmoteDeformation(false, false, false));
}

TEST_CASE("planEmoteMeshDivision honors authored density without name folds") {
    // G01: division clamps to 1..50 only; the unit-bp → 2x2 fold and the
    // extra cap of 20 are gone. meshDivisionRatio is the perf knob.
    const auto dense = motion::detail::planEmoteMeshDivision(
        50, 1.0, true, true, true, 100.0, 100.0);
    REQUIRE_FALSE(dense.useAffineGrid);
    REQUIRE(dense.divX + dense.divY - 2 == 50);
    REQUIRE(dense.divX == 26);
    REQUIRE(dense.divY == 26);
    // Ratio scales the authored axis.
    const auto scaled = motion::detail::planEmoteMeshDivision(
        40, 0.5, true, true, true, 100.0, 100.0);
    REQUIRE(scaled.divX + scaled.divY - 2 == 20);
    // No authored mesh: affine fallback, unaffected by unit-bp data.
    const auto plain = motion::detail::planEmoteMeshDivision(
        20, 1.0, true, false, false, 100.0, 100.0);
    REQUIRE(plain.useAffineGrid);
    REQUIRE(plain.divX == 2);
    // An authored mesh with unit bp keeps the mesh path instead of folding.
    const auto unitKept = motion::detail::planEmoteMeshDivision(
        20, 1.0, true, true, true, 100.0, 100.0);
    REQUIRE_FALSE(unitKept.useAffineGrid);
}
