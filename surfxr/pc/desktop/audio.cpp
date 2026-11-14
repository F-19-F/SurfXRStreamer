#include "audio.hpp"
#include <ksmedia.h>
#include <algorithm>

namespace surfxr::transport::desktop {
Audio::Audio(const std::wstring& endpoint,bool manage_mute){
    try{
        Com<IMMDeviceEnumerator> enumerator;check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"desktop_audio_enumerator");
        Com<IMMDevice> device;check(endpoint.empty()?enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device):enumerator->GetDevice(endpoint.c_str(),&device),"desktop_audio_endpoint");
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client_.GetAddressOf())),"desktop_audio_client");
        WAVEFORMATEX* raw=nullptr;check(client_->GetMixFormat(&raw),"desktop_audio_format");std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)> format(raw,CoTaskMemFree);
        channels_=raw->nChannels;block_align_=raw->nBlockAlign;require(channels_>0&&channels_<=8&&block_align_>0&&block_align_<=32&&raw->nSamplesPerSec>=8000&&raw->nSamplesPerSec<=192000,"desktop_audio_format_unsupported");
        auto tag=raw->wFormatTag;std::uint64_t mask=0;
        if(tag==WAVE_FORMAT_EXTENSIBLE){require(raw->cbSize>=22,"desktop_audio_format_unsupported");const auto* extended=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(raw);mask=extended->dwChannelMask;
            if(IsEqualGUID(extended->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))tag=WAVE_FORMAT_IEEE_FLOAT;else if(IsEqualGUID(extended->SubFormat,KSDATAFORMAT_SUBTYPE_PCM))tag=WAVE_FORMAT_PCM;}
        AVSampleFormat sample=AV_SAMPLE_FMT_NONE;
        if(tag==WAVE_FORMAT_IEEE_FLOAT&&raw->wBitsPerSample==32)sample=AV_SAMPLE_FMT_FLT;
        else if(tag==WAVE_FORMAT_PCM&&raw->wBitsPerSample==16)sample=AV_SAMPLE_FMT_S16;
        else if(tag==WAVE_FORMAT_PCM&&raw->wBitsPerSample==32)sample=AV_SAMPLE_FMT_S32;
        require(sample!=AV_SAMPLE_FMT_NONE,"desktop_audio_sample_unsupported");AVChannelLayout in{},out=AV_CHANNEL_LAYOUT_STEREO;
        if(mask)av_channel_layout_from_mask(&in,mask);else av_channel_layout_default(&in,static_cast<int>(channels_));
        const auto allocated=swr_alloc_set_opts2(&resampler_,&out,AV_SAMPLE_FMT_S16,48000,&in,sample,static_cast<int>(raw->nSamplesPerSec),0,nullptr);av_channel_layout_uninit(&in);require(allocated>=0&&resampler_&&swr_init(resampler_)>=0,"desktop_audio_resampler");
        check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,200000,0,raw,nullptr),"desktop_audio_initialize");
        check(client_->GetService(IID_PPV_ARGS(&capture_)),"desktop_audio_capture");check(client_->Start(),"desktop_audio_start");
        if(manage_mute){check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(volume_.GetAddressOf())),"desktop_audio_volume");check(volume_->GetMute(&previous_mute_),"desktop_audio_mute_state");}
    }catch(...){swr_free(&resampler_);throw;}
}
Audio::~Audio(){if(client_)client_->Stop();if(volume_&&overridden_)volume_->SetMute(previous_mute_,nullptr);swr_free(&resampler_);}
void Audio::mute(bool mute){if(!volume_||mute==overridden_)return;check(volume_->SetMute(mute?TRUE:previous_mute_,nullptr),"desktop_audio_mute_failed");overridden_=mute;}
Bytes Audio::poll(){
    Bytes output;UINT32 count=0;check(capture_->GetNextPacketSize(&count),"desktop_audio_packet_size");
    for(unsigned i=0;count&&i<8&&output.size()<=192000-76800;++i){
      {
        BYTE* bytes=nullptr;UINT32 frames=0;DWORD flags=0;const auto result=capture_->GetBuffer(&bytes,&frames,&flags,nullptr,nullptr);check(result,"desktop_audio_get_buffer");if(result==AUDCLNT_S_BUFFER_EMPTY)break;
        struct Release {IAudioCaptureClient* c;UINT32 n;~Release(){c->ReleaseBuffer(n);}} release{capture_.Get(),frames};
        require(frames<=19200,"desktop_audio_packet_bound");if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY){swr_close(resampler_);require(swr_init(resampler_)>=0,"desktop_audio_resampler_reset");}
        {
            Bytes silence;if(flags&AUDCLNT_BUFFERFLAGS_SILENT){silence.resize(static_cast<std::size_t>(frames)*block_align_);bytes=silence.data();}
            const auto capacity=swr_get_out_samples(resampler_,static_cast<int>(frames));require(capacity>0&&capacity<=19200,"desktop_audio_output_bound");const auto at=output.size();output.resize(at+static_cast<std::size_t>(capacity)*4);
            auto* target=output.data()+at;const std::uint8_t* source=bytes;const auto converted=swr_convert(resampler_,&target,capacity,&source,static_cast<int>(frames));require(converted>=0,"desktop_audio_convert");output.resize(at+static_cast<std::size_t>(converted)*4);
        }
      }
      check(capture_->GetNextPacketSize(&count),"desktop_audio_packet_size");
    }
    require(output.size()<=192000,"desktop_audio_output_bound");return output;
}
}
