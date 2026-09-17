// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aimrt::plugins::dds_plugin::test {

using namespace std::chrono_literals;

struct ProcessResult {
  int status = -1;
  bool process_group_established = false;
  bool term_sent = false;
  bool kill_fallback_sent = false;
  bool child_reaped = false;
  bool no_residual_process_group = false;
};

class OwnedProcessGroup {
 public:
  static std::unique_ptr<OwnedProcessGroup> Launch(
      std::string mode, std::vector<std::string> arguments = {},
      bool protocol = false) {
    int event_pipe[2] = {-1, -1};
    int control_pipe[2] = {-1, -1};
    if (protocol &&
        (pipe2(event_pipe, O_CLOEXEC) != 0 ||
         pipe2(control_pipe, O_CLOEXEC) != 0)) {
      ClosePipe(event_pipe);
      ClosePipe(control_pipe);
      return {};
    }

    sigset_t termination_signals;
    sigset_t previous_mask;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGTERM);
    sigaddset(&termination_signals, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &termination_signals, &previous_mask) != 0) {
      ClosePipe(event_pipe);
      ClosePipe(control_pipe);
      return {};
    }

    auto process = std::unique_ptr<OwnedProcessGroup>(new OwnedProcessGroup());
    const pid_t child = fork();
    if (child < 0) {
      pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
      ClosePipe(event_pipe);
      ClosePipe(control_pipe);
      return {};
    }
    if (child == 0) {
      sigprocmask(SIG_UNBLOCK, &termination_signals, nullptr);
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() == 1 || setpgid(0, 0) != 0) _exit(124);
      if (protocol) {
        close(event_pipe[0]);
        close(control_pipe[1]);
        if (!MakeInheritable(event_pipe[1]) ||
            !MakeInheritable(control_pipe[0])) {
          _exit(125);
        }
        arguments.emplace_back(std::to_string(event_pipe[1]));
        arguments.emplace_back(std::to_string(control_pipe[0]));
      }
      std::vector<std::string> owned_args{"/proc/self/exe", std::move(mode)};
      owned_args.insert(owned_args.end(), arguments.begin(), arguments.end());
      std::vector<char*> argv;
      argv.reserve(owned_args.size() + 1);
      for (auto& argument : owned_args) argv.emplace_back(argument.data());
      argv.emplace_back(nullptr);
      execv(argv.front(), argv.data());
      _exit(127);
    }

    process->child_ = child;
    process->group_ = child;
    process->finished_ = false;
    if (protocol) {
      close(event_pipe[1]);
      close(control_pipe[0]);
      process->event_fd_ = event_pipe[0];
      process->control_fd_ = control_pipe[1];
    }
    const int set_result = setpgid(child, child);
    const int set_error = errno;
    const pid_t observed_group = getpgid(child);
    process->group_established_ =
        observed_group == child &&
        (set_result == 0 || set_error == EACCES || set_error == EPERM);
    process->result_.process_group_established =
        process->group_established_;
    pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    if (!process->group_established_) return {};
    return process;
  }

  ~OwnedProcessGroup() {
    const auto result = Finish(0ms);
    if (group_ > 0 &&
        (!result.child_reaped || !result.no_residual_process_group)) {
      std::terminate();
    }
  }

  OwnedProcessGroup(const OwnedProcessGroup&) = delete;
  OwnedProcessGroup& operator=(const OwnedProcessGroup&) = delete;

  bool ReadEvent(char expected,
                 std::chrono::milliseconds timeout = 5s) const {
    std::lock_guard lock(mutex_);
    if (event_fd_ < 0) return false;
    pollfd event_poll{.fd = event_fd_, .events = POLLIN, .revents = 0};
    char event = 0;
    return poll(&event_poll, 1, static_cast<int>(timeout.count())) == 1 &&
           read(event_fd_, &event, 1) == 1 && event == expected;
  }

  bool SendControl(char control) const {
    std::lock_guard lock(mutex_);
    return control_fd_ >= 0 && WriteWithoutSigpipe(control_fd_, control);
  }

  int EventFdForTesting() const { return event_fd_; }
  int ControlFdForTesting() const { return control_fd_; }

  ProcessResult Finish(std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mutex_);
        if (finished_) return result_;
        const pid_t waited = waitpid(child_, &result_.status, WNOHANG);
        if (waited == child_) {
          child_ = -1;
          result_.child_reaped = true;
          return FinalizeLocked();
        }
        if (waited < 0 && errno != EINTR) {
          if (errno == ECHILD) child_ = -1;
          return FinalizeLocked();
        }
      }
      std::this_thread::sleep_for(10ms);
    }
    std::lock_guard lock(mutex_);
    if (finished_) return result_;
    TerminateAndReapLocked();
    return FinalizeLocked();
  }

 private:
  OwnedProcessGroup() = default;

  static bool MakeInheritable(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
  }

  static bool WriteWithoutSigpipe(int fd, char value) {
    sigset_t sigpipe;
    sigset_t previous_mask;
    sigset_t pending;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sigpipe, &previous_mask) != 0) return false;
    const bool was_pending =
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
    ssize_t written = -1;
    do {
      written = write(fd, &value, 1);
    } while (written < 0 && errno == EINTR);
    const int write_error = errno;
    if (written < 0 && write_error == EPIPE && !was_pending) {
      const timespec no_wait{};
      sigtimedwait(&sigpipe, nullptr, &no_wait);
    }
    pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    errno = write_error;
    return written == 1;
  }

  static void ClosePipe(int (&pipe_fds)[2]) {
    for (auto& fd : pipe_fds) {
      if (fd >= 0) close(fd);
      fd = -1;
    }
  }

  static void CloseFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
  }

  void TerminateAndReapLocked() noexcept {
    if (child_ > 0) {
      const int result =
          kill(group_established_ ? -group_ : child_, SIGTERM);
      result_.term_sent = result == 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (child_ > 0 && std::chrono::steady_clock::now() < deadline) {
      const pid_t waited = waitpid(child_, &result_.status, WNOHANG);
      if (waited == child_) {
        child_ = -1;
        result_.child_reaped = true;
        return;
      }
      if (waited < 0 && errno != EINTR) {
        if (errno == ECHILD) child_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    if (child_ > 0) {
      const int result =
          kill(group_established_ ? -group_ : child_, SIGKILL);
      result_.kill_fallback_sent = result == 0;
    }
    if (child_ > 0) {
      pid_t waited = -1;
      do {
        waited = waitpid(child_, &result_.status, 0);
      } while (waited < 0 && errno == EINTR);
      result_.child_reaped = waited == child_;
      child_ = -1;
    }
  }

  bool WaitForNoResidualGroup() const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    do {
      if (kill(-group_, 0) < 0 && errno == ESRCH) return true;
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return kill(-group_, 0) < 0 && errno == ESRCH;
  }

  bool EnsureNoResidualGroup() const noexcept {
    if (WaitForNoResidualGroup()) return true;
    kill(-group_, SIGTERM);
    if (WaitForNoResidualGroup()) return true;
    kill(-group_, SIGKILL);
    return WaitForNoResidualGroup();
  }

  ProcessResult FinalizeLocked() noexcept {
    CloseFd(event_fd_);
    CloseFd(control_fd_);
    result_.no_residual_process_group = EnsureNoResidualGroup();
    finished_ = true;
    return result_;
  }

  mutable std::mutex mutex_;
  pid_t child_ = -1;
  pid_t group_ = -1;
  int event_fd_ = -1;
  int control_fd_ = -1;
  bool group_established_ = false;
  bool finished_ = true;
  ProcessResult result_;
};

inline volatile sig_atomic_t g_process_test_signal_fd = -1;
inline volatile sig_atomic_t g_process_test_pending_signal = 0;

inline void ProcessTestSignalHandler(int signal) {
  const int saved_errno = errno;
  g_process_test_pending_signal = signal;
  const int fd = g_process_test_signal_fd;
  if (fd >= 0) {
    const unsigned char value = static_cast<unsigned char>(signal);
    const ssize_t ignored = write(fd, &value, sizeof(value));
    (void)ignored;
  }
  errno = saved_errno;
}

class ProcessTestScope {
 public:
  explicit ProcessTestScope(std::chrono::milliseconds timeout)
      : deadline_(std::chrono::steady_clock::now() + timeout) {
    if (pipe2(signal_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) return;
    g_process_test_pending_signal = 0;
    g_process_test_signal_fd = signal_pipe_[1];
    struct sigaction action {};
    action.sa_handler = ProcessTestSignalHandler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, &previous_sigterm_) != 0) {
      CloseSignalPipe();
      return;
    }
    if (sigaction(SIGINT, &action, &previous_sigint_) != 0) {
      sigaction(SIGTERM, &previous_sigterm_, nullptr);
      CloseSignalPipe();
      return;
    }
    active_ = true;
    try {
      signal_waiter_ = std::thread([this] {
        while (!stop_signal_waiter_.load()) {
          const int pending = g_process_test_pending_signal;
          if (pending == SIGTERM || pending == SIGINT) {
            received_signal_.store(pending);
            return;
          }
          pollfd signal_poll{
              .fd = signal_pipe_[0], .events = POLLIN, .revents = 0};
          const int poll_result = poll(&signal_poll, 1, 50);
          if (poll_result < 0 && errno != EINTR) {
            signal_wait_failed_.store(true);
            return;
          }
          if (poll_result <= 0) continue;
          unsigned char received = 0;
          if (read(signal_pipe_[0], &received, sizeof(received)) !=
              sizeof(received)) {
            continue;
          }
          if (received == 0) return;
          if (received == SIGTERM || received == SIGINT) {
            received_signal_.store(received);
            return;
          }
        }
      });
    } catch (...) {
      active_ = false;
      g_process_test_signal_fd = -1;
      sigaction(SIGINT, &previous_sigint_, nullptr);
      sigaction(SIGTERM, &previous_sigterm_, nullptr);
      CloseSignalPipe();
      return;
    }
  }

  ~ProcessTestScope() {
    stop_signal_waiter_.store(true);
    if (signal_pipe_[1] >= 0) {
      const unsigned char stop = 0;
      const ssize_t ignored = write(signal_pipe_[1], &stop, sizeof(stop));
      (void)ignored;
    }
    if (signal_waiter_.joinable()) signal_waiter_.join();
    for (auto& process : processes_) process->Finish(0ms);
    if (active_) {
      g_process_test_signal_fd = -1;
      sigaction(SIGINT, &previous_sigint_, nullptr);
      sigaction(SIGTERM, &previous_sigterm_, nullptr);
    }
    CloseSignalPipe();
  }

  ProcessTestScope(const ProcessTestScope&) = delete;
  ProcessTestScope& operator=(const ProcessTestScope&) = delete;

  bool Ready() const { return active_ && !signal_wait_failed_.load(); }
  int ReceivedSignal() const { return received_signal_.load(); }
  bool Expired() const {
    return std::chrono::steady_clock::now() >= deadline_;
  }

  std::chrono::milliseconds Remaining(
      std::chrono::milliseconds maximum) const {
    if (ReceivedSignal() != 0 || signal_wait_failed_.load()) return 0ms;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) return 0ms;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
    return std::min(maximum, remaining);
  }

  OwnedProcessGroup* Launch(std::string mode,
                            std::vector<std::string> arguments = {},
                            bool protocol = false) {
    if (!Ready() || Remaining(1ms) == 0ms) return nullptr;
    auto process = OwnedProcessGroup::Launch(std::move(mode),
                                             std::move(arguments), protocol);
    if (!process) return nullptr;
    auto* pointer = process.get();
    processes_.emplace_back(std::move(process));
    return pointer;
  }

  bool ReadEvent(OwnedProcessGroup* process, char expected,
                 std::chrono::milliseconds maximum = 5s) const {
    const auto timeout = Remaining(maximum);
    return process != nullptr && timeout > 0ms &&
           process->ReadEvent(expected, timeout);
  }

  bool SendControl(OwnedProcessGroup* process, char control) const {
    return ReceivedSignal() == 0 && process != nullptr &&
           process->SendControl(control);
  }

  ProcessResult Finish(OwnedProcessGroup* process,
                       std::chrono::milliseconds maximum = 8s) const {
    if (process == nullptr) return {};
    return process->Finish(Remaining(maximum));
  }

 private:
  void CloseSignalPipe() {
    g_process_test_signal_fd = -1;
    g_process_test_pending_signal = 0;
    for (auto& fd : signal_pipe_) {
      if (fd >= 0) close(fd);
      fd = -1;
    }
  }

  std::chrono::steady_clock::time_point deadline_;
  int signal_pipe_[2] = {-1, -1};
  struct sigaction previous_sigterm_ {};
  struct sigaction previous_sigint_ {};
  std::atomic_int received_signal_ = 0;
  std::atomic_bool stop_signal_waiter_ = false;
  std::atomic_bool signal_wait_failed_ = false;
  bool active_ = false;
  std::thread signal_waiter_;
  std::vector<std::unique_ptr<OwnedProcessGroup>> processes_;
};

}  // namespace aimrt::plugins::dds_plugin::test
