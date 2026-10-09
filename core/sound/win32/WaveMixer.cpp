#include "WaveMixer.h"
#include "tjsCommHead.h"

#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/alc.h>
#include <AL/alext.h>
#endif

#ifdef __ANDROID__
#include "oboe/Oboe.h"
#endif

#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "TickCount.h"
#include "WaveImpl.h"
#include "WaveMixerConverter.h"

#include <algorithm>
#include <assert.h>
#include <iomanip>
#include <math.h>
#include <sstream>
#include <string.h>
#include <unordered_set>

class iTVPAudioRenderer;

static iTVPAudioRenderer *TVPAudioRenderer;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static float readFloatLE(const float &f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    u = ((u & 0xFF000000u) >> 24) | ((u & 0x00FF0000u) >> 8) |
        ((u & 0x0000FF00u) << 8) | ((u & 0x000000FFu) << 24);
    float r;
    memcpy(&r, &u, sizeof(r));
    return r;
}

static void writeFloatLE(float *dst, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    u = ((u & 0xFF000000u) >> 24) | ((u & 0x00FF0000u) >> 8) |
        ((u & 0x0000FF00u) << 8) | ((u & 0x000000FFu) << 24);
    memcpy(dst, &u, sizeof(u));
}
#else
static float readFloatLE(const float &f) { return f; }

static void writeFloatLE(float *dst, float v) { *dst = v; }
#endif

template <int ch>
void MixAudioS16CPP(void *dst, const void *src, int samples, int16_t *volume) {
    int16_t *dst16 = (int16_t *)dst;
    const int16_t *src16 = (const int16_t *)src;
    while(samples--) {
        for(int i = 0; i < ch; ++i) {
            int src_sample = *src16++;
            src_sample = (src_sample * volume[i]) >> 14;
            int dest_sample = *dst16;
            if(src_sample > 0 && dest_sample > 0) {
                dest_sample = src_sample + dest_sample -
                    ((dest_sample * src_sample + 0x8000) >> 15);
            } else if(src_sample < 0 && dest_sample < 0) {
                dest_sample = src_sample + dest_sample +
                    ((dest_sample * src_sample) >> 15);
            } else {
                dest_sample += src_sample;
            }
            *dst16++ = dest_sample;
        }
    }
}

template <int ch>
void MixAudioF32CPP(void *dst, const void *src, int samples, int16_t *volume) {
    float *dst32 = (float *)dst;
    const float *src32 = (const float *)src;
    const float fmaxvolume = 1.0f / 16384 /*tTVPSoundBuffer::MAX_VOLUME*/;
    float fvolume[ch];
    for(int i = 0; i < ch; ++i)
        fvolume[i] = volume[i] * fmaxvolume;
    while(samples--) {
        for(int i = 0; i < ch; ++i) {
            float src_sample = readFloatLE(*src32++) * fvolume[i];
            float dest_sample = readFloatLE(*dst32);
            if(src_sample > 0 && dest_sample > 0) {
                dest_sample =
                    src_sample + dest_sample - dest_sample * src_sample;
            } else if(src_sample < 0 && dest_sample < 0) {
                dest_sample =
                    src_sample + dest_sample + dest_sample * src_sample;
            } else {
                dest_sample += src_sample;
            }
            writeFloatLE(dst32++, dest_sample);
        }
    }
}

typedef void(FAudioMix)(void *dst, const void *src, int samples,
                        int16_t *volume);

static FAudioMix *_AudioMixS16[8] = { // 7.1 max
    &MixAudioS16CPP<1>, &MixAudioS16CPP<2>, &MixAudioS16CPP<3>,
    &MixAudioS16CPP<4>, &MixAudioS16CPP<5>, &MixAudioS16CPP<6>,
    &MixAudioS16CPP<7>, &MixAudioS16CPP<8>
};
static FAudioMix *_AudioMixF32[8] = { // 7.1 max
    &MixAudioF32CPP<1>, &MixAudioF32CPP<2>, &MixAudioF32CPP<3>,
    &MixAudioF32CPP<4>, &MixAudioF32CPP<5>, &MixAudioF32CPP<6>,
    &MixAudioF32CPP<7>, &MixAudioF32CPP<8>
};

extern "C" void TVPWaveMixer_ASM_Init(FAudioMix **func16, FAudioMix **func32);

class tTVPSoundBuffer : public iTVPSoundBuffer {
public:
    bool _playing = false;
    float _volume = 1;
    float _pan = 0;
    const signed int MAX_VOLUME = 16384; // limit in signed 16bit
    int16_t _volume_raw[8];
    ITVPAudioConverter *_cvt = nullptr;
    int _buffer_frame_size = 0;
    int _frame_size = 0;

    void RecalcVolume() {
        if(_pan > 0) {
            _volume_raw[0] = (1.0f - _pan) * _volume * MAX_VOLUME;
        } else {
            _volume_raw[0] = _volume * MAX_VOLUME;
        }
        if(_pan < 0) {
            _volume_raw[1] = (_pan + 1.0f) * _volume * MAX_VOLUME;
        } else {
            _volume_raw[1] = _volume * MAX_VOLUME;
        }
        _volume_raw[2] = _volume_raw[0]; // for SIMD
        _volume_raw[3] = _volume_raw[1];
    }

    std::mutex _buffer_mtx;
    std::deque<std::vector<uint8_t>> _buffers;
    tjs_uint _sendedFrontBuffer = 0;
    tjs_uint _sendedSamples = 0, _inCachedSamples = 0;

    tTVPSoundBuffer(int framesize, ITVPAudioConverter *cvt) :
        _frame_size(framesize), _cvt(cvt) {
        _buffer_frame_size = cvt ? cvt->dstFrameSize() : framesize;
        RecalcVolume();
    }

    ~tTVPSoundBuffer() override;

    void Release() override { delete this; }

    void Play() override { _playing = true; }

    void Pause() override { _playing = false; }

    void Stop() override {
        _playing = false;
        Reset();
    }

    void Reset() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        _buffers.clear();
        _inCachedSamples = 0;
        _sendedFrontBuffer = 0;
        _sendedSamples = 0;
    }

    bool IsPlaying() override { return _playing; }

    void SetVolume(float v) override {
        _volume = v;
        RecalcVolume();
    }

    float GetVolume() override { return _volume; }

    void SetPan(float v) override {
        _pan = v;
        RecalcVolume();
    }

    float GetPan() override { return _pan; }

    void AppendBuffer(const void *_inbuf,
                      unsigned int inlen /*, int tag = 0*/) override {
        if(_cvt) {
            std::vector<uint8_t> buffer;
            _cvt->convert(_inbuf, inlen, buffer);
            std::lock_guard<std::mutex> lk(_buffer_mtx);
            _inCachedSamples += buffer.size() / _buffer_frame_size;
            _buffers.emplace_back();
            _buffers.back().swap(buffer);
        } else {
            std::lock_guard<std::mutex> lk(_buffer_mtx);
            _buffers.emplace_back((uint8_t *)_inbuf,
                                  ((uint8_t *)_inbuf) + inlen);
            _inCachedSamples += inlen / _buffer_frame_size;
        }
    }

    bool IsBufferValid() override {
        return true; // unlimited buffer size
                     // return !_buffers.empty(); // thread safe if
                     // read only
    }

    tjs_uint GetLatencySamples() override;

    // 	virtual void SetSampleOffset(tjs_uint n) override {
    // 		_sendedSamples = n;
    // 	}
    int GetRemainBuffers() override { return _buffers.size(); }

    tjs_uint GetCurrentPlaySamples() override;

    float GetLatencySeconds() override;

    void FillBuffer(uint8_t *out, int len);
};

class iTVPAudioRenderer {
protected:
    TVPAudioSpec _spec;
    std::mutex _streams_mtx;
    std::unordered_set<tTVPSoundBuffer *> _streams;
    int _frame_size = 0;

public:
    iTVPAudioRenderer() {
        _spec.freq = 48000;
        _spec.format = TVPAudioSampleFormat::S16;
        _spec.channels = 2;
        _spec.frame_size =
            TVPAudioSpec::calcFrameSize(_spec.format, _spec.channels);
        _frame_size = _spec.frame_size;
    }

    virtual ~iTVPAudioRenderer() = default;

    void InitMixer() {}

    FAudioMix *DoMixAudio{};

    void SetupMixer() {
        if(_spec.format == TVPAudioSampleFormat::S16) {
            DoMixAudio = _AudioMixS16[_spec.channels - 1];
        } else if(_spec.format == TVPAudioSampleFormat::F32) {
            DoMixAudio = _AudioMixF32[_spec.channels - 1];
        } else {
            DoMixAudio = [](void *dst, const void *src, int samples,
                            int16_t *volume) {};
        }
    }

    virtual bool Init() = 0;

    virtual tTVPSoundBuffer *CreateStream(tTVPWaveFormat &fmt, int bufcount) {
        (void)bufcount;
        TVPAudioSampleFormat srcFormat = waveFormatToSampleFormat(fmt);
        if(srcFormat == TVPAudioSampleFormat::Unknown) {
            return nullptr;
        }

        ITVPAudioConverter *cvt = nullptr;
        if(fmt.SamplesPerSec != _spec.freq || fmt.Channels != _spec.channels ||
           srcFormat != _spec.format) {
            cvt = CreateTVPAudioConverter(srcFormat, fmt.Channels,
                                          fmt.SamplesPerSec, _spec.format,
                                          _spec.channels, _spec.freq);
            if(!cvt) {
                return nullptr;
            }
        }

        tTVPSoundBuffer *s =
            new tTVPSoundBuffer(fmt.BytesPerSample * fmt.Channels, cvt);
        std::lock_guard<std::mutex> lk(_streams_mtx);
        _streams.emplace(s);
        return s;
    }

    void ReleaseStream(tTVPSoundBuffer *s) {
        std::lock_guard<std::mutex> lk(_streams_mtx);
        _streams.erase(s);
    }

    void FillBuffer(uint8_t *buf, int len) {
        // memset(buf, 0, len);
        std::lock_guard<std::mutex> lk(_streams_mtx);
        for(tTVPSoundBuffer *s : _streams) {
            s->FillBuffer(buf, len);
        }
    }

    int MixAudio(uint8_t *dst, uint8_t *src, int len, int16_t *vol) {
        int samples = len / _frame_size;
        DoMixAudio(dst, src, samples, vol);
        return samples;
    }

    const TVPAudioSpec &GetSpec() const { return _spec; }

    virtual int32_t GetUnprocessedSamples() { return 0; }
};

tTVPSoundBuffer::~tTVPSoundBuffer() {
    tTVPSoundBuffer::Stop();
    TVPAudioRenderer->ReleaseStream(this);
    DestroyTVPAudioConverter(_cvt);
}

tjs_uint tTVPSoundBuffer::GetLatencySamples() {
    int32_t samples = TVPAudioRenderer->GetUnprocessedSamples();
    return samples + _inCachedSamples;
}

tjs_uint tTVPSoundBuffer::GetCurrentPlaySamples() {
    int32_t samples = TVPAudioRenderer->GetUnprocessedSamples();
    if(samples > _sendedSamples)
        return 0;
    return _sendedSamples - samples; // -GetLatencySamples();
}

float tTVPSoundBuffer::GetLatencySeconds() {
    return GetLatencySamples() /
        static_cast<float>(TVPAudioRenderer->GetSpec().freq);
}

void tTVPSoundBuffer::FillBuffer(uint8_t *out, int len) {
    if(!_playing)
        return;
    std::lock_guard<std::mutex> lk(_buffer_mtx);
    while(len > 0 && !_buffers.empty()) {
        std::vector<uint8_t> &buf = _buffers.front();
        if(buf.size() > _sendedFrontBuffer) {
            int n = std::min((size_t)len, buf.size() - _sendedFrontBuffer);
            int samples = TVPAudioRenderer->MixAudio(
                out, &buf.front() + _sendedFrontBuffer, n, _volume_raw);
            _sendedSamples += samples;
            _inCachedSamples -= samples;
            _sendedFrontBuffer += n;
            out += n;
            len -= n;
        }
        if(_sendedFrontBuffer >= buf.size()) {
            _sendedFrontBuffer = 0;
            _buffers.pop_front();
        }
    }
}

#ifdef __ANDROID__

class tTVPAudioRendererOboe : public iTVPAudioRenderer,
                              public oboe::AudioStreamCallback {
    oboe::AudioStream *_oboeAudioStream = nullptr;

public:
    virtual ~tTVPAudioRendererOboe() {
        if(_oboeAudioStream)
            delete _oboeAudioStream;
    }

    bool Init() override {
        InitMixer();
        // Create a builder
        oboe::AudioStreamBuilder builder;
        // builder.setFormat(oboe::AudioFormat::I16);
        builder.setChannelCount(2);
        // builder.setSampleRate(oboe::kUnspecified);
        builder.setCallback(this);
        // 	builder.setPerformanceMode(PerformanceMode::None);
        // 	builder.setSharingMode(SharingMode::Shared);
        oboe::Result result = builder.openStream(&_oboeAudioStream);
        // 		if (result != oboe::Result::OK) {
        // 			// try down sample rate
        // 			_spec.freq = 44100;
        // 			builder.setSampleRate(_spec.freq);
        // 			result = builder.openStream(&_oboeAudioStream);
        // 		}
        if(result == oboe::Result::OK) {
            _spec.freq = _oboeAudioStream->getSampleRate();
            switch(_oboeAudioStream->getFormat()) {
                case oboe::AudioFormat::I16:
                    _spec.format = TVPAudioSampleFormat::S16;
                    break;
                case oboe::AudioFormat::Float:
                    _spec.format = TVPAudioSampleFormat::F32;
                    break;
                default:
                    _spec.format = TVPAudioSampleFormat::S16;
                    break;
            }
            _spec.frame_size =
                TVPAudioSpec::calcFrameSize(_spec.format, _spec.channels);
            _frame_size = _spec.frame_size;
            _oboeAudioStream->requestStart();
            waveMixerAudioLogf("Audio Device: Oboe @%uHz", _spec.freq);
            SetupMixer();
            return true;
        }
        waveMixerAudioLogf("Fail to open Oboe audio");
        return false;
    }

    virtual oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream,
                                                  void *audioData,
                                                  int32_t numFrames) override {
        (void)oboeStream;
        int len = _frame_size * numFrames;
        memset(audioData, 0, _frame_size * numFrames);
        // if (oboeStream == _oboeAudioStream)
        FillBuffer((uint8_t *)audioData, len);
        // 		else
        // 			fillCaptureBuffer((uint8_t*)audioData, /*Mono*/2
        // * numFrames);
        return oboe::DataCallbackResult::Continue;
    }

    virtual int32_t GetUnprocessedSamples() {
        int64_t hardwareFrameIndex;
        int64_t timeNanoseconds;
        oboe::Result result = _oboeAudioStream->getTimestamp(
            CLOCK_MONOTONIC, &hardwareFrameIndex, &timeNanoseconds);
        if(result != oboe::Result::OK) { // OpenSL TODO accumulate calc
            return 0;
        }
        int64_t appFrameIndex = _oboeAudioStream->getFramesWritten();
        return appFrameIndex - hardwareFrameIndex;
    }
};

#endif

class tTVPSoundBufferAL : public tTVPSoundBuffer {
    typedef tTVPSoundBuffer inherit;

    ALuint _alSource;
    ALenum _alFormat;
    ALuint *_bufferIds, *_bufferIds2;
    tjs_uint *_bufferSize;
    tjs_uint _bufferCount;
    int _bufferIdx = -1;
    tTVPWaveFormat _format;

public:
    tTVPSoundBufferAL(tTVPWaveFormat &desired, int bufcount) :
        tTVPSoundBuffer(desired.BytesPerSample * desired.Channels, nullptr),
        _bufferCount(bufcount) {
        _bufferIds = new ALuint[bufcount];
        _bufferIds2 = new ALuint[bufcount];
        _bufferSize = new tjs_uint[bufcount];
        _format = desired;
        alGenSources(1, &_alSource);
        alGenBuffers(_bufferCount, _bufferIds);
        alSourcef(_alSource, AL_GAIN, 1.0f);
        if(desired.Channels == 1) {
            switch(desired.BitsPerSample) {
                case 8:
                    _alFormat = AL_FORMAT_MONO8;
                    break;
                case 16:
                    _alFormat = AL_FORMAT_MONO16;
                    break;
                default:
                    assert(false);
            }
        } else if(desired.Channels == 2) {
            switch(desired.BitsPerSample) {
                case 8:
                    _alFormat = AL_FORMAT_STEREO8;
                    break;
                case 16:
                    _alFormat = AL_FORMAT_STEREO16;
                    break;
                default:
                    assert(false);
            }
        } else {
            assert(false);
        }
    }

    ~tTVPSoundBufferAL() override {
        alDeleteBuffers(_bufferCount, _bufferIds);
        alDeleteSources(1, &_alSource);
        delete[] _bufferIds;
        delete[] _bufferIds2;
        delete[] _bufferSize;
    }

    bool IsBufferValid() override {
        ALint processed = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        if(processed > 0)
            return true;
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        return queued < _bufferCount;
    }

    void AppendBuffer(const void *buf,
                      unsigned int len /*, int tag = 0*/) override {
        if(len <= 0)
            return;
        std::lock_guard<std::mutex> lk(_buffer_mtx);

        /* First remove any processed buffers. */
        ALint processed = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        if(processed > 0) {
            alSourceUnqueueBuffers(_alSource, processed, _bufferIds2);
            checkerr("alSourceUnqueueBuffers");
            for(int i = 0; i < processed; ++i) {
                for(int j = 0; j < _bufferCount; ++j) {
                    if(_bufferIds[j] == _bufferIds2[i]) {
                        _sendedSamples += _bufferSize[j] / _frame_size;
                        break;
                    }
                }
            }
        }

        /* Refill the buffer queue. */
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);

        if(queued >= _bufferCount)
            return;
        ++_bufferIdx;
        if(_bufferIdx >= _bufferCount)
            _bufferIdx = 0;
        ALuint bufid = _bufferIds[_bufferIdx];
        alBufferData(bufid, _alFormat, buf, len, _format.SamplesPerSec);
        checkerr("alBufferData");
        alSourceQueueBuffers(_alSource, 1, &bufid);
        checkerr("alSourceQueueBuffers");
        //_tags[_bufferIdx] = tag;
        _bufferSize[_bufferIdx] = len;
    }

    void Reset() override {
        inherit::Reset();
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        alSourceRewind(_alSource);
        alSourcei(_alSource, AL_BUFFER, 0);
    }

    void Pause() override {
        alSourcePause(_alSource);
        checkerr("Pause");
        _playing = false;
    }

    static void checkerr(const char *funcname);

    void Play() override {
        ALenum state;
        alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        checkerr("Play");
        if(state != AL_PLAYING) {
            alSourcePlay(_alSource);
            checkerr("Play");
        }

        _playing = true;
    }

    void Stop() override {
        alSourceStop(_alSource);
        checkerr("Stop");
        Reset();
        _bufferIdx = -1;
        _playing = false;
    }

    void SetVolume(float volume) override {
        alSourcef(_alSource, AL_GAIN, volume);
        checkerr("SetVolume");
    }

    float GetVolume() override {
        float volume = 0;
        alGetSourcef(_alSource, AL_GAIN, &volume);
        return volume;
    }

    void SetPan(float pan) override {
        float sourcePosAL[] = { pan, 0.0f, 0.0f };
        alSourcefv(_alSource, AL_POSITION, sourcePosAL);
    }

    float GetPan() override {
        float sourcePosAL[3];
        alGetSourcefv(_alSource, AL_POSITION, sourcePosAL);
        return sourcePosAL[0];
    }

    bool IsPlaying() override {
        ALenum state;
        alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        return state == AL_PLAYING;
    }

    void SetPosition(float x, float y, float z) override {
        float sourcePosAL[] = { x, y, z };
        alSourcefv(_alSource, AL_POSITION, sourcePosAL);
        checkerr("SetPosition");
    }

    int GetRemainBuffers() override {
        ALint processed, queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        return queued - processed;
    }

    tjs_uint GetLatencySamples() override {
        std::lock_guard<std::mutex> lk(_buffer_mtx);
        ALint offset = 0, queued = 0;
        alGetSourcei(_alSource, AL_BYTE_OFFSET, &offset);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        int remainBuffers = queued;
        if(remainBuffers == 0)
            return 0;
        tjs_int total = -offset;
        for(int i = 0; i < remainBuffers; ++i) {
            int idx = _bufferIdx + 1 - remainBuffers + i;
            if(idx >= _bufferCount)
                idx -= _bufferCount;
            else if(idx < 0)
                idx += _bufferCount;
            total += _bufferSize[idx];
        }
        return total / _frame_size;
    }

    float GetLatencySeconds() override {
        return (float)GetLatencySamples() / _format.SamplesPerSec;
    }

    tjs_uint GetCurrentPlaySamples() override {
        ALint offset = 0;
        alGetSourcei(_alSource, AL_SAMPLE_OFFSET, &offset);
        return _sendedSamples + offset;
    }
};

class tTVPAudioRendererAL : public iTVPAudioRenderer {
    ALCdevice *_device = nullptr;
    ALCcontext *_context = nullptr;

public:
    ~tTVPAudioRendererAL() override {
        if(_context) {
            // alDeleteSources(TVP_MAX_AUDIO_COUNT, _alSources);
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(_context);
        }
        if(_device)
            alcCloseDevice(_device);
    }

    bool Init() override {
        ALboolean enumeration =
            alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT");
        if(enumeration == AL_FALSE) {
            // enumeration not supported
            _device = alcOpenDevice(nullptr);
        } else {
            // enumeration supported
            const ALCchar *devices =
                alcGetString(nullptr, ALC_DEVICE_SPECIFIER);
            std::vector<std::string> alldev;
            ttstr log(TJS_W("(info) Sound Driver/Device found : "));
            while(*devices) {
                TVPAddImportantLog(log + devices);
                alldev.emplace_back(devices);
                devices += alldev.back().length();
            }
            _device = alcOpenDevice(alldev[0].c_str());
        }
        if(!_device)
            return false;

        _context = alcCreateContext(_device, nullptr);
        alcMakeContextCurrent(_context);

        return true;
    }

    tTVPSoundBuffer *CreateStream(tTVPWaveFormat &fmt, int bufcount) override {
        tTVPSoundBuffer *s = new tTVPSoundBufferAL(fmt, bufcount);
        _streams.emplace(s);
        return s;
    }

    ALCcontext *GetContext() { return _context; }
};

void tTVPSoundBufferAL::checkerr(const char *funcname) {
#if _DEBUG
    ALCcontext *ctx =
        static_cast<tTVPAudioRendererAL *>(TVPAudioRenderer)->GetContext();
    if(alcGetCurrentContext() != ctx) {
        alcMakeContextCurrent(ctx);
    }
    ALenum err = alGetError();
    if(AL_NO_ERROR == err)
        return;
    waveMixerAudioLogf("%s OpenAL Error %X", funcname, err);
#endif
}

static iTVPAudioRenderer *CreateAudioRenderer() {
    iTVPAudioRenderer *renderer = nullptr;
#ifdef __ANDROID__
    renderer = new tTVPAudioRendererOboe;
    if(renderer->Init())
        return renderer;
    delete renderer;
#endif
    renderer = new tTVPAudioRendererAL;
    renderer->Init();
    return renderer;
}

void TVPInitDirectSound(int freq) {
    (void)freq;
    if(!TVPAudioRenderer) {
        TVPAudioRenderer = CreateAudioRenderer();
    }
    // TVPInitSoundOptions();
}

void TVPUninitDirectSound() {
    // nothing to do
}

iTVPSoundBuffer *TVPCreateSoundBuffer(tTVPWaveFormat &fmt, int bufcount) {
    return TVPAudioRenderer->CreateStream(fmt, bufcount);
}

#if 0
int TVPALSoundWrap::GetNextBufferIndex() {
    int n = _bufferIdx + 1;
    if (n >= TVPAL_BUFFER_COUNT) n = 0;
    return n;
}

void TVPALSoundWrap::SetSampleOffset(tjs_uint n /*= 0*/) {
    _samplesProcessed = n;
}
#endif
