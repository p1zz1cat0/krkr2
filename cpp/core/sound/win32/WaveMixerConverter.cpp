#include "WaveMixerConverter.h"

#include "DebugIntf.h"

#include <cstdarg>
#include <cstdio>

int TVPAudioSpec::calcFrameSize(TVPAudioSampleFormat fmt, uint32_t ch) {
    int bytesPerSample = 0;
    switch(fmt) {
        case TVPAudioSampleFormat::U8:
            bytesPerSample = 1;
            break;
        case TVPAudioSampleFormat::S16:
            bytesPerSample = 2;
            break;
        case TVPAudioSampleFormat::S32:
        case TVPAudioSampleFormat::F32:
            bytesPerSample = 4;
            break;
        default:
            return 0;
    }
    return bytesPerSample * static_cast<int>(ch);
}

TVPAudioSampleFormat waveFormatToSampleFormat(const tTVPWaveFormat &fmt) {
    if(fmt.IsFloat) {
        return TVPAudioSampleFormat::F32;
    }
    switch(fmt.BitsPerSample) {
        case 8:
            return TVPAudioSampleFormat::U8;
        case 16:
            return TVPAudioSampleFormat::S16;
        case 32:
            return TVPAudioSampleFormat::S32;
        default:
            return TVPAudioSampleFormat::Unknown;
    }
}

void waveMixerAudioLogf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    TVPAddImportantLog(ttstr(buf));
}

#ifndef __ANDROID__

ITVPAudioConverter *CreateTVPAudioConverter(TVPAudioSampleFormat srcFmt,
                                            uint32_t srcCh, uint32_t srcRate,
                                            TVPAudioSampleFormat dstFmt,
                                            uint32_t dstCh, uint32_t dstRate) {
    (void)srcFmt;
    (void)srcCh;
    (void)srcRate;
    (void)dstFmt;
    (void)dstCh;
    (void)dstRate;
    return nullptr;
}

void DestroyTVPAudioConverter(ITVPAudioConverter *converter) {
    delete converter;
}

#endif
