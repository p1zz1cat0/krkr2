// Deterministic controller and physics primitives for MotionPlayer.
//
// This layer intentionally has no TJS, PSB, renderer, or Player dependency so
// native tests can compare it with frozen outputs produced directly by the
// original Windows DLL machine code.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace motion::physics {

    // The 2016 DLL constructors leave these fields untouched even though the
    // update helpers read them. Zero is a deterministic compatibility policy,
    // not a claim about the original undefined heap contents.
    inline constexpr double kDeterministicLegacyBustYBias = 0.0;
    inline constexpr double kDeterministicPendVerticalReference = 0.0;

    inline constexpr double kMaximumSubstepFrames = 1.1;
    inline constexpr double kSubstepEpsilon = 1.1920928955078125e-7;

    struct Vec2 {
        double x = 0.0;
        double y = 0.0;
    };

    struct Vec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct BustConfig {
        double gravity = 0.0;
        double spring = 0.0;
        double friction = 0.0;
        double scaleX = 0.0;
        double scaleY = 0.0;
        double metadataOffset = 0.0;
        std::string label;
        std::string parameter;
        std::string baseLayer;
        Vec3 initialAnchor;
        Vec3 initialPosition;
        Vec3 initialVelocity;
        bool hasInitialState = false;
        std::string varLr;
        std::string varUd;
    };

    struct PendConfig {
        double gravity = 0.0;
        double frictionX = 0.0;
        double frictionY = 0.0;
        double backRate = 0.0;
        double velocityBound = 0.0;
        int verticalOutputSegment = 0;
        std::array<double, 2> length{};
        std::array<double, 2> scaleX{};
        std::array<double, 2> scaleY{};
        double bendSpeed = 0.0;
        double bendVolume = 0.0;
        double metadataOffset = 0.0;
        std::string label;
        std::string parameter;
        std::string baseLayer;
        Vec3 initialAnchor;
        std::array<Vec3, 2> initialRest{};
        std::array<Vec3, 2> initialPosition{};
        std::array<Vec3, 2> initialVelocity{};
        double initialBendPhase = 0.0;
        double initialBendEnvelope = 0.0;
        bool hasInitialState = false;
        std::string varLr;
        std::string varLrm;
        std::string varUd;
    };

    struct EyeConfig {
        double beginFrame = 0.0;
        double endFrame = 0.0;
        double blinkIntervalMin = 0.0;
        double blinkIntervalMax = 0.0;
        double blinkFrameCount = 0.0;
        bool blinkEnabled = false;
        std::string label;
    };

    class OuterForceAnimator {
    public:
        enum class QueueMode { Replace, Append };

        void set(double x, double y, double duration, double easing,
                 QueueMode mode = QueueMode::Replace);
        void clear();
        void step(double dt);

        [[nodiscard]] Vec2 current() const noexcept { return current_; }
        [[nodiscard]] bool active() const noexcept { return !queue_.empty(); }

    private:
        struct Segment {
            Vec2 target;
            double duration = 0.0;
            double exponent = 1.0;
        };

        std::deque<Segment> queue_;
        Vec2 current_;
        Vec2 segmentStart_;
        double progress_ = 1.0;
        bool segmentStarted_ = false;
    };

    class WindControl {
    public:
        static constexpr std::size_t kSlotCount = 128;
        using RandomSource = std::function<double()>;

        struct Gust {
            bool active = false;
            double position = 0.0;
            double power = 0.0;
        };

        void start(double start, double goal, double speed, double powerMin,
                   double powerMax);
        void stop();
        void step(double dt, const RandomSource &random);

        [[nodiscard]] double sample(double x) const noexcept;
        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] double signedSpeed() const noexcept {
            return signedSpeed_;
        }
        [[nodiscard]] const std::array<Gust, kSlotCount> &gusts() const {
            return gusts_;
        }

    private:
        bool active_ = false;
        double start_ = 0.0;
        double goal_ = 0.0;
        double powerMin_ = 0.0;
        double powerMax_ = 0.0;
        double signedSpeed_ = 0.0;
        double spawnAccumulator_ = 0.0;
        std::array<Gust, kSlotCount> gusts_{};
    };

    class BustControl {
    public:
        explicit BustControl(BustConfig config);

        [[nodiscard]] const BustConfig &config() const noexcept {
            return config_;
        }
        [[nodiscard]] std::array<double, 2>
        stepFrame(Vec2 input, Vec2 force, double dt, double globalScale,
                  double angleRadians);

    private:
        void step(Vec2 input, Vec2 force, double dt, double globalScale,
                  double angleRadians);

        BustConfig config_;
        bool first_ = true;
        bool consumerInitialized_ = false;
        Vec3 anchor_;
        Vec2 inputOffset_;
        Vec3 position_;
        Vec3 velocity_;
        Vec2 previousInput_;
        std::array<double, 2> output_{};
    };

    class PendControl {
    public:
        explicit PendControl(PendConfig config);

        [[nodiscard]] const PendConfig &config() const noexcept {
            return config_;
        }
        [[nodiscard]] std::array<double, 3>
        stepFrame(Vec2 input, Vec2 force, double dt, double globalScale,
                  double angleRadians, const WindControl *wind);

    private:
        void step(Vec2 input, Vec2 force, double dt, double globalScale,
                  double angleRadians, const WindControl *wind);
        void applyBend(double dt);

        PendConfig config_;
        bool first_ = true;
        bool consumerInitialized_ = false;
        Vec3 anchor_;
        Vec2 inputOffset_;
        std::array<Vec3, 2> rest_{};
        std::array<Vec3, 2> position_{};
        std::array<Vec3, 2> velocity_{};
        Vec2 previousInput_;
        double bendPhase_ = 0.0;
        double bendEnvelope_ = 0.0;
        std::array<double, 3> output_{};
    };

    class EyeControl {
    public:
        using RandomSource = WindControl::RandomSource;

        explicit EyeControl(EyeConfig config);

        [[nodiscard]] const EyeConfig &config() const noexcept {
            return config_;
        }
        [[nodiscard]] std::optional<double>
        step(double dt, double currentVariableValue,
             const RandomSource &random);

    private:
        enum class Phase { Idle, Closing, Hold, Opening };

        void resetCountdown(const RandomSource &random);
        [[nodiscard]] bool atBegin(double value) const noexcept;
        [[nodiscard]] bool reachedEnd(double value) const noexcept;
        [[nodiscard]] bool reachedBegin(double value) const noexcept;

        EyeConfig config_;
        Phase phase_ = Phase::Idle;
        double current_ = 0.0;
        double countdown_ = -1.0;
        double hold_ = 0.0;
        double lastPublished_ = 0.0;
        bool hasPublished_ = false;
        bool suppressed_ = false;
    };

    struct ControllerRuntimeState {
        std::uint64_t motionGeneration = 0;
        std::vector<BustControl> bust;
        std::vector<PendControl> hair;
        std::vector<PendControl> parts;
        std::vector<EyeControl> eyes;
        OuterForceAnimator bustForce;
        OuterForceAnimator hairForce;
        OuterForceAnimator partsForce;
        WindControl wind;
        std::unordered_map<std::string, double> stagedPhysicsOutputs;
    };

} // namespace motion::physics
