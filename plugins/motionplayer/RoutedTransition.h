// Routed variable transition of the After E-mote driver (emotedriver.dll,
// PE 2025-04-28). Expression controls (eyebrowControl) do not move a variable
// in a straight line. They walk the shortest path on a graph:
//
//   edge  [a, b]       a range on the variable axis that interpolates.
//   node  {p, q, ...}  end points with the same shape; a jump costs 0.
//
// Example (face_eyebrow): 怒り 20 -> 笑い 40 goes 20 -> 11, jumps to 31, then
// goes 31 -> 40. A straight line would show 哀しみ (30) on the way.
//
// This layer has no TJS, PSB, or Player dependency, so unit tests can compare
// it with values that the original DLL returned (see feature 0010).
#pragma once

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace motion::routed {

    struct Graph {
        std::vector<std::pair<float, float>> edges;
        std::vector<std::vector<float>> nodes;

        [[nodiscard]] bool empty() const {
            return edges.empty() && nodes.empty();
        }
    };

    struct Leg {
        float from = 0.0f;
        float to = 0.0f;
    };

    // Planner FUN_10022c70 with search FUN_10022230. Writes the movement legs
    // (jumps are implicit: the next leg starts at the jump target) and
    // returns the route length L. If no route exists, writes {to, to} and
    // returns 0, so the value changes instantly.
    float planRoute(const Graph &graph, float from, float to,
                    std::vector<Leg> &legs);

    // One routed control (emote::EPGraph inside EPEyebrowControl).
    // set() is FUN_100200d0. advance() is FUN_1001fc70 for one sub-step; the
    // caller splits progress(dt) into sub-steps of at most 1.1 frames.
    class Transition {
    public:
        Transition(std::shared_ptr<const Graph> graph, float value);

        void set(float value, float duration, float weight, bool queue);
        void advance(float dt);

        [[nodiscard]] float value() const { return _value; }
        [[nodiscard]] bool busy() const {
            return _state != 0 || !_keyframes.empty();
        }

    private:
        struct Keyframe {
            float value;
            float duration;
            float weight;
        };

        // The final target of the route in flight.
        [[nodiscard]] float routeTarget() const;

        std::shared_ptr<const Graph> _graph;
        std::deque<Keyframe> _keyframes; // +0x30
        std::deque<Leg> _legs; // +0x64
        int _state = 0; // +0x44: 0 fetch keyframe, 1 fetch leg, 2 advance
        float _value = 0.0f; // +0x48
        float _invDuration = 0.0f; // +0x4c
        float _dir = 1.0f; // +0x50
        float _legTarget = 0.0f; // +0x54
        float _length = 0.0f; // +0x58
        float _progress = 0.0f; // +0x5c
        float _weight = 1.0f; // +0x60
    };

} // namespace motion::routed
