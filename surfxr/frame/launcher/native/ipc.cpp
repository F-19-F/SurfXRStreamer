#include "controller.hpp"
#include "network_monitor.hpp"
#include <cerrno>
#include <cstring>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace surfxr::launcher {
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop_signal(int) { stopped = 1; }
sockaddr_un address(const fs::path& path) {
  sockaddr_un value{}; value.sun_family = AF_UNIX;
  const auto text = path.string(); if (text.size() >= sizeof(value.sun_path)) throw std::runtime_error("socket_path_too_long");
  std::memcpy(value.sun_path, text.c_str(), text.size()+1); return value;
}
void peer(int fd) {
  ucred credentials{}; socklen_t size = sizeof(credentials);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || credentials.uid != ::getuid()) throw std::runtime_error("peer_user_mismatch");
}
std::string receive(int fd, size_t limit, int milliseconds) {
  std::string data; const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
  while (data.size() < limit && Clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 10) <= 0) continue;
    char buffer[1024]; const auto n = ::recv(fd, buffer, std::min(sizeof(buffer), limit-data.size()), MSG_DONTWAIT);
    if (n <= 0) { if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue; break; }
    data.append(buffer, static_cast<size_t>(n));
    const auto end = data.find('\n'); if (end != std::string::npos) return data.substr(0, end);
  }
  throw std::runtime_error("invalid_ipc_message");
}
void send(int fd, const std::string& data) {
  if (data.size() > 8192) throw std::runtime_error("ipc_response_too_large");
  size_t offset = 0; const auto deadline = Clock::now()+std::chrono::milliseconds(250);
  while (offset < data.size() && Clock::now() < deadline) {
    const auto n = ::send(fd, data.data()+offset, data.size()-offset, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) offset += static_cast<size_t>(n);
    else if (n < 0 && (errno == EINTR || errno == EAGAIN)) { pollfd p{fd,POLLOUT,0}; (void)::poll(&p,1,10); }
    else throw std::runtime_error("ipc_send_failed");
  }
  if (offset != data.size()) throw std::runtime_error("ipc_send_timeout");
}
}
Json ipc(const Paths& paths, const Json& request) {
  Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  const auto addr = address(paths.runtime/"control.sock");
  if (fd.get() < 0 || ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))) throw std::runtime_error("connection_service_unavailable");
  peer(fd.get()); send(fd.get(), request.dump()+"\n"); return Json::parse(receive(fd.get(),8192,1000));
}
void viewer_command(const Paths& paths, const std::string& command) {
  Fd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  auto addr = address(paths.runtime/"viewer-command.sock");
  if (fd.get() < 0 || ::sendto(fd.get(), command.data(), command.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != static_cast<ssize_t>(command.size())) throw std::runtime_error("viewer_not_ready");
}
int serve(const Paths& paths, std::function<Json()> network_reader) {
  NetworkMonitor monitor;
  const bool probe_network = !network_reader;
  if (probe_network) { monitor.poll(); network_reader = [&monitor] { monitor.poll(); return monitor.snapshot(); }; }
  ::umask(0077); private_directory(paths.runtime); Lock lock(paths.runtime/"service.lock");
  auto endpoint = paths.runtime/"control.sock";
  struct stat st{};
  if (::lstat(endpoint.c_str(), &st) == 0 && (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())) throw std::runtime_error("unexpected_ipc_endpoint");
  fs::remove(endpoint);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove(path,error); } } cleanup{endpoint};
  Fd server(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  const auto addr = address(endpoint);
  if (server.get() < 0 || ::bind(server.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) || ::chmod(endpoint.c_str(),0600) || ::listen(server.get(),8)) throw std::runtime_error("ipc_service_failed");
  stopped = 0; ::signal(SIGINT,stop_signal); ::signal(SIGTERM,stop_signal); ::signal(SIGPIPE,SIG_IGN);
  Controller manager(paths,std::move(network_reader));
  while (!stopped) {
    if (probe_network) monitor.poll();
    manager.poll(); pollfd poller{server.get(), POLLIN, 0}; if (::poll(&poller,1,50) <= 0) continue;
    Fd connection(::accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (connection.get() < 0) continue;
    try { peer(connection.get()); auto response = manager.request(Json::parse(receive(connection.get(),1024,250))); send(connection.get(),response.dump()+"\n"); } catch (...) {}
  }
  manager.shutdown(); return 0;
}
}
