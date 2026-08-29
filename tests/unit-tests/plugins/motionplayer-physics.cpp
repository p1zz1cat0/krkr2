#include <array>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "motionplayer/EmoteCompatInternal.h"
#include "motionplayer/HitTestInternal.h"
#include "motionplayer/MotionPhysics.h"
#include "test_config.h"

namespace {
    struct GoldenRow {
        std::string controller;
        int frame = 0;
        std::array<double, 3> output{};
    };

    std::vector<GoldenRow> readGoldenRows() {
        std::ifstream input(TEST_FILES_PATH
                            "/motionplayer-physics/"
                            "emoteplayer-2016-golden.tsv");
        REQUIRE(input.good());
        std::vector<GoldenRow> rows;
        std::string line;
        while(std::getline(input, line)) {
            if(line.empty() || line.front() == '#') {
                continue;
            }
            GoldenRow row;
            std::istringstream parser(line);
            REQUIRE(parser >> row.controller >> row.frame >> row.output[0] >>
                    row.output[1] >> row.output[2]);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    motion::physics::BustConfig bustConfig() {
        motion::physics::BustConfig config;
        config.gravity = 0.35;
        config.spring = 0.22;
        config.friction = 0.08;
        config.scaleX = 1.1;
        config.scaleY = 0.9;
        config.varLr = "bust_lr";
        config.varUd = "bust_ud";
        return config;
    }

    motion::physics::PendConfig pendConfig() {
        motion::physics::PendConfig config;
        config.gravity = 0.35;
        config.frictionX = 0.08;
        config.frictionY = 0.11;
        config.backRate = 0.25;
        config.velocityBound = 1.4;
        config.verticalOutputSegment = 1;
        config.length = { 20.0, 14.0 };
        config.scaleX = { 1.1, 0.8 };
        config.scaleY = { 0.9, 0.7 };
        config.bendSpeed = 0.16;
        config.bendVolume = 0.42;
        config.varLr = "hair_lr";
        config.varLrm = "hair_lrm";
        config.varUd = "hair_ud";
        return config;
    }
} // namespace

TEST_CASE("Motion physics matches frozen 2016 DLL helper output") {
    const auto golden = readGoldenRows();
    motion::physics::BustControl bust(bustConfig());
    motion::physics::PendControl pend(pendConfig());
    const motion::physics::Vec2 input{ 12.0, -4.0 };
    const motion::physics::Vec2 force{ 0.6, -0.25 };

    std::size_t rowIndex = 0;
    for(int frame = 0; frame < 4; ++frame) {
        REQUIRE(rowIndex < golden.size());
        const auto output = bust.stepFrame(input, force, 1.0, 1.25, 0.2);
        const auto &row = golden[rowIndex++];
        REQUIRE(row.controller == "bust");
        REQUIRE(row.frame == frame);
        CHECK(output[0] == Catch::Approx(row.output[0]).margin(0.00001));
        CHECK(output[1] == Catch::Approx(row.output[1]).margin(0.00001));
    }
    for(int frame = 0; frame < 4; ++frame) {
        REQUIRE(rowIndex < golden.size());
        const auto output =
            pend.stepFrame(input, force, 1.0, 1.25, 0.2, nullptr);
        const auto &row = golden[rowIndex++];
        REQUIRE(row.controller == "pend");
        REQUIRE(row.frame == frame);
        CHECK(output[0] == Catch::Approx(row.output[0]).margin(0.00002));
        CHECK(output[1] == Catch::Approx(row.output[1]).margin(0.00002));
        CHECK(output[2] == Catch::Approx(row.output[2]).margin(0.00002));
    }
}

TEST_CASE("Multi-cache selection binds exact latest motion snapshot") {
    using motion::detail::MultiCacheCandidate;
    const std::vector<MultiCacheCandidate> candidates = {
        { "chocola", "waiting", 1, false, true },
        { "vanilla", "waiting", 3, true, true },
        { "chocola", "waiting", 2, false, true },
        { "chocola", "action", 4, false, true },
    };

    CHECK(motion::detail::selectMultiCacheCandidate(
              candidates, "chocola", "waiting") == 2);
    CHECK(motion::detail::selectMultiCacheCandidate(
              candidates, "vanilla", "waiting") == 1);
    CHECK(motion::detail::selectMultiCacheCandidate(
              candidates, "chocola", "missing") ==
          std::numeric_limits<std::size_t>::max());
}

TEST_CASE("Private Motion GLL skips only unusable source textures") {
    CHECK_FALSE(motion::detail::shouldAppendPrivateMotionGLLItem(false, 0,
                                                                 0));
    CHECK_FALSE(motion::detail::shouldAppendPrivateMotionGLLItem(true, 0,
                                                                 64));
    CHECK(motion::detail::shouldAppendPrivateMotionGLLItem(true, 64, 64));
}

TEST_CASE("E-mote alpha masks use the layer command renderer") {
    CHECK(motion::detail::shouldUseContinuousEmoteMask(true, 1));
    CHECK_FALSE(motion::detail::shouldUseContinuousEmoteMask(true, 0));
    CHECK_FALSE(motion::detail::shouldUseContinuousEmoteMask(false, 1));
}

TEST_CASE("Nested render sources are cached per owning motion") {
    CHECK(motion::detail::renderSourceCacheIdentity(
              "chocola/action.psb", "src/face/eye") !=
          motion::detail::renderSourceCacheIdentity(
              "vanilla/action.psb", "src/face/eye"));
    CHECK(motion::detail::renderSourceCacheIdentity(
              "chocola/action.psb", "src/face/eye") ==
          "chocola/action.psb\nsrc/face/eye");
}

TEST_CASE("Mask alpha mode preserves soft facial part edges") {
    constexpr int threshold = 64;
    CHECK(motion::detail::applyMotionMaskAlpha(200, 63, 1, 0, threshold) ==
          0);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 64, 1, 0, threshold) ==
          200);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 63, 1, 1, threshold) ==
          49);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 255, 1, 1, threshold) ==
          200);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 64, 2, 0, threshold) ==
          0);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 63, 2, 0, threshold) ==
          200);
    CHECK(motion::detail::applyMotionMaskAlpha(200, 255, 5, 1, threshold) ==
          255);
}

TEST_CASE("Composite mask unions sources before cropping the colour group") {
    constexpr int threshold = 64;

    auto unionAlpha = motion::detail::unionMotionMaskAlpha(0, 64, 1, threshold);
    unionAlpha =
        motion::detail::unionMotionMaskAlpha(unionAlpha, 128, 1, threshold);
    CHECK(unionAlpha == 159);
    CHECK(motion::detail::applyMotionCompositeMaskAlpha(
              200, unionAlpha, 5, 1, threshold) == 124);

    CHECK(motion::detail::applyMotionCompositeMaskAlpha(
              200, 0, 5, 0, threshold) == 0);
    CHECK(motion::detail::applyMotionCompositeMaskAlpha(
              200, 255, 5, 0, threshold) == 200);
}

TEST_CASE("Top-level zero dt does not initialize post-Core physics") {
    motion::physics::BustControl bust(bustConfig());
    const auto zeroOutput =
        bust.stepFrame({ 100.0, 50.0 }, { 0.6, -0.25 }, 0.0, 1.25, 0.2);
    CHECK(zeroOutput[0] == 0.0);
    CHECK(zeroOutput[1] == 0.0);
    const auto output =
        bust.stepFrame({ 12.0, -4.0 }, { 0.6, -0.25 }, 1.0, 1.25, 0.2);
    CHECK(output[0] == Catch::Approx(0.8839972019195557).margin(0.00001));
    CHECK(output[1] == Catch::Approx(-0.0252272617071867).margin(0.00001));
}

TEST_CASE("Outer force uses confirmed easing and replace semantics") {
    motion::physics::OuterForceAnimator force;
    force.set(8.0, -4.0, 4.0, 1.0);
    force.step(2.0);
    CHECK(force.current().x == Catch::Approx(2.0));
    CHECK(force.current().y == Catch::Approx(-1.0));
    force.set(-2.0, 6.0, 0.0, 0.0);
    CHECK(force.current().x == Catch::Approx(-2.0));
    CHECK(force.current().y == Catch::Approx(6.0));
    CHECK_FALSE(force.active());
}

TEST_CASE("Outer force append keeps three sequential targets") {
    motion::physics::OuterForceAnimator force;
    force.set(4.0, 0.0, 2.0, 0.0);
    force.set(8.0, 4.0, 2.0, 0.0,
              motion::physics::OuterForceAnimator::QueueMode::Append);
    force.step(2.0);
    CHECK(force.current().x == Catch::Approx(4.0));
    REQUIRE(force.active());
    force.step(1.0);
    CHECK(force.current().x == Catch::Approx(6.0));
    CHECK(force.current().y == Catch::Approx(2.0));
    force.step(1.0);
    CHECK(force.current().x == Catch::Approx(8.0));
    CHECK(force.current().y == Catch::Approx(4.0));
    CHECK_FALSE(force.active());
}

TEST_CASE("Outer force carries long dt across appended segments") {
    motion::physics::OuterForceAnimator force;
    force.set(2.0, 0.0, 1.0, 0.0);
    force.set(4.0, 2.0, 1.0, 0.0,
              motion::physics::OuterForceAnimator::QueueMode::Append);
    force.step(1.5);
    CHECK(force.current().x == Catch::Approx(3.0));
    CHECK(force.current().y == Catch::Approx(1.0));
    REQUIRE(force.active());
    force.step(0.5);
    CHECK(force.current().x == Catch::Approx(4.0));
    CHECK(force.current().y == Catch::Approx(2.0));
    CHECK_FALSE(force.active());
}

TEST_CASE("Wind spawns fixed slots and returns the first matching force") {
    motion::physics::WindControl wind;
    wind.start(0.0, 20.0, 1.0, 2.0, 2.0);
    wind.step(1.0, [] { return 0.5; });
    REQUIRE(wind.gusts()[0].active);
    CHECK(wind.gusts()[0].position == Catch::Approx(1.0));
    CHECK(wind.sample(-3.0) == Catch::Approx(2.0));
    CHECK(wind.sample(5.0) == Catch::Approx(2.0));
    CHECK(wind.sample(5.01) == Catch::Approx(0.0));
    wind.stop();
    CHECK_FALSE(wind.active());
    CHECK(wind.sample(1.0) == Catch::Approx(0.0));
}

TEST_CASE("Wind normalizes negative speed and never exceeds fixed slots") {
    motion::physics::WindControl wind;
    wind.start(0.0, 300.0, -256.0, 1.0, 1.0);
    CHECK(wind.signedSpeed() == Catch::Approx(-256.0));
    wind.step(1.0, [] { return 0.0; });
    std::size_t active = 0;
    for(const auto &gust : wind.gusts()) {
        active += gust.active ? 1u : 0u;
    }
    CHECK(active <= motion::physics::WindControl::kSlotCount);
}

TEST_CASE("Frame substeps are equivalent at the 1.1 boundary") {
    motion::physics::BustControl longFrame(bustConfig());
    motion::physics::BustControl splitFrame(bustConfig());
    const motion::physics::Vec2 input{ 12.0, -4.0 };
    const motion::physics::Vec2 force{ 0.6, -0.25 };
    const auto longOutput =
        longFrame.stepFrame(input, force, 2.2, 1.25, 0.2);
    const auto firstSplitOutput =
        splitFrame.stepFrame(input, force, 1.1, 1.25, 0.2);
    CHECK(std::isfinite(firstSplitOutput[0]));
    const auto splitOutput =
        splitFrame.stepFrame(input, force, 1.1, 1.25, 0.2);
    CHECK(longOutput[0] == Catch::Approx(splitOutput[0]).margin(0.0000001));
    CHECK(longOutput[1] == Catch::Approx(splitOutput[1]).margin(0.0000001));
}

TEST_CASE("Physics rejects non-finite frame inputs") {
    motion::physics::BustControl bust(bustConfig());
    CHECK_THROWS_AS(
        bust.stepFrame({ std::numeric_limits<double>::infinity(), 0.0 },
                       { 0.0, 0.0 }, 1.0, 1.0, 0.0),
        std::invalid_argument);
    motion::physics::PendControl pend(pendConfig());
    CHECK_THROWS_AS(
        pend.stepFrame({ 0.0, 0.0 }, { 0.0, 0.0 },
                       std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0,
                       nullptr),
        std::invalid_argument);
}

TEST_CASE("Eye is a pre-Core controller and zero dt republishes its value") {
    motion::physics::EyeConfig config;
    config.beginFrame = 0.0;
    config.endFrame = 10.0;
    config.blinkIntervalMin = 0.0;
    config.blinkIntervalMax = 0.0;
    config.blinkFrameCount = 2.0;
    config.blinkEnabled = true;
    config.label = "face_eye_open";
    motion::physics::EyeControl eye(config);

    REQUIRE(eye.step(0.0, 0.0, [] { return 0.0; }).value() ==
            Catch::Approx(0.0));
    REQUIRE(eye.step(1.0, 0.0, [] { return 0.0; }).value() ==
            Catch::Approx(0.0));
    REQUIRE(eye.step(1.0, 0.0, [] { return 0.0; }).value() ==
            Catch::Approx(5.0));
}

TEST_CASE("Eye supports reverse ranges and manual suppression") {
    motion::physics::EyeConfig config;
    config.beginFrame = 10.0;
    config.endFrame = 0.0;
    config.blinkIntervalMin = 0.0;
    config.blinkIntervalMax = 0.0;
    config.blinkFrameCount = 2.0;
    config.blinkEnabled = true;
    config.label = "face_eye_open";
    motion::physics::EyeControl eye(config);

    REQUIRE(eye.step(1.0, 10.0, [] { return 0.0; }).value() ==
            Catch::Approx(10.0));
    REQUIRE(eye.step(1.0, 10.0, [] { return 0.0; }).value() ==
            Catch::Approx(5.0));
    CHECK_FALSE(eye.step(1.0, 7.0, [] { return 0.0; }).has_value());
    REQUIRE(eye.step(0.0, 10.0, [] { return 0.0; }).value() ==
            Catch::Approx(10.0));
}

TEST_CASE("Emote bounded child merge keeps head and face motions") {
    CHECK(motion::detail::shouldMergeEmoteBoundedChild(
        3, "motion/face_parts/鼻(左右切り替え)", ""));
    CHECK(motion::detail::shouldMergeEmoteBoundedChild(3, "", "■目L"));
    CHECK_FALSE(motion::detail::shouldMergeEmoteBoundedChild(
        3, "motion/particle/dust", "dust"));
    CHECK_FALSE(
        motion::detail::shouldMergeEmoteBoundedChild(0, "motion/face_parts/目L",
                                                    "■目L"));
}

TEST_CASE("Emote mesh division keeps authored face density") {
    CHECK(motion::detail::kEmoteRasterStretchType == 1);

    const auto identity = motion::detail::planEmoteMeshDivision(
        20, 1.0, true, true, false, 100.0, 100.0);
    CHECK(identity.useAffineGrid);
    CHECK(identity.divX == 2);
    CHECK(identity.divY == 2);

    const auto face = motion::detail::planEmoteMeshDivision(
        20, 1.0, true, true, true, 100.0, 80.0);
    CHECK_FALSE(face.useAffineGrid);
    CHECK(face.divX >= 2);
    CHECK(face.divY >= 2);
    CHECK(face.divX + face.divY == 22);

    const auto capped = motion::detail::planEmoteMeshDivision(
        40, 1.0, true, false, true, 100.0, 100.0);
    CHECK(capped.divX + capped.divY == 22);
}

TEST_CASE("Face clip labels accept parenthesized variants") {
    CHECK(motion::detail::clipLabelMatchesRequest("鼻(左右切り替え)", "鼻"));
    CHECK(motion::detail::clipLabelMatchesRequest("目L", "目L"));
    CHECK_FALSE(motion::detail::clipLabelMatchesRequest("口パク", "口"));
}

TEST_CASE("Hit shapes reject degenerate origin boxes") {
    CHECK_FALSE(motion::detail::boundsAreUsable(0.0, 0.0, 0.0, 0.0));
    CHECK(motion::detail::pointInAabb(10.0, 20.0, 0.0, 0.0, 40.0, 80.0));
    CHECK_FALSE(motion::detail::pointInAabb(10.0, 20.0, 0.0, 0.0, 0.0, 0.0));

    const auto emptyRect = motion::detail::makeRectHitData(0.0, 0.0, 0.0, 0.0);
    CHECK(motion::detail::hitDataIsDegenerate(emptyRect));
    const auto bust = motion::detail::makeQuadHitData(100.0, 200.0, 300.0,
                                                      200.0, 300.0, 360.0,
                                                      100.0, 360.0);
    CHECK_FALSE(motion::detail::hitDataIsDegenerate(bust));
    CHECK(motion::detail::hitTestHitData(bust, 200.0, 280.0));
    CHECK_FALSE(motion::detail::hitTestHitData(bust, 10.0, 10.0));
}

TEST_CASE("Null mesh.bp rest keys lerp against the unit grid") {
    std::vector<double> deformed = { 0.0, -0.01, 1.0 / 3.0, -0.01, 2.0 / 3.0,
                                     -0.01, 1.0, -0.01,     0.0,   1.0 / 3.0,
                                     1.0 / 3.0,  1.0 / 3.0, 2.0 / 3.0,
                                     1.0 / 3.0,  1.0,       1.0 / 3.0,
                                     0.0,        2.0 / 3.0, 1.0 / 3.0,
                                     2.0 / 3.0,  2.0 / 3.0, 2.0 / 3.0,
                                     1.0,        2.0 / 3.0, 0.0,   1.0,
                                     1.0 / 3.0,  1.0,       2.0 / 3.0, 1.0,
                                     1.0,        1.0 };
    REQUIRE(deformed.size() == 32);

    std::vector<double> towardRest = deformed;
    motion::detail::lerpMeshBezierPoints(towardRest, {}, 0.5);
    REQUIRE(towardRest.size() == 32);
    CHECK(towardRest[1] == Catch::Approx(-0.005).margin(1.0e-12));
    CHECK(towardRest[2] == Catch::Approx(1.0 / 3.0).margin(1.0e-12));

    std::vector<double> fromRest;
    motion::detail::lerpMeshBezierPoints(fromRest, deformed, 0.5);
    REQUIRE(fromRest.size() == 32);
    CHECK(fromRest[1] == Catch::Approx(-0.005).margin(1.0e-12));

    std::vector<double> empty;
    motion::detail::lerpMeshBezierPoints(empty, {}, 0.5);
    CHECK(empty.empty());

    std::vector<double> missing;
    motion::detail::fillUnitMeshBezierIfEmpty(missing);
    REQUIRE(missing.size() == 32);
    CHECK(missing[0] == Catch::Approx(0.0));
    CHECK(missing[2] == Catch::Approx(1.0 / 3.0));
    CHECK(missing[31] == Catch::Approx(1.0));
    CHECK(motion::detail::isExactUnitMeshBezier(missing));
    CHECK(motion::detail::isExactUnitMeshBezier({}));
    CHECK_FALSE(motion::detail::isExactUnitMeshBezier(deformed));
}

TEST_CASE("Difference timeline ownership survives a zero-valued track") {
    CHECK(motion::detail::differenceTrackOwnsLabel(
        true, 2, 1.0, "body_UD", "body_UD", false));
    CHECK_FALSE(motion::detail::differenceTrackOwnsLabel(
        true, 2, 1.0, "body_UD", "head_UD", false));
    CHECK_FALSE(motion::detail::differenceTrackOwnsLabel(
        true, 2, 0.0, "body_UD", "body_UD", false));
    CHECK_FALSE(motion::detail::differenceTrackOwnsLabel(
        true, 2, 1.0, "body_UD", "body_UD", true));
}

TEST_CASE("Warped quad uses all four corners in a 2x2 mesh") {
    const std::array<float, 8> corners = { 1.0f,  2.0f,  11.0f, 3.0f,
                                            13.0f, 17.0f, -2.0f, 19.0f };
    const auto points = motion::detail::quadCornersToMeshPoints(corners);
    const std::array<float, 8> expected = { 1.0f,  2.0f,  11.0f, 3.0f,
                                            -2.0f, 19.0f, 13.0f, 17.0f };
    CHECK(points == expected);
}

TEST_CASE("Hit coordinates undo the draw affine transform") {
    double localX = 0.0;
    double localY = 0.0;

    const std::array<double, 6> translate = { 1.0, 0.0, 0.0, 1.0, 480.0,
                                              320.0 };
    REQUIRE(motion::detail::inverseAffinePoint(translate, 500.0, 340.0, localX,
                                               localY));
    CHECK(localX == Catch::Approx(20.0));
    CHECK(localY == Catch::Approx(20.0));
    CHECK(motion::detail::pointInAabb(localX, localY, 0.0, 0.0, 40.0, 40.0));

    // Regression: the touch box used to collapse toward the top-left because
    // draw-space points were compared against local-space bounds. With a
    // half-scale placement at (480,320), a click on the middle of a character
    // whose local box is 0..400 x 0..800 must still land inside it.
    const std::array<double, 6> scaled = { 0.5, 0.0, 0.0, 0.5, 480.0, 320.0 };
    REQUIRE(motion::detail::inverseAffinePoint(scaled, 580.0, 520.0, localX,
                                               localY));
    CHECK(localX == Catch::Approx(200.0));
    CHECK(localY == Catch::Approx(400.0));
    CHECK(motion::detail::pointInAabb(localX, localY, 0.0, 0.0, 400.0, 800.0));

    // The un-inverted point (580,520) would have missed that box entirely,
    // and only clicks near the origin would have registered.
    CHECK_FALSE(
        motion::detail::pointInAabb(580.0, 520.0, 0.0, 0.0, 400.0, 800.0));

    // Rotation must round-trip too (90deg CCW about the origin).
    const std::array<double, 6> rotated = { 0.0, 1.0, -1.0, 0.0, 0.0, 0.0 };
    REQUIRE(motion::detail::inverseAffinePoint(rotated, -50.0, 10.0, localX,
                                               localY));
    CHECK(localX == Catch::Approx(10.0));
    CHECK(localY == Catch::Approx(50.0));

    // Degenerate matrices must be rejected rather than dividing by zero.
    const std::array<double, 6> singular = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    CHECK_FALSE(motion::detail::inverseAffinePoint(singular, 1.0, 1.0, localX,
                                                   localY));
}
