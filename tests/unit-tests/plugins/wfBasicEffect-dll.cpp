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

namespace {
class SamplesSource : public tTVPSampleAndLabelSource {
public:
    tTVPWaveFormat format{};
    std::vector<float> data;
    unsigned position = 0;
    SamplesSource(std::vector<float> samples, unsigned rate = 48000, unsigned channels = 1, bool pcm = false)
        : data(std::move(samples)) {
        format.SamplesPerSec = rate; format.Channels = channels;
        format.IsFloat = !pcm; format.BitsPerSample = pcm ? 16 : 32;
        format.BytesPerSample = pcm ? 2 : 4; format.TotalSamples = data.size() / channels;
    }
    const tTVPWaveFormat &GetFormat() const override { return format; }
    void Decode(void *dest, unsigned count, unsigned &written, tTVPWaveSegmentQueue &queue) override {
        written = std::min<unsigned>(count, format.TotalSamples - position);
        for(unsigned i = 0; i < written * format.Channels; ++i) {
            const auto value = data[position * format.Channels + i];
            if(format.IsFloat) static_cast<float *>(dest)[i] = value;
            else static_cast<tjs_int16 *>(dest)[i] = static_cast<tjs_int16>(value * 32768);
        }
        if(written) queue.Enqueue(tTVPWaveSegment(position, written));
        if(position <= 3 && position + written > 3)
            queue.Enqueue(tTVPWaveLabel(3, TJS_W("point"), 3 - position));
        position += written;
    }
};
void InitGain(basicEffect::GainLimit &filter, double db, double threshold = 1, int mode = 0, int count = 3) {
    tTJSVariant a(db), b(threshold), c(mode), result(42);
    tTJSVariant *args[] = {&a, &b, &c};
    const auto status = basicEffect::GainLimit::Init(&result, count, args, &filter);
    REQUIRE(status == TJS_S_OK);
    REQUIRE(result.Type() == tvtVoid);
}
void SetBand(basicEffect::GraphicEqualizer &filter, int index, double gain) {
    tTJSVariant a(index), b(gain), result(42);
    tTJSVariant *args[] = {&a, &b};
    const auto status = basicEffect::GraphicEqualizer::SetGain(&result, 2, args, &filter);
    REQUIRE(status == TJS_S_OK);
    REQUIRE(result.Type() == tvtVoid);
}
double GetBand(basicEffect::GraphicEqualizer &filter, int index) {
    tTJSVariant a(index), result;
    tTJSVariant *args[] = {&a};
    REQUIRE(basicEffect::GraphicEqualizer::GetGain(&result, 1, args, &filter) == TJS_S_OK);
    return static_cast<double>(result);
}
}

TEST_CASE("GainLimit applies dB gain and the recovered soft curves", "[plugin][audio]") {
    const double atTwo[] = {0, 2.0 / 3, 0.8038134760954126, 0.8944271909999159, 0.9640275800758169};
    const double aboveHalf[] = {0, 0.75, 0.8195464633859458, 0.8535533905932737, 0.8807970779778824};
    for(int mode = 1; mode <= 4; ++mode) {
        basicEffect::GainLimit filter;
        SamplesSource source({-2, -0.5f, 0, 0.5f, 2});
        InitGain(filter, 0, 0, mode);
        filter.Recreate(&source);
        auto output = Render(filter, 2);
        REQUIRE(output.size() == source.data.size());
        REQUIRE(output.front() == Catch::Approx(-atTwo[mode]).margin(2e-7));
        REQUIRE(output.back() == Catch::Approx(atTwo[mode]).margin(2e-7));
        REQUIRE(output[2] == 0);
        source.data = {-1, -0.5f, 0, 0.5f, 1}; source.position = 0; filter.Reset();
        InitGain(filter, 0, -0.5, mode);
        filter.Reset();
        output = Render(filter, 3);
        REQUIRE(output.front() == Catch::Approx(-aboveHalf[mode]).margin(2e-7));
        REQUIRE(output.back() == Catch::Approx(aboveHalf[mode]).margin(2e-7));
        REQUIRE(output[1] == -0.5f); REQUIRE(output[3] == 0.5f);
        InitGain(filter, 20, 0, mode, 1);
        source.position = 0; filter.Reset();
        REQUIRE(Render(filter, 7).back() == 10);
        InitGain(filter, -20, 0, mode, 1);
        source.position = 0; filter.Reset();
        REQUIRE(Render(filter, 7).back() == Catch::Approx(0.1).margin(1e-7));
    }
    for(double threshold : {0.0, 0x1p-24, 0x1p-23, 0.5, 1.0, 1.25}) {
        for(int mode : {-1, 0, 1, 2, 3, 4, 5}) {
            basicEffect::GainLimit filter;
            SamplesSource source({-2, 0, 2});
            InitGain(filter, 0, threshold, mode);
            filter.Recreate(&source);
            auto output = Render(filter, 5);
            REQUIRE(output[0] == -output[2]); REQUIRE(output[1] == 0);
            if(mode == 0) REQUIRE(output[2] == 2);
            else if(mode < 1 || mode > 4 || threshold >= 1)
                REQUIRE(output[2] == Catch::Approx(threshold));
            else REQUIRE(output[2] <= 1);
        }
    }
}

TEST_CASE("GainLimit preserves format channels labels and live updates without a tail", "[plugin][audio]") {
    for(unsigned rate : {44100u, 48000u}) for(bool pcm : {false, true}) {
        basicEffect::GainLimit filter;
        SamplesSource source({0.5f, -0.25f, 0, 0.5f, -0.5f, 0, 0.25f, -0.75f}, rate, 2, pcm);
        filter.Recreate(&source);
        REQUIRE(Render(filter, 3) == source.data);
        source.position = 0; filter.Reset();
        InitGain(filter, 20, 0.5, 99);
        REQUIRE(Render(filter, 7) == std::vector<float>{0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, -0.5f});
        InitGain(filter, 0, 0.5, 99, 0);
        source.position = 0; filter.Reset();
        REQUIRE(Render(filter, 1)[0] == 0.5f);
        InitGain(filter, 0, 0.5, 99, 1);
        source.position = 0; filter.Reset();
        float output[20]{}; unsigned written; tTVPWaveSegmentQueue segments;
        filter.Decode(output, 10, written, segments);
        REQUIRE(written == 4); REQUIRE(segments.GetFilteredLength() == 4);
        REQUIRE(segments.GetLabels().size() == 1); REQUIRE(segments.GetLabels().front().Offset == 3);
        REQUIRE(segments.FilteredPositionToDecodePosition(3) == 3);
        REQUIRE_THROWS(filter.Recreate(&source));
        filter.Clear(); source.position = 0; filter.Reset();
        REQUIRE(filter.Recreate(&source) == &filter);
        REQUIRE(Render(filter, 2) == source.data);
    }
    basicEffect::GainLimit live;
    SamplesSource liveSource({0.25f, 0.25f, 0.25f});
    live.Recreate(&liveSource);
    float value; unsigned written; tTVPWaveSegmentQueue queue;
    live.Decode(&value, 1, written, queue);
    REQUIRE(value == 0.25f);
    InitGain(live, 20);
    live.Decode(&value, 1, written, queue);
    REQUIRE(value == 2.5f);
    basicEffect::GainLimit filter;
    REQUIRE_THROWS(InitGain(filter, std::nan("")));
    REQUIRE_THROWS(InitGain(filter, 121)); REQUIRE_THROWS(InitGain(filter, 0, 17));
    InitGain(filter, 120, 0, 3);
    SamplesSource huge({std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
    filter.Recreate(&huge);
    REQUIRE(Render(filter, 3) == std::vector<float>{1, -1});
}

TEST_CASE("GEQ keeps ten linear gains and applies changes only on reconnect", "[plugin][audio]") {
    for(unsigned rate : {44100u, 48000u}) for(bool pcm : {false, true}) {
        basicEffect::GraphicEqualizer filter;
        for(int i = 0; i < 10; ++i) REQUIRE(GetBand(filter, i) == 1);
        SetBand(filter, -1, 2); SetBand(filter, 10, 2);
        REQUIRE(GetBand(filter, -1) == 0); REQUIRE(GetBand(filter, 10) == 0);
        ImpulseSource source(rate, 2, pcm); source.length = 2048; source.format.TotalSamples = source.length;
        filter.Recreate(&source);
        const auto original = Render(filter, 71);
        REQUIRE(original.size() == 4096); REQUIRE(original[0] == 5);
        for(size_t i = 1; i < original.size(); ++i) REQUIRE(original[i] == 0);
        SetBand(filter, 4, 4);
        REQUIRE(GetBand(filter, 4) == 4);
        source.position = 0; filter.Reset(); filter.Update();
        REQUIRE(Render(filter, 233) == original);
        filter.Clear(); source.position = 0; filter.Reset(); filter.Recreate(&source);
        const auto changed = Render(filter, 127);
        REQUIRE(changed != original);
        REQUIRE(changed[2] != 0);
        for(size_t i = 1; i < changed.size(); i += 2) REQUIRE(changed[i] == 0);
        source.position = 0; filter.Reset();
        REQUIRE(Render(filter, 1024) == changed);
        filter.Clear(); source.format.SamplesPerSec = 22050;
        REQUIRE_THROWS(filter.Recreate(&source));
        source.format.SamplesPerSec = rate; source.position = 0; filter.Reset();
        REQUIRE(filter.Recreate(&source) == &filter);
    }
    basicEffect::GraphicEqualizer *raw = nullptr;
    std::array<tTJSVariant, 11> args;
    std::array<tTJSVariant *, 11> values;
    for(unsigned i = 0; i < args.size(); ++i) { args[i] = double(i + 1); values[i] = &args[i]; }
    REQUIRE(basicEffect::GraphicEqualizer::Create(&raw, 11, values.data(), nullptr) == TJS_S_OK);
    std::unique_ptr<basicEffect::GraphicEqualizer> filter(raw);
    REQUIRE(GetBand(*filter, 0) == 1); REQUIRE(GetBand(*filter, 9) == 10);
    args[10] = std::nan("");
    REQUIRE_THROWS(basicEffect::GraphicEqualizer::Create(&raw, 11, values.data(), nullptr));
    REQUIRE_THROWS(SetBand(*filter, 0, -1)); REQUIRE_THROWS(SetBand(*filter, 0, 17));
    REQUIRE_THROWS(SetBand(*filter, 0, 0.0001));
    REQUIRE(GetBand(*filter, 9) == 10);
    REQUIRE(basicEffect::GraphicEqualizer::SetGain(nullptr, 0, nullptr, filter.get()) == TJS_E_BADPARAMCOUNT);
    REQUIRE(basicEffect::GraphicEqualizer::GetGain(nullptr, 0, nullptr, filter.get()) == TJS_E_BADPARAMCOUNT);
}

TEST_CASE("GEQ matches independently evaluated steady state transfer gains", "[plugin][audio]") {
    // Expected magnitudes use the recovered all-pass transfer in the z domain.
    const double expected[2][2][4] = {
        {{9.30700222, 9.25, 9.50514523, 9.93055314}, {11.42209556, 13, 10.37905323, 10.02268751}},
        {{9.30700661, 9.25, 9.50507170, 9.93000614}, {11.42203349, 13, 10.37919765, 10.02288310}}
    };
    const unsigned rates[] = {44100, 48000}; const double frequencies[] = {125, 250, 1000, 4000};
    for(unsigned r = 0; r < 2; ++r) for(unsigned g = 0; g < 2; ++g) for(unsigned f = 0; f < 4; ++f) {
        std::vector<float> data(16384);
        for(unsigned i = 0; i < data.size(); ++i)
            data[i] = static_cast<float>(0.01 * std::sin(2 * basicEffect::Pi * frequencies[f] * i / rates[r]));
        SamplesSource source(data, rates[r]);
        basicEffect::GraphicEqualizer filter; SetBand(filter, 4, g ? 4 : 0.25);
        filter.Recreate(&source); const auto output = Render(filter, 333);
        // Fit sine and cosine to avoid a partial-period RMS bias.
        double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
        for(unsigned i = 8192; i < data.size(); ++i) {
            const double angle = 2 * basicEffect::Pi * frequencies[f] * i / rates[r];
            const double sn = std::sin(angle), cs = std::cos(angle);
            ss += sn * sn; cc += cs * cs; sc += sn * cs;
            ys += output[i] * sn; yc += output[i] * cs;
        }
        const double determinant = ss * cc - sc * sc;
        const double magnitude = std::hypot((ys * cc - yc * sc) / determinant,
                                            (yc * ss - ys * sc) / determinant) / 0.01;
        REQUIRE(magnitude == Catch::Approx(expected[r][g][f]).margin(0.003));
    }
}

TEST_CASE("GEQ handles zero gains and supported rate endpoints without marginal state drift", "[plugin][audio]") {
    for(unsigned rate : {32001u, 384000u}) for(double gain : {0.0, 1.0 / 1024, 16.0}) {
        basicEffect::GraphicEqualizer filter;
        for(int i = 0; i < 10; ++i) SetBand(filter, i, gain);
        SamplesSource source(std::vector<float>(16384, 0.01f), rate);
        filter.Recreate(&source); auto output = Render(filter, 257);
        for(float value : output) REQUIRE(std::abs(value) < 2);
        if(gain == 0) for(float value : output) REQUIRE(value == 0.01f);
        source.position = 0; filter.Reset();
        REQUIRE(Render(filter, 999) == output);
    }
}
