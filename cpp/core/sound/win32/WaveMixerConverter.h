#pragma once

#include "WaveIntf.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class TVPAudioSampleFormat {
    Unknown,
    U8,
    S16,
    S32,
    F32,
};

struct TVPAudioSpec {
    uint32_t freq = 48000;
    TVPAudioSampleFormat format = TVPAudioSampleFormat::S16;
    uint32_t channels = 2;
    int frame_size = 4;

    static int calcFrameSize(TVPAudioSampleFormat fmt, uint32_t ch);
};

class ITVPAudioConverter {
public:
    virtual ~ITVPAudioConverter() = default;

    virtual bool valid() const = 0;

    virtual int dstFrameSize() const = 0;

    virtual void convert(const void *in, size_t inLen,
                         std::vector<uint8_t> &out) = 0;
};

ITVPAudioConverter *CreateTVPAudioConverter(TVPAudioSampleFormat srcFmt,
                                            uint32_t srcCh, uint32_t srcRate,
                                            TVPAudioSampleFormat dstFmt,
                                            uint32_t dstCh, uint32_t dstRate);

void DestroyTVPAudioConverter(ITVPAudioConverter *converter);

TVPAudioSampleFormat waveFormatToSampleFormat(const tTVPWaveFormat &fmt);

void waveMixerAudioLogf(const char *fmt, ...);
