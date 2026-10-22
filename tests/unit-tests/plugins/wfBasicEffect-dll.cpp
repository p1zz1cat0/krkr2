#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "wfBasicEffect/wfBasicEffect.cpp"

namespace {
class ImpulseSource : public tTVPSampleAndLabelSource {
public:
    tTVPWaveFormat format{};
    unsigned position = 0, length = 8;
    ImpulseSource(unsigned rate = 48000, unsigned channels = 1, bool pcm = false) {
        format.SamplesPerSec = rate;
        format.Channels = channels;
        format.IsFloat = !pcm;
        format.BitsPerSample = pcm ? 16 : 32;
        format.BytesPerSample = pcm ? 2 : 4;
        format.TotalSamples = length;
    }
    const tTVPWaveFormat &GetFormat() const override { return format; }
    void Decode(void *dest, unsigned count, unsigned &written, tTVPWaveSegmentQueue &queue) override {
        written = std::min(count, length - position);
        for(unsigned f = 0; f < written; ++f)
            for(unsigned c = 0; c < format.Channels; ++c) {
                const float value = position + f == 0 && c == 0 ? 0.5f : 0;
                const auto offset = f * format.Channels + c;
                if(format.IsFloat) static_cast<float *>(dest)[offset] = value;
                else static_cast<tjs_int16 *>(dest)[offset] = static_cast<tjs_int16>(value * 32768);
            }
        if(written) queue.Enqueue(tTVPWaveSegment(position, written));
        position += written;
    }
};
void Init(basicEffect::DelayEffect &filter, int delay = 1, double feedback = 0.5, double wet = 1, int extend = 10) {
    tTJSVariant a(delay), b(feedback), c(wet), d(extend);
    tTJSVariant *values[] = {&a, &b, &c, &d};
    basicEffect::DelayEffect::Init(nullptr, 4, values, &filter);
}
std::vector<float> Render(basicEffect::WaveEffect &filter, unsigned chunk) {
    const auto channels = filter.GetFormat().Channels;
    std::vector<float> result;
    std::vector<float> buffer(chunk * channels + 1, 123);
    for(unsigned n = 0; n < 100000; ++n) {
        unsigned written;
        tTVPWaveSegmentQueue queue;
        filter.Decode(buffer.data(), chunk, written, queue);
        REQUIRE(written <= chunk);
        REQUIRE(buffer.back() == 123);
        REQUIRE(queue.GetFilteredLength() == written);
        for(unsigned i = 0; i < written * channels; ++i) REQUIRE(std::isfinite(buffer[i]));
        result.insert(result.end(), buffer.begin(), buffer.begin() + written * channels);
        if(written < chunk) return result;
    }
    FAIL("effect did not reach EOF");
    return {};
}
}
TEST_CASE("wfBasicEffect delay preserves channels and renders bounded EOF echoes", "[plugin][audio]") {
    for(unsigned rate : {44100u, 48000u}) for(unsigned channels : {1u, 2u}) for(bool pcm : {false, true}) {
        ImpulseSource source(rate, channels, pcm);
        basicEffect::DelayEffect filter;
        Init(filter);
        filter.Recreate(&source);
        const auto output = Render(filter, 37);
        REQUIRE(output.size() == (8 + rate / 100) * channels);
        REQUIRE(output[0] == 0.5f);
        const auto delay = rate / 1000;
        REQUIRE(output[delay * channels] == 0.25f);
        REQUIRE(output[delay * 2 * channels] == 0.125f);
        if(channels == 2) for(size_t f = 0; f < output.size() / 2; ++f) REQUIRE(output[f * 2 + 1] == 0);
        source.position = 0;
        filter.Reset();
        REQUIRE(Render(filter, 1024) == output);
        filter.Clear();
        REQUIRE(Render(filter, 17).empty());
        REQUIRE(filter.Recreate(&source) == &filter);
    }
}
TEST_CASE("wfBasicEffect validation leaves disconnected instances reusable", "[plugin][audio]") {
    basicEffect::DelayEffect filter;
    REQUIRE_THROWS(Init(filter, -1));
    REQUIRE_THROWS(Init(filter, 1, 1.01));
    REQUIRE_THROWS(Init(filter, 1, std::nan("")));
    REQUIRE_THROWS(Init(filter, 10001));
    ImpulseSource source;
    source.format.Channels = 3;
    REQUIRE_THROWS(filter.Recreate(&source));
    source.format.Channels = 1;
    source.format.TotalSamples = std::numeric_limits<tjs_uint64>::max();
    REQUIRE_THROWS(filter.Recreate(&source));
    source.format.TotalSamples = source.length;
    Init(filter);
    REQUIRE(filter.Recreate(&source) == &filter);
    REQUIRE_THROWS(filter.Recreate(&source));
}
TEST_CASE("wfBasicEffect FreeVerb resets histories and isolates sample rates", "[plugin][audio]") {
    for(unsigned rate : {44100u, 48000u}) {
        basicEffect::StkFreeVerb filter;
        ImpulseSource source(rate);
        filter.Recreate(&source);
        REQUIRE(filter.GetFormat().Channels == 2);
        const auto first = Render(filter, 333);
        REQUIRE(first.size() == (8 + rate) * 2);
        double energy = 0;
        for(size_t i = 2000; i < first.size(); ++i) {
            REQUIRE(std::abs(first[i]) <= 1);
            energy += first[i] * first[i];
        }
        REQUIRE(energy > 0.00001);
        basicEffect::StkFreeVerb other;
        ImpulseSource otherSource(96000);
        other.Recreate(&otherSource);
        basicEffect::StkFreeVerb later;
        ImpulseSource laterSource(rate);
        later.Recreate(&laterSource);
        REQUIRE(Render(later, 777) == first);
        source.position = 0;
        filter.Reset();
        REQUIRE(Render(filter, 1024) == first);
        filter.Clear();
        source.position = 0;
        filter.SetMix(0);
        filter.SetExtend(0);
        filter.Recreate(&source);
        const auto dry = Render(filter, 13);
        REQUIRE(dry.size() == 16);
        REQUIRE(dry[0] == 0.5f);
        REQUIRE(dry[1] == 0.5f);
        REQUIRE_THROWS(filter.SetRoom(-0.1));
        REQUIRE_THROWS(filter.SetMix(std::nan("")));
    }
}
TEST_CASE("wave tail segments survive dequeue and scale without advancing source position", "[plugin][audio]") {
    tTVPWaveSegmentQueue queue, head;
    queue.Enqueue(tTVPWaveSegment(0, 8));
    queue.Enqueue(tTVPWaveSegment(8, 0, 100));
    queue.Enqueue(tTVPWaveLabel(8, TJS_W("after"), 50));
    queue.Dequeue(head, 20);
    REQUIRE(head.GetFilteredLength() == 20);
    REQUIRE(head.FilteredPositionToDecodePosition(19) == 8);
    REQUIRE(queue.GetFilteredLength() == 88);
    REQUIRE(queue.GetLabels().front().Offset == 30);
    queue.Scale(44);
    REQUIRE(queue.GetFilteredLength() == 44);
    REQUIRE(queue.FilteredPositionToDecodePosition(43) == 8);
    REQUIRE(queue.GetLabels().front().Offset == 15);
    queue.Dequeue(head, 44);
    REQUIRE(head.GetFilteredLength() == 44);
    REQUIRE(queue.GetFilteredLength() == 0);
}

TEST_CASE("wfBasicEffect FreeVerb freeze suppresses new wet input", "[plugin][audio]") {
    basicEffect::StkFreeVerb filter;
    filter.SetMix(1);
    filter.SetMode(true);
    REQUIRE_THROWS(filter.SetExtend(tjs_int64(1) << 32));
    ImpulseSource source(44100, 2, true);
    filter.Recreate(&source);
    const auto frozen = Render(filter, 1000);
    for(float value : frozen) REQUIRE(value == 0);
    source.position = 0;
    filter.SetMode(false);
    filter.Reset();
    const auto active = Render(filter, 1000);
    REQUIRE(std::any_of(active.begin(), active.end(), [](float v) { return v != 0; }));
}
