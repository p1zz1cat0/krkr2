#define NCB_MODULE_NAME TJS_W("wfBasicEffect.dll")

#include "ncbind.hpp"
#include "common/PluginSafety.h"
#include "WaveIntf.h"
#include "WaveLoopManager.h"
#include "FreeVerb.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <limits>
#include <mutex>
#include <vector>

namespace basicEffect {
constexpr unsigned BlockFrames = 1024;
constexpr unsigned MaxDelayMs = 10000;
constexpr unsigned MaxExtendMs = 60000;

void Require(bool valid) {
    if(!valid) TVPThrowExceptionMessage(TJS_W("wfBasicEffect: invalid parameter."));
}
double Real(const tTJSVariant &value, double minimum, double maximum) {
    const auto result = pluginSafety::readFiniteReal(value, minimum, maximum);
    Require(static_cast<bool>(result));
    return result.value;
}
unsigned Milliseconds(const tTJSVariant &value, unsigned maximum) {
    const auto result = pluginSafety::readBoundedInteger(value, 0, maximum);
    Require(static_cast<bool>(result));
    return static_cast<unsigned>(result.value);
}

class WaveEffect : public iTVPBasicWaveFilter, public tTVPSampleAndLabelSource {
public:
    virtual ~WaveEffect() = default;
    tjs_int64 GetInterface() const {
        return reinterpret_cast<tjs_int64>(static_cast<const iTVPBasicWaveFilter *>(this));
    }
    tTVPSampleAndLabelSource *Recreate(tTVPSampleAndLabelSource *source) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(source_) TVPThrowExceptionMessage(TJS_W("Cannot connect multiple wave sound buffer at once."));
        Require(source != nullptr);
        const auto input = source->GetFormat();
        Require(input.SamplesPerSec >= 1000 && input.SamplesPerSec <= 384000 &&
                input.Channels >= 1 && input.Channels <= 2 &&
                ((!input.IsFloat && input.BitsPerSample == 16 && input.BytesPerSample == 2) ||
                 (input.IsFloat && input.BitsPerSample == 32 && input.BytesPerSample == 4)));
        const auto tailFrames = static_cast<tjs_uint64>(input.SamplesPerSec) * extendMs_ / 1000;
        Require(input.TotalSamples <= std::numeric_limits<tjs_uint64>::max() - tailFrames);
        Require(input.TotalSamples + tailFrames <= std::numeric_limits<tjs_uint64>::max() / 1000);
        Prepare(input.SamplesPerSec, input.Channels);
        input_ = output_ = input;
        output_.IsFloat = true;
        output_.BitsPerSample = 32;
        output_.BytesPerSample = 4;
        output_.Channels = OutputChannels(input.Channels);
        if(output_.Channels != input.Channels) output_.SpeakerConfig = 0;
        tailFrames_ = tailFrames;
        if(output_.TotalSamples) {
            output_.TotalSamples += tailFrames_;
            output_.TotalTime = output_.TotalSamples * 1000 / input.SamplesPerSec;
        }
        eof_ = false;
        remaining_ = tailFrames_;
        tailPosition_ = 0;
        source_ = source;
        return this;
    }
    void Clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = nullptr;
        remaining_ = 0;
        ReleaseEffect();
    }
    void Reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetEffect();
        eof_ = false;
        remaining_ = tailFrames_;
        tailPosition_ = 0;
    }
    void Update() override {}
    const tTVPWaveFormat &GetFormat() const override { return output_; }
    void Decode(void *dest, tjs_uint samples, tjs_uint &written,
                tTVPWaveSegmentQueue &segments) override {
        std::lock_guard<std::mutex> lock(mutex_);
        written = 0;
        if(!source_) return;
        auto *output = static_cast<float *>(dest);
        while(written < samples) {
            const unsigned request = std::min(samples - written, BlockFrames);
            unsigned got = 0;
            tTVPWaveSegmentQueue chunk;
            if(!eof_) {
                if(input_.IsFloat) source_->Decode(inputBuffer_.data(), request, got, chunk);
                else {
                    source_->Decode(pcm_.data(), request, got, chunk);
                    Require(got <= request);
                    TVPConvertPCMToFloat(inputBuffer_.data(), pcm_.data(), input_, got);
                }
                Require(got <= request);
                if(!chunk.GetSegments().empty()) {
                    const auto &last = chunk.GetSegments().back();
                    tailPosition_ = last.Start + last.Length;
                }
                segments.Enqueue(chunk);
                eof_ = got < request;
            }
            const auto tail = eof_ ? static_cast<unsigned>(std::min<tjs_uint64>(request - got, remaining_)) : 0u;
            remaining_ -= tail;
            const unsigned frames = got + tail;
            if(!frames) break;
            if(tail) {
                std::fill_n(inputBuffer_.data() + got * input_.Channels, tail * input_.Channels, 0.0f);
                // Hold the source position while the effect renders its tail.
                segments.Enqueue(tTVPWaveSegment(tailPosition_, 0, tail));
            }
            for(unsigned f = 0; f < frames; ++f) {
                float left = inputBuffer_[f * input_.Channels];
                float right = input_.Channels == 2 ? inputBuffer_[f * 2 + 1] : left;
                if(!std::isfinite(left)) left = 0;
                if(!std::isfinite(right)) right = 0;
                Process(left, right);
                const auto offset = (static_cast<size_t>(written) + f) * output_.Channels;
                output[offset] = std::isfinite(left) ? left : 0;
                if(output_.Channels == 2) output[offset + 1] = std::isfinite(right) ? right : 0;
            }
            written += frames;
        }
    }
protected:
    virtual unsigned OutputChannels(unsigned channels) const { return channels; }
    virtual void Prepare(unsigned rate, unsigned channels) = 0;
    virtual void Process(float &left, float &right) = 0;
    virtual void ResetEffect() = 0;
    virtual void ReleaseEffect() = 0;
    mutable std::mutex mutex_;
    unsigned extendMs_ = 1000;
private:
    tTVPSampleAndLabelSource *source_ = nullptr;
    tTVPWaveFormat input_{}, output_{};
    bool eof_ = false;
    tjs_uint64 tailFrames_ = 0, remaining_ = 0;
    tjs_int64 tailPosition_ = 0;
    std::array<float, BlockFrames * 2> inputBuffer_{};
    std::array<tjs_int16, BlockFrames * 2> pcm_{};
};

class DelayEffect : public WaveEffect {
public:
    static tjs_error Create(DelayEffect **result, tjs_int count, tTJSVariant **values, iTJSDispatch2 *) {
        auto instance = std::make_unique<DelayEffect>();
        instance->Init(nullptr, count, values, instance.get());
        *result = instance.release();
        return TJS_S_OK;
    }
    static tjs_error Init(tTJSVariant *result, tjs_int count, tTJSVariant **values, DelayEffect *self) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if(count >= 3) {
            const auto delay = Milliseconds(*values[0], MaxDelayMs);
            const auto feedback = Real(*values[1], -1, 1);
            const auto wet = Real(*values[2], 0, 1);
            const auto extend = count >= 4 ? Milliseconds(*values[3], MaxExtendMs) : self->extendMs_;
            self->delayMs_ = delay;
            self->feedback_ = static_cast<float>(feedback);
            self->wet_ = static_cast<float>(wet);
            self->extendMs_ = extend;
        }
        if(result) result->Clear();
        return TJS_S_OK;
    }
protected:
    void Prepare(unsigned rate, unsigned channels) override {
        const size_t frames = static_cast<tjs_uint64>(rate) * delayMs_ / 1000;
        const auto elements = pluginSafety::checkedMultiply(frames, channels);
        Require(static_cast<bool>(elements));
        const auto bytes = pluginSafety::checkedElementBytes(elements.value, sizeof(float));
        pluginSafety::OperationBudget budget;
        Require(static_cast<bool>(bytes));
        Require(static_cast<bool>(pluginSafety::validateAllocationBudget(bytes.value, budget)));
        std::vector<float> candidate(elements.value, 0);
        ring_.swap(candidate);
        frames_ = frames;
        channels_ = channels;
        position_ = 0;
    }
    void Process(float &left, float &right) override {
        if(!frames_) return;
        float *values[] = {&left, &right};
        for(unsigned c = 0; c < channels_; ++c) {
            auto &stored = ring_[position_ * channels_ + c];
            const float delayed = stored;
            stored = (*values[c] + delayed) * feedback_;
            *values[c] += delayed * wet_;
        }
        if(++position_ == frames_) position_ = 0;
    }
    void ResetEffect() override { std::fill(ring_.begin(), ring_.end(), 0); position_ = 0; }
    void ReleaseEffect() override { ring_.clear(); frames_ = position_ = 0; }
private:
    unsigned delayMs_ = 0, channels_ = 0;
    float feedback_ = 0, wet_ = 0;
    size_t frames_ = 0, position_ = 0;
    std::vector<float> ring_;
};

class StkFreeVerb : public WaveEffect {
public:
    static tjs_error Create(StkFreeVerb **result, tjs_int count, tTJSVariant **values, iTJSDispatch2 *) {
        auto instance = std::make_unique<StkFreeVerb>();
        if(count > 0) instance->SetMix(Real(*values[0], -1e6, 1e6));
        if(count > 1) instance->SetRoom(Real(*values[1], 0, 1));
        if(count > 2) instance->SetDamping(Real(*values[2], 0, 1));
        if(count > 3) instance->SetWidth(Real(*values[3], 0, 1));
        if(count > 4) instance->SetMode(static_cast<bool>(*values[4]));
        if(count > 5) instance->SetExtend(Milliseconds(*values[5], MaxExtendMs));
        *result = instance.release();
        return TJS_S_OK;
    }
    double GetMix() const { std::lock_guard<std::mutex> lock(mutex_); return mix_; }
    double GetRoom() const { std::lock_guard<std::mutex> lock(mutex_); return room_; }
    double GetDamping() const { std::lock_guard<std::mutex> lock(mutex_); return damping_; }
    double GetWidth() const { std::lock_guard<std::mutex> lock(mutex_); return width_; }
    bool GetMode() const { std::lock_guard<std::mutex> lock(mutex_); return mode_; }
    tjs_int GetExtend() const { std::lock_guard<std::mutex> lock(mutex_); return extendMs_; }
    void SetMix(double value) {
        Require(std::isfinite(value));
        std::lock_guard<std::mutex> lock(mutex_);
        mix_ = value;
        if(verb_) verb_->setEffectMix(std::clamp(value, 0.0, 1.0));
    }
    void SetRoom(double value) {
        Require(std::isfinite(value) && value >= 0 && value <= 1);
        std::lock_guard<std::mutex> lock(mutex_);
        room_ = value;
        if(verb_) verb_->setRoomSize(value);
    }
    void SetDamping(double value) {
        Require(std::isfinite(value) && value >= 0 && value <= 1);
        std::lock_guard<std::mutex> lock(mutex_);
        damping_ = value;
        if(verb_) verb_->setDamping(value);
    }
    void SetWidth(double value) {
        Require(std::isfinite(value) && value >= 0 && value <= 1);
        std::lock_guard<std::mutex> lock(mutex_);
        width_ = value;
        if(verb_) verb_->setWidth(value);
    }
    void SetMode(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = value;
        if(verb_) verb_->setMode(value);
    }
    void SetExtend(tjs_int64 value) {
        Require(value >= 0 && value <= MaxExtendMs);
        std::lock_guard<std::mutex> lock(mutex_);
        extendMs_ = value;
    }
protected:
    unsigned OutputChannels(unsigned) const override { return 2; }
    void Prepare(unsigned rate, unsigned) override {
        auto candidate = std::make_unique<stk::FreeVerb>(rate);
        candidate->setEffectMix(std::clamp(mix_, 0.0, 1.0));
        candidate->setRoomSize(room_);
        candidate->setDamping(damping_);
        candidate->setWidth(width_);
        candidate->setMode(mode_);
        candidate->clear();
        verb_.swap(candidate);
    }
    void Process(float &left, float &right) override {
        left = static_cast<float>(verb_->tick(left, right));
        right = static_cast<float>(verb_->lastOut(1));
    }
    void ResetEffect() override { if(verb_) verb_->clear(); }
    void ReleaseEffect() override { verb_.reset(); }
private:
    double mix_ = 0.75, room_ = 0.75, damping_ = 0.25, width_ = 1;
    bool mode_ = false;
    std::unique_ptr<stk::FreeVerb> verb_;
};

void EnsureWaveSoundBuffer() {
    tTJSVariant waveClass;
    TVPExecuteExpression(TJS_W("WaveSoundBuffer"), &waveClass);
    if(waveClass.Type() != tvtObject) TVPThrowExceptionMessage(TJS_W("wfBasicEffect requires WaveSoundBuffer."));
}
} // namespace basicEffect

using basicEffect::DelayEffect;
using basicEffect::StkFreeVerb;
using basicEffect::EnsureWaveSoundBuffer;
struct BasicEffectWaveOwner {};
NCB_PRE_REGIST_CALLBACK(EnsureWaveSoundBuffer);
NCB_REGISTER_SUBCLASS(DelayEffect) {
    Factory(&Class::Create);
    Property(TJS_W("interface"), &Class::GetInterface, (int)0);
    RawCallback(TJS_W("init"), &Class::Init, 0);
}
NCB_REGISTER_SUBCLASS(StkFreeVerb) {
    Factory(&Class::Create);
    Property(TJS_W("interface"), &Class::GetInterface, (int)0);
    Property(TJS_W("effectMix"), &Class::GetMix, &Class::SetMix);
    Property(TJS_W("roomSize"), &Class::GetRoom, &Class::SetRoom);
    Property(TJS_W("damping"), &Class::GetDamping, &Class::SetDamping);
    Property(TJS_W("width"), &Class::GetWidth, &Class::SetWidth);
    Property(TJS_W("mode"), &Class::GetMode, &Class::SetMode);
    Property(TJS_W("extend"), &Class::GetExtend, &Class::SetExtend);
}
NCB_ATTACH_CLASS(BasicEffectWaveOwner, WaveSoundBuffer) {
    NCB_SUBCLASS(DelayEffect, DelayEffect);
    NCB_SUBCLASS(StkFreeVerb, StkFreeVerb);
}
extern "C" void TVPWfBasicEffectPluginAnchor() {}
