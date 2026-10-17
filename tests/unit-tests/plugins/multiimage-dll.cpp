//
// multiimage 队列回归测试：直接包含插件源文件，不依赖 TJS 运行时。
// 只覆盖 MultiImage 自身的状态转换；Layer 回调由 smoke fixture 覆盖。
//

#include <catch2/catch_test_macros.hpp>

#include "multiimage/multiimage.cpp"

TEST_CASE("MultiImage queue tracks pushed entries") {
    MultiImage multi;

    CHECK(static_cast<tjs_int>(multi.count()) == 0);

    multi.push(tTJSVariant(), 10, 20);
    CHECK(static_cast<tjs_int>(multi.count()) == 1);

    multi.push(tTJSVariant(), -1, -2);
    CHECK(static_cast<tjs_int>(multi.count()) == 2);

    multi.addZoom(2.0, 3.0, 5);
    CHECK(static_cast<tjs_int>(multi.count()) == 2);

    multi.clear();
    CHECK(static_cast<tjs_int>(multi.count()) == 0);
}

TEST_CASE("MultiImage compatibility methods return void") {
    MultiImage multi;

    CHECK(multi.calcMultiAffine(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 8, 8, 255,
                                0)
              .Type() == tvtVoid);

    multi.divisionLayer(2.0);
    CHECK(static_cast<tjs_int>(multi.count()) == 0);
}
