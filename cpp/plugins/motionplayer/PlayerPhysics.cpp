// PlayerPhysics.cpp — transactional metadata parsing and progress phases.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "PlayerInternal.h"

using namespace motion::internal;

namespace {
    constexpr tjs_int kMaximumControlEntries = 4096;
    constexpr double kMaximumMetadataMagnitude = 1.0e9;
    constexpr double kPi = 3.141592653589793238462643383279502884;

    std::string lowerAscii(std::string value) {
        for(char &ch : value) {
            ch =
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    [[noreturn]] void invalidMetadata(const std::string &message) {
        throw std::runtime_error("motionplayer: invalid physics metadata: " +
                                 message);
    }

    bool optionalProperty(const tTJSVariant &object, const tjs_char *name,
                          tTJSVariant &value) {
        return getObjectProperty(object, name, value) &&
            value.Type() != tvtVoid;
    }

    tTJSVariant requiredProperty(const tTJSVariant &object,
                                 const tjs_char *name,
                                 const char *context) {
        tTJSVariant value;
        if(!optionalProperty(object, name, value)) {
            invalidMetadata(std::string(context) + " missing property");
        }
        return value;
    }

    bool readBool(const tTJSVariant &object, const tjs_char *name,
                  const char *context) {
        const auto value = requiredProperty(object, name, context);
        if(value.Type() != tvtInteger && value.Type() != tvtReal) {
            invalidMetadata(std::string(context) + " must be boolean/numeric");
        }
        const double number = value.AsReal();
        if(!std::isfinite(number)) {
            invalidMetadata(std::string(context) + " must be finite");
        }
        return number != 0.0;
    }

    bool enabledEntry(const tTJSVariant &entry, const char *context) {
        tTJSVariant value;
        if(!optionalProperty(entry, TJS_W("enabled"), value)) {
            return false;
        }
        if(value.Type() != tvtInteger && value.Type() != tvtReal) {
            invalidMetadata(std::string(context) + ".enabled must be numeric");
        }
        const double number = value.AsReal();
        if(!std::isfinite(number)) {
            invalidMetadata(std::string(context) + ".enabled must be finite");
        }
        return number != 0.0;
    }

    double readNumber(const tTJSVariant &object, const tjs_char *name,
                      const char *context) {
        const auto value = requiredProperty(object, name, context);
        if(value.Type() != tvtInteger && value.Type() != tvtReal) {
            invalidMetadata(std::string(context) + " must be numeric");
        }
        const double number = value.AsReal();
        if(!std::isfinite(number) ||
           std::abs(number) > kMaximumMetadataMagnitude) {
            invalidMetadata(std::string(context) +
                            " must be finite and bounded");
        }
        return number;
    }

    std::string readString(const tTJSVariant &object, const tjs_char *name,
                           const char *context) {
        const auto value = requiredProperty(object, name, context);
        if(value.Type() != tvtString) {
            invalidMetadata(std::string(context) + " must be a string");
        }
        const auto result = motion::detail::narrow(value);
        if(result.empty()) {
            invalidMetadata(std::string(context) + " must not be empty");
        }
        return result;
    }

    tjs_int boundedCount(const tTJSVariant &array, const char *context) {
        if(array.Type() != tvtObject || array.AsObjectNoAddRef() == nullptr) {
            invalidMetadata(std::string(context) + " must be an array");
        }
        const tjs_int count = getObjectCount(array);
        if(count < 0 || count > kMaximumControlEntries) {
            invalidMetadata(std::string(context) + " count is out of range");
        }
        return count;
    }

    tTJSVariant arrayItem(const tTJSVariant &array, tjs_int index,
                          const char *context) {
        tTJSVariant item;
        if(!getArrayItem(array, index, item) || item.Type() != tvtObject ||
           item.AsObjectNoAddRef() == nullptr) {
            invalidMetadata(std::string(context) + " contains non-object item");
        }
        return item;
    }

    std::array<double, 2> readNumberPair(const tTJSVariant &object,
                                         const tjs_char *name,
                                         const char *context) {
        const auto array = requiredProperty(object, name, context);
        if(boundedCount(array, context) < 2) {
            invalidMetadata(std::string(context) +
                            " must contain at least two numbers");
        }
        std::array<double, 2> result{};
        for(tjs_int index = 0; index < 2; ++index) {
            tTJSVariant value;
            if(!getArrayItem(array, index, value) ||
               (value.Type() != tvtInteger && value.Type() != tvtReal)) {
                invalidMetadata(std::string(context) +
                                " contains a non-numeric item");
            }
            result[static_cast<std::size_t>(index)] = value.AsReal();
            if(!std::isfinite(result[static_cast<std::size_t>(index)]) ||
               std::abs(result[static_cast<std::size_t>(index)]) >
                   kMaximumMetadataMagnitude) {
                invalidMetadata(std::string(context) +
                                " contains an unbounded value");
            }
        }
        return result;
    }

    motion::physics::Vec3 readVec3(const tTJSVariant &object,
                                   const char *context) {
        return motion::physics::Vec3{
            readNumber(object, TJS_W("x"), (std::string(context) + ".x").c_str()),
            readNumber(object, TJS_W("y"), (std::string(context) + ".y").c_str()),
            readNumber(object, TJS_W("z"), (std::string(context) + ".z").c_str()),
        };
    }

    std::array<motion::physics::Vec3, 2>
    readVec3Pair(const tTJSVariant &object, const tjs_char *name,
                 const char *context) {
        const auto array = requiredProperty(object, name, context);
        if(boundedCount(array, context) < 2) {
            invalidMetadata(std::string(context) +
                            " must contain at least two vectors");
        }
        return { readVec3(arrayItem(array, 0, context), context),
                 readVec3(arrayItem(array, 1, context), context) };
    }

    template <typename Callback>
    void forEachEnabledControl(const tTJSVariant &metadata,
                               const tjs_char *property,
                               const char *context, Callback callback) {
        tTJSVariant list;
        if(!optionalProperty(metadata, property, list)) {
            return;
        }
        const auto count = boundedCount(list, context);
        for(tjs_int index = 0; index < count; ++index) {
            const auto item = arrayItem(list, index, context);
            if(enabledEntry(item, context)) {
                callback(item, index);
            }
        }
    }

    std::unique_ptr<motion::physics::ControllerRuntimeState>
    buildControllerCandidate(const tTJSVariant &metadata,
                             std::uint64_t motionGeneration) {
        if(metadata.Type() != tvtObject ||
           metadata.AsObjectNoAddRef() == nullptr) {
            invalidMetadata("root must be an object");
        }

        auto candidate =
            std::make_unique<motion::physics::ControllerRuntimeState>();
        candidate->motionGeneration = motionGeneration;

        forEachEnabledControl(
            metadata, TJS_W("bustControl"), "bustControl",
            [&](const tTJSVariant &item, tjs_int) {
                motion::physics::BustConfig config;
                config.gravity = readNumber(
                    item, TJS_W("gravity"), "bustControl.gravity");
                config.spring = readNumber(
                    item, TJS_W("spring"), "bustControl.spring");
                config.friction = readNumber(
                    item, TJS_W("friction"), "bustControl.friction");
                if(config.spring < 0.0 || config.friction < 0.0 ||
                   config.friction > 1.0) {
                    invalidMetadata(
                        "bustControl spring/friction range is invalid");
                }
                config.scaleX = readNumber(
                    item, TJS_W("scale_x"), "bustControl.scale_x");
                config.scaleY = readNumber(
                    item, TJS_W("scale_y"), "bustControl.scale_y");
                config.label = readString(
                    item, TJS_W("label"), "bustControl.label");
                config.parameter = readString(
                    item, TJS_W("parameter"), "bustControl.parameter");
                config.baseLayer = readString(
                    item, TJS_W("baseLayer"), "bustControl.baseLayer");
                const auto param = requiredProperty(
                    item, TJS_W("param"), "bustControl.param");
                config.metadataOffset = readNumber(
                    param, TJS_W("ofs"), "bustControl.param.ofs");
                config.initialAnchor = readVec3(
                    requiredProperty(param, TJS_W("op"),
                                     "bustControl.param.op"),
                    "bustControl.param.op");
                config.initialPosition = readVec3(
                    requiredProperty(param, TJS_W("p"),
                                     "bustControl.param.p"),
                    "bustControl.param.p");
                config.initialVelocity = readVec3(
                    requiredProperty(param, TJS_W("pv"),
                                     "bustControl.param.pv"),
                    "bustControl.param.pv");
                config.hasInitialState = true;
                config.varLr = readString(
                    item, TJS_W("var_lr"), "bustControl.var_lr");
                config.varUd = readString(
                    item, TJS_W("var_ud"), "bustControl.var_ud");
                candidate->bust.emplace_back(std::move(config));
            });

        const auto parsePendList =
            [&](const tjs_char *property, const char *context,
                std::vector<motion::physics::PendControl> &destination) {
                forEachEnabledControl(
                    metadata, property, context,
                    [&](const tTJSVariant &item, tjs_int) {
                        motion::physics::PendConfig config;
                        config.gravity = readNumber(
                            item, TJS_W("gravity"), "pend.gravity");
                        config.frictionX = readNumber(
                            item, TJS_W("friction_x"), "pend.friction_x");
                        config.frictionY = readNumber(
                            item, TJS_W("friction_y"), "pend.friction_y");
                        config.backRate = readNumber(
                            item, TJS_W("b_rate"), "pend.b_rate");
                        config.velocityBound = readNumber(
                            item, TJS_W("v_bound"), "pend.v_bound");
                        if(config.frictionX < 0.0 ||
                           config.frictionX > 1.0 ||
                           config.frictionY < 0.0 ||
                           config.frictionY > 1.0 ||
                           config.backRate < 0.0 ||
                           config.velocityBound < 0.0) {
                            invalidMetadata(
                                "pend friction/response range is invalid");
                        }
                        const double segment = readNumber(
                            item, TJS_W("ud_eft"), "pend.ud_eft");
                        if(segment != 0.0 && segment != 1.0) {
                            invalidMetadata("pend.ud_eft must be 0 or 1");
                        }
                        config.verticalOutputSegment =
                            static_cast<int>(segment);
                        config.length = readNumberPair(
                            item, TJS_W("length"), "pend.length");
                        if(config.length[0] <= 0.0 ||
                           config.length[1] <= 0.0) {
                            invalidMetadata("pend.length values must be positive");
                        }
                        config.scaleX = readNumberPair(
                            item, TJS_W("scale_x"), "pend.scale_x");
                        config.scaleY = readNumberPair(
                            item, TJS_W("scale_y"), "pend.scale_y");
                        config.bendSpeed = readNumber(
                            item, TJS_W("bend_spd"), "pend.bend_spd");
                        config.bendVolume = readNumber(
                            item, TJS_W("bend_vol"), "pend.bend_vol");
                        config.label = readString(
                            item, TJS_W("label"), "pend.label");
                        config.parameter = readString(
                            item, TJS_W("parameter"), "pend.parameter");
                        config.baseLayer = readString(
                            item, TJS_W("baseLayer"), "pend.baseLayer");
                        const auto param = requiredProperty(
                            item, TJS_W("param"), "pend.param");
                        config.metadataOffset = readNumber(
                            param, TJS_W("ofs"), "pend.param.ofs");
                        config.initialAnchor = readVec3(
                            requiredProperty(param, TJS_W("op"),
                                             "pend.param.op"),
                            "pend.param.op");
                        config.initialRest = readVec3Pair(
                            param, TJS_W("bp"), "pend.param.bp");
                        config.initialPosition = readVec3Pair(
                            param, TJS_W("p"), "pend.param.p");
                        config.initialVelocity = readVec3Pair(
                            param, TJS_W("pv"), "pend.param.pv");
                        config.initialBendPhase = readNumber(
                            param, TJS_W("bendR"), "pend.param.bendR");
                        config.initialBendEnvelope = readNumber(
                            param, TJS_W("bendS"), "pend.param.bendS");
                        config.hasInitialState = true;
                        config.varLr = readString(
                            item, TJS_W("var_lr"), "pend.var_lr");
                        config.varLrm = readString(
                            item, TJS_W("var_lrm"), "pend.var_lrm");
                        config.varUd = readString(
                            item, TJS_W("var_ud"), "pend.var_ud");
                        destination.emplace_back(std::move(config));
                    });
            };
        parsePendList(TJS_W("hairControl"), "hairControl", candidate->hair);
        parsePendList(TJS_W("partsControl"), "partsControl",
                      candidate->parts);

        forEachEnabledControl(
            metadata, TJS_W("eyeControl"), "eyeControl",
            [&](const tTJSVariant &item, tjs_int) {
                motion::physics::EyeConfig config;
                config.beginFrame = readNumber(
                    item, TJS_W("beginFrame"), "eyeControl.beginFrame");
                config.endFrame = readNumber(
                    item, TJS_W("endFrame"), "eyeControl.endFrame");
                config.blinkIntervalMin = readNumber(
                    item, TJS_W("blinkIntervalMin"),
                    "eyeControl.blinkIntervalMin");
                config.blinkIntervalMax = readNumber(
                    item, TJS_W("blinkIntervalMax"),
                    "eyeControl.blinkIntervalMax");
                config.blinkFrameCount = readNumber(
                    item, TJS_W("blinkFrameCount"),
                    "eyeControl.blinkFrameCount");
                config.blinkEnabled = readBool(
                    item, TJS_W("blinkEnabled"),
                    "eyeControl.blinkEnabled");
                config.label = readString(
                    item, TJS_W("label"), "eyeControl.label");
                if(config.blinkIntervalMin < 0.0 ||
                   config.blinkIntervalMax < config.blinkIntervalMin ||
                   config.blinkFrameCount <= 0.0) {
                    invalidMetadata("eyeControl blink ranges are invalid");
                }
                candidate->eyes.emplace_back(std::move(config));
            });

        return candidate;
    }
} // namespace

namespace motion {

    void Player::initPhysics(tTJSVariant metadata) {
        const tTJSVariant &source =
            metadata.Type() == tvtVoid ? _metadata : metadata;
        try {
            auto candidate = buildControllerCandidate(
                source, _runtime ? _runtime->motionGeneration : 0);

            const bool hasPostCoreControls = !candidate->bust.empty() ||
                !candidate->hair.empty() || !candidate->parts.empty();
            if(hasPostCoreControls &&
               (!_runtime || _runtime->nodes.empty())) {
                invalidMetadata(
                    "physics controls require an active motion node tree");
            }

            // `baseLayer` is a required, non-empty metadata field, but it is
            // not required to be present in the root node deque at this
            // point.  NEKOPARA's physics labels (for example `center_bust`)
            // are resolved through child motion players after their clips
            // are materialized.  Rejecting the candidate here aborts the
            // entire affine-source layer before it can render.  The physics
            // consumer performs the recursive lookup when the first positive
            // dt reaches the post-Core phase and keeps the deterministic root
            // fallback for a still-unmaterialized child.

            const auto requireOutputLabel = [this](const std::string &label) {
                if(_variableValues.find(label) == _variableValues.end()) {
                    invalidMetadata("output does not bind an active motion "
                                    "variable: " + label);
                }
            };
            for(const auto &control : candidate->bust) {
                requireOutputLabel(control.config().varLr);
                requireOutputLabel(control.config().varUd);
            }
            for(const auto &control : candidate->hair) {
                requireOutputLabel(control.config().varLr);
                requireOutputLabel(control.config().varLrm);
                requireOutputLabel(control.config().varUd);
            }
            for(const auto &control : candidate->parts) {
                requireOutputLabel(control.config().varLr);
                requireOutputLabel(control.config().varLrm);
                requireOutputLabel(control.config().varUd);
            }
            for(const auto &control : candidate->eyes) {
                requireOutputLabel(control.config().label);
            }

            if(_controllerState && _runtime &&
               _controllerState->motionGeneration ==
                   _runtime->motionGeneration) {
                candidate->bustForce = _controllerState->bustForce;
                candidate->hairForce = _controllerState->hairForce;
                candidate->partsForce = _controllerState->partsForce;
                candidate->wind = _controllerState->wind;
            }

            _controllerState.swap(candidate);
            _progressTransactionOpen = false;
            _progressTransactionDt = 0.0;
            LOGGER->debug(
                "Player::initPhysics committed generation={} bust={} hair={} "
                "parts={} eye={}",
                _controllerState->motionGeneration,
                _controllerState->bust.size(), _controllerState->hair.size(),
                _controllerState->parts.size(), _controllerState->eyes.size());
        } catch(const std::exception &error) {
            LOGGER->error("Player::initPhysics transaction rejected: {}",
                          error.what());
            throw;
        }
    }

    void Player::startWind(double start, double goal, double speed,
                           double powerMin, double powerMax) {
        if(!std::isfinite(start) || !std::isfinite(goal) ||
           !std::isfinite(speed) || !std::isfinite(powerMin) ||
           !std::isfinite(powerMax) || powerMin < 0.0 ||
           powerMax < powerMin) {
            throw std::runtime_error(
                "motionplayer: startWind arguments are invalid");
        }
        if(!_controllerState || !_runtime ||
           _controllerState->motionGeneration != _runtime->motionGeneration) {
            _controllerState =
                std::make_unique<physics::ControllerRuntimeState>();
            _controllerState->motionGeneration =
                _runtime ? _runtime->motionGeneration : 0;
        }
        _controllerState->wind.start(start, goal, speed, powerMin, powerMax);
        _emoteDirty = true;
    }

    void Player::stopWind() {
        if(_controllerState) {
            _controllerState->wind.stop();
        }
        _emoteDirty = true;
    }

    void Player::setOuterForce(ttstr label, double x, double y,
                               double transition, double ease) {
        if(!std::isfinite(x) || !std::isfinite(y) ||
           !std::isfinite(transition) || !std::isfinite(ease) ||
           transition < 0.0) {
            throw std::runtime_error(
                "motionplayer: setOuterForce arguments must be finite");
        }
        if(!_controllerState || !_runtime ||
           _controllerState->motionGeneration != _runtime->motionGeneration) {
            _controllerState =
                std::make_unique<physics::ControllerRuntimeState>();
            _controllerState->motionGeneration =
                _runtime ? _runtime->motionGeneration : 0;
        }

        const auto key = lowerAscii(detail::narrow(label));
        physics::OuterForceAnimator *target = nullptr;
        if(key == "bust") {
            target = &_controllerState->bustForce;
        } else if(key == "hair" || key == "h") {
            target = &_controllerState->hairForce;
        } else if(key == "parts") {
            target = &_controllerState->partsForce;
        } else {
            return;
        }
        target->set(x, y, transition, ease);
        _emoteDirty = true;
    }

    tTJSVariant Player::getOuterForce(ttstr label) {
        if(!_controllerState || !_runtime ||
           _controllerState->motionGeneration != _runtime->motionGeneration) {
            return tTJSVariant();
        }
        const auto key = lowerAscii(detail::narrow(label));
        const physics::OuterForceAnimator *target = nullptr;
        if(key == "bust") {
            target = &_controllerState->bustForce;
        } else if(key == "hair" || key == "h") {
            target = &_controllerState->hairForce;
        } else if(key == "parts") {
            target = &_controllerState->partsForce;
        } else {
            return tTJSVariant();
        }
        const auto value = target->current();
        return detail::makeDictionary({ { "x", value.x }, { "y", value.y } });
    }

    void Player::beginProgressTransaction(double dt) {
        if(_progressTransactionOpen) {
            LOGGER->error(
                "MotionPlayer progress transaction reopened before Core "
                "evaluation completed");
            _progressTransactionOpen = false;
        }
        _progressTransactionOpen = true;
        _progressTransactionDt = std::max(0.0, dt);

        if(!_controllerState || !_runtime ||
           _controllerState->motionGeneration != _runtime->motionGeneration) {
            return;
        }

        for(const auto &[label, value] :
            _controllerState->stagedPhysicsOutputs) {
            writeEvalResultValueLike_0x6C4668(label, value);
        }

        const auto randomSource = [this]() { return random(); };
        for(auto &eye : _controllerState->eyes) {
            const auto &config = eye.config();
            double currentValue = config.beginFrame;
            if(const auto it = _variableValues.find(config.label);
               it != _variableValues.end()) {
                currentValue = it->second;
            }
            if(const auto output =
                   eye.step(_progressTransactionDt, currentValue,
                            randomSource)) {
                writeEvalResultValueLike_0x6C4668(config.label, *output);
            }
        }
        _controllerState->wind.step(_progressTransactionDt, randomSource);
    }

    void Player::finishProgressTransaction() {
        if(!_progressTransactionOpen) {
            return;
        }
        const double dt = _progressTransactionDt;
        _progressTransactionOpen = false;
        _progressTransactionDt = 0.0;

        if(dt <= 0.0 || _physicsDisabled || !_controllerState || !_runtime ||
           _controllerState->motionGeneration != _runtime->motionGeneration ||
           _runtime->nodes.empty()) {
            return;
        }

        _controllerState->bustForce.step(dt);
        _controllerState->hairForce.step(dt);
        _controllerState->partsForce.step(dt);

        const auto inputForLayer = [this](const std::string &baseLayer) {
            std::function<const detail::MotionNode *(const Player *)> find =
                [&](const Player *player) -> const detail::MotionNode * {
                    if(!player || !player->_runtime) {
                        return nullptr;
                    }
                    if(const auto it = player->_runtime->nodeLabelMap.find(
                           baseLayer);
                       it != player->_runtime->nodeLabelMap.end()) {
                        const auto index = it->second;
                        if(index >= 0 &&
                           index < static_cast<int>(player->_runtime->nodes.size())) {
                            return &player->_runtime->nodes[
                                static_cast<size_t>(index)];
                        }
                    }
                    for(const auto &node : player->_runtime->nodes) {
                        if(node.nodeType == 3) {
                            if(const auto *found = find(node.getChildPlayer())) {
                                return found;
                            }
                        } else if(node.nodeType == 4) {
                            const int count = node.getParticleCount();
                            for(int index = 0; index < count; ++index) {
                                if(const auto *found =
                                       find(node.getParticleChild(index))) {
                                    return found;
                                }
                            }
                        }
                    }
                    return nullptr;
                };
            const auto *node = find(this);
            if(!node) {
                node = &_runtime->nodes.front();
            }
            return physics::Vec2{ node->accumulated.posX,
                                  node->accumulated.posY };
        };
        const double angleRadians = _rotateAngle * kPi / 180.0;

        const auto writeBust = [&](physics::BustControl &control) {
            const auto force = _controllerState->bustForce.current();
            const auto &config = control.config();
            const auto output = control.stepFrame(
                inputForLayer(config.baseLayer), force, dt, _bustScale,
                angleRadians);
            _controllerState->stagedPhysicsOutputs[config.varLr] = output[0];
            _controllerState->stagedPhysicsOutputs[config.varUd] = output[1];
        };
        for(auto &control : _controllerState->bust) {
            writeBust(control);
        }

        const auto writePend =
            [&](physics::PendControl &control,
                const physics::OuterForceAnimator &animator, double scale) {
                const auto &config = control.config();
                const auto output = control.stepFrame(
                    inputForLayer(config.baseLayer), animator.current(), dt,
                    scale, angleRadians,
                    _controllerState->wind.active()
                        ? &_controllerState->wind
                        : nullptr);
                _controllerState->stagedPhysicsOutputs[config.varLr] =
                    output[0];
                _controllerState->stagedPhysicsOutputs[config.varLrm] =
                    output[1];
                _controllerState->stagedPhysicsOutputs[config.varUd] =
                    output[2];
                static const bool pendDiag = [] {
                    const char *env = std::getenv("KRKR_EMOTE_GEOM_DUMP");
                    return env && env[0] != '\0' && env[0] != '0';
                }();
                if(pendDiag) {
                    if(auto logger = spdlog::get("plugin")) {
                        logger->warn(
                            "emote.pend var={} lr={:.4f} lrm={:.4f} "
                            "ud={:.4f} dt={:.4f}",
                            config.varLr, output[0], output[1], output[2],
                            dt);
                    }
                }
            };
        for(auto &control : _controllerState->hair) {
            writePend(control, _controllerState->hairForce, _hairScale);
        }
        for(auto &control : _controllerState->parts) {
            writePend(control, _controllerState->partsForce, _partsScale);
        }
        _emoteDirty = true;
    }

} // namespace motion
