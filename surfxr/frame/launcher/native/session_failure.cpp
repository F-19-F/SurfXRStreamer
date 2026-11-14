#include "session_failure.hpp"
#include <regex>
#include <cstring>
#include <thread>

namespace surfxr::launcher {
namespace {
Json failure(const char* code, const char* title, const std::string& detail,
             const char* action, int exit_code) {
  return object({{"code",code},{"title",title},{"detail",detail},{"action",action},{"exit_code",exit_code}});
}
struct Reason { const char* code; const char* title; const char* detail; const char* action; };
constexpr Reason reasons[] = {
  {"surfxr_pc_untrusted", "Trust Frame on the PC",
   "The PC SurfXR Dashboard requires approval before starting SteamVR for this Frame.",
   "Trust the discovered Frame in PC SURFXR Devices, then reconnect. Existing PC auto-trust settings are preserved."},
  {"surfxr_connect_failed", "SURFXR PC connection failed",
   "The PC did not accept the Frame's control connection.",
   "Open the companion PC SurfXR Dashboard; check the selected link and firewall, then Find PCs and connect to start SteamVR."},
  {"surfxr_connect_timeout", "SURFXR PC connection timed out",
   "The Frame could not open the PC control connection before its deadline.",
   "Check PC SurfXR Dashboard, the selected IP/link and TCP/UDP 19943 firewall access, then reconnect to start SteamVR."},
  {"surfxr_peer_closed", "SURFXR PC ended the connection",
   "The PC closed reliable control and the bounded reconnect window ended.",
   "Open PC SurfXR Dashboard, trust this Frame in Devices and reconnect. SURFXR v21 starts SteamVR from the client."},
  {"surfxr_tcp_media_deprecated_select_udp", "SURFXR requires UDP media",
   "The PC SURFXR Streamer selected deprecated TCP media.",
   "Select UDP transport in the PC SURFXR Streamer, then reconnect with SteamVR."},
  {"surfxr_incompatible_server_version", "SURFXR protocol version mismatch",
   "The PC SURFXR Streamer's protocol does not match the selected Frame backend version.",
   "Disconnect, select the matching SURFXR v20 or v21 backend, then reconnect with SteamVR."},
  {"surfxr_handshake_timeout", "SURFXR handshake timed out",
   "The PC did not finish the SURFXR control handshake before its deadline.",
   "Check the PC IP, trust this Frame in the SURFXR client list and confirm PC SteamVR is running."},
  {"surfxr_accept_timeout", "SURFXR connection timed out",
   "The expected PC did not open the SURFXR connection before its deadline.",
   "Check the PC IP, selected link and SURFXR trusted client address, then reconnect with SteamVR."},
  {"desktop_switch_timed_out", "Desktop switch timed out",
   "No new desktop frame decoded within 10 seconds; source, keyframe and stream recovery exhausted.",
   "Choose Desktop to reconnect. If it repeats, save last-session.json and check PC desktop capture."},
  {"discovery_no_response", "PC discovery timed out",
   "The selected PC did not answer the SURFXR discovery request.",
   "Use Find PCs or enter the current PC IP. Confirm SurfXR Dashboard is listening on the selected WiFi / 6 GHz link."},
  {"tcp_connect_deadline", "PC connection timed out",
   "SURFXR discovery succeeded, but the streaming TCP connection did not open before its deadline.",
   "Check PC firewall access for SurfXR Dashboard and that the selected PC IP is reachable from this link."},
  {"v4l2_requires_h264_or_hevc8", "Hardware codec unsupported",
   "The incoming codec is not supported by this Frame hardware decoder; no software fallback is enabled.",
   "Select H.264 or HEVC 8-bit in SurfXR Dashboard, then reconnect."},
  {"v4l2_decoder_unavailable", "Hardware decoder unavailable",
   "The requested V4L2 hardware decoder is unavailable on Frame.",
   "Check Frame decoder device access and the installed client build. Try H.264 in SurfXR Dashboard."},
  {"hevc_device_progress_timeout", "HEVC decoder timed out",
   "The Frame HEVC hardware decoder stopped producing output before its progress deadline.",
   "Reconnect with Desktop. If repeated, try H.264 and save last-session.json for decoder diagnostics."},
  {"hevc_drain_timeout", "HEVC decoder shutdown timed out",
   "The Frame HEVC hardware decoder did not finish draining before its deadline.",
   "Try H.264 on the next connection and save last-session.json for decoder diagnostics."},
  {"v4l2_corrupt_decoded_frame", "Hardware decoder rejected a frame",
   "The Frame hardware decoder marked the decoded video frame as corrupt.",
   "Reconnect; if repeated, try H.264 or a lower bitrate and save last-session.json."},
  {"v4l2_output_not_nv12", "Unsupported hardware video output",
   "The hardware decoder returned an output format that the Frame viewer cannot display directly.",
   "Select H.264 or HEVC 8-bit in SurfXR Dashboard, then reconnect."},
  {"tcp_receive_failed", "Video connection read failed",
   "The viewer could no longer read the PC streaming TCP socket.",
   "Check the selected WiFi / 6 GHz link, then reconnect. Save last-session.json if it repeats."},
  {"receive_poll_failed", "Streaming socket check failed",
   "The Frame socket polling operation failed during streaming.",
   "Reconnect and save last-session.json if the same socket failure repeats."}
};
constexpr const char* decoder_checks[] = {
  "hevc_device_open_failed", "hevc_stateful_device_unavailable", "hevc_output_format_failed",
  "hevc_output_format_mismatch", "hevc_capture_format_failed", "hevc_capture_layout_mismatch",
  "hevc_request_buffers_failed", "hevc_buffer_count_invalid", "hevc_query_buffer_failed",
  "hevc_mapping_failed", "hevc_queue_failed", "hevc_stream_on_failed", "hevc_subscribe_failed",
  "hevc_capture_dequeue_failed", "hevc_output_dequeue_failed", "hevc_event_failed",
  "hevc_event_budget_exceeded", "hevc_unexpected_source_change", "hevc_drain_failed",
  "hevc_packet_exceeds_device_buffer", "hevc_visible_size_mismatch", "hevc_plane_count_invalid",
  "hevc_capture_buffer_too_small", "hevc_dequeue_index_invalid", "hevc_mapping_budget_exceeded",
  "hevc_frame_allocation_failed", "v4l2_decoder_open_failed", "v4l2_dimensions_below_128",
  "decoder_allocation_failed", "codec_allocation_failed", "configured_dimensions_exceed_policy",
  "decoded_dimensions_exceed_policy", "decoder_made_no_progress", "video_receive_failed",
  "video_send_failed", "invalid_h264_annexb_sequence", "invalid_hevc_annexb_sequence",
  "h264_sps_configuration_dimensions_mismatch", "unsupported_or_automatic_codec"
};
}
Json session_failure(const Json& report, int exit_code) {
  const auto receiver = report["receiver"];
  const bool desktop = receiver["stage"].text() == "surfxr_desktop";
  for (const auto& value : {report["worker_error"], receiver["error"], report["startup_error"]}) {
    if (value.text() == "surfxr_steamvr_start_failed") {
      const auto reason = receiver["steamvr_startup"]["reason"].integer();
      const char* detail = reason == 1 ? "The PC has not trusted this Frame." :
          reason == 2 ? "The PC SteamVR launcher reported a launch failure." :
          reason == 3 ? "The SURFXR driver did not become ready before the startup watchdog ended." :
          reason == 4 ? "The PC SteamVR process exited during startup." :
          "The PC reported a SteamVR startup failure.";
      return failure("surfxr_steamvr_start_failed","PC SteamVR startup failed",detail,
          "Check the PC SurfXR Dashboard and SteamVR startup status, then try SteamVR again.",exit_code);
    }
    if (value.text() == "surfxr_unified_backend_failed")
      return failure("surfxr_unified_backend_failed","PC streaming source ended",
          "The active PC media source ended unexpectedly.",
          "Check the PC SurfXR Dashboard and retry the desired streaming mode.",exit_code);
    for(const auto* code:{"surfxr_unified_control_queue_full","surfxr_unified_tracking_queue_full",
        "surfxr_unified_video_queue_full","surfxr_unified_priority_queue_full","surfxr_unified_control_timeout",
        "surfxr_unified_keepalive_timeout","surfxr_unified_unexpected_control",
        "surfxr_unified_transport_failed"})
      if(value.text()==code)return failure(code,"Unified streaming connection ended",
        "The persistent connection stopped at the local check: "+std::string(code)+".",
        "Save last-session.json and check the PC daemon Application events for the same time.",exit_code);
    if(desktop)for(const auto* code:{"surfxr_dashboard_desktop_unavailable","surfxr_dashboard_desktop_timeout","surfxr_dashboard_desktop_disabled_or_host_unavailable"})
      if(value.text()==code)return failure(code,"SURFXR desktop is unavailable",
        "The PC Dashboard could not admit a desktop session.",
        "Open SurfXR Dashboard, trust this Frame in Devices and check Desktop status, access rules and the required SYSTEM service.",exit_code);
    if(desktop)for(const auto* code:{"surfxr_receive_failed","surfxr_peer_closed","surfxr_desktop_keepalive_timeout","surfxr_desktop_control_timeout","surfxr_send_failed","surfxr_send_timeout","surfxr_desktop_audio_receive_failed"})
      if(value.text()==code)return failure(code,"SURFXR desktop connection ended",
        "The desktop connection stopped at the local check: "+std::string(code)+".",
        "Check SurfXRDesktop and its Application event log on the PC, then reconnect with Desktop; save last-session.json if repeated.",exit_code);
    for (const auto& reason : reasons) if (value.text() == reason.code) {
      std::string action=reason.action;
      return failure(reason.code,reason.title,reason.detail,action.c_str(),exit_code);
    }
    for (const auto* code : decoder_checks) if (value.text() == code)
      return failure(code,"Frame video decoder check failed","Decoding stopped at the local check: " + std::string(code) + ".",
                     "Try H.264 or HEVC 8-bit and reconnect; save last-session.json if the decoder check repeats.",exit_code);
    for (const auto* code : {"surfxr_foveation_not_supported","surfxr_hdr_not_supported","surfxr_10bit_not_supported",
                            "surfxr_encoding_gamma_not_supported","surfxr_decoder_pixel_limit","surfxr_invalid_resolution"}) if (value.text() == code)
      return failure(code,"SURFXR video configuration unsupported","The PC SURFXR configuration failed the check: " + std::string(code) + ".",
                     "Use H.264 or HEVC 8-bit, gamma 1, HDR and upstream foveated encoding off; check per-eye dimensions on PC.",exit_code);
    for (const auto* code : {"trigger_local_bind_failed","tcp_local_bind_failed"}) if (value.text() == code)
      return failure(code,"Frame network address unavailable","The selected local address could not be bound to a streaming socket.",
                     "Select the current Frame WiFi / 6 GHz address, or use an automatic local address.",exit_code);
    for (const auto* code : {"send_deadline","tcp_send_failed"}) if (value.text() == code)
      return failure(code,"PC connection write failed","The viewer could not send streaming control before the socket deadline.",
                     "Check the selected WiFi / 6 GHz link and PC reachability, then reconnect.",exit_code);
  }
  for (auto key : {"startup_error","xr_error"}) {
    const auto error = report[key].text();
    if (std::regex_match(error,std::regex("(xr|vk)[A-Za-z0-9]{1,64}: (XrResult|VkResult) -?[0-9]{1,10}")))
      return failure("frame_graphics_api_failed","Frame graphics operation failed",error,
                     "Check Frame SteamVR runtime availability. Save last-session.json with this API result.",exit_code);
    if (error == "OpenXR session loss pending" || error == "OpenXR instance loss pending")
      return failure("frame_xr_session_lost","Frame runtime session lost",
                     "The Frame OpenXR runtime reported that its session or instance is being lost.",
                     "Return to Frame home, confirm the headset runtime is available, then reconnect.",exit_code);
  }
  if (report["xr_failed"].boolean())
    return failure("frame_xr_failed","Frame display failed",
                   "The viewer stopped in the Frame OpenXR / Vulkan display module.",
                   "Check the Frame runtime and client build; save last-session.json for the graphics diagnostics.",exit_code);
  if (receiver["peer_closed"].boolean() || receiver["remote_disconnect"].boolean())
    return failure("pc_closed_stream","PC ended the streaming connection",
                   "The PC closed the streaming socket or sent a disconnect before the viewer could complete normally.",
                   "Confirm the PC Streamer session is available, then reconnect. Save last-session.json if it repeats.",exit_code);
  const auto stage = receiver["stage"].text();
  const bool known_stage = stage == "discovery" || stage == "trigger" || stage == "tcp_connect" ||
                           stage == "client_settings" || stage == "receiving" || stage == "closed" ||
                           stage == "surfxr_preflight" || stage == "surfxr_wait_server" || stage == "surfxr_handshake" ||
                           stage == "surfxr_stream_accept" || stage == "surfxr_streaming" || stage == "surfxr_desktop";
  return failure("viewer_exited","Streaming client exited",
                 known_stage ? "The viewer stopped during " + stage + "; no recognized failure reason was reported."
                             : "The viewer exited without a recognized failure report.",
                 !desktop?"Save last-session.json and the exit code shown here; check PC SURFXR and reconnect with SteamVR.":
                      "Save last-session.json and the exit code shown here; reconnect with Desktop to try again.",exit_code);
}
Json observe_start(const std::function<Json()>& read_status, const std::string& mode,
                   std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  do {
    auto current = read_status();
    if (current["state"].text() == "streaming" && current["mode"].text() == mode) return current;
    if (current.is(json_type_null) || current["state"].text() == "error" || current["state"].text() == "idle") {
      if (!current.is(json_type_object)) current = object({{"state","unavailable"}});
      current.set("ok",false); current.set("error","frame_viewer_not_streaming");
      if (current["message"].text().empty()) current.set("message","The connection service is unavailable; the observer did not stop the viewer.");
      return current;
    }
    if (Clock::now() >= deadline) {
      current.set("ok",false); current.set("error","frame_streaming_start_timeout");
      current.set("message","Timed out waiting for the requested mode. The viewer is still running; use Dashboard to select a mode or Disconnect.");
      return current;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (true);
}
}
