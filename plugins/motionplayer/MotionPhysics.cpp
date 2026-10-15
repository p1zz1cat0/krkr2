#include "MotionPhysics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace motion::physics {
    namespace {
        constexpr double kPi = 3.141592653589793238462643383279502884;
        constexpr double kSoftScale = kPi / 80.0;
        constexpr double kSoftGain = 1.149999976158142;
        constexpr double kPendLengthGuard = 0.015625;

        double soft(double value) {
            return std::atan(value * kSoftGain * kSoftScale) / kSoftScale;
        }

        double clampRandom(double value) {
            if(!std::isfinite(value)) {
                return 0.0;
            }
            return std::clamp(value, 0.0,
                              std::nextafter(1.0, 0.0));
        }

        double easingExponent(double easing) {
            if(easing > 0.0) {
                return easing + 1.0;
            }
            if(easing < 0.0) {
                return 1.0 / (1.0 - easing);
            }
            return 1.0;
        }

        Vec2 interpolate(Vec2 from, Vec2 to, double ratio) {
            return Vec2{ from.x + (to.x - from.x) * ratio,
                         from.y + (to.y - from.y) * ratio };
        }

        void requireFiniteFrame(Vec2 input, Vec2 force, double dt,
                                double scale, double angle) {
            if(!std::isfinite(input.x) || !std::isfinite(input.y) ||
               !std::isfinite(force.x) || !std::isfinite(force.y) ||
               !std::isfinite(dt) || !std::isfinite(scale) ||
               !std::isfinite(angle)) {
                throw std::invalid_argument(
                    "motion physics frame input must be finite");
            }
        }
    } // namespace

    void OuterForceAnimator::set(double x, double y, double duration,
                                 double easing, QueueMode mode) {
        if(duration <= 0.0) {
            clear();
            current_ = Vec2{ x, y };
            return;
        }
        if(mode == QueueMode::Replace) {
            queue_.clear();
            segmentStarted_ = false;
            progress_ = 1.0;
        }
        queue_.push_back(
            Segment{ Vec2{ x, y }, duration, easingExponent(easing) });
    }

    void OuterForceAnimator::clear() {
        queue_.clear();
        segmentStarted_ = false;
        progress_ = 1.0;
    }

    void OuterForceAnimator::step(double dt) {
        double remaining = dt;
        while(!queue_.empty() && remaining > kSubstepEpsilon) {
            auto &segment = queue_.front();
            if(!segmentStarted_) {
                segmentStart_ = current_;
                progress_ = 0.0;
                segmentStarted_ = true;
            }
            const double segmentRemaining =
                segment.duration * std::max(0.0, 1.0 - progress_);
            const double consumed = std::min(remaining, segmentRemaining);
            progress_ += consumed / segment.duration;
            remaining -= consumed;
            if(progress_ >= 1.0 - kSubstepEpsilon) {
                current_ = segment.target;
                queue_.pop_front();
                segmentStarted_ = false;
                progress_ = 1.0;
                continue;
            }
            const double curved = std::pow(progress_, segment.exponent);
            current_ = interpolate(segmentStart_, segment.target, curved);
        }
    }

    void WindControl::start(double start, double goal, double speed,
                            double powerMin, double powerMax) {
        stop();
        if(start == 0.0 && goal == 0.0 && speed == 0.0 && powerMin == 0.0 &&
           powerMax == 0.0) {
            return;
        }
        if(speed < 0.0) {
            std::swap(start, goal);
            speed = -speed;
        }
        if(speed == 0.0 || start == goal) {
            return;
        }
        active_ = true;
        start_ = start;
        goal_ = goal;
        powerMin_ = powerMin;
        powerMax_ = powerMax;
        signedSpeed_ = goal >= start ? speed : -speed;
    }

    void WindControl::stop() {
        active_ = false;
        start_ = 0.0;
        goal_ = 0.0;
        powerMin_ = 0.0;
        powerMax_ = 0.0;
        signedSpeed_ = 0.0;
        spawnAccumulator_ = 0.0;
        gusts_ = {};
    }

    void WindControl::step(double dt, const RandomSource &random) {
        if(!active_ || dt <= 0.0) {
            return;
        }
        spawnAccumulator_ += std::abs(signedSpeed_) * dt;
        while(spawnAccumulator_ >= 1.0) {
            auto freeSlot = std::find_if(
                gusts_.begin(), gusts_.end(),
                [](const Gust &gust) { return !gust.active; });
            if(freeSlot != gusts_.end()) {
                const double unit = random ? clampRandom(random()) : 0.0;
                freeSlot->active = true;
                freeSlot->position = start_;
                freeSlot->power =
                    powerMin_ + (powerMax_ - powerMin_) * unit;
            }
            spawnAccumulator_ -= 1.0;
        }

        for(auto &gust : gusts_) {
            if(!gust.active) {
                continue;
            }
            gust.position += signedSpeed_ * dt;
            if((signedSpeed_ >= 0.0 && gust.position >= goal_) ||
               (signedSpeed_ < 0.0 && gust.position <= goal_)) {
                gust.active = false;
            }
        }
    }

    double WindControl::sample(double x) const noexcept {
        if(!active_) {
            return 0.0;
        }
        // Select the strongest gust whose interval covers x. This keeps sampled
        // force within powerMax * abs(signedSpeed_) and independent of slot
        // recycling order.
        double strongest = 0.0;
        for(const auto &gust : gusts_) {
            if(!gust.active) {
                continue;
            }
            const double radius = 2.0 * gust.power;
            if(x >= gust.position - radius && x <= gust.position + radius) {
                if(gust.power > strongest) {
                    strongest = gust.power;
                }
            }
        }
        return strongest * signedSpeed_;
    }

    BustControl::BustControl(BustConfig config) : config_(std::move(config)) {
        if(config_.hasInitialState) {
            anchor_ = config_.initialAnchor;
            position_ = config_.initialPosition;
            velocity_ = config_.initialVelocity;
        }
    }

    void BustControl::step(Vec2 input, Vec2 force, double dt,
                           double globalScale, double angleRadians) {
        if(first_) {
            first_ = false;
            inputOffset_.x = anchor_.x - input.x;
            inputOffset_.y = anchor_.y - input.y;
        } else {
            anchor_.x = inputOffset_.x + input.x;
            anchor_.y = inputOffset_.y + input.y;
        }

        const double sinAngle = std::sin(angleRadians);
        const double cosAngle = std::cos(angleRadians);
        const Vec2 localForce{ cosAngle * force.x + sinAngle * force.y,
                               -sinAngle * force.x + cosAngle * force.y };
        const Vec3 delta{ anchor_.x - position_.x,
                          anchor_.y - position_.y,
                          anchor_.z - position_.z };
        const double springStep = config_.spring * dt;
        velocity_.x += springStep * delta.x + dt * localForce.x +
            dt * config_.gravity * sinAngle;
        velocity_.y += springStep * delta.y + dt * localForce.y +
            dt * config_.gravity * cosAngle;
        velocity_.z += springStep * delta.z;

        const double friction = 1.0 - config_.friction * dt;
        velocity_.x *= friction;
        velocity_.y *= friction;
        velocity_.z *= friction;
        position_.x += dt * velocity_.x;
        position_.y += dt * velocity_.y;
        position_.z += dt * velocity_.z;

        const double dx = globalScale * (anchor_.x - position_.x);
        const double dy = globalScale * (anchor_.y - position_.y);
        output_[0] = soft(-config_.scaleX * dx);
        output_[1] = soft(config_.scaleY *
                          (-dy - kDeterministicLegacyBustYBias));
    }

    std::array<double, 2>
    BustControl::stepFrame(Vec2 input, Vec2 force, double dt,
                           double globalScale, double angleRadians) {
        requireFiniteFrame(input, force, dt, globalScale, angleRadians);
        if(dt <= 0.0) {
            return output_;
        }
        if(!consumerInitialized_) {
            step(input, force, 0.0, globalScale, angleRadians);
            previousInput_ = input;
            consumerInitialized_ = true;
        }

        double elapsed = 0.0;
        while(dt - elapsed > kSubstepEpsilon) {
            const double substep = std::min(kMaximumSubstepFrames,
                                            dt - elapsed);
            elapsed += substep;
            const Vec2 substepInput =
                interpolate(previousInput_, input, elapsed / dt);
            step(substepInput, force, substep, globalScale, angleRadians);
        }
        previousInput_ = input;
        return output_;
    }

    PendControl::PendControl(PendConfig config) : config_(std::move(config)) {
        if(config_.hasInitialState) {
            anchor_ = config_.initialAnchor;
            rest_ = config_.initialRest;
            position_ = config_.initialPosition;
            velocity_ = config_.initialVelocity;
            bendPhase_ = config_.initialBendPhase;
            bendEnvelope_ = config_.initialBendEnvelope;
        } else {
            rest_[0].y = config_.length[0];
            rest_[1].y = config_.length[0] + config_.length[1];
            position_ = rest_;
        }
    }

    void PendControl::step(Vec2 input, Vec2 force, double dt,
                           double globalScale, double angleRadians,
                           const WindControl *wind) {
        if(first_) {
            first_ = false;
            inputOffset_.x = anchor_.x - input.x;
            inputOffset_.y = anchor_.y - input.y;
        } else {
            anchor_.x = inputOffset_.x + input.x;
            anchor_.y = inputOffset_.y + input.y;
        }

        rest_[0] = Vec3{ anchor_.x, anchor_.y + config_.length[0],
                         anchor_.z };
        rest_[1] = Vec3{ rest_[0].x,
                         rest_[0].y + config_.length[1], rest_[0].z };

        const double sinAngle = std::sin(angleRadians);
        const double cosAngle = std::cos(angleRadians);
        const Vec2 localForce{ cosAngle * force.x + sinAngle * force.y,
                               -sinAngle * force.x + cosAngle * force.y };

        for(std::size_t index = 0; index < 2; ++index) {
            const Vec3 base = index == 0 ? anchor_ : position_[0];
            Vec3 delta{ base.x - position_[index].x,
                        base.y - position_[index].y,
                        base.z - position_[index].z };
            const double radius = std::sqrt(delta.x * delta.x +
                                            delta.y * delta.y +
                                            delta.z * delta.z);
            if(radius > config_.length[index] &&
               radius >= kPendLengthGuard) {
                const Vec3 normal{ delta.x / radius, delta.y / radius,
                                   delta.z / radius };
                const double stretch = radius - config_.length[index];
                if(index == 0) {
                    const double response =
                        stretch * config_.backRate * dt;
                    velocity_[index].x += normal.x * response;
                    velocity_[index].y += normal.y * response;
                    velocity_[index].z += normal.z * response;
                } else {
                    position_[index].x += normal.x * stretch;
                    position_[index].y += normal.y * stretch;
                    position_[index].z += normal.z * stretch;
                    const double radial = velocity_[index].x * normal.x +
                        velocity_[index].y * normal.y +
                        velocity_[index].z * normal.z;
                    const double response =
                        -config_.velocityBound * radial * dt;
                    velocity_[index].x += normal.x * response;
                    velocity_[index].y += normal.y * response;
                    velocity_[index].z += normal.z * response;
                }
            }

            velocity_[index].x += dt * localForce.x +
                dt * config_.gravity * sinAngle;
            velocity_[index].y += dt * localForce.y +
                dt * config_.gravity * cosAngle;
            if(wind) {
                // Wind is an acceleration like every other term in this
                // integrator, so it takes the same dt. Without it the gust
                // force landed as a raw per-frame velocity impulse and the
                // result changed with the frame rate.
                velocity_[index].x +=
                    dt * wind->sample(position_[index].x);
            }
            velocity_[index].x *= 1.0 - config_.frictionX * dt;
            velocity_[index].y *= 1.0 - config_.frictionY * dt;
            position_[index].x += dt * velocity_[index].x;
            position_[index].y += dt * velocity_[index].y;
            position_[index].z += dt * velocity_[index].z;

            const double dx = rest_[index].x - position_[index].x;
            const double dy = rest_[index].y - position_[index].y;
            output_[index] =
                soft(-dx * config_.scaleX[index] * globalScale);
            if(static_cast<int>(index) == config_.verticalOutputSegment) {
                // Measure vertical output from the authored rest sag (`pend.param.ofs`).
                // A missing metadataOffset is treated as zero, preserving the rest-pose
                // equilibrium used by the bend oscillator.
                output_[2] = soft((config_.metadataOffset - dy) *
                                  config_.scaleY[index] * globalScale);
            }
        }
    }

    void PendControl::applyBend(double dt) {
        if(std::abs(output_[2]) > 28.0) {
            bendEnvelope_ = std::min(1.0, bendEnvelope_ + dt / 32.0);
        } else {
            bendEnvelope_ = std::max(0.0, bendEnvelope_ - dt / 32.0);
        }
        bendPhase_ = std::fmod(
            bendPhase_ + config_.bendSpeed * bendEnvelope_, 2.0 * kPi);
        const double bend =
            std::sin(bendPhase_) * bendEnvelope_ * config_.bendVolume;
        output_[0] -= bend;
        output_[1] += bend;
    }

    std::array<double, 3>
    PendControl::stepFrame(Vec2 input, Vec2 force, double dt,
                           double globalScale, double angleRadians,
                           const WindControl *wind) {
        requireFiniteFrame(input, force, dt, globalScale, angleRadians);
        if(dt <= 0.0) {
            return output_;
        }
        if(!consumerInitialized_) {
            step(input, force, 0.0, globalScale, angleRadians, wind);
            applyBend(0.0);
            previousInput_ = input;
            consumerInitialized_ = true;
        }

        double elapsed = 0.0;
        while(dt - elapsed > kSubstepEpsilon) {
            const double substep = std::min(kMaximumSubstepFrames,
                                            dt - elapsed);
            elapsed += substep;
            const Vec2 substepInput =
                interpolate(previousInput_, input, elapsed / dt);
            step(substepInput, force, substep, globalScale, angleRadians,
                 wind);
            applyBend(substep);
        }
        previousInput_ = input;
        return output_;
    }

    EyeControl::EyeControl(EyeConfig config,
                           std::shared_ptr<const routed::Graph> graph) :
        config_(std::move(config)),
        route_(graph ? std::move(graph) : std::make_shared<routed::Graph>(),
               static_cast<float>(config_.beginFrame)),
        current_(static_cast<float>(config_.beginFrame)), published_(current_) {}

    void EyeControl::set(float value, float duration, float weight, bool queue) {
        route_.set(value, duration, weight, queue);
    }

    void EyeControl::resetCountdown(const RandomSource &random) {
        const float unit = random ? static_cast<float>(clampRandom(random())) : 0.0f;
        const float minimum = static_cast<float>(config_.blinkIntervalMin);
        const float maximum = static_cast<float>(config_.blinkIntervalMax);
        countdown_ = minimum + (maximum - minimum) * unit;
        intervalInitialized_ = true;
    }

    float EyeControl::step(float dt, const RandomSource &random) {
        route_.advance(dt);
        const float begin = static_cast<float>(config_.beginFrame);
        const float end = static_cast<float>(config_.endFrame);
        const float frames = static_cast<float>(config_.blinkFrameCount);
        if(!intervalInitialized_) {
            resetCountdown(random);
        }
        // Reuse the full dt after a phase change. Bound a zero-interval cycle.
        for(int phaseSteps = 0; phaseSteps < 5; ++phaseSteps) {
            if(phase_ == Phase::Idle) {
                if(!config_.blinkEnabled || std::trunc(current_) != begin) {
                    break;
                }
                countdown_ -= dt;
                if(countdown_ > 0.0f) {
                    break;
                }
                phase_ = Phase::Closing;
            } else if(phase_ == Phase::Closing) {
                current_ += dt * 2.5f / frames * (end - begin);
                if(end >= begin ? current_ < end : current_ > end) {
                    break;
                }
                current_ = end;
                phase_ = Phase::Hold;
                countdown_ = frames / 5.0f;
            } else if(phase_ == Phase::Hold) {
                countdown_ -= dt;
                if(countdown_ > 0.0f) {
                    break;
                }
                phase_ = Phase::Opening;
                resetCountdown(random);
            } else {
                current_ -= dt * 2.5f / frames * (end - begin);
                if(end >= begin ? current_ > begin : current_ < begin) {
                    break;
                }
                current_ = begin;
                phase_ = Phase::Idle;
            }
        }
        published_ = route_.value();
        if(end != begin && std::min(begin, end) <= published_ &&
           published_ <= std::max(begin, end)) {
            published_ += (current_ - begin) * (end - published_) / (end - begin);
        }
        return published_;
    }

} // namespace motion::physics
