#include "dashboard_model.hpp"

namespace surfxr::dashboard {
Status request_payload(const std::string& directory, const std::string& payload) {
  try {
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (fd.get() < 0) throw std::runtime_error("The connection service is unavailable.");
    const auto addr = address(directory + "/control.sock");
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
      throw std::runtime_error("The connection service is unavailable.");
    ucred peer{};
    socklen_t peer_size = sizeof(peer);
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
        peer.uid != ::getuid())
      throw std::runtime_error("Cannot verify the connection service.");
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    size_t sent = 0;
    std::string response;
    while (Clock::now() < deadline && !stop_requested) {
      const int timeout = static_cast<int>(std::max<int64_t>(1,
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
      pollfd p{fd.get(), static_cast<short>(sent < payload.size() ? POLLOUT : POLLIN), 0};
      if (::poll(&p, 1, timeout) <= 0) continue;
      if (sent < payload.size()) {
        const ssize_t n = ::send(fd.get(), payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += static_cast<size_t>(n);
        else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
      } else {
        char buffer[1024];
        const ssize_t n = ::recv(fd.get(), buffer, sizeof(buffer), 0);
        if (n > 0) {
          response.append(buffer, static_cast<size_t>(n));
          if (response.size() > 8192) throw std::runtime_error("The service response is too large.");
          const size_t end = response.find('\n');
          if (end != std::string::npos) return parse_status(response.substr(0, end + 1));
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) break;
      }
      if (p.revents & (POLLERR | POLLNVAL)) break;
    }
    throw std::runtime_error("The connection service did not respond.");
  } catch (const std::exception& e) {
    Status s;
    s.message = e.what();
    return s;
  }
}

Status request(const std::string& directory, const char* command, const char* mode) {
  std::string payload = std::string("{\"command\":\"") + command + "\"";
  if (std::strcmp(command, "start") == 0) payload += std::string(",\"mode\":\"") + mode + "\"";
  return request_payload(directory, payload + "}\n");
}

Status save_connection(const std::string& directory, const char* profile, const ConnectionChoice& choice) {
  json_object* body = json_object_new_object();
  if (!body) return Status{};
  json_object_object_add(body, "command", json_object_new_string("config_set"));
  json_object_object_add(body, "profile", json_object_new_string(profile));
  json_object_object_add(body, "host", json_object_new_string(choice.host.c_str()));
  json_object_object_add(body, "local_address", json_object_new_string(choice.local_address.c_str()));
  const std::string payload = std::string(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN)) + "\n";
  json_object_put(body);
  return request_payload(directory, payload);
}



Status save_quality(const std::string& directory, size_t row, double value, bool local) {
  constexpr std::array<const char*, 6> names{{"vr_quality", "vr_fps", "vr_bitrate_mbps", "foveated_streaming", "", "desktop_pointer_priority"}};
  constexpr std::array<const char*, 5> local_names{{"desktop_filter", "vr_upscaling", "vr_sharpening_enabled", "vr_sharpening", "vr_timewarp"}};
  json_object* body = json_object_new_object();
  if (!body) return Status{};
  json_object_object_add(body, "command", json_object_new_string(local ? "image_quality_set" : "settings_set"));
  json_object_object_add(body, "setting", json_object_new_string(local ? local_names.at(row) : names.at(row)));
  json_object_object_add(body, "value", json_object_new_int64(static_cast<int64_t>(std::llround(value))));
  const std::string payload = std::string(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN)) + "\n";
  json_object_put(body);
  return request_payload(directory, payload);
}

std::string input_preference_payload(unsigned id,int value) {
  if(id>=kInputPreferenceCount)throw std::runtime_error("Invalid input preference.");
  const auto preference=static_cast<InputPreferenceId>(id);
  const auto* spec=input_preference_spec(preference);
  if(!spec||!valid_input_preference_change({preference,value}))throw std::runtime_error("Invalid input preference.");
  return std::string("{\"command\":\"input_preferences_set\",\"setting\":\"")+spec->name+"\",\"value\":"+std::to_string(value)+"}\n";
}
Status save_input_preference(const std::string& directory, int value, unsigned id) {
  return request_payload(directory,input_preference_payload(id,value));
}


} // namespace surfxr::dashboard
