#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "hft/core.hpp"
#include "hft/stats.hpp"
#include "hft/wire.hpp"

// HFT's front door: a Unix domain socket that one client at a time connects
// to. A Unix socket is a connection between two programs on the same machine
// that never touches the network, so nothing outside the machine can reach it,
// and the file's permissions decide who on the machine can.
//
// The server reads whatever bytes have arrived, hands each whole message to
// Core, and writes back what Core had to say. When nothing is arriving it
// sleeps in poll(), using no CPU at all.
//
// If the client goes away, HFT keeps the books as they are and waits for it,
// or its replacement, to connect again.

namespace hft {

struct ServerOptions {
  std::string socket_path;

  // Where recordings and statistics are written. Empty keeps nothing on disk.
  std::string data_dir = {};

  // Stop when the process that started this one is gone. A desktop
  // application that is killed cannot tell its helpers to stop; with this on,
  // they notice for themselves.
  bool exit_with_parent = false;

  // How often to write the statistics files while running.
  std::chrono::seconds stats_every{5};

  // The most a session's recording may grow to, in megabytes; 0 for no limit.
  // The session goes on past it, unrecorded.
  std::uint64_t record_limit_mb = 1'024;

  // How many sessions' recordings and statistics to keep in data_dir. When a
  // new session starts, older ones beyond this are deleted; 0 keeps them all.
  std::size_t keep_sessions = 5;
};

class Server {
 public:
  explicit Server(ServerOptions options)
      : options_(std::move(options)),
        core_(CoreOptions{
            .data_dir = options_.data_dir,
            .recording_limit = options_.record_limit_mb * (std::uint64_t{1} << 20) /
                               sizeof(lob::RecordedCommand),
        }) {}

  ~Server() { close_everything(); }

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Creates the socket. Returns false, with the reason in error(), if it
  // cannot; nothing else has been disturbed.
  [[nodiscard]] bool listen() {
    sockaddr_un address{};
    if (options_.socket_path.empty() || options_.socket_path.size() >= sizeof(address.sun_path)) {
      error_ = "the socket path is empty or too long";
      return false;
    }
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return fail_with_errno("socket");
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, options_.socket_path.c_str(), options_.socket_path.size() + 1);
    // A socket file left behind by a run that was killed would stop this one
    // from binding.
    ::unlink(options_.socket_path.c_str());
    // Only this user may connect: the mask applies to the file bind() makes.
    const mode_t old_mask = ::umask(0077);
    const int bound =
        ::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    ::umask(old_mask);
    if (bound != 0) {
      return fail_with_errno("bind");
    }
    if (::listen(listen_fd_, 4) != 0) {
      return fail_with_errno("listen");
    }
    return true;
  }

  // Serves until a Shutdown message arrives, request_stop() is called, or the
  // parent goes away. Returns 0.
  int run() {
    parent_ = ::getppid();
    auto last_stats = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire) && !core_.shutdown_requested()) {
      pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {client_fd_, POLLIN, 0}};
      const int ready = ::poll(fds, client_fd_ >= 0 ? 2 : 1, 250);
      if (ready < 0 && errno != EINTR) {
        break;
      }
      if (ready > 0) {
        if ((fds[0].revents & POLLIN) != 0) {
          accept_client();
        } else if (client_fd_ >= 0 && fds[1].revents != 0) {
          read_client();
        }
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - last_stats >= options_.stats_every) {
        last_stats = now;
        write_stats();
        if (options_.exit_with_parent && ::getppid() != parent_) {
          break;
        }
      }
    }
    core_.finish();
    write_stats();
    close_everything();
    return 0;
  }

  // Safe to call from a signal handler or another thread.
  void request_stop() noexcept { stop_.store(true, std::memory_order_release); }

  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] Core& core() noexcept { return core_; }

 private:
  [[nodiscard]] bool fail_with_errno(const char* what) {
    error_ = std::string(what) + ": " + std::strerror(errno);
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    return false;
  }

  void accept_client() {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      return;
    }
    if (client_fd_ >= 0) {
      ::close(fd);  // one client at a time, and the one already here stays
      return;
    }
    // A client that stops reading must not be able to hang HFT: give up on a
    // write that has made no progress for five seconds.
    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    client_fd_ = fd;
    inbox_.clear();
    outbox_.clear();
  }

  void drop_client() {
    if (client_fd_ >= 0) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
    inbox_.clear();
    outbox_.clear();
  }

  void read_client() {
    std::byte buffer[1 << 16];
    const ssize_t got = ::recv(client_fd_, buffer, sizeof(buffer), 0);
    if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
      drop_client();  // it hung up
      return;
    }
    if (got < 0) {
      return;
    }
    inbox_.insert(inbox_.end(), buffer, buffer + got);

    // Every whole message that has arrived, in order.
    std::size_t at = 0;
    bool understood = true;
    while (understood && inbox_.size() - at >= sizeof(Header)) {
      Header header{};
      std::memcpy(&header, inbox_.data() + at, sizeof(header));
      if (header.size > kMaxPayload) {
        outbox_.send(Kind::Error, std::string_view("that message is too large"));
        understood = false;
        break;
      }
      if (inbox_.size() - at < sizeof(Header) + header.size) {
        break;  // the rest of it has not arrived yet
      }
      const auto kind = static_cast<Kind>(header.kind);
      if (kind == Kind::Hello) {
        write_stats();  // the last word on the session that Hello may be about to end
        remove_old_sessions();
      }
      understood = core_.handle(
          kind, std::span<const std::byte>(inbox_.data() + at + sizeof(Header), header.size),
          outbox_);
      at += sizeof(Header) + header.size;
      if (outbox_.bytes().size() >= kFlushAt && !send_outbox()) {
        return;
      }
    }
    inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(at));

    if (!send_outbox()) {
      return;
    }
    if (!understood) {
      drop_client();
    }
  }

  // Sends everything waiting. Returns false if the client had to be dropped.
  [[nodiscard]] bool send_outbox() {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    std::span<const std::byte> left = outbox_.bytes();
    while (!left.empty()) {
      const ssize_t sent = ::send(client_fd_, left.data(), left.size(), flags);
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent <= 0) {
        drop_client();
        return false;
      }
      left = left.subspan(static_cast<std::size_t>(sent));
    }
    outbox_.clear();
    return true;
  }

  // Writes the statistics as JSON and as text, each in two places: under a
  // fixed name, for whoever wants the latest, and beside the session's
  // recording, to be kept. Each file is written whole and then renamed into
  // place, so a reader never finds half of one.
  void write_stats() {
    if (options_.data_dir.empty() || !core_.running()) {
      return;
    }
    const Stats& stats = core_.stats();
    const std::string json = to_json(stats);
    const std::string text = to_text(stats);
    std::string session = stats.recording_file;
    if (session.size() > 4 && session.ends_with(".rec")) {
      session.resize(session.size() - 4);
    } else {
      session = options_.data_dir + "/hft-session-" + std::to_string(stats.session);
    }
    write_whole(options_.data_dir + "/hft-stats.json", json);
    write_whole(options_.data_dir + "/hft-stats.txt", text);
    write_whole(session + ".stats.json", json);
    write_whole(session + ".stats.txt", text);
  }

  // Deletes the files of all but the newest sessions, so that the directory
  // does not grow for ever. Only files this program names itself are touched:
  // "hft-session-<date>-<time>-<number>" with one of its own three endings.
  // The date and time lead the name, so the names sort oldest first.
  void remove_old_sessions() const {
    if (options_.data_dir.empty() || options_.keep_sessions == 0) {
      return;
    }
    static constexpr std::string_view kPrefix = "hft-session-";
    static constexpr std::string_view kEndings[] = {".rec", ".stats.json", ".stats.txt"};
    std::vector<std::string> sessions;  // names without their endings
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(options_.data_dir, ignored)) {
      const std::string name = entry.path().filename().string();
      for (const std::string_view ending : kEndings) {
        if (name.starts_with(kPrefix) && name.ends_with(ending)) {
          sessions.push_back(name.substr(0, name.size() - ending.size()));
        }
      }
    }
    std::sort(sessions.begin(), sessions.end());
    sessions.erase(std::unique(sessions.begin(), sessions.end()), sessions.end());
    // One fewer than asked, because a new session is about to begin.
    const std::size_t keep = options_.keep_sessions - 1;
    for (std::size_t index = 0; index + keep < sessions.size(); ++index) {
      for (const std::string_view ending : kEndings) {
        std::filesystem::remove(
            std::filesystem::path(options_.data_dir) / (sessions[index] + std::string(ending)),
            ignored);
      }
    }
  }

  static void write_whole(const std::string& path, const std::string& contents) {
    const std::string partial = path + ".part";
    if (std::FILE* const file = std::fopen(partial.c_str(), "wb")) {
      const bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
      if (std::fclose(file) == 0 && written) {
        std::rename(partial.c_str(), path.c_str());
      } else {
        std::remove(partial.c_str());
      }
    }
  }

  void close_everything() {
    drop_client();
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      ::unlink(options_.socket_path.c_str());
    }
  }

  static constexpr std::size_t kFlushAt = std::size_t{1} << 18;

  ServerOptions options_;
  Core core_;
  std::string error_;
  int listen_fd_ = -1;
  int client_fd_ = -1;
  pid_t parent_ = 0;
  std::vector<std::byte> inbox_;
  Outbox outbox_;
  std::atomic<bool> stop_{false};
};

}  // namespace hft
