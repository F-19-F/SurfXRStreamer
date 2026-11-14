#pragma once
#include "windows.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
extern "C" {
#include <libswresample/swresample.h>
}

namespace surfxr::transport::desktop {
class Audio {
public:
    explicit Audio(const std::wstring& endpoint={},bool manage_mute=false);~Audio();
    Bytes poll();
    void mute(bool);
private:
    Com<IAudioClient> client_;Com<IAudioCaptureClient> capture_;
    SwrContext* resampler_=nullptr;unsigned channels_=0,block_align_=0;
    Com<IAudioEndpointVolume> volume_;BOOL previous_mute_=FALSE;bool overridden_=false;
};
}
