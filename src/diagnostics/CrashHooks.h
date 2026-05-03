#pragma once

#include <string>

namespace vkpt::diagnostics {

// ---- CrashHooks -------------------------------------------------------------
// Install OS-level crash handlers that:
//   1. Write a crash artifact via CrashRecorder::flush().
//   2. Flush the structured logger.
//   3. Re-raise / continue the default OS crash path.
//
// Call install_crash_hooks() once at application startup, before any threads
// are created.  The handlers are deliberately minimal and avoid heap allocation
// to stay safe in the crash signal context.
//
// Supported platforms:
//   Windows — SEH unhandled exception filter + SetAbortBehavior
//   POSIX   — SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS signal handlers

void install_crash_hooks(const std::string& crash_artifact_dir = "artifacts/crashes");

}  // namespace vkpt::diagnostics
