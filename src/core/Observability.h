#pragma once

// Convenience bootstrap that wires the entire observability surface (logger
// → metrics → trace → health → REPL → signal handlers) according to a
// parsed-flags struct. Subsystems can also opt to start individual modules
// themselves.
//
// Typical usage from main():
//
//   int main(int argc, char** argv) {
//     auto flags = vkpt::core::cli::Parse(argc, argv);
//     vkpt::core::obs::InitFromFlags(flags);
//     ... run app ...
//     vkpt::core::obs::Shutdown();
//     return 0;
//   }

#include <string>

#include "core/cli/Flags.h"

namespace vkpt::core::obs {

// One-shot init. Idempotent; a second call is a no-op. Spawns: logger writer
// thread, metrics heartbeat thread, health heartbeat thread. Installs signal
// handlers that dump the per-thread crash rings on SIGSEGV / SIGABRT / SIGINT.
void Init();
void InitFromFlags(const vkpt::core::cli::ObservabilityFlags& flags);

// Reverse of Init(): joins all background threads, flushes sinks, and writes
// trace + metrics output files if requested by flags.
void Shutdown();

// Path captured from --metrics-out, written on Shutdown(). Empty if unset.
std::string MetricsOutputPath();

}  // namespace vkpt::core::obs
