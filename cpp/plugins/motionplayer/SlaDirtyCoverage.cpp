// SlaDirtyCoverage.cpp — Phase 2 计数插桩（判定后连同 .h 里的状态一起删除）。
//
// 只回答一个问题：accurate SLA 的 private canvas 每帧重放里，变化 item 的
// old/new dirty union 有多大、按 z-order 还要重放多少相交命令——也就是
// 「保留上一帧画布 + 只清 dirty union + 在 clip 内重放」是否值得实现。
// 只读命令列表与 item 几何，不写 GL、不改合成语义、不参与任何判定分支。
//
#include "RuntimeSupport.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace motion {
    namespace detail {
        namespace {
            std::uint64_t hashBytes(std::uint64_t hash, const void *data,
                                    std::size_t size) {
                const auto *bytes = static_cast<const unsigned char *>(data);
                for(std::size_t i = 0; i < size; ++i) {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
                return hash;
            }

            template <typename T>
            std::uint64_t hashValue(std::uint64_t hash, const T &value) {
                return hashBytes(hash, &value, sizeof(value));
            }

            // 跨帧稳定的命令身份：拥有者 runtime 指针 + 本地节点号 +
            // 合并后的节点号。与 cache 的 slot key 同源，不依赖命令顺序。
            std::uint64_t commandKey(
                const PlayerRuntime::ScopedRenderCommand &command) {
                std::uint64_t hash = 1469598103934665603ull;
                const std::uintptr_t scope =
                    reinterpret_cast<std::uintptr_t>(command.renderScopeId);
                hash = hashValue(hash, scope);
                hash = hashValue(hash, command.scopedNodeIndex);
                hash = hashValue(hash, command.nodeIndex);
                return hash;
            }

            // 与像素结果相关的 item 形状：源、顶点、clip、mesh/混合/透明度与
            // 打包颜色。任一变化都意味着上一帧的像素不能直接复用。
            std::uint64_t itemSignature(
                const PlayerRuntime::PreparedRenderItem &item) {
                std::uint64_t hash = 1469598103934665603ull;
                hash = hashBytes(hash, item.sourceKey.data(),
                                 item.sourceKey.size());
                hash = hashBytes(hash, item.corners.data(),
                                 sizeof(float) * item.corners.size());
                hash = hashBytes(hash, item.meshPoints.data(),
                                 sizeof(float) * item.meshPoints.size());
                hash = hashBytes(hash, item.clipRect.data(),
                                 sizeof(int) * item.clipRect.size());
                hash = hashBytes(hash, item.packedColors.data(),
                                 sizeof(std::uint32_t) *
                                     item.packedColors.size());
                hash = hashValue(hash, item.meshType);
                hash = hashValue(hash, item.meshDivX);
                hash = hashValue(hash, item.meshDivY);
                hash = hashValue(hash, item.blendMode);
                hash = hashValue(hash, item.opacity);
                return hash;
            }

            bool rectsIntersect(const std::array<int, 4> &lhs,
                                const std::array<int, 4> &rhs) {
                return lhs[0] < rhs[2] && rhs[0] < lhs[2] &&
                    lhs[1] < rhs[3] && rhs[1] < lhs[3];
            }

            // 精确并集面积（x 方向切片 + y 区间合并），用于「清 old/new
            // union」这一步的成本上界；bbox 会把不相邻的部件算进去。
            double unionArea(const std::vector<std::array<int, 4>> &rects) {
                if(rects.empty()) {
                    return 0.0;
                }
                std::vector<int> xs;
                xs.reserve(rects.size() * 2);
                for(const auto &rect : rects) {
                    if(rect[0] >= rect[2] || rect[1] >= rect[3]) {
                        continue;
                    }
                    xs.push_back(rect[0]);
                    xs.push_back(rect[2]);
                }
                if(xs.size() < 2) {
                    return 0.0;
                }
                std::sort(xs.begin(), xs.end());
                xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
                double area = 0.0;
                for(std::size_t i = 0; i + 1 < xs.size(); ++i) {
                    const int left = xs[i];
                    const int right = xs[i + 1];
                    std::vector<std::pair<int, int>> spans;
                    for(const auto &rect : rects) {
                        if(rect[0] <= left && rect[2] >= right &&
                           rect[1] < rect[3]) {
                            spans.emplace_back(rect[1], rect[3]);
                        }
                    }
                    if(spans.empty()) {
                        continue;
                    }
                    std::sort(spans.begin(), spans.end());
                    long long covered = 0;
                    int spanStart = spans.front().first;
                    int spanEnd = spans.front().second;
                    for(std::size_t k = 1; k < spans.size(); ++k) {
                        if(spans[k].first > spanEnd) {
                            covered += spanEnd - spanStart;
                            spanStart = spans[k].first;
                            spanEnd = spans[k].second;
                        } else {
                            spanEnd = std::max(spanEnd, spans[k].second);
                        }
                    }
                    covered += spanEnd - spanStart;
                    area += static_cast<double>(right - left) *
                        static_cast<double>(covered);
                }
                return area;
            }

            double percentile(std::vector<double> values, double fraction) {
                if(values.empty()) {
                    return 0.0;
                }
                std::sort(values.begin(), values.end());
                const double index =
                    fraction * static_cast<double>(values.size() - 1);
                const auto clamped = static_cast<std::size_t>(
                    std::clamp(index, 0.0,
                               static_cast<double>(values.size() - 1)));
                return values[clamped];
            }

            double perFrame(std::uint64_t total, std::uint64_t frames) {
                return frames ? static_cast<double>(total) /
                        static_cast<double>(frames)
                              : 0.0;
            }

            // 自校验（KRKR_SLA_DIRTY_SELFCHECK=1，默认关闭）：用满分辨率整数
            // 网格重新盖章算一遍并集面积，与解析式切片结果逐帧比对。判定门槛
            // 建立在这个面积上，所以算法本身要有独立证据，而不是只靠复查代码。
            bool selfCheckEnabled() {
                static const bool enabled = [] {
                    const char *env =
                        std::getenv("KRKR_SLA_DIRTY_SELFCHECK");
                    return env && env[0] != '\0' && env[0] != '0';
                }();
                return enabled;
            }

            void runSelfCheck(PlayerRuntime::SlaDirtyCoverageState &state,
                              const std::vector<std::array<int, 4>> &rects,
                              tjs_int canvasWidth, tjs_int canvasHeight,
                              double analyticArea) {
                const std::size_t width =
                    static_cast<std::size_t>(std::max<tjs_int>(canvasWidth, 0));
                const std::size_t height =
                    static_cast<std::size_t>(std::max<tjs_int>(canvasHeight, 0));
                if(width == 0 || height == 0) {
                    return;
                }
                state.selfCheckGrid.assign(width * height, 0);
                for(const auto &rect : rects) {
                    const int left = std::clamp(rect[0], 0,
                                                static_cast<int>(width));
                    const int right = std::clamp(rect[2], 0,
                                                 static_cast<int>(width));
                    const int top = std::clamp(rect[1], 0,
                                               static_cast<int>(height));
                    const int bottom = std::clamp(rect[3], 0,
                                                  static_cast<int>(height));
                    for(int y = top; y < bottom; ++y) {
                        auto *row = state.selfCheckGrid.data() +
                            static_cast<std::size_t>(y) * width;
                        for(int x = left; x < right; ++x) {
                            row[x] = 1;
                        }
                    }
                }
                std::size_t stamped = 0;
                for(std::uint8_t cell : state.selfCheckGrid) {
                    stamped += cell ? 1u : 0u;
                }
                if(std::fabs(static_cast<double>(stamped) - analyticArea) >
                   0.5) {
                    if(auto logger = spdlog::get("plugin")) {
                        logger->warn(
                            "sla.dirty.selfcheck MISMATCH grid={} analytic={} "
                            "rects={} canvas={}x{}",
                            stamped, analyticArea, rects.size(), width, height);
                    }
                    return;
                }
                ++state.selfCheckPassed;
            }
        } // namespace

        void recordSlaDirtyCoverage(PlayerRuntime &runtime,
                                    const std::string &motionPath,
                                    tjs_int canvasWidth, tjs_int canvasHeight) {
            auto &state = runtime.slaDirtyCoverage;
            const double nowSeconds = std::chrono::duration<double>(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch())
                                          .count();
            if(state.canvasWidth != canvasWidth ||
               state.canvasHeight != canvasHeight) {
                state.previous.clear();
                state.current.clear();
                state.dirty.clear();
                state.areaRatios.clear();
                state.replayRatios.clear();
                state.selfCheckPassed = 0;
                state.frames = 0;
                state.commands = 0;
                state.changedItems = 0;
                state.replayCommands = 0;
                state.canvasWidth = canvasWidth;
                state.canvasHeight = canvasHeight;
                state.hasPreviousFrame = false;
                state.windowStartSeconds = nowSeconds;
                state.windowOutputHitsStart =
                    runtime.emoteCommandOutputCacheHits;
                state.windowLeafHitsStart = runtime.emoteCommandLeafCacheHits;
            }
            if(state.windowStartSeconds == 0.0) {
                state.windowStartSeconds = nowSeconds;
                state.windowOutputHitsStart =
                    runtime.emoteCommandOutputCacheHits;
                state.windowLeafHitsStart = runtime.emoteCommandLeafCacheHits;
            }

            const bool hadPreviousFrame = state.hasPreviousFrame;
            state.current.clear();
            state.dirty.clear();
            std::uint64_t commands = 0;
            std::vector<std::uint64_t> currentOrder;
            std::unordered_set<std::uint64_t> changedKeys;
            for(const auto &command : runtime.renderCommands) {
                const auto *item = command.item;
                if(!item || !item->drawFlag) {
                    continue;
                }
                const std::size_t order = currentOrder.size();
                const std::uint64_t key = commandKey(command);
                currentOrder.push_back(key);
                state.current[key] = {
                    item->clipRect, itemSignature(*item), order
                };
                ++commands;
            }

            const auto appendDirtyRect = [&](const std::array<int, 4> &rect) {
                const int width = std::max<tjs_int>(canvasWidth, 0);
                const int height = std::max<tjs_int>(canvasHeight, 0);
                const std::array<int, 4> clipped = {
                    std::clamp(rect[0], 0, width),
                    std::clamp(rect[1], 0, height),
                    std::clamp(rect[2], 0, width),
                    std::clamp(rect[3], 0, height),
                };
                if(clipped[0] < clipped[2] && clipped[1] < clipped[3]) {
                    state.dirty.push_back(clipped);
                }
            };
            const auto markChanged = [&](std::uint64_t key) {
                if(!changedKeys.insert(key).second) {
                    return;
                }
                const auto previous = state.previous.find(key);
                if(previous != state.previous.end()) {
                    appendDirtyRect(previous->second.rect);
                }
                const auto current = state.current.find(key);
                if(current != state.current.end()) {
                    appendDirtyRect(current->second.rect);
                }
            };

            if(hadPreviousFrame) {
                for(const auto &[key, current] : state.current) {
                    const auto previous = state.previous.find(key);
                    if(previous == state.previous.end() ||
                       previous->second.signature != current.signature) {
                        markChanged(key);
                    }
                }
                // Commands which disappear still leave their previous pixels
                // on a retained canvas, so their old rect must be cleared.
                for(const auto &[key, previous] : state.previous) {
                    (void)previous;
                    if(state.current.find(key) == state.current.end()) {
                        markChanged(key);
                    }
                }
                // Compare relative order only among surviving commands. This
                // catches a real z-order inversion without marking every item
                // after an insertion/removal as reordered.
                std::vector<std::uint64_t> survivingOrder;
                survivingOrder.reserve(currentOrder.size());
                for(const auto key : currentOrder) {
                    if(state.previous.find(key) != state.previous.end()) {
                        survivingOrder.push_back(key);
                    }
                }
                for(std::size_t i = 0; i < survivingOrder.size(); ++i) {
                    const auto &previousI = state.previous.at(survivingOrder[i]);
                    for(std::size_t j = i + 1; j < survivingOrder.size(); ++j) {
                        const auto &previousJ =
                            state.previous.at(survivingOrder[j]);
                        if(previousI.order > previousJ.order) {
                            markChanged(survivingOrder[i]);
                            markChanged(survivingOrder[j]);
                        }
                    }
                }
            }

            const std::uint64_t changed = changedKeys.size();

            std::uint64_t replay = 0;
            if(hadPreviousFrame && changed > 0) {
                for(const auto &command : runtime.renderCommands) {
                    const auto *item = command.item;
                    if(!item || !item->drawFlag) {
                        continue;
                    }
                    for(const auto &rect : state.dirty) {
                        if(rectsIntersect(item->clipRect, rect)) {
                            ++replay;
                            break;
                        }
                    }
                }
            }

            state.previous.swap(state.current);
            state.hasPreviousFrame = true;
            ++state.frames;
            state.commands += commands;
            state.changedItems += changed;
            state.replayCommands += replay;
            if(hadPreviousFrame && canvasWidth > 0 && canvasHeight > 0) {
                const double canvasArea = static_cast<double>(canvasWidth) *
                    static_cast<double>(canvasHeight);
                const double dirtyArea = unionArea(state.dirty);
                state.areaRatios.push_back(dirtyArea / canvasArea);
                state.replayRatios.push_back(
                    commands ? static_cast<double>(replay) /
                            static_cast<double>(commands)
                             : 0.0);
                if(selfCheckEnabled()) {
                    runSelfCheck(state, state.dirty, canvasWidth, canvasHeight,
                                 dirtyArea);
                }
            }

            if(nowSeconds - state.windowStartSeconds < 3.0) {
                return;
            }
            if(auto logger = spdlog::get("plugin")) {
                const std::uint64_t outputHits =
                    runtime.emoteCommandOutputCacheHits -
                    state.windowOutputHitsStart;
                const std::uint64_t leafHits = runtime.emoteCommandLeafCacheHits -
                    state.windowLeafHitsStart;
                logger->info(
                    "sla.dirty.window path={} windowMs={:.0f} frames={} "
                    "commandsPerFrame={:.1f} changedPerFrame={:.1f} "
                    "areaP50={:.1f}% areaP90={:.1f}% replayP50={:.1f}% "
                    "replayP90={:.1f}% outputHitsPerFrame={:.1f} "
                    "leafHitsPerFrame={:.1f} selfCheckPassed={}",
                    motionPath, (nowSeconds - state.windowStartSeconds) * 1000.0,
                    state.frames, perFrame(state.commands, state.frames),
                    perFrame(state.changedItems, state.frames),
                    percentile(state.areaRatios, 0.5) * 100.0,
                    percentile(state.areaRatios, 0.9) * 100.0,
                    percentile(state.replayRatios, 0.5) * 100.0,
                    percentile(state.replayRatios, 0.9) * 100.0,
                    perFrame(outputHits, state.frames),
                    perFrame(leafHits, state.frames),
                    state.selfCheckPassed);
            }
            state.areaRatios.clear();
            state.replayRatios.clear();
            state.selfCheckPassed = 0;
            state.frames = 0;
            state.commands = 0;
            state.changedItems = 0;
            state.replayCommands = 0;
            state.windowStartSeconds = nowSeconds;
            state.windowOutputHitsStart = runtime.emoteCommandOutputCacheHits;
            state.windowLeafHitsStart = runtime.emoteCommandLeafCacheHits;
        }
    } // namespace detail
} // namespace motion
