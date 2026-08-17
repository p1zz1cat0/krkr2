#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/PluginSafety.h"

using namespace pluginSafety;

namespace {
SafetyResult<ValidatedAllocation> consumeInChild(OperationBudget &budget,
                                                  std::size_t bytes) {
    const auto checked = checkedElementBytes(bytes, 1);
    if(!checked)
        return SafetyResult<ValidatedAllocation>::failure(checked.error);
    return validateAllocationBudget(checked.value, budget);
}
}

TEST_CASE("PluginSafety checks arithmetic before narrowing or allocation") {
    CHECK(checkedAdd(2, 3).value == 5);
    CHECK_FALSE(checkedAdd(std::numeric_limits<std::size_t>::max(), 1));
    CHECK(checkedMultiply(7, 9).value == 63);
    CHECK_FALSE(checkedMultiply(std::numeric_limits<std::size_t>::max(), 2));
    CHECK(checkedElementBytes(16, sizeof(tjs_uint32)).value.bytes == 64);
}

TEST_CASE("PluginSafety operation budget is cumulative and never refunds") {
    OperationBudget budget;
    const auto firstBytes = checkedElementBytes(32u * kMiB, 1);
    const auto secondBytes = checkedElementBytes(32u * kMiB, 1);
    REQUIRE(firstBytes);
    REQUIRE(secondBytes);

    REQUIRE(validateAllocationBudget(firstBytes.value, budget));
    CHECK(budget.consumed() == 32u * kMiB);
    CHECK(budget.remaining() == 224u * kMiB);

    for(int i = 0; i < 7; ++i)
        REQUIRE(validateAllocationBudget(secondBytes.value, budget));
    CHECK(budget.consumed() == kDefaultOperationLimit);
    CHECK(budget.remaining() == 0);

    const auto consumedBefore = budget.consumed();
    const auto remainingBefore = budget.remaining();
    const auto rejected = validateAllocationBudget(
        checkedElementBytes(1, 1).value, budget);
    CHECK_FALSE(rejected);
    CHECK(rejected.error == SafetyError::operationBudgetExceeded);
    CHECK(budget.consumed() == consumedBefore);
    CHECK(budget.remaining() == remainingBefore);
}

TEST_CASE("PluginSafety rejects a single allocation above 64 MiB") {
    OperationBudget budget;
    const auto bytes = checkedElementBytes(kDefaultSingleAllocationLimit + 1, 1);
    REQUIRE(bytes);
    const auto result = validateAllocationBudget(bytes.value, budget);
    CHECK_FALSE(result);
    CHECK(result.error == SafetyError::allocationLimitExceeded);
    CHECK(budget.consumed() == 0);
    CHECK(budget.remaining() == kDefaultOperationLimit);
}

TEST_CASE("PluginSafety child functions share the parent operation budget") {
    OperationBudget budget(48u * kMiB);
    REQUIRE(consumeInChild(budget, 16u * kMiB));
    REQUIRE(consumeInChild(budget, 16u * kMiB));
    REQUIRE(consumeInChild(budget, 16u * kMiB));
    CHECK_FALSE(consumeInChild(budget, 1));
    CHECK(budget.consumed() == 48u * kMiB);
    CHECK(budget.remaining() == 0);
}

TEST_CASE("PluginSafety validates TJS numeric inputs") {
    CHECK(readBoundedInteger(tTJSVariant(4), 1, 8).value == 4);
    CHECK_FALSE(readBoundedInteger(tTJSVariant(-1), 0, 8));
    CHECK_FALSE(readBoundedInteger(tTJSVariant(TJS_W("4")), 0, 8));
    CHECK(readFiniteReal(tTJSVariant(0.5), 0.0, 1.0).value == 0.5);
    CHECK_FALSE(readFiniteReal(
        tTJSVariant(std::numeric_limits<double>::infinity()), 0.0, 1.0));
    CHECK(readDuration(tTJSVariant(2)).value == 2);
    CHECK_FALSE(readDuration(tTJSVariant(-1)));
    CHECK_FALSE(readDuration(tTJSVariant(86'400'001)));
}

TEST_CASE("PluginSafety pins the current positive-pitch Layer layout") {
    const auto valid = validateLayerLayout(4, 3, 16);
    REQUIRE(valid);
    CHECK(valid.value.rowBytes == 16);
    CHECK(valid.value.spanBytes == 48);

    CHECK_FALSE(validateLayerLayout(0, 3, 16));
    CHECK_FALSE(validateLayerLayout(4, 3, 15));
    const auto negative = validateLayerLayout(4, 3, -16);
    CHECK_FALSE(negative);
    CHECK(negative.error == SafetyError::invalidPitch);
}
