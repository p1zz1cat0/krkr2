// RoutedTransition.cpp — After emotedriver routed variable transition.
#include "RoutedTransition.h"

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <tuple>

namespace motion::routed {

    namespace {
        // DAT_1007a10c / DAT_1007a110: leg-finish tolerance.
        constexpr float kLegTolerance = 0.0001f;
        constexpr double kCostEpsilon = 1e-9;

        bool inRange(float a, float b, float v) {
            return std::min(a, b) <= v && v <= std::max(a, b);
        }

        bool sameGroup(const Graph &graph, float a, float b) {
            for(const auto &group : graph.nodes) {
                bool hasA = false;
                bool hasB = false;
                for(const float v : group) {
                    hasA = hasA || v == a;
                    hasB = hasB || v == b;
                }
                if(hasA && hasB) {
                    return true;
                }
            }
            return false;
        }

        // Cost to finish at `to` from vertex `v`, or nullopt.
        std::optional<double> goalCost(const Graph &graph, float v, float to) {
            std::optional<double> best;
            for(const auto &[a, b] : graph.edges) {
                if(a != b && inRange(a, b, to) && inRange(a, b, v)) {
                    const double cost = std::abs(static_cast<double>(to) - v);
                    if(!best || cost < *best) {
                        best = cost;
                    }
                }
            }
            if(sameGroup(graph, v, to) && !best) {
                best = 0.0;
            }
            return best;
        }

        struct Link {
            float to;
            double cost;
            bool move; // false: a zero-cost jump inside a node group
            float legFrom;
        };

        // Vertex keys: the start point is kept apart from graph vertices
        // because its value can equal one of them.
        using Vertex = std::pair<bool, float>; // {isStart, value}
    } // namespace

    float planRoute(const Graph &graph, float from, float to,
                    std::vector<Leg> &legs) {
        legs.clear();

        const auto containingEdge = [&](float value) {
            for(size_t i = 0; i < graph.edges.size(); ++i) {
                const auto [a, b] = graph.edges[i];
                if(inRange(a, b, value)) {
                    return i;
                }
            }
            return graph.edges.size();
        };
        const auto startEdge = containingEdge(from);
        if(startEdge < graph.edges.size() && startEdge == containingEdge(to)) {
            legs.push_back({ from, to });
            return std::abs(to - from);
        }

        // FUN_100229cd selects the first internal node on the start edge.
        // Preserve node order. A node at the start value is not a free exit.
        std::optional<float> internalExit;
        if(startEdge < graph.edges.size()) {
            const auto [a, b] = graph.edges[startEdge];
            for(const auto &group : graph.nodes) {
                for(const float value : group) {
                    if(value != a && value != b && inRange(a, b, value)) {
                        internalExit = value;
                        break;
                    }
                }
                if(internalExit) {
                    break;
                }
            }
        }

        std::map<Vertex, std::vector<Link>> adjacency;
        const Vertex start{ true, from };
        adjacency[start];
        if(internalExit) {
            adjacency[start].push_back(
                { *internalExit, std::abs(static_cast<double>(*internalExit) - from),
                  true, from });
        }
        for(const auto &[a, b] : graph.edges) {
            adjacency[{ false, a }];
            adjacency[{ false, b }];
            if(a != b) {
                const double cost = std::abs(static_cast<double>(b) - a);
                adjacency[{ false, a }].push_back({ b, cost, true, a });
                adjacency[{ false, b }].push_back({ a, cost, true, b });
                if(inRange(a, b, from)) {
                    const float lo = std::min(a, b);
                    const float hi = std::max(a, b);
                    if(from != lo) {
                        adjacency[start].push_back(
                            { lo, static_cast<double>(from) - lo, true, from });
                    }
                    if(from != hi) {
                        adjacency[start].push_back(
                            { hi, static_cast<double>(hi) - from, true, from });
                    }
                }
            }
        }
        for(const auto &group : graph.nodes) {
            for(const float a : group) {
                adjacency[{ false, a }];
                for(const float b : group) {
                    if(a != b) {
                        adjacency[{ false, a }].push_back({ b, 0.0, false, 0 });
                    }
                }
                if(a == from && !internalExit) {
                    for(const float b : group) {
                        if(b != from) {
                            adjacency[start].push_back({ b, 0.0, false, 0 });
                        }
                    }
                }
            }
        }

        struct Parent {
            Vertex vertex;
            bool move;
            float legFrom;
        };
        std::map<Vertex, double> dist;
        std::map<Vertex, Parent> parent;
        // {cost, insertion order, vertex}: the order keeps the pop order of
        // equal costs stable, so the tie rule below is deterministic.
        using Entry = std::tuple<double, long, Vertex>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
        long counter = 0;
        const auto distanceOf = [&](const Vertex &v) {
            const auto it = dist.find(v);
            return it == dist.end() ? std::numeric_limits<double>::infinity()
                                    : it->second;
        };
        for(const auto &link : adjacency[start]) {
            const Vertex v{ false, link.to };
            if(link.cost < distanceOf(v) - kCostEpsilon) {
                dist[v] = link.cost;
                parent[v] = { start, link.move, link.legFrom };
                queue.emplace(link.cost, counter++, v);
            }
        }

        double best = std::numeric_limits<double>::infinity();
        double bestEntry = -std::numeric_limits<double>::infinity();
        std::optional<Vertex> bestVertex;
        // Start and target on the same edge: the direct move is a candidate.
        if(const auto cost = goalCost(graph, from, to); cost && !internalExit) {
            best = *cost;
            bestEntry = 0.0;
            bestVertex = start;
        }
        while(!queue.empty()) {
            const auto [d, order, v] = queue.top();
            queue.pop();
            if(d > distanceOf(v) + kCostEpsilon) {
                continue;
            }
            // The cost update in FUN_10022230 is not strict: on equal totals
            // the candidate settled last wins. Settle order is cost order, so
            // the winner is the one with the shortest final leg.
            if(const auto cost = goalCost(graph, v.second, to)) {
                const double total = d + *cost;
                if(total < best - kCostEpsilon ||
                   (total < best + kCostEpsilon && d > bestEntry + kCostEpsilon)) {
                    best = total;
                    bestEntry = d;
                    bestVertex = v;
                }
            }
            for(const auto &link : adjacency[v]) {
                const Vertex w{ false, link.to };
                const double nd = d + link.cost;
                if(nd < distanceOf(w) - kCostEpsilon) {
                    dist[w] = nd;
                    parent[w] = { v, link.move, link.legFrom };
                    queue.emplace(nd, counter++, w);
                }
            }
        }

        if(!bestVertex) {
            legs.push_back({ to, to });
            return 0.0f;
        }

        // Rebuild backwards: the final approach, then each move toward start.
        // A final approach that is a jump (no edge holds both points) is not
        // a move; a {to, to} leg sets the value when the route ends.
        bool finalJump = false;
        std::vector<Leg> reversed;
        if(bestVertex->second != to) {
            bool onEdge = false;
            for(const auto &[a, b] : graph.edges) {
                onEdge = onEdge ||
                    (a != b && inRange(a, b, to) &&
                     inRange(a, b, bestVertex->second));
            }
            if(onEdge) {
                reversed.push_back({ bestVertex->second, to });
            } else {
                finalJump = true;
            }
        }
        Vertex v = *bestVertex;
        while(!v.first) {
            const auto &p = parent.at(v);
            if(p.move) {
                reversed.push_back({ p.legFrom, v.second });
            }
            v = p.vertex;
        }
        double length = 0.0;
        for(auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
            if(it->from != it->to) {
                legs.push_back(*it);
                length += std::abs(static_cast<double>(it->to) - it->from);
            }
        }
        if(finalJump || legs.empty()) {
            legs.push_back({ to, to });
        }
        return static_cast<float>(length);
    }

    Transition::Transition(std::shared_ptr<const Graph> graph, float value) :
        _graph(std::move(graph)), _value(value) {}

    float Transition::routeTarget() const {
        return _legs.empty() ? _legTarget : _legs.back().to;
    }

    void Transition::set(float value, float duration, float weight,
                         bool queue) {
        if(duration <= 0.0f) {
            _keyframes.clear();
            _legs.clear();
            _state = 0;
            _value = value;
            return;
        }
        if(queue) {
            _keyframes.push_back({ value, duration, weight });
            return;
        }
        // The same target is already in flight and arrives sooner than the
        // new request would: keep the route.
        if(_state != 0 && routeTarget() == value && _length > 0.0f) {
            const double s = std::pow(static_cast<double>(_progress) / _length,
                                      1.0 / _weight);
            if((1.0 - s) / _invDuration < duration) {
                return;
            }
        }
        _keyframes.clear();
        _legs.clear();
        _state = 0;
        _keyframes.push_back({ value, duration, weight });
    }

    void Transition::advance(float dt) {
        for(;;) {
            if(_state == 0) {
                if(_keyframes.empty()) {
                    return;
                }
                const auto keyframe = _keyframes.front();
                _keyframes.pop_front();
                std::vector<Leg> legs;
                _length = planRoute(*_graph, _value, keyframe.value, legs);
                _legs.assign(legs.begin(), legs.end());
                _invDuration = 1.0f / keyframe.duration;
                _weight = keyframe.weight;
                _progress = 0.0f;
                _state = 1;
                continue;
            }
            if(_state == 1) {
                if(_legs.empty()) {
                    _state = 0;
                    continue;
                }
                const auto leg = _legs.front();
                _legs.pop_front();
                if(leg.from == leg.to) {
                    _value = leg.to;
                    continue;
                }
                _legTarget = leg.to;
                _value = leg.from;
                _dir = leg.to - leg.from >= 0.0f ? 1.0f : -1.0f;
                _state = 2;
                continue;
            }

            // State 2. A finished leg goes back to state 1 inside the same
            // sub-step, so dt is added again for the next leg (as the DLL).
            const double s0 = _length > 0.0f
                ? std::pow(static_cast<double>(_progress) / _length,
                           1.0 / _weight)
                : 0.0;
            const float s = static_cast<float>(s0) + dt * _invDuration;
            const double u = std::pow(static_cast<double>(s), _weight);
            const double delta = u * _length - _progress;
            const float moved =
                static_cast<float>(_value + _dir * delta);
            _value = moved;
            const bool finished = _dir <= 0.0f
                ? !(moved - _legTarget > kLegTolerance)
                : !(moved - _legTarget < -kLegTolerance);
            if(!finished) {
                _progress = static_cast<float>(_progress + delta);
                return;
            }
            // Clamp to the leg end and take the overshoot back from progress,
            // so the rest of the route must hurry (oracle: the second leg
            // runs ahead of a uniform walk).
            _value = _legTarget;
            _progress = static_cast<float>(
                _progress + (_legTarget - moved) * _dir);
            _state = 1;
        }
    }

} // namespace motion::routed
