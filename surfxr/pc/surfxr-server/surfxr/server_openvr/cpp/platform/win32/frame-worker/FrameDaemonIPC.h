#pragma once
#include <cstdint>
namespace frame_worker {
constexpr wchar_t DaemonPipe[]=L"\\\\.\\pipe\\SteamFrameSURFXRDaemonWorker-v1";
struct DaemonRequest {uint32_t magic=0x46445731,bytes=sizeof(DaemonRequest);uint64_t mapping=0,wake=0;};
struct DaemonReply {uint32_t magic=0x46445731,pid=0;};
}
