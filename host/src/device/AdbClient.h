#pragma once

// Async wrapper around adb.exe.
// We never link against adb's libs — we spawn the extracted adb.exe and read
// stdout/stderr through pipes. One AdbClient owns a worker thread that
// serialises commands, so callers can fire-and-forget from the UI thread.
// Typical use:
//   auto& adb = cr::device::AdbClient::instance();
//   adb.run({"devices", "-l"}, [](auto r) { /* parse r.stdout */ });
// Heavy commands (logcat, stream reads) use run_streaming() with a per-line
// callback and a stop token.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cr::device
{

struct AdbResult
{
  int exit_code = -1; // -1 = failed to spawn
  std::string stdout_;
  std::string stderr_;

  // Convenience: true if process exited with 0 and stdout isn't empty.
  bool ok_with_output() const noexcept
  {
    return exit_code == 0 && !stdout_.empty();
  }
};

// Handle for a streaming (long-running) command. Destructor stops the stream.
class AdbStream
{
public:
  // Special members out-of-line because Impl is only forward-declared here
  // — unique_ptr<Impl> needs the full type at instantiation time.
  AdbStream();
  ~AdbStream();
  AdbStream(AdbStream&&) noexcept;
  AdbStream& operator=(AdbStream&&) noexcept;
  AdbStream(const AdbStream&) = delete;
  AdbStream& operator=(const AdbStream&) = delete;

  void stop();
  bool is_running() const noexcept;

private:
  friend class AdbClient;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class AdbClient
{
public:
  static AdbClient& instance();

  // Point adb at the extracted binary from EmbeddedFs. Call once from
  // App::ensure_host_runtime_ready() after extract_adb_suite().
  void set_adb_path(std::filesystem::path p);

  // --- blocking forms (internal / tests) ----------------------------------
  // These block the calling thread while adb runs; intended for the worker,
  // not the UI. Most callers should prefer run() (async).
  AdbResult run_sync(const std::vector<std::string>& args, int timeout_ms = 0);
  AdbResult run_sync_to(const std::string& serial, const std::vector<std::string>& args);

  // --- async form — the normal path ---------------------------------------
  // `on_done` is invoked on the worker thread; marshal to UI yourself.
  using Callback = std::function<void(AdbResult)>;
  void run(const std::vector<std::string>& args, Callback on_done);
  void run_to(const std::string& serial, const std::vector<std::string>& args, Callback on_done);

  // --- streaming ----------------------------------------------------------
  // `on_line` is called from a dedicated reader thread for every \n-line of
  // stdout. `on_exit` (optional) fires once when adb exits.
  using LineCallback = std::function<void(std::string)>;
  using ExitCallback = std::function<void(int exit_code)>;
  AdbStream run_streaming(const std::vector<std::string>& args, LineCallback on_line, ExitCallback on_exit = {});
  AdbStream run_streaming_to(const std::string& serial, const std::vector<std::string>& args, LineCallback on_line, ExitCallback on_exit = {});

  // High-level helpers (build on top of run).

  // adb start-server / kill-server — safe to call multiple times.
  void start_server();
  void kill_server();
  AdbResult kill_server_sync(int timeout_ms = 10000);
  void clear_adb_path();

  // Build the argv for a `su -c <cmd>` invocation that survives `adb shell`'s
  // argv-joining quirk. adb concatenates its trailing args with single spaces
  // and sends the result as one shell string to the device, so a command
  // like `su -c killall; rm file; true` ends up being parsed device-side as
  // three separate commands — two of which (rm, true) run as the *shell*
  // uid, not root. Packaging the whole thing as a single single-quoted
  // argument keeps su as the parent process for every command in the chain.
  // Returns the argv you pass to run() / run_sync_to().
  static std::vector<std::string> su_shell(std::string cmd);

  // Default args to spawn adb.exe (handy if a sub-system needs raw argv).
  const std::filesystem::path& adb_path() const noexcept
  {
    return adb_path_;
  }

private:
  AdbClient();
  ~AdbClient();
  AdbClient(const AdbClient&) = delete;
  AdbClient& operator=(const AdbClient&) = delete;

  struct Task
  {
    std::vector<std::string> args;
    Callback cb;
  };
  void worker_loop();
  void submit(Task t);

  std::filesystem::path adb_path_;
  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  std::atomic<bool> running_{true};
};

} // namespace cr::device
