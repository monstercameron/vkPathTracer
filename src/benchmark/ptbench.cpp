#include "benchmark/BenchmarkCli.h"

#include "core/ExceptionBoundary.h"

int main(int argc, char** argv) {
  return vkpt::core::RunWithExceptionBoundary(
      "ptbench",
      "RunBenchmarkCli",
      [&]() { return vkpt::benchmark::ptbench::RunBenchmarkCli(argc, argv); });
}
