#include <catch2/catch_test_macros.hpp>
#include "wfTypicalDSP/wfTypicalDSP.cpp"

namespace {
class SineSource : public tTVPSampleAndLabelSource {
public:
    tTVPWaveFormat format{};
    unsigned position = 0;
    unsigned length = 48000;
    double frequency = 1080;
    SineSource(bool pcm = false, unsigned channels = 1, unsigned rate = 48000) {
        format.SamplesPerSec = rate;
        format.Channels = channels;
        format.IsFloat = !pcm;
        format.BitsPerSample = pcm ? 16 : 32;
        format.BytesPerSample = pcm ? 2 : 4;
    }
    const tTVPWaveFormat &GetFormat() const override { return format; }
    void Decode(void *dest, tjs_uint samples, tjs_uint &written,
                tTVPWaveSegmentQueue &segments) override {
        written = std::min(samples, length - position);
        for(unsigned f = 0; f < written; ++f) {
            const float value = 0.25f * std::sin(2 * Dsp::doublePi * frequency * (position + f) / format.SamplesPerSec);
            for(unsigned c = 0; c < format.Channels; ++c) {
                const auto index = f * format.Channels + c;
                const float sample = c == 0 ? value : 0;
                if(format.IsFloat) static_cast<float *>(dest)[index] = sample;
                else static_cast<tjs_int16 *>(dest)[index] = static_cast<tjs_int16>(sample * 32768);
            }
        }
        if(written) {
            segments.Enqueue(tTVPWaveLabel(position, TJS_W("chunk"), 0));
            segments.Enqueue(tTVPWaveSegment(position, written));
        }
        position += written;
    }
};

void Set(typicalDSP::WaveDSPFilter &filter, double cutoff, double width = 0, int order = 2) {
    tTJSVariant rate, n(order), frequency(cutoff), bandwidth(width), unused;
    tTJSVariant *values[] = {&rate, &n, &frequency, &bandwidth, &unused, &unused};
    if(width == 0) values[3] = &unused;
    REQUIRE(filter.ApplyParams(6, values) == TJS_S_OK);
}

double Gain(bool band, double frequency, bool pcm = false, unsigned rate = 48000) {
    typicalDSP::WaveDSPFilter filter(band ? TJS_W("BandPass") : TJS_W("LowPass"), TJS_W("Butterworth"));
    Set(filter, band ? 1300 : 1080, band ? 1800 : 0);
    SineSource source(pcm, 2, rate);
    source.frequency = frequency;
    filter.Recreate(&source);
    REQUIRE(filter.GetFormat().IsFloat);
    REQUIRE(filter.GetFormat().SamplesPerSec == rate);
    std::vector<float> output(source.length * 2 + 2, 9);
    unsigned written = 0;
    tTVPWaveSegmentQueue segments;
    filter.Decode(output.data(), source.length + 1, written, segments);
    REQUIRE(written == source.length);
    REQUIRE(output[source.length * 2] == 9);
    REQUIRE(segments.GetFilteredLength() == written);
    REQUIRE(segments.GetLabels().at(1).Offset == 1024);
    double energy = 0;
    for(unsigned f = 4096; f < written; ++f) {
        REQUIRE(std::isfinite(output[f * 2]));
        REQUIRE(std::abs(output[f * 2 + 1]) < 1e-7f);
        energy += output[f * 2] * output[f * 2];
    }
    return std::sqrt(energy / (written - 4096)) / (0.25 / std::sqrt(2.0));
}
}

TEST_CASE("wfTypicalDSP Butterworth processes float and PCM16 with source sample rate", "[plugin][audio]") {
    for(bool pcm : {false, true}) {
        for(unsigned rate : {44100u, 48000u}) {
            REQUIRE(Gain(false, 1080, pcm, rate) > 0.69);
            REQUIRE(Gain(false, 1080, pcm, rate) < 0.72);
            REQUIRE(Gain(false, 4000, pcm, rate) < 0.08);
            REQUIRE(Gain(true, 1300, pcm, rate) > 0.95);
            REQUIRE(Gain(true, 100, pcm, rate) < 0.1);
            REQUIRE(Gain(true, 8000, pcm, rate) < 0.05);
        }
    }
}

TEST_CASE("wfTypicalDSP reset, detach, validation and chunk continuity", "[plugin][audio]") {
    typicalDSP::WaveDSPFilter filter(TJS_W("LowPass"), TJS_W("Butterworth"));
    Set(filter, 1080);
    SineSource source;
    filter.Recreate(&source);
    REQUIRE_THROWS(filter.Recreate(&source));
    std::array<float, 2048> one{}, two{};
    tTVPWaveSegmentQueue segments;
    unsigned written = 0;
    filter.Decode(one.data(), 2048, written, segments);
    REQUIRE(written == 2048);
    source.position = 0;
    filter.Reset();
    segments.Clear();
    filter.Decode(two.data(), 700, written, segments);
    REQUIRE(written == 700);
    filter.Decode(two.data() + 700, 1348, written, segments);
    REQUIRE(written == 1348);
    REQUIRE(one == two);
    filter.Clear();
    filter.Decode(two.data(), 10, written, segments);
    REQUIRE(written == 0);
    REQUIRE(filter.Recreate(&source) == &filter);
    filter.Clear();
    Dsp::Params params{};
    params[0] = 48000; params[1] = 2; params[2] = 1080;
    REQUIRE(typicalDSP::ValidParams(typicalDSP::Kind::LowPass, params));
    params[1] = 51;
    REQUIRE_FALSE(typicalDSP::ValidParams(typicalDSP::Kind::LowPass, params));
    params[1] = 1.5;
    REQUIRE_FALSE(typicalDSP::ValidParams(typicalDSP::Kind::LowPass, params));
    params[1] = 2; params[2] = std::nan("");
    REQUIRE_FALSE(typicalDSP::ValidParams(typicalDSP::Kind::LowPass, params));
}

TEST_CASE("wfTypicalDSP defers automatic sample-rate validation until connection", "[plugin][audio]") {
    typicalDSP::WaveDSPFilter filter(TJS_W("LowPass"), TJS_W("Butterworth"));
    Set(filter, 23000);
    SineSource lowRate(false, 1, 44100), highRate(false, 1, 96000);
    REQUIRE_THROWS(filter.Recreate(&lowRate));
    REQUIRE(filter.Recreate(&highRate) == &filter);
    filter.Clear();
    lowRate.format.BytesPerSample = 3;
    REQUIRE_THROWS(filter.Recreate(&lowRate));
    REQUIRE(filter.Recreate(&highRate) == &filter);
}
