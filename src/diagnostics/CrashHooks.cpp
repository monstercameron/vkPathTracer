#include "diagnostics/CrashHooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "diagnostics/CrashRecorder.h"
#include "core/Logging.h"

// Keep the crash artifact directory in a static buffer so signal handlers can
// access it without heap allocation.
namespace {
static char g_crashDir[512] = "artifacts/crashes";
}

namespace vkpt::diagnostics {

// ---- Shared crash handler body (called from every handler) ------------------

static void handle_crash_signal(const char* signal_name) noexcept {
  // Attempt to log via the structured logger; may fail if heap is corrupt.
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Fatal, "crash-hooks",
        std::string("crash signal: ") + signal_name);
  } catch (...) {}

  std::fprintf(stderr, "\n[crash-hooks] signal: %s — writing crash artifacts...\n",
               signal_name);
  std::fflush(stderr);

  // Write crash artifacts (best-effort; may throw and get caught internally).
  CrashRecorder::instance().set_last_error(signal_name);
  const std::string dir = CrashRecorder::instance().flush(g_crashDir);
  if (!dir.empty()) {
    std::fprintf(stderr, "[crash-hooks] artifacts written to: %s\n", dir.c_str());
  } else {
    std::fprintf(stderr, "[crash-hooks] artifact write failed.\n");
  }
  std::fflush(stderr);
}

// ============================================================================
// Windows implementation
// ============================================================================
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static LONG WINAPI PtUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
  (void)info;
  handle_crash_signal("UNHANDLED_EXCEPTION");
  // Continue with the default OS behavior (which typically terminates).
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_hooks(const std::string& crash_artifact_dir) {
  const auto len = std::min(crash_artifact_dir.size(), sizeof(g_crashDir) - 1u);
  std::memcpy(g_crashDir, crash_artifact_dir.data(), len);
  g_crashDir[len] = '\0';

  SetUnhandledExceptionFilter(PtUnhandledExceptionFilter);

  // Make abort() trigger the exception filter on Windows instead of just
  // closing the window silently.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

// ============================================================================
// POSIX implementation
// ============================================================================
#else  // !_WIN32

#include <signal.h>

static void posix_signal_handler(int sig) noexcept {
  const char* name = "UNKNOWN";
  switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGFPE:  name = "SIGFPE";  break;
    case SIGILL:  name = "SIGILL";  break;
#if defined(SIGBUS)
    case SIGBUS:  name = "SIGBUS";  break;
#endif
  }
  handle_crash_signal(name);

  // Restore default handler so the OS generates a proper core dump.
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, nullptr);
  raise(sig);
}

void install_crash_hooks(const std::string& crash_artifact_dir) {
  const auto len = std::min(crash_artifact_dir.size(), sizeof(g_crashDir) - 1u);
  std::memcpy(g_crashDir, crash_artifact_dir.data(), len);
  g_crashDir[len] = '\0';

  const int signals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL
#if defined(SIGBUS)
    , SIGBUS
#endif
  };
  struct sigaction sa{};
  sa.sa_handler = posix_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;  // restore default after first signal

  for (int sig : signals) {
    sigaction(sig, &sa, nullptr);
  }
}

#endif  // _WIN32

}  // namespace vkpt::diagnostics
