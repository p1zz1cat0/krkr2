#define NCB_MODULE_NAME TJS_W("wfTypicalDSP.dll")

#include "ncbind.hpp"
#include "common/PluginSafety.h"
#include "WaveIntf.h"
#include "WaveLoopManager.h"
#include "DspFilters/Butterworth.h"
#include "DspFilters/Filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

namespace typicalDSP {
constexpr unsigned BlockFrames = 1024;
constexpr unsigned MaxChannels = 8;

enum class Kind { LowPass, BandPass };

std::unique_ptr<Dsp::Filter> MakeFilter(Kind kind) {
    if(kind == Kind::LowPass)
        return std::make_unique<Dsp::FilterDesign<Dsp::Butterworth::Design::LowPass<50>, 1>>();
    return std::make_unique<Dsp::FilterDesign<Dsp::Butterworth::Design::BandPass<50>, 1>>();
}

bool ValidParams(Kind kind, const Dsp::Params &p) {
    if(!std::isfinite(p[0]) || p[0] < 1000 || p[0] > 384000 ||
       !std::isfinite(p[1]) || p[1] < 1 || p[1] > 50 || std::floor(p[1]) != p[1] ||
       !std::isfinite(p[2]) || p[2] <= 0 || p[2] >= p[0] / 2)
        return false;
    return kind != Kind::BandPass ||
        (std::isfinite(p[3]) && p[3] > 0 &&
         p[2] - p[3] / 2 > 0 && p[2] + p[3] / 2 < p[0] / 2);
}

class WaveDSPFilter : public iTVPBasicWaveFilter, public tTVPSampleAndLabelSource {
public:
    WaveDSPFilter(const ttstr &type, const ttstr &design) {
        if(design != TJS_W("Butterworth") ||
           (type != TJS_W("LowPass") && type != TJS_W("BandPass")))
            TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: unsupported type/design."));
        kind_ = type == TJS_W("LowPass") ? Kind::LowPass : Kind::BandPass;
        params_ = MakeFilter(kind_)->getDefaultParams();
    }

    tjs_int64 GetInterface() const {
        return reinterpret_cast<tjs_int64>(static_cast<const iTVPBasicWaveFilter *>(this));
    }

    static tjs_error SetParams(tTJSVariant *, tjs_int count,
                               tTJSVariant **values, iTJSDispatch2 *objthis) {
        auto *self = ncbInstanceAdaptor<WaveDSPFilter>::GetNativeInstance(objthis);
        if(!self) return TJS_E_FAIL;
        return self->ApplyParams(count, values);
    }

    tjs_error ApplyParams(tjs_int count, tTJSVariant **values) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto candidate = params_;
        bool autoRate = autoRate_;
        const int parameterCount = kind_ == Kind::LowPass ? 3 : 4;
        for(int i = 0; i < count; ++i) {
            if(values[i]->Type() == tvtVoid) {
                if(i == 0) autoRate = true;
                continue;
            }
            if(i >= parameterCount) return TJS_E_BADPARAMCOUNT;
            auto value = pluginSafety::readFiniteReal(*values[i], 0, 384000);
            if(!value)
                TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: invalid numeric parameter."));
            candidate[i] = value.value;
            if(i == 0) autoRate = false;
        }
        candidate[0] = autoRate ? (source_ ? input_.SamplesPerSec : 44100) : candidate[0];
        auto validation = candidate;
        if(autoRate && !source_) validation[0] = 384000;
        if(!ValidParams(kind_, validation))
            TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: invalid filter parameters."));
        auto filters = BuildFilters(candidate, filters_.size());
        params_ = candidate;
        autoRate_ = autoRate;
        filters_.swap(filters);
        return TJS_S_OK;
    }

    tTVPSampleAndLabelSource *Recreate(tTVPSampleAndLabelSource *source) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(source_)
            TVPThrowExceptionMessage(TJS_W("Cannot connect multiple wave sound buffer at once."));
        if(!source)
            TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: missing source."));
        auto format = source->GetFormat();
        if(format.SamplesPerSec < 1000 || format.SamplesPerSec > 384000 ||
           format.Channels < 1 || format.Channels > MaxChannels ||
           !((!format.IsFloat && format.BitsPerSample == 16 && format.BytesPerSample == 2) ||
             (format.IsFloat && format.BitsPerSample == 32 && format.BytesPerSample == 4)))
            TVPThrowExceptionMessage(TJS_W("HiRes format not supported."));
        auto candidate = params_;
        if(autoRate_) candidate[0] = format.SamplesPerSec;
        if(!ValidParams(kind_, candidate))
            TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: parameters incompatible with source sample rate."));
        auto filters = BuildFilters(candidate, format.Channels);
        input_ = output_ = format;
        output_.IsFloat = true;
        output_.BitsPerSample = 32;
        output_.BytesPerSample = 4;
        params_ = candidate;
        filters_.swap(filters);
        source_ = source;
        return this;
    }

    void Clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = nullptr;
        filters_.clear();
    }
    void Reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto &filter : filters_) filter->reset();
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
            const tjs_uint request = std::min(samples - written, BlockFrames);
            tjs_uint got = 0;
            tTVPWaveSegmentQueue chunkSegments;
            if(input_.IsFloat) {
                source_->Decode(interleaved_.data(), request, got, chunkSegments);
            } else {
                source_->Decode(pcm_.data(), request, got, chunkSegments);
                if(got > request)
                    TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: invalid decoder sample count."));
                TVPConvertPCMToFloat(interleaved_.data(), pcm_.data(), input_, got);
            }
            if(got > request)
                TVPThrowExceptionMessage(TJS_W("wfTypicalDSP: invalid decoder sample count."));
            for(unsigned c = 0; c < input_.Channels; ++c) {
                for(unsigned f = 0; f < got; ++f) {
                    const float value = interleaved_[f * input_.Channels + c];
                    channel_[f] = std::isfinite(value) ? value : 0;
                }
                float *channel = channel_.data();
                filters_[c]->process(static_cast<int>(got), &channel);
                for(unsigned f = 0; f < got; ++f)
                    output[(static_cast<size_t>(written) + f) * input_.Channels + c] = channel_[f];
            }
            segments.Enqueue(chunkSegments);
            written += got;
            if(got < request) break;
        }
    }

private:
    std::vector<std::unique_ptr<Dsp::Filter>> BuildFilters(const Dsp::Params &params, size_t channels) {
        std::vector<std::unique_ptr<Dsp::Filter>> result;
        for(size_t c = 0; c < channels; ++c) {
            auto filter = MakeFilter(kind_);
            filter->setParams(params);
            filter->reset();
            result.push_back(std::move(filter));
        }
        return result;
    }

    std::mutex mutex_;
    Kind kind_;
    Dsp::Params params_;
    bool autoRate_ = true;
    tTVPSampleAndLabelSource *source_ = nullptr;
    tTVPWaveFormat input_{};
    tTVPWaveFormat output_{};
    std::vector<std::unique_ptr<Dsp::Filter>> filters_;
    std::array<tjs_int16, BlockFrames * MaxChannels> pcm_{};
    std::array<float, BlockFrames * MaxChannels> interleaved_{};
    std::array<float, BlockFrames> channel_{};
};

void EnsureWaveSoundBuffer() {
    tTJSVariant waveClass;
    TVPExecuteExpression(TJS_W("WaveSoundBuffer"), &waveClass);
    if(waveClass.Type() != tvtObject)
        TVPThrowExceptionMessage(TJS_W("wfTypicalDSP requires WaveSoundBuffer."));
}
} // namespace typicalDSP

using typicalDSP::WaveDSPFilter;
using typicalDSP::EnsureWaveSoundBuffer;
struct TypicalDSPWaveOwner {};

NCB_PRE_REGIST_CALLBACK(EnsureWaveSoundBuffer);
NCB_REGISTER_SUBCLASS(WaveDSPFilter) {
    NCB_CONSTRUCTOR((ttstr, ttstr));
    Property(TJS_W("interface"), &Class::GetInterface, (int)0);
    RawCallback(TJS_W("setParams"), &Class::SetParams, 0);
}
NCB_ATTACH_CLASS(TypicalDSPWaveOwner, WaveSoundBuffer) {
    NCB_SUBCLASS(WaveDSPFilter, WaveDSPFilter);
}
extern "C" void TVPWfTypicalDSPPluginAnchor() {}
