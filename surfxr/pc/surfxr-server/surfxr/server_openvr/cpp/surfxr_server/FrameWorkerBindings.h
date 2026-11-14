#pragma once
#include <cstddef>

// Same-build, private IPC. Public SURFXR packet bytes are unchanged.
struct FfiFrameWorkerSource {
    unsigned int valid;
    FfiViewParams views[2];
    unsigned int crops[2][2];
    unsigned long long captureTimestampNs;
};
struct FfiFrameWorkerTransport {
    unsigned long long socket;
    unsigned long long pacingBytesPerSecond;
    unsigned int packetSize, queueCapacity;
    unsigned int fec, haptics, avoidGlitching, imageTimestamps;
    unsigned int gaze;
};
extern "C" bool FrameWorkerSource(unsigned long long timestamp, FfiFrameWorkerSource* source);
extern "C" bool FrameWorkerTransport(FfiFrameWorkerTransport* transport);
extern "C" void FrameWorkerObserve(unsigned long long timestamp, unsigned int bytes,
    bool idr, bool repeated, const unsigned char* payload, unsigned int length);
extern "C" bool FrameWorkerCaptureEnabled();
extern "C" bool FrameWorkerDaemonRequired();
extern "C" void FrameWorkerHapticsObserver(bool enabled);
extern "C" void FrameWorkerUpdateHaptics(unsigned int hand, float amplitude,
    unsigned long long durationUs);
extern "C" int FrameWorkerRun(unsigned long long mapping, unsigned long long wake);
extern "C" int FrameWorkerDaemonRun(unsigned long long mapping, unsigned long long wake, unsigned long long socket);
using FfiFrameWorkerSend=bool (*)(void*,const unsigned char*,std::size_t);
extern "C" int FrameWorkerDaemonRunDirect(unsigned long long mapping,unsigned long long wake,void* owner,FfiFrameWorkerSend send);
extern "C" bool FrameWorkerNetworkReadyDirect(void* owner,FfiFrameWorkerSend send,const FfiFrameWorkerTransport* transport);
extern "C" bool FrameWorkerNetworkReady(unsigned long long socket,
    const FfiFrameWorkerTransport* transport);
extern "C" void FrameWorkerNetworkStop();
extern "C" void FrameWorkerBeginVideo(const FfiFrameWorkerSource* source,
    unsigned long long timestamp, bool repeated);
extern "C" void FrameWorkerImportHaptics(unsigned int hand, unsigned long long serial,
    float amplitude, unsigned long long remainingUs, unsigned long long pendingSerial,
    float pendingAmplitude, unsigned long long pendingAgeUs);
extern "C" void FrameWorkerRelayLog(const char* message, unsigned int level);
extern "C" void FrameWorkerRelayVideo(unsigned long long timestamp, unsigned int bytes,
    bool idr, bool repeated, const unsigned char* payload);
extern "C" void FrameWorkerRelayConfig(const unsigned char* data, unsigned int size, int codec);
extern "C" FfiDynamicEncoderParams FrameWorkerEncoderParams();
extern "C" void FrameWorkerChildRequestIDR();
