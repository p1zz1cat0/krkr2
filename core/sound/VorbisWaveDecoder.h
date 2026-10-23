#pragma once

#include "WaveIntf.h"

#include <algorithm>
#include <cstdint>

// Mix interleaved multitrack PCM to interleaved stereo. Channels 2k and 2k+1
// are the left and right of track k, as in Ogg Opus mapping family 255 music.
// All tracks play: even channels go to the left and odd channels to the right.
// A last single channel goes to both sides. The sums clip to 16 bits.
inline void TVPMixTrackPairsToStereo(const std::int16_t *src, int frames,
                                     int channels, std::int16_t *dst) {
    const bool oddLast = channels % 2 != 0;
    for(int i = 0; i < frames; ++i, src += channels) {
        int l = 0, r = 0;
        for(int c = 0; c + 1 < channels; c += 2) {
            l += src[c];
            r += src[c + 1];
        }
        if(oddLast) {
            l += src[channels - 1];
            r += src[channels - 1];
        }
        dst[i * 2] = static_cast<std::int16_t>(std::clamp(l, -32768, 32767));
        dst[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(r, -32768, 32767));
    }
}

class VorbisWaveDecoderCreator : public tTVPWaveDecoderCreator {
public:
    // VorbisWaveDecoderCreator() {
    // TVPRegisterWaveDecoderCreator(this); }
    tTVPWaveDecoder *Create(const ttstr &storagename,
                            const ttstr &extension) override;
};

class OpusWaveDecoderCreator : public tTVPWaveDecoderCreator {
public:
    // VorbisWaveDecoderCreator() {
    // TVPRegisterWaveDecoderCreator(this); }
    tTVPWaveDecoder *Create(const ttstr &storagename,
                            const ttstr &extension) override;
};