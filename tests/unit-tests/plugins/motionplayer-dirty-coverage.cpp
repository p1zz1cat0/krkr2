#include <catch2/catch_test_macros.hpp>

#include "motionplayer/RuntimeSupport.h"

namespace {
    using Runtime = motion::detail::PlayerRuntime;

    Runtime::PreparedRenderItem makeItem(int node, std::array<int, 4> rect) {
        Runtime::PreparedRenderItem item;
        item.nodeIndex = node;
        item.drawFlag = true;
        item.clipRect = rect;
        item.corners = {
            static_cast<float>(rect[0]), static_cast<float>(rect[1]),
            static_cast<float>(rect[2]), static_cast<float>(rect[1]),
            static_cast<float>(rect[2]), static_cast<float>(rect[3]),
            static_cast<float>(rect[0]), static_cast<float>(rect[3]),
        };
        item.meshType = 1;
        item.meshDivX = 2;
        item.meshDivY = 2;
        item.meshPoints.assign(item.corners.begin(), item.corners.end());
        return item;
    }

    Runtime::ScopedRenderCommand makeCommand(
        Runtime &runtime, Runtime::PreparedRenderItem &item) {
        Runtime::ScopedRenderCommand command;
        command.item = &item;
        command.nodeIndex = item.nodeIndex;
        command.renderScopeId = &runtime;
        command.scopedNodeIndex = item.nodeIndex;
        return command;
    }
} // namespace

TEST_CASE("SLA dirty coverage sees mesh deformation") {
    Runtime runtime;
    auto item = makeItem(1, { 10, 10, 40, 40 });
    runtime.renderCommands = { makeCommand(runtime, item) };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    item.meshPoints[0] += 1.0f;
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    CHECK(runtime.slaDirtyCoverage.changedItems == 1);
    CHECK_FALSE(runtime.slaDirtyCoverage.dirty.empty());
}

TEST_CASE("SLA dirty coverage clears commands which disappear") {
    Runtime runtime;
    auto item = makeItem(1, { 10, 10, 40, 40 });
    runtime.renderCommands = { makeCommand(runtime, item) };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    runtime.renderCommands.clear();
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    CHECK(runtime.slaDirtyCoverage.changedItems == 1);
    REQUIRE(runtime.slaDirtyCoverage.dirty.size() == 1);
    const std::array<int, 4> expected{ 10, 10, 40, 40 };
    CHECK(runtime.slaDirtyCoverage.dirty.front() == expected);
}

TEST_CASE("SLA dirty coverage sees a command appearing after an empty frame") {
    Runtime runtime;
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    auto item = makeItem(1, { 10, 10, 40, 40 });
    runtime.renderCommands = { makeCommand(runtime, item) };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    CHECK(runtime.slaDirtyCoverage.changedItems == 1);
    CHECK_FALSE(runtime.slaDirtyCoverage.dirty.empty());
}

TEST_CASE("SLA dirty coverage sees relative draw-order changes") {
    Runtime runtime;
    auto first = makeItem(1, { 10, 10, 50, 50 });
    auto second = makeItem(2, { 20, 20, 60, 60 });
    runtime.renderCommands = {
        makeCommand(runtime, first), makeCommand(runtime, second)
    };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    runtime.renderCommands = {
        makeCommand(runtime, second), makeCommand(runtime, first)
    };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    CHECK(runtime.slaDirtyCoverage.changedItems == 2);
    CHECK(runtime.slaDirtyCoverage.dirty.size() == 4);
}

TEST_CASE("SLA dirty coverage restarts when the canvas changes size") {
    Runtime runtime;
    auto item = makeItem(1, { 10, 10, 40, 40 });
    runtime.renderCommands = { makeCommand(runtime, item) };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 100, 100);

    item.clipRect = { 5, 5, 20, 20 };
    motion::detail::recordSlaDirtyCoverage(runtime, "fixture", 50, 50);

    CHECK(runtime.slaDirtyCoverage.changedItems == 0);
    CHECK(runtime.slaDirtyCoverage.dirty.empty());
    CHECK(runtime.slaDirtyCoverage.canvasWidth == 50);
    CHECK(runtime.slaDirtyCoverage.canvasHeight == 50);
}
