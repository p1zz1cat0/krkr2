#include "WaveMixerConverter.h"

#include <miniaudio.h>

#include <vector>

namespace {

    static ma_format toMaFormat(TVPAudioSampleFormat fmt) {
        switch(fmt) {
            case TVPAudioSampleFormat::U8:
                return ma_format_u8;
            case TVPAudioSampleFormat::S16:
                return ma_format_s16;
            case TVPAudioSampleFormat::S32:
                return ma_format_s32;
            case TVPAudioSampleFormat::F32:
                return ma_format_f32;
            default:
                return ma_format_unknown;
        }
    }

    class TVPAudioConverterMiniaudio final : public ITVPAudioConverter {
    public:
        TVPAudioConverterMiniaudio(TVPAudioSampleFormat srcFmt, uint32_t srcCh,
                                   uint32_t srcRate,
                                   TVPAudioSampleFormat dstFmt, uint32_t dstCh,
                                   uint32_t dstRate) :
            _srcFrameSize(ma_get_bytes_per_sample(toMaFormat(srcFmt)) * srcCh),
            _dstFrameSize(ma_get_bytes_per_sample(toMaFormat(dstFmt)) * dstCh) {
            ma_data_converter_config cfg = ma_data_converter_config_init(
                toMaFormat(srcFmt), toMaFormat(dstFmt), srcCh, dstCh, srcRate,
                dstRate);
            if(ma_data_converter_init(&cfg, nullptr, &_converter) ==
               MA_SUCCESS) {
                _valid = true;
            }
        }

        ~TVPAudioConverterMiniaudio() override {
            if(_valid) {
                ma_data_converter_uninit(&_converter, nullptr);
            }
        }

        bool valid() const override { return _valid; }

        int dstFrameSize() const override { return _dstFrameSize; }

        void convert(const void *in, size_t inLen,
                     std::vector<uint8_t> &out) override {
            if(!_valid || inLen == 0 || _srcFrameSize <= 0) {
                return;
            }

            const uint8_t *inBytes = static_cast<const uint8_t *>(in);
            size_t inRemaining = inLen;
            while(inRemaining >= static_cast<size_t>(_srcFrameSize)) {
                ma_uint64 framesIn =
                    inRemaining / static_cast<size_t>(_srcFrameSize);
                ma_uint64 framesOut = framesIn + 256;
                if(ma_data_converter_get_expected_output_frame_count(
                       &_converter, framesIn, &framesOut) != MA_SUCCESS) {
                    framesOut = framesIn + 256;
                }

                _outBuf.resize(static_cast<size_t>(framesOut) *
                               static_cast<size_t>(_dstFrameSize));

                ma_uint64 framesInAvail = framesIn;
                ma_uint64 framesOutAvail = framesOut;
                ma_data_converter_process_pcm_frames(
                    &_converter, inBytes, &framesInAvail, _outBuf.data(),
                    &framesOutAvail);

                const size_t bytesConsumed = (framesIn - framesInAvail) *
                    static_cast<size_t>(_srcFrameSize);
                inBytes += bytesConsumed;
                inRemaining -= bytesConsumed;

                const size_t bytesProduced =
                    framesOutAvail * static_cast<size_t>(_dstFrameSize);
                out.insert(out.end(), _outBuf.begin(),
                           _outBuf.begin() + bytesProduced);

                if(framesInAvail == framesIn) {
                    break;
                }
            }
        }

    private:
        ma_data_converter _converter{};
        bool _valid = false;
        int _srcFrameSize = 0;
        int _dstFrameSize = 0;
        std::vector<uint8_t> _outBuf;
    };

} // namespace

ITVPAudioConverter *CreateTVPAudioConverter(TVPAudioSampleFormat srcFmt,
                                            uint32_t srcCh, uint32_t srcRate,
                                            TVPAudioSampleFormat dstFmt,
                                            uint32_t dstCh, uint32_t dstRate) {
    if(srcFmt == TVPAudioSampleFormat::Unknown ||
       dstFmt == TVPAudioSampleFormat::Unknown) {
        return nullptr;
    }

    auto *converter = new TVPAudioConverterMiniaudio(srcFmt, srcCh, srcRate,
                                                     dstFmt, dstCh, dstRate);
    if(!converter->valid()) {
        delete converter;
        return nullptr;
    }
    return converter;
}

void DestroyTVPAudioConverter(ITVPAudioConverter *converter) {
    delete converter;
}
