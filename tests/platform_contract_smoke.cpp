#include "platform/HeadlessPlatform.h"

#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "platform_contract_smoke: " << message << "\n";
    return false;
  }
  return true;
}

class ScriptedInputSource final : public vkpt::platform::IInputSource {
 public:
  std::size_t poll(std::vector<vkpt::platform::InputEvent>& out) override {
    out.clear();
    if (consumed_) {
      return 0u;
    }
    consumed_ = true;
    out.push_back(vkpt::platform::InputEventNormalizer::key(65, true));
    return out.size();
  }

 private:
  bool consumed_ = false;
};

}  // namespace

int main() {
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IPlatform&>().initialize()),
                vkpt::core::Result<void>>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IPlatform&>().initialize_status()),
                vkpt::core::Status>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IPlatform&>().determinism_context()),
                vkpt::core::DeterminismContext>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IPlatform&>().status()),
                vkpt::platform::PlatformStatus>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IFileSystem&>()
                             .read_text_file(std::string_view{})),
                vkpt::core::Result<std::string>>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IWindow&>()
                             .initialize_status(1u, 1u, std::string_view{})),
                vkpt::core::Status>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IWindow&>().poll_events_status()),
                vkpt::core::Status>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IEvents&>()
                             .publish_status(std::string_view{},
                                             std::declval<const vkpt::platform::InputEvent&>())),
                vkpt::core::Status>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IInput&>()
                             .set_source_status(nullptr)),
                vkpt::core::Status>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IClipboard&>()
                             .set_text(std::string_view{})),
                vkpt::core::Result<void>>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::platform::IClipboard&>().get_text()),
                vkpt::core::Result<std::string>>);

  vkpt::platform::HeadlessPlatform platform;
  const auto determinism =
      vkpt::core::MakeDeterminismContext(true, 0xBADC0DEu, 42u, "platform-smoke");
  platform.set_determinism(determinism);
  if (!Check(platform.determinism_context() == determinism,
             "platform should retain DeterminismContext")) {
    return 1;
  }

  if (!Check(static_cast<bool>(platform.initialize()),
             "headless platform should initialize")) {
    return 1;
  }

  auto status = platform.status();
  if (!Check(status.initialized &&
                 status.lifecycle ==
                     vkpt::core::contracts::ComponentLifecycle::Ready &&
                 status.headless &&
                 status.window_open &&
                 status.input_focused &&
                 status.vsync_mode == "headless" &&
                 status.determinism_context() == determinism &&
                 status.current_flow_id >= determinism.frame_index,
             "platform status should expose window, focus, vsync, and determinism state")) {
    return 1;
  }
  auto platform_probe = vkpt::platform::CreatePlatformHealthProbe(
      [&platform]() { return platform.status(); });
  if (!Check(platform_probe->name() == "platform" &&
                 platform_probe->check().status ==
                     vkpt::core::health::Status::Ok,
             "platform health probe should report initialized headless platform")) {
    return 1;
  }

  vkpt::platform::SetUiTelemetryBackend("headless");
  auto ui_probe = vkpt::platform::CreateUiHealthProbe(
      []() { return vkpt::platform::GetUiStatus(); });
  auto ui_status = vkpt::platform::GetUiStatus();
  if (!Check(ui_probe->name() == "app/ui" &&
                 ui_probe->check().status == vkpt::core::health::Status::Ok,
             "UI health probe should report a configured UI backend") ||
      !Check(ui_status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Ready &&
                 ui_status.backend == "headless" &&
                 ui_status.determinism_context() == determinism,
             "UI status should expose lifecycle, backend, and determinism context")) {
    return 1;
  }

  auto source = std::make_shared<ScriptedInputSource>();
  platform.input()->set_source(source);
  std::vector<vkpt::platform::InputEvent> input;
  const auto sourced = platform.input()->consume(input);
  if (!Check(sourced == 1u &&
                 input.size() == 1u &&
                 input.front().type == vkpt::platform::InputEventType::KeyDown &&
                 input.front().code == 65,
             "input source should feed deterministic playback events")) {
    return 1;
  }

  platform.events()->publish("smoke", vkpt::platform::InputEventNormalizer::key(66, true));
  platform.events()->publish("smoke", vkpt::platform::InputEventNormalizer::key(66, false));
  ui_status = vkpt::platform::GetUiStatus();

  std::vector<vkpt::platform::InputEvent> firstRead;
  std::vector<vkpt::platform::InputEvent> secondRead;
  std::vector<vkpt::platform::InputEvent> drained;
  const auto firstCount = platform.events()->consume(firstRead);
  const auto secondCount = platform.events()->consume(secondRead);
  const auto beforeDrain = platform.events()->status();
  const auto drainCount = platform.events()->drain(drained);
  const auto afterDrain = platform.events()->status();

  platform.shutdown();
  const auto stoppedStatus = platform.status();

  if (!Check(firstCount == 2u && secondCount == 2u,
             "event consume should be non-destructive") ||
      !Check(beforeDrain.depth == 2u &&
                 beforeDrain.high_water_mark == 2u &&
                 beforeDrain.dropped_total == 0u,
             "event status should expose depth, high-water mark, and drops") ||
      !Check(drainCount == 2u && drained.size() == 2u,
             "event drain should destructively consume queued events") ||
      !Check(afterDrain.depth == 0u &&
                 afterDrain.high_water_mark == 2u,
             "event status should retain high-water mark after drain") ||
      !Check(ui_status.current_flow_id >= 3u &&
                 ui_status.last_event_ns != 0u &&
                 ui_status.last_tick_ns == ui_status.last_event_ns &&
                 ui_status.ticks_total >= ui_status.current_flow_id,
             "UI status should expose input flow id, tick count, and last event time") ||
      !Check(stoppedStatus.current_flow_id >= determinism.frame_index &&
                 stoppedStatus.determinism_context() == determinism,
             "platform status should expose the latest UI flow id and determinism context") ||
      !Check(stoppedStatus.lifecycle ==
                 vkpt::core::contracts::ComponentLifecycle::Uninitialized,
             "platform status should return to uninitialized after shutdown")) {
    return 1;
  }

  std::cout << "platform_contract_smoke: ok\n";
  return 0;
}
