#include "app/AppRuntime.h"

#include "core/ExceptionBoundary.h"

int main(int argc, char** argv) {
  return vkpt::core::RunWithExceptionBoundary(
      "ptapp", "RunApp", [&]() { return vkpt::app::RunApp(argc, argv); });
}
