// HFT: the order book matching engine as a program of its own.
//
//   hft serve --socket PATH [--data DIR] [--record-limit-mb N] [--keep-sessions N]
//             [--exit-with-parent]
//       Run the engine. Another program connects to the socket, says which
//       books to run, and sends orders; see hft/wire.hpp for the messages.
//       With --data, every session is recorded there and its statistics are
//       written beside it. A recording stops growing at 1,024 MB unless told
//       otherwise (0 for no limit), and the five newest sessions are kept.
//
//   hft simulate [--messages N] [--symbols N] [--seed N] [--data DIR] [--socket PATH]
//       Try the engine on a made-up market and print how it did. With
//       --socket it drives an HFT that is already running, through its
//       socket; without, one inside this program, which measures the engine
//       alone.
//
//   hft verify RECORDING [FINGERPRINT]
//       Replay a recorded session from nothing and print the fingerprint of
//       what the engine did. Given the fingerprint from the session's
//       statistics, say whether the replay matched it.
#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hft/client.hpp"
#include "hft/core.hpp"
#include "hft/server.hpp"
#include "hft/simulator.hpp"
#include "hft/stats.hpp"

namespace {

hft::Server* g_server = nullptr;

void on_signal(int) {
  if (g_server != nullptr) {
    g_server->request_stop();
  }
}

// The command line as "--name value" pairs and bare words.
struct Arguments {
  std::vector<std::string_view> words;
  std::vector<std::pair<std::string_view, std::string_view>> named;
  std::vector<std::string_view> flags;

  Arguments(int argc, char** argv) {
    static constexpr std::string_view kFlags[] = {"--exit-with-parent", "--json"};
    for (int index = 2; index < argc; ++index) {
      const std::string_view word = argv[index];
      if (!word.starts_with("--")) {
        words.push_back(word);
      } else if (std::find(std::begin(kFlags), std::end(kFlags), word) != std::end(kFlags)) {
        flags.push_back(word);
      } else if (index + 1 < argc) {
        named.emplace_back(word, argv[++index]);
      }
    }
  }

  [[nodiscard]] std::string text(std::string_view name, std::string_view otherwise = "") const {
    for (const auto& [key, value] : named) {
      if (key == name) {
        return std::string(value);
      }
    }
    return std::string(otherwise);
  }

  [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t otherwise) const {
    const std::string value = text(name);
    return value.empty() ? otherwise : std::strtoull(value.c_str(), nullptr, 10);
  }

  [[nodiscard]] bool has(std::string_view flag) const {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
  }
};

int usage() {
  std::fputs(
      "usage:\n"
      "  hft serve --socket PATH [--data DIR] [--record-limit-mb N] [--keep-sessions N]\n"
      "            [--exit-with-parent]\n"
      "  hft simulate [--messages N] [--symbols N] [--seed N] [--data DIR] [--socket PATH] [--json]\n"
      "  hft verify RECORDING [FINGERPRINT]\n",
      stderr);
  return 2;
}

int serve(const Arguments& arguments) {
  hft::ServerOptions options;
  options.socket_path = arguments.text("--socket");
  options.data_dir = arguments.text("--data");
  options.exit_with_parent = arguments.has("--exit-with-parent");
  options.record_limit_mb = arguments.number("--record-limit-mb", options.record_limit_mb);
  options.keep_sessions =
      static_cast<std::size_t>(arguments.number("--keep-sessions", options.keep_sessions));
  if (options.socket_path.empty()) {
    return usage();
  }

  hft::Server server(options);
  if (!server.listen()) {
    std::fprintf(stderr, "hft: cannot listen on %s: %s\n", options.socket_path.c_str(),
                 server.error().c_str());
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  std::fprintf(stderr, "hft: listening on %s\n", options.socket_path.c_str());
  const int result = server.run();
  g_server = nullptr;
  std::fputs("hft: stopped\n", stderr);
  return result;
}

int verify(const Arguments& arguments) {
  if (arguments.words.empty()) {
    return usage();
  }
  const std::string path(arguments.words[0]);
  const hft::Verification result = hft::verify_recording(path);
  if (!result.loaded) {
    std::fprintf(stderr, "hft: %s is not a recording this program can read\n", path.c_str());
    return 1;
  }
  const std::string fingerprint = hft::detail::hex(result.digest);
  std::printf("commands replayed   %s\n", hft::detail::grouped(result.commands).c_str());
  std::printf("events produced     %s\n", hft::detail::grouped(result.events).c_str());
  std::printf("trades              %s\n", hft::detail::grouped(result.trades).c_str());
  std::printf("fingerprint         %s\n", fingerprint.c_str());
  if (result.damaged) {
    std::puts("note: the file stops part-way through a command; everything before it was replayed");
  }
  if (arguments.words.size() > 1) {
    const bool matched = fingerprint == arguments.words[1];
    std::printf("replay %s the session\n", matched ? "matches" : "DOES NOT MATCH");
    return matched ? 0 : 1;
  }
  return 0;
}

template <typename Link>
int run_simulation(Link& link, const hft::SimulationOptions& options, const hft::Stats* stats,
                   hft::Core* core, bool json) {
  hft::Simulator<Link> simulator(link, options);
  const hft::SimulationResult result = simulator.run();
  if (!result.completed) {
    std::fputs("hft: lost contact with the engine part-way through\n", stderr);
    return 1;
  }

  if (core != nullptr) {
    core->finish();
    stats = &core->stats();
  }
  if (stats != nullptr && json) {
    std::puts(hft::to_json(*stats).c_str());
    return 0;
  }
  if (stats != nullptr) {
    std::fputs(hft::to_text(*stats).c_str(), stdout);
  }
  std::printf("\nThe simulated market\n");
  std::printf("  messages sent                     %s\n", hft::detail::grouped(result.messages).c_str());
  std::printf("  replies received                  %s\n", hft::detail::grouped(result.replies).c_str());
  std::printf("  took                              %.2f seconds\n", result.seconds);
  if (result.seconds > 0) {
    std::printf("  messages a second, end to end     %s\n",
                hft::detail::grouped(static_cast<std::uint64_t>(
                                         static_cast<double>(result.messages) / result.seconds))
                    .c_str());
  }
  std::printf("  errors                            %s\n", hft::detail::grouped(result.errors).c_str());

  // If the session was recorded, replay it and see that it comes out the same.
  if (core != nullptr && stats->recording) {
    const hft::Verification replay = hft::verify_recording(stats->recording_file);
    const bool matched =
        replay.loaded && !replay.damaged && replay.digest == stats->recorded_digest;
    std::printf("\nReplay check\n");
    std::printf("  recording                         %s\n", stats->recording_file.c_str());
    std::printf("  commands replayed                 %s\n", hft::detail::grouped(replay.commands).c_str());
    std::printf("  replay gives the same session     %s\n", matched ? "yes" : "NO");
    return matched && result.errors == 0 ? 0 : 1;
  }
  return result.errors == 0 ? 0 : 1;
}

int simulate(const Arguments& arguments) {
  hft::SimulationOptions options;
  options.messages = arguments.number("--messages", options.messages);
  options.symbols = static_cast<std::size_t>(arguments.number("--symbols", options.symbols));
  options.seed = static_cast<std::uint32_t>(arguments.number("--seed", options.seed));
  options.batch = static_cast<std::size_t>(arguments.number("--batch", options.batch));
  if (options.symbols == 0 || options.symbols > 1'000 || options.batch == 0) {
    return usage();
  }

  const std::string socket_path = arguments.text("--socket");
  if (!socket_path.empty()) {
    std::signal(SIGPIPE, SIG_IGN);
    hft::SocketLink link;
    if (!link.connect(socket_path)) {
      std::fprintf(stderr, "hft: cannot connect to %s\n", socket_path.c_str());
      return 1;
    }
    return run_simulation(link, options, nullptr, nullptr, false);
  }

  hft::Core core(hft::CoreOptions{.data_dir = arguments.text("--data")});
  hft::DirectLink link(core);
  return run_simulation(link, options, nullptr, &core, arguments.has("--json"));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string_view command = argv[1];
  const Arguments arguments(argc, argv);
  if (command == "serve") {
    return serve(arguments);
  }
  if (command == "simulate") {
    return simulate(arguments);
  }
  if (command == "verify") {
    return verify(arguments);
  }
  return usage();
}
