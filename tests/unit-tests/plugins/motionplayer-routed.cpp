#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "motionplayer/RoutedTransition.h"

namespace {
    using motion::routed::Graph;
    using motion::routed::Leg;
    using motion::routed::Transition;

    // metadata/eyebrowControl "face_eyebrow" of dx_e-moteアズキハワイa.psb
    // (NEKOPARA After).
    std::shared_ptr<const Graph> eyebrowGraph() {
        auto graph = std::make_shared<Graph>();
        graph->edges = { { 0, 10 }, { 11, 20 }, { 21, 30 }, { 31, 49 },
                         { 50, 50 }, { 51, 51 }, { 52, 52 }, { 53, 53 },
                         { 54, 54 }, { 55, 55 } };
        graph->nodes = { { 10, 11, 21, 31 }, { 0, 49 } };
        return graph;
    }

    float easeWeight(float ease) {
        if(ease > 0.0f) {
            return ease + 1.0f;
        }
        return ease < 0.0f ? 1.0f / (1.0f - ease) : 1.0f;
    }

    // Player::progress splits dt into sub-steps of at most 1.1 frames.
    void progress(Transition &transition, float dt) {
        float remaining = dt;
        do {
            const float step = remaining < 1.1f ? remaining : 1.1f;
            remaining -= step;
            transition.advance(step);
        } while(remaining > 0.0f);
    }

    struct Sample {
        float dt;
        float value;
    };

    struct OracleCase {
        int player;
        float from;
        float to;
        float duration;
        float ease;
        std::vector<Sample> samples;
    };

    // Every face_eyebrow setVariable with a transition time in the VM oracle
    // trace (fork plugins/DrawDeviceD3D/origin/oracle/
    // after-oracle-trace-pass2.txt). dt is the progress() between two
    // getVariable reads.
    const std::vector<OracleCase> &oracleCases() {
        static const std::vector<OracleCase> cases{
        { 5, 0.0f, 20.0f, 9.09f, 1.0f,
          { { 1.26f, 0.365062177f },
            { 0.72f, 0.901480198f },
            { 1.02f, 2.0695138f },
            { 1.08f, 3.82777286f },
            { 0.96f, 5.84099627f },
            { 0.96f, 8.27805519f },
            { 1.08f, 13.9595766f },
            { 0.96f, 17.040638f },
            { 0.96f, 20.0f } } },
        { 5, 20.0f, 40.0f, 9.09f, -1.0f,
          { { 1.02f, 13.9703703f },
            { 1.2f, 11.1045685f },
            { 0.9f, 32.9291191f },
            { 0.9f, 34.5211334f },
            { 1.02f, 36.0813408f },
            { 1.02f, 37.4664726f },
            { 0.96f, 38.6537628f },
            { 0.96f, 39.7537537f },
            { 1.02f, 40.0f } } },
        { 12, 10.0f, 27.5f, 9.09f, 1.0f,
          { { 1.08f, 21.0917549f },
            { 0.9f, 21.3084011f },
            { 0.96f, 21.6799545f },
            { 1.08f, 22.2712688f },
            { 0.96f, 22.9509411f },
            { 1.2f, 24.0044327f },
            { 1.2f, 25.284481f },
            { 0.66f, 26.0850792f },
            { 0.9f, 27.2872486f } } },
        { 12, 27.2872486f, 45.0f, 9.09f, -1.0f,
          { { 1.08f, 2.26607752f },
            { 1.02f, 46.1103287f },
            { 0.9f, 45.0f } } },
        { 27, 0.0f, 30.0f, 9.0f, 1.0f,
          { { 3.12f, 2.28337765f },
            { 0.84f, 3.67840004f },
            { 1.02f, 5.81737804f },
            { 1.02f, 8.44444466f },
            { 1.14f, 24.1459942f },
            { 0.9f, 27.0603409f },
            { 0.9f, 30.0f } } },
        { 30, 0.0f, 30.0f, 9.0f, 1.0f,
          { { 3.06f, 2.19639969f },
            { 0.84f, 3.5677774f },
            { 0.96f, 5.54039907f },
            { 1.02f, 8.11004353f },
            { 1.02f, 23.84729f },
            { 1.2f, 27.8219318f },
            { 0.84f, 30.0f } } },
        { 33, 0.0f, 20.0f, 9.09f, 1.0f,
          { { 0.84f, 0.162249863f },
            { 0.96f, 0.74502486f },
            { 1.08f, 1.90726399f },
            { 0.9f, 3.28556013f },
            { 1.02f, 5.29795551f },
            { 1.08f, 7.95024538f },
            { 0.9f, 13.5310974f },
            { 1.08f, 17.0601311f },
            { 1.02f, 20.0f } } },
        { 33, 20.0f, 40.0f, 9.09f, -1.0f,
          { { 0.96f, 14.1504002f },
            { 0.96f, 11.727417f },
            { 1.08f, 33.3711357f },
            { 0.9f, 34.9596481f },
            { 1.08f, 36.60289f },
            { 1.02f, 37.9777184f },
            { 0.9f, 39.086647f },
            { 1.08f, 40.0f } } },
        { 34, 10.0f, 27.5f, 9.09f, 1.0f,
          { { 2.04f, 21.3273754f },
            { 0.96f, 21.7079926f },
            { 1.02f, 22.2712708f },
            { 1.02f, 22.9982357f },
            { 0.96f, 23.8319664f },
            { 0.96f, 24.8106937f },
            { 1.08f, 26.0850792f },
            { 0.96f, 27.3719254f } } },
        { 34, 27.3719254f, 45.0f, 9.09f, -1.0f,
          { { 1.02f, 2.70860338f },
            { 1.14f, 45.8018799f },
            { 1.38f, 45.0f } } },
        { 38, 10.0f, 25.0f, 9.09f, 1.0f,
          { { 1.14f, 21.0629139f },
            { 0.9f, 21.2014618f },
            { 1.02f, 21.453289f },
            { 0.9f, 21.7591419f },
            { 1.02f, 22.2005806f },
            { 1.08f, 22.7777786f },
            { 0.9f, 23.3450432f },
            { 1.08f, 24.1292801f },
            { 0.96f, 24.9211845f } } },
        { 38, 24.9211845f, 40.0f, 9.09f, -1.0f,
          { { 0.96f, 35.4861984f },
            { 1.08f, 37.4054031f },
            { 0.96f, 38.7061386f },
            { 1.02f, 39.8751793f },
            { 1.02f, 40.0f } } },
        { 48, 10.0f, 27.5f, 9.09f, 1.0f,
          { { 1.44f, 21.1631222f },
            { 0.54f, 21.3084011f },
            { 1.08f, 21.7365952f },
            { 0.9f, 22.2336044f },
            { 1.08f, 22.9982357f },
            { 0.96f, 23.8319664f },
            { 1.02f, 24.8766785f },
            { 1.02f, 26.0850792f },
            { 1.02f, 27.4571686f } } },
        { 48, 27.4571686f, 45.0f, 9.09f, -1.0f,
          { { 0.96f, 3.1581769f },
            { 1.02f, 0.25949192f },
            { 1.02f, 46.3679352f },
            { 0.9f, 45.0f } } },
        { 47, 0.0f, 15.0f, 9.0f, 1.0f,
          { { 3.06f, 1.61839974f },
            { 0.96f, 2.79315519f },
            { 0.96f, 4.28648853f },
            { 1.02f, 6.22222281f },
            { 0.96f, 8.37262344f },
            { 1.08f, 13.6111803f },
            { 0.9f, 15.0f } } },
        { 47, 15.0f, 30.0f, 9.0f, -1.0f,
          { { 1.08f, 26.0347061f },
            { 0.96f, 27.7130032f },
            { 1.02f, 29.1002617f },
            { 0.96f, 30.0f } } },
        };
        return cases;
    }
} // namespace

TEST_CASE("routed transition matches the After driver on every oracle sample",
          "[motionplayer][routed]") {
    for(const auto &c : oracleCases()) {
        INFO("player " << c.player << ": " << c.from << " -> " << c.to);
        Transition transition(eyebrowGraph(), c.from);
        transition.set(c.to, c.duration, easeWeight(c.ease), false);
        for(const auto &sample : c.samples) {
            progress(transition, sample.dt);
            CHECK(transition.value() == Catch::Approx(sample.value).margin(0.001));
        }
        // Some cases end early: the game sent the next target first.
        if(c.samples.back().value == c.to) {
            CHECK_FALSE(transition.busy());
        }
    }
}

TEST_CASE("route jumps between equal-shape end points",
          "[motionplayer][routed]") {
    std::vector<Leg> legs;
    // 怒り -> 笑い: back to 中間点(怒), jump to 中間点(笑). Never via 哀しみ.
    CHECK(motion::routed::planRoute(*eyebrowGraph(), 20, 40, legs) == 18.0f);
    REQUIRE(legs.size() == 2);
    CHECK((legs[0].from == 20 && legs[0].to == 11));
    CHECK((legs[1].from == 31 && legs[1].to == 40));

    // Two routes of 20.29: the driver takes the one with the shorter last
    // leg (via 通常 0 / 通常移行 49), not the one via 31.
    motion::routed::planRoute(*eyebrowGraph(), 27.5f, 45, legs);
    REQUIRE(legs.size() == 3);
    CHECK(legs[2].from == 49);
}

TEST_CASE("unreachable target changes the value instantly",
          "[motionplayer][routed]") {
    std::vector<Leg> legs;
    // 特殊3 is a single-point edge with no node: FUN_10022c70 writes
    // {target, target} and L = 0.
    CHECK(motion::routed::planRoute(*eyebrowGraph(), 20, 52, legs) == 0.0f);
    REQUIRE(legs.size() == 1);
    CHECK((legs[0].from == 52 && legs[0].to == 52));

    Transition transition(eyebrowGraph(), 20);
    transition.set(52, 9, 1, false);
    progress(transition, 0.5f);
    CHECK(transition.value() == 52);
}

TEST_CASE("final jump inside a node group is not a move",
          "[motionplayer][routed]") {
    std::vector<Leg> legs;
    // 25 -> 10: walk 25 -> 21 (4), then jump 21 => 10. L must stay 4.
    CHECK(motion::routed::planRoute(*eyebrowGraph(), 25, 10, legs) == 4.0f);
    REQUIRE_FALSE(legs.empty());
    CHECK(legs.back().to == 10);

    Transition transition(eyebrowGraph(), 25);
    transition.set(10, 4, 1, false);
    progress(transition, 4.5f);
    CHECK(transition.value() == 10);
    CHECK_FALSE(transition.busy());
}

TEST_CASE("instant set and repeated target", "[motionplayer][routed]") {
    Transition transition(eyebrowGraph(), 0);
    transition.set(20, 9, 2, false);
    progress(transition, 3);
    // Same target, but the new request is slower than what is left: ignored.
    // A restart with T = 30 could not arrive within the next 6.5 frames.
    transition.set(20, 30, 2, false);
    progress(transition, 6.5f);
    CHECK(transition.value() == 20);

    transition.set(5, 0, 1, false);
    CHECK(transition.value() == 5);
    CHECK_FALSE(transition.busy());
}
