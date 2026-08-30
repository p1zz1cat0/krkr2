//
// F01 parameter-axis contract fixtures (REF transToTick single track).
// parameterizedClipTime is the only parameterize→tick mapping; these cases
// pin the division axis, the totalFrames−1 fallback, range clamping and
// selector quantization so the axis cannot silently flip back to a raw-frame
// or totalFrames-first contract.
//
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "motionplayer/RuntimeSupport.h"

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
