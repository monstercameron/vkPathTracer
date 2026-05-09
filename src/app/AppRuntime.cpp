#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "build_info.generated.h"
#include "core/Assert.h"
#include "core/Config.h"
#include "core/ExecutionTrace.h"
#include "core/Logging.h"
#include "core/contracts/Determinism.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/metrics/UiInputLatency.h"
#include "core/repl/Repl.h"
#include "benchmark/BenchmarkSchema.h"
#include "app/AppBenchmarkActions.h"
#include "app/AppEditorWorldSupport.h"
#include "app/AppRuntime.h"
#include "app/AppOptions.h"
#include "app/AppRuntimeSupport.h"
#include "audio/AudioSystem.h"
#include "app/DoctorChecks.h"
#include "app/RuntimeMode.h"
#include "app/UiValidation.h"
#include "editor/UiModels.h"
#include "diagnostics/CrashHooks.h"
#include "diagnostics/CrashRecorder.h"
#include "diagnostics/StatusFile.h"
#include "pathtracer/ImageIo.h"
#include "pathtracer/PathTracer.h"
#include "cpu/TiledCpuPathTracer.h"
#ifdef PT_ENABLE_VULKAN
#include "gpu/VulkanGpuPathTracer.h"
#endif
#ifdef PT_ENABLE_D3D12
#include "gpu/D3D12GpuPathTracer.h"
#endif
#if defined(PT_ENABLE_RAW_DESKTOP)
#include "platform/DesktopPlatform.h"
#endif
#ifdef PT_ENABLE_QT
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>
#include "platform/qt/QtPlatform.h"
#include "app/AppQtSceneDocumentActions.h"
#include "app/QtDockPanels.h"
#include "app/ViewportInteraction.h"
#include "assets/SceneAssetLoader.h"
#endif
#include "platform/PlatformFactory.h"
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"
#include "scene/SceneScriptBootstrap.h"
#include "scene/SnapshotRing.h"
#include "scripting/ScriptRuntime.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/D3D12Backend.h"
#include "render/backends/VulkanBackend.h"
#include "render/RenderCoordinator.h"
#include "render/interface/RenderContracts.h"
#include "jobs/JobSystem.h"

namespace {

using vkpt::app::RunDoctor;
using vkpt::app::RunUiModelSmokeTests;
using vkpt::app::RunUiReleaseGateCheck;

#ifdef _WIN32
class ScopedWindowsTimerResolution {
 public:
  explicit ScopedWindowsTimerResolution(UINT period_ms) : m_periodMs(period_ms) {
    m_winmm = LoadLibraryA("winmm.dll");
    if (m_winmm == nullptr) {
      return;
    }
    m_begin = reinterpret_cast<TimePeriodFn>(GetProcAddress(m_winmm, "timeBeginPeriod"));
    m_end = reinterpret_cast<TimePeriodFn>(GetProcAddress(m_winmm, "timeEndPeriod"));
    if (m_begin != nullptr && m_end != nullptr && m_begin(m_periodMs) == 0u) {
      m_active = true;
    }
  }

  ScopedWindowsTimerResolution(const ScopedWindowsTimerResolution&) = delete;
  ScopedWindowsTimerResolution& operator=(const ScopedWindowsTimerResolution&) = delete;

  ~ScopedWindowsTimerResolution() {
    if (m_active && m_end != nullptr) {
      (void)m_end(m_periodMs);
    }
    if (m_winmm != nullptr) {
      FreeLibrary(m_winmm);
    }
  }

  bool active() const {
    return m_active;
  }

 private:
  using TimePeriodFn = UINT(WINAPI*)(UINT);

  UINT m_periodMs = 1u;
  HMODULE m_winmm = nullptr;
  TimePeriodFn m_begin = nullptr;
  TimePeriodFn m_end = nullptr;
  bool m_active = false;
};
#endif

#ifdef PT_ENABLE_QT
void SleepUntilFrameTarget(std::chrono::steady_clock::time_point target) {
  while (std::chrono::steady_clock::now() < target) {
    if (QCoreApplication::instance() != nullptr) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= target) {
      break;
    }
    const auto remaining = target - now;
    if (remaining > std::chrono::milliseconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
#ifdef _WIN32
    YieldProcessor();
#endif
    std::this_thread::yield();
  }
}
#endif

struct RuntimeMeshStats {
  std::size_t instances = 0u;
  std::size_t vertices = 0u;
  std::size_t triangles = 0u;
};

vkpt::core::StableId RuntimeMaxAuthoredSceneId(const vkpt::scene::SceneDocument& document) {
  vkpt::core::StableId maxId = 0u;
  for (const auto& material : document.materials) maxId = std::max(maxId, material.id);
  for (const auto& geometry : document.geometry) maxId = std::max(maxId, geometry.id);
  for (const auto& entity : document.entities) maxId = std::max(maxId, entity.id);
  for (const auto& asset : document.assets) maxId = std::max(maxId, asset.id);
  for (const auto& emitter : document.particle_emitters) maxId = std::max(maxId, emitter.id);
  for (const auto& sdf : document.sdf_primitives) maxId = std::max(maxId, sdf.id);
  for (const auto& camera : document.cameras) maxId = std::max(maxId, camera.id);
  for (const auto& light : document.lights) maxId = std::max(maxId, light.id);
  return maxId;
}

RuntimeMeshStats RuntimeCountAuthoredMeshStats(const vkpt::scene::SceneDocument& document) {
  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneGeometryDefinition*> geometryById;
  geometryById.reserve(document.geometry.size());
  for (const auto& geometry : document.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  RuntimeMeshStats stats;
  for (const auto& entity : document.entities) {
    if (!entity.visible || !entity.has_mesh) {
      continue;
    }
    const auto found = geometryById.find(entity.mesh.mesh_id);
    if (found == geometryById.end()) {
      continue;
    }
    const auto& geometry = *found->second;
    if (geometry.vertices.empty() || geometry.indices.empty() ||
        geometry.indices.size() % 3u != 0u) {
      continue;
    }
    ++stats.instances;
    stats.vertices += geometry.vertices.size();
    stats.triangles += geometry.indices.size() / 3u;
  }
  return stats;
}

RuntimeMeshStats RuntimeAppendOffscreenCullingBallast(vkpt::scene::SceneDocument& document) {
  constexpr std::uint32_t kGrid = 64u;
  constexpr std::uint32_t kInstances = 128u;
  vkpt::core::StableId nextId =
      std::max<vkpt::core::StableId>(900000000ull, RuntimeMaxAuthoredSceneId(document) + 1u);

  vkpt::core::StableId materialId = 0u;
  if (!document.materials.empty()) {
    materialId = document.materials.front().id;
  } else {
    vkpt::scene::SceneMaterialDefinition material;
    material.id = nextId++;
    material.name = "Qt stress culling ballast";
    material.family = "diffuse";
    material.albedo = {0.45f, 0.48f, 0.52f};
    document.materials.push_back(material);
    materialId = material.id;
  }

  vkpt::scene::SceneGeometryDefinition geometry;
  geometry.id = nextId++;
  geometry.primitive = "triangle";
  geometry.material_id = materialId;
  geometry.tags.push_back("qt_stress_performance_culling_ballast");
  geometry.vertices.reserve(static_cast<std::size_t>(kGrid + 1u) * (kGrid + 1u));
  geometry.indices.reserve(static_cast<std::size_t>(kGrid) * kGrid * 6u);
  for (std::uint32_t z = 0u; z <= kGrid; ++z) {
    for (std::uint32_t x = 0u; x <= kGrid; ++x) {
      geometry.vertices.push_back(vkpt::scene::Vec3{
          (static_cast<float>(x) - static_cast<float>(kGrid) * 0.5f) * 0.5f,
          0.0f,
          (static_cast<float>(z) - static_cast<float>(kGrid) * 0.5f) * 0.5f});
    }
  }
  for (std::uint32_t z = 0u; z < kGrid; ++z) {
    for (std::uint32_t x = 0u; x < kGrid; ++x) {
      const std::uint32_t row0 = z * (kGrid + 1u);
      const std::uint32_t row1 = (z + 1u) * (kGrid + 1u);
      const std::uint32_t i0 = row0 + x;
      const std::uint32_t i1 = row0 + x + 1u;
      const std::uint32_t i2 = row1 + x;
      const std::uint32_t i3 = row1 + x + 1u;
      geometry.indices.insert(geometry.indices.end(), {i0, i2, i1, i1, i2, i3});
    }
  }

  const auto geometryId = geometry.id;
  const std::size_t verticesPerInstance = geometry.vertices.size();
  const std::size_t trianglesPerInstance = geometry.indices.size() / 3u;
  document.geometry.push_back(std::move(geometry));

  for (std::uint32_t i = 0u; i < kInstances; ++i) {
    vkpt::scene::SceneEntityDefinition entity;
    entity.id = nextId++;
    entity.name = "Qt Stress Culling Ballast " + std::to_string(i);
    entity.visible = true;
    entity.has_transform = true;
    entity.transform.translation = {
        600.0f + static_cast<float>(i % 16u) * 28.0f,
        -6.0f,
        420.0f + static_cast<float>(i / 16u) * 28.0f};
    entity.has_mesh = true;
    entity.mesh.mesh_id = geometryId;
    entity.mesh.material_id = materialId;
    document.entities.push_back(std::move(entity));
  }

  return RuntimeMeshStats{
      kInstances,
      verticesPerInstance * static_cast<std::size_t>(kInstances),
      trianglesPerInstance * static_cast<std::size_t>(kInstances)};
}

// ---- ptdoctor checks -------------------------------------------------------

std::uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::optional<std::string> ReadEnvironmentString(const char* name) {
#if defined(_WIN32) && defined(_MSC_VER)
  char* raw = nullptr;
  std::size_t rawSize = 0u;
  if (_dupenv_s(&raw, &rawSize, name) != 0 || raw == nullptr) {
    return std::nullopt;
  }
  std::string value(raw);
  std::free(raw);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
#endif
}

std::chrono::milliseconds ResolveStatusFilePeriod() {
  constexpr std::chrono::milliseconds kDefaultPeriod{5000};
  const auto value = ReadEnvironmentString("PTAPP_STATUS_FILE_PERIOD_SECONDS");
  if (!value) {
    return kDefaultPeriod;
  }
  char* end = nullptr;
  const char* begin = value->c_str();
  const unsigned long seconds = std::strtoul(begin, &end, 10);
  if (end == begin || (end != nullptr && *end != '\0') || seconds == 0ul) {
    return kDefaultPeriod;
  }
  return std::chrono::seconds(seconds);
}

constexpr std::string_view kDefaultSceneAssetPath = "assets/scenes/cornell_native.json";

vkpt::scene::SceneDocument LoadDefaultSceneDocument() {
  if (auto document = vkpt::scene::SceneDocument::load_from_file(
          std::string(kDefaultSceneAssetPath))) {
    return document.value();
  }
  vkpt::scene::SceneDocument document;
  document.metadata.scene_name = "empty";
  return document;
}

std::string RuntimeSceneDisplayName(const std::string& scenePath) {
  return scenePath.empty() ? std::string(kDefaultSceneAssetPath) : scenePath;
}

std::string_view ToString(vkpt::platform::InputEventType type) {
  switch (type) {
    case vkpt::platform::InputEventType::MenuCommand: return "menu_command";
    case vkpt::platform::InputEventType::KeyDown: return "key_down";
    case vkpt::platform::InputEventType::KeyUp: return "key_up";
    case vkpt::platform::InputEventType::MouseMove: return "mouse_move";
    case vkpt::platform::InputEventType::MouseButtonDown: return "mouse_button_down";
    case vkpt::platform::InputEventType::MouseButtonUp: return "mouse_button_up";
    case vkpt::platform::InputEventType::MouseWheel: return "mouse_wheel";
    case vkpt::platform::InputEventType::WindowResize: return "window_resize";
    case vkpt::platform::InputEventType::FocusLost: return "focus_lost";
    case vkpt::platform::InputEventType::FocusGained: return "focus_gained";
    case vkpt::platform::InputEventType::CloseRequested: return "close_requested";
    case vkpt::platform::InputEventType::None:
    default: return "none";
  }
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index,
                 std::string_view old_value,
                 std::string_view new_value,
                 std::string_view command_result);

void SyncRuntimePanelState(vkpt::editor::UiRuntimeState& runtime_state,
                          const vkpt::editor::UiLayoutDocument& layout_state) {
  runtime_state.active_layout_name = layout_state.active_layout_name;
  runtime_state.dpi_scale = layout_state.dpi_scale;
  runtime_state.ui_scale = layout_state.ui_scale;
  runtime_state.visible_panels.clear();
  runtime_state.collapsed_panels.clear();
  for (const auto& panel : layout_state.panels) {
    if (panel.visible) {
      runtime_state.visible_panels.push_back(panel.id);
    }
    if (panel.collapsed) {
      runtime_state.collapsed_panels.push_back(panel.id);
    }
  }
}

void ApplyWindowMetricsToLayout(vkpt::editor::UiLayoutDocument& layout_state,
                               std::size_t width,
                               std::size_t height) {
  for (auto& panel : layout_state.panels) {
    if (panel.id == vkpt::editor::ToString(vkpt::editor::UiPanelId::Viewport)) {
      panel.width = static_cast<float>(width);
      panel.height = static_cast<float>(height);
      return;
    }
  }
  if (!layout_state.panels.empty()) {
    layout_state.panels.front().width = static_cast<float>(width);
    layout_state.panels.front().height = static_cast<float>(height);
  }
}

#ifndef PT_ENABLE_QT
std::optional<vkpt::scene::SceneWorld> BuildSceneWorldSnapshot(
    const vkpt::scene::SceneDocument& document) {
  auto worldResult = document.to_world();
  if (!worldResult) {
    return std::nullopt;
  }
  auto world = std::move(worldResult.value());
  world.recompute_world_transforms();
  return world;
}

vkpt::scene::TransformComponent ResolveEntityWorldTransform(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* world) {
  if (world != nullptr) {
    if (const auto* worldTransform = world->world_transform(entity.id)) {
      vkpt::scene::TransformComponent transform = *worldTransform;
      transform.dirty = entity.has_transform ? entity.transform.dirty : false;
      return transform;
    }
  }
  return entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
}

int RunDynamicPhysicsPerformanceGate(std::string scenePath,
                                     std::string backend,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t frames) {
  (void)scenePath;
  (void)backend;
  (void)width;
  (void)height;
  (void)frames;
  std::cerr << "dynamic physics gate requires a Qt/D3D12-enabled build\n";
  return 2;
}

int RunThirdPersonScriptPerformanceGate(std::string scenePath,
                                        std::string backend,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t frames) {
  (void)scenePath;
  (void)backend;
  (void)width;
  (void)height;
  (void)frames;
  std::cerr << "third-person script gate requires a Qt/D3D12/Lua-enabled build\n";
  return 2;
}
#endif
#ifdef _WIN32
bool IsVirtualKeyDown(int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

[[maybe_unused]] bool IsCtrlDown() {
  return IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) || IsVirtualKeyDown(VK_RCONTROL);
}

[[maybe_unused]] bool IsShiftDown() {
  return IsVirtualKeyDown(VK_SHIFT) || IsVirtualKeyDown(VK_LSHIFT) || IsVirtualKeyDown(VK_RSHIFT);
}

[[maybe_unused]] bool IsAltDown() {
  return IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) || IsVirtualKeyDown(VK_RMENU);
}
#else
[[maybe_unused]] bool IsCtrlDown() { return false; }
[[maybe_unused]] bool IsShiftDown() { return false; }
[[maybe_unused]] bool IsAltDown() { return false; }
#endif

[[maybe_unused]] std::string ResolveShortcutAction(const std::vector<vkpt::editor::UiShortcut>& shortcuts,
                                                   int key_code,
                                                   bool ctrl,
                                                   bool shift,
                                                   bool alt) {
  const int normalized_key = (key_code >= 'a' && key_code <= 'z')
    ? static_cast<int>(std::toupper(static_cast<unsigned char>(key_code)))
    : key_code;

  for (const auto& shortcut : shortcuts) {
    if (static_cast<int>(shortcut.key_code) == normalized_key &&
        shortcut.ctrl == ctrl &&
        shortcut.shift == shift &&
        shortcut.alt == alt) {
      return shortcut.action_id;
    }
  }

#ifdef _WIN32
  if (key_code == VK_DELETE) {
    for (const auto& shortcut : shortcuts) {
      if (shortcut.key_code == 127 && !shortcut.ctrl && !shortcut.shift && !shortcut.alt) {
        return shortcut.action_id;
      }
    }
  }
#endif
  return {};
}

void LogWindowInput(vkpt::editor::UiEventLog& event_log,
                    vkpt::editor::UiRuntimeState& runtime_state,
                    const vkpt::platform::InputEvent& event,
                    vkpt::core::FrameIndex frame_index) {
  std::string_view widget = ToString(event.type);
  std::ostringstream oldValue;
  std::ostringstream newValue;
  if (event.raw_code != 0) {
    oldValue << event.raw_code;
  }
  newValue << "x=" << event.x << ", y=" << event.y
           << ", dx=" << event.delta_x << ", dy=" << event.delta_y
           << ", dz=" << event.delta_z;
  PushUiEvent(event_log,
              "window_event",
              "window",
              widget,
              frame_index,
              oldValue.str(),
              newValue.str(),
              runtime_state.active_layout_name);
  runtime_state.status_message = "window_event:" + std::string(widget);
  runtime_state.focused_panel = "window";
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index = 0,
                 std::string_view old_value = {},
                 std::string_view new_value = {},
                 std::string_view command_result = {});

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index,
                 std::string_view old_value,
                 std::string_view new_value,
                 std::string_view command_result) {
  vkpt::editor::UiEvent event;
  event.event_type = std::string(event_type);
  event.panel_id = std::string(panel_id);
  event.widget_id = std::string(widget_id);
  event.frame_index = frame_index;
  event.timestamp_ms = NowMs();
  event.thread_id = "main";
  event.old_value = std::string(old_value);
  event.new_value = std::string(new_value);
  event.command_result = std::string(command_result);
  log.push(event);
}

vkpt::editor::EditorCommand MakeUnsupportedUiCommand(std::string_view action_id,
                                                    std::string_view reason,
                                                    std::string_view source_widget = "app_shell",
                                                    vkpt::core::FrameIndex frame_index = 0) {
  vkpt::editor::EditorCommand cmd;
  cmd.command_id = std::string(action_id);
  cmd.kind = vkpt::editor::EditorCommandKind::kUnsupportedUiAction;
  cmd.source_widget = std::string(source_widget);
  cmd.frame_index = frame_index;
  cmd.undoable = false;
  cmd.redoable = false;
  cmd.validated = false;
  cmd.payload = vkpt::editor::UnsupportedUiActionCommand{
    std::string(action_id),
    std::string(reason)
  };
  return cmd;
}

void UpdateCrashArtifactsFromUiState(
    const vkpt::editor::UiRuntimeState& runtime_state,
    const vkpt::editor::SelectionState& selection_state,
    const vkpt::editor::UiLayoutDocument& layout_state,
    const vkpt::editor::UiEventLog& event_log,
    const vkpt::editor::EditorCommandHistory& command_history) {
  auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
  recorder.update_ui_state_json(vkpt::editor::SerializeUiRuntimeState(runtime_state));
  recorder.update_selection_state_json(vkpt::editor::SerializeSelectionState(selection_state));
  recorder.update_layout_state_json(vkpt::editor::SerializeLayoutDocument(layout_state));
  recorder.update_ui_events_jsonl(vkpt::editor::SerializeUiEventsJsonl(event_log.events(), 256));
  recorder.update_editor_commands_jsonl(
      vkpt::editor::SerializeEditorCommandsJsonl(command_history.history(), 256));
}

void RegisterDiagnosticsHealthProbe() {
  static std::once_flag once;
  std::call_once(once, [] {
    vkpt::core::health::HealthRegistry::instance().register_probe(
        std::make_shared<vkpt::core::health::FunctionProbe>(
            "diagnostics.crash_recorder",
            [] {
              const auto status = vkpt::diagnostics::CrashRecorder::instance().status();
              if (status.health == vkpt::core::contracts::SubsystemHealth::Failed) {
                return vkpt::core::health::Report{
                    vkpt::core::health::Status::Failed,
                    status.last_error.empty() ? "crash recorder failed" : status.last_error};
              }
              const bool unflushed = status.has_unflushed_record;
              return vkpt::core::health::Report{
                  unflushed ? vkpt::core::health::Status::Degraded
                            : vkpt::core::health::Status::Ok,
                  unflushed ? "crash recorder has unflushed state" : "ok"};
            }));
  });
}

std::pair<std::string, std::uint64_t> DumpGracefulCrashRings() {
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  const std::filesystem::path path = "artifacts/status/latest_crash_rings.jsonl";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    return {{}, 0u};
  }
  for (const auto& event : events) {
    out << "{\"ts\":" << event.ts_ns
        << ",\"comp\":\"" << event.component
        << "\",\"ev\":\"" << event.event
        << "\",\"level\":" << static_cast<int>(event.level)
        << "}\n";
  }
  return {path.generic_string(), static_cast<std::uint64_t>(events.size())};
}

}  // namespace

namespace vkpt::app {

int RunApp(int argc, char** argv) {
  // ---- Early init: logging + crash hooks ------------------------------------
  EnableOptionalConsole(ShouldEnableOptionalConsole(argc, argv));
  InitializeLogging();
  InitializeCrashRecorder();
  RegisterDiagnosticsHealthProbe();
  vkpt::diagnostics::install_crash_hooks("artifacts/crashes");
  vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("startup", 0);

  auto& logger = vkpt::log::Logger::instance();

  // ---- Parse CLI args -------------------------------------------------------
  const auto parseResult = ParseAppOptions(argc, argv);
  if (parseResult.exit_requested || !parseResult.ok) {
    return parseResult.exit_code;
  }
  const AppOptions parsedOptions = parseResult.options;

  const bool showVersion = parsedOptions.show_version;
  const bool versionJson = parsedOptions.version_json;
  const bool headless = parsedOptions.headless;
  const bool crashTest = parsedOptions.crash_test;
  const bool doctor = parsedOptions.doctor;
  const bool checkBuild = parsedOptions.check_build;
  const bool checkCpu = parsedOptions.check_cpu;
  const bool checkBackends = parsedOptions.check_backends;
  const bool checkAssets = parsedOptions.check_assets;
  const bool checkShaders = parsedOptions.check_shaders;
  const bool checkJobSystem = parsedOptions.check_job_system;
  const bool checkSceneSchema = parsedOptions.check_scene_schema;
  const bool checkBenchmarkArtifact = parsedOptions.check_benchmark_artifact;
  const bool dumpConfig = parsedOptions.dump_config;
  const bool listBackends = parsedOptions.list_backends;
  const bool listAccelerators = parsedOptions.list_accelerators;
  const bool doRender = parsedOptions.do_render;
  const bool uiModelSmoke = parsedOptions.ui_model_smoke;
  const bool uiReleaseGate = parsedOptions.ui_release_gate;
  const bool dynamicPhysicsGate = parsedOptions.dynamic_physics_gate;
  const bool thirdPersonScriptGate = parsedOptions.third_person_script_gate;
  const bool qtStressGate = parsedOptions.qt_stress_gate;
  const std::string qtStressOutput = parsedOptions.qt_stress_output;
  const std::string qtStressSceneOverride = parsedOptions.qt_stress_scene;
  const std::uint32_t qtStressPhaseSeconds = parsedOptions.qt_stress_phase_seconds;
  const bool openWindow = parsedOptions.open_window || qtStressGate;
  const bool listGpus = parsedOptions.list_gpus;
  const bool autoExitWindow = parsedOptions.auto_exit_window;
  const std::string configFilePath = parsedOptions.config_file_path;
  const std::string envFilePath = parsedOptions.env_file_path;
  const bool envFileExplicit = parsedOptions.env_file_explicit;
  const bool envFileEnabled = parsedOptions.env_file_enabled;
  const std::string scenePath = parsedOptions.scene_path;
  const std::string backend = parsedOptions.backend;
  const std::string audioBackend = parsedOptions.audio_backend;
  const std::string platformName = parsedOptions.platform_name;
  const std::string outputPath = parsedOptions.output_path;
  const std::string exrOutputPath = parsedOptions.exr_output_path;
  const std::string logLevel = parsedOptions.log_level;
  const uint32_t width = parsedOptions.width;
  const uint32_t height = parsedOptions.height;
  uint32_t windowWidth = parsedOptions.window_width;
  uint32_t windowHeight = parsedOptions.window_height;
  const uint32_t windowFrameLimit = parsedOptions.window_frame_limit;
  const uint32_t spp = parsedOptions.spp;
  const uint32_t maxDepth = parsedOptions.max_depth;
  const uint32_t renderFrame = parsedOptions.render_frame;
  const uint32_t renderSequenceFrames = parsedOptions.render_sequence_frames;
  const uint32_t renderFps = parsedOptions.render_fps;
  const std::optional<float> renderTimeSeconds = parsedOptions.render_time_seconds;
  const bool gpuDenoiser = parsedOptions.gpu_denoiser;
  const bool temporalAa = parsedOptions.temporal_aa;
  const bool audioMute = parsedOptions.audio_mute;
  const bool deterministic = parsedOptions.deterministic;
  const bool snapshotBus = parsedOptions.snapshot_bus;
  const std::optional<uint32_t> uiPresentHz = parsedOptions.ui_present_hz;
  if (envFileEnabled) {
    std::error_code envFileEc;
    const bool envFileExists = std::filesystem::exists(envFilePath, envFileEc);
    if (envFileExplicit || (envFileExists && !envFileEc)) {
      std::string envFileError;
      if (!vkpt::config::LoadDotEnvFile(envFilePath, false, &envFileError)) {
        std::cerr << envFileError << "\n";
        return 1;
      }
      BootStep("loaded dotenv file: " + envFilePath);
    }
  }

  // ---- Build resolved config (A10) ------------------------------------------
  vkpt::config::RuntimeConfig config = vkpt::config::BuildDefaultConfig(configFilePath);
  // CLI flags override file/env values.
  if (!backend.empty())   { config.backend    = {std::string(backend),    vkpt::config::ConfigSource::CliFlag}; }
  if (!platformName.empty())  { config.platform   = {std::string(platformName),   vkpt::config::ConfigSource::CliFlag}; }
  if (!scenePath.empty()) { config.scene_path = {std::string(scenePath),  vkpt::config::ConfigSource::CliFlag}; }
  if (qtStressGate && !qtStressSceneOverride.empty()) {
    config.scene_path = {qtStressSceneOverride, vkpt::config::ConfigSource::CliFlag};
  }
  if (!logLevel.empty())  { config.log_level  = {std::string(logLevel),   vkpt::config::ConfigSource::CliFlag}; }
  if (headless)           { config.headless   = {true,                    vkpt::config::ConfigSource::CliFlag}; }
  if (width != 320)       { config.render_width  = {width,  vkpt::config::ConfigSource::CliFlag}; }
  if (height != 240)      { config.render_height = {height, vkpt::config::ConfigSource::CliFlag}; }
  if (spp != 16)          { config.spp           = {spp,    vkpt::config::ConfigSource::CliFlag}; }
  if (maxDepth != 6)      { config.max_depth     = {maxDepth, vkpt::config::ConfigSource::CliFlag}; }
  if (uiPresentHz)        { config.ui_present_hz = {*uiPresentHz, vkpt::config::ConfigSource::CliFlag}; }
  if (!outputPath.empty()) { config.output_path  = {std::string(outputPath), vkpt::config::ConfigSource::CliFlag}; }
  if (!exrOutputPath.empty()) {
    config.exr_output_path = {std::string(exrOutputPath), vkpt::config::ConfigSource::CliFlag};
  }

  // Canonicalize backend aliases (e.g. "dxr" -> "d3d12-dxr").
  config.backend.value = vkpt::render::NormalizeBackendName(config.backend.value);
  const auto requestedPlatform = vkpt::platform::ParseRuntimePlatform(config.platform.value);
  if (requestedPlatform == vkpt::platform::RuntimePlatformKind::Invalid) {
    std::cerr << "invalid platform: " << config.platform.value
              << " (expected auto|raw|qt|headless; raw aliases include desktop|native|win32|x11|wayland|cocoa)\n";
    return 1;
  }
  const auto selectedPlatform = vkpt::platform::ResolveRuntimePlatform(
      requestedPlatform,
      openWindow,
      config.headless.value);
  const bool doctorMode = doctor || checkBuild || checkCpu || checkBackends || checkAssets || checkShaders ||
                          checkJobSystem || checkSceneSchema || checkBenchmarkArtifact;
  const bool nonGuiCommandMode = doctorMode || showVersion || dumpConfig || listBackends || listAccelerators ||
                                 listGpus || doRender || uiModelSmoke || uiReleaseGate || dynamicPhysicsGate;
  auto effectivePlatform = selectedPlatform;
  if (nonGuiCommandMode && selectedPlatform != vkpt::platform::RuntimePlatformKind::Headless) {
    effectivePlatform = vkpt::platform::RuntimePlatformKind::Headless;
    BootStep("non-gui command requested; using headless platform shell");
  }
  if (!ValidateRuntimePlatformSelection(
          requestedPlatform, effectivePlatform, openWindow, headless)) {
    return 1;
  }
  config.platform.value = vkpt::platform::RuntimePlatformKindName(effectivePlatform);
  if (effectivePlatform == vkpt::platform::RuntimePlatformKind::Headless) {
    config.headless = {true, config.platform.source};
  }
  {
    std::ostringstream resolved;
    resolved << "runtime config resolved backend=" << config.backend.value
             << " platform=" << config.platform.value
             << " requested_platform=" << vkpt::platform::RuntimePlatformKindName(requestedPlatform)
             << " selected_platform=" << vkpt::platform::RuntimePlatformKindName(selectedPlatform)
             << " scene=" << (config.scene_path.value.empty() ? "none" : config.scene_path.value);
    BootStep(resolved.str());
  }

  vkpt::editor::UiRuntimeState ui_runtime_state = vkpt::editor::CreateDefaultRuntimeState();
  vkpt::editor::SelectionState ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
  vkpt::editor::UiLayoutDocument ui_layout_state = vkpt::editor::CreateDefaultLayout();
  vkpt::editor::UiEventLog ui_event_log(256);
  vkpt::editor::EditorCommandHistory ui_command_history(256);

  ui_runtime_state.active_scene = config.scene_path.value.empty() ? "none" : config.scene_path.value;
  ui_runtime_state.active_renderer_backend = config.backend.value;
  PushUiEvent(ui_event_log, "app_start", "app_shell", "startup", 0, {}, {}, "bootstrap");
  UpdateCrashArtifactsFromUiState(
      ui_runtime_state, ui_selection_state, ui_layout_state,
      ui_event_log, ui_command_history);

  logger.log(vkpt::log::Severity::Info, "app", "arg parse complete");
  LogRuntimeMetadata(logger, "post_config", requestedPlatform, selectedPlatform, effectivePlatform,
                     openWindow, doRender, autoExitWindow, windowFrameLimit,
                     config.ui_present_hz.value);
  const std::string executionMode = [&]() {
    if (doRender) return std::string("render");
    if (qtStressGate) return std::string("qt_stress_gate");
    if (openWindow) return std::string("window");
    if (doctorMode) return std::string("doctor");
    if (dynamicPhysicsGate) return std::string("dynamic_physics_gate");
    if (thirdPersonScriptGate) return std::string("third_person_script_gate");
    if (listBackends) return std::string("list_backends");
    if (listAccelerators) return std::string("list_accelerators");
    if (listGpus) return std::string("list_gpus");
    if (showVersion) return std::string("version");
    if (dumpConfig) return std::string("dump_config");
    return std::string("headless_shell");
  }();
  std::optional<vkpt::core::DeterminismContext> startupDeterminismContext;
  if (deterministic) {
    startupDeterminismContext = vkpt::core::MakeDeterminismContext(
        true, 0xD37E'0001ull, 0u, "ptapp." + executionMode);
    vkpt::core::EmitDeterminismChanged("app", *startupDeterminismContext);
    vkpt::core::EmitDeterminismChanged("jobs", *startupDeterminismContext);
    vkpt::core::EmitDeterminismChanged("scripts", *startupDeterminismContext);
  }
  vkpt::core::TraceExecution("app_route_resolved", {
    {"mode", executionMode},
    {"requested_backend", config.backend.value},
    {"platform", config.platform.value},
    {"scene", RuntimeSceneDisplayName(config.scene_path.value)},
    {"resolution", std::to_string(config.render_width.value) + "x" +
                       std::to_string(config.render_height.value)},
    {"spp", std::to_string(config.spp.value)},
    {"headless", config.headless.value ? "true" : "false"},
    {"snapshot_bus", snapshotBus ? "true" : "false"},
    {"qt_built", IsQtPlatformBuilt() ? "true" : "false"},
    {"raw_built", IsRawPlatformBuilt() ? "true" : "false"}
  });
  BootStep("command line parsed");

  // ---- Status tracking (A13) ------------------------------------------------
  vkpt::diagnostics::StatusFileData status;
  status.build_status           = "ok";
  status.last_run_status        = "running";
  status.enabled_backend        = config.backend.value;
  status.selected_scene         = config.scene_path.value.empty() ? "none" : config.scene_path.value;
  status.selected_renderer_path = "cpu_scalar";
  std::mutex statusFileMutex;
  auto snapshotStatusFile = [&]() {
    std::scoped_lock lock(statusFileMutex);
    return status;
  };
  auto updateStatusFile = [&](auto&& update) {
    std::scoped_lock lock(statusFileMutex);
    update(status);
  };
  std::unique_ptr<vkpt::diagnostics::PeriodicStatusFile> periodicStatusFile;
  const auto writeStatus = [&](const std::string& runStatus, const std::string& error = "") {
    const auto crashRingDump = DumpGracefulCrashRings();
    updateStatusFile([&](auto& fileStatus) {
      fileStatus.last_run_status = runStatus;
      fileStatus.last_error = error;
      fileStatus.crash_ring_dump = crashRingDump.first;
      fileStatus.crash_ring_events = crashRingDump.second;
    });
    if (!config.write_status_file.value) {
      return;
    }
    std::string writeErr;
    const bool wrote = periodicStatusFile
        ? periodicStatusFile->write_now(&writeErr)
        : vkpt::diagnostics::WriteStatusFile(snapshotStatusFile(), config.status_file_path.value, &writeErr);
    if (!wrote) {
      logger.log(vkpt::log::Severity::Warning, "app", "status file write failed: " + writeErr);
    }
  };
  periodicStatusFile = std::make_unique<vkpt::diagnostics::PeriodicStatusFile>(
      vkpt::diagnostics::PeriodicStatusFileConfig{
          config.write_status_file.value,
          config.status_file_path.value,
          ResolveStatusFilePeriod()},
      snapshotStatusFile);
  periodicStatusFile->start();
  (void)periodicStatusFile->write_now();
  const auto recordUiAction = [&](std::string_view action_id,
                                 std::string_view event_widget,
                                 const std::string& action_status,
                                 const vkpt::editor::EditorCommand& command) {
    auto logged_command = command;
    logged_command.command_id = std::string(action_id);
    if (logged_command.source_widget.empty()) {
      logged_command.source_widget = "app_shell";
    }
    logged_command.frame_index = 0;
    ui_command_history.push(logged_command);
    ui_runtime_state.status_message = action_status;
    ui_runtime_state.last_menu_action = std::string(action_id);
    ui_runtime_state.focused_panel = logged_command.source_widget;
    PushUiEvent(ui_event_log, "app_action", logged_command.source_widget, event_widget, 0, {}, {}, action_status);
    UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                   ui_event_log, ui_command_history);
  };

  // ---- --version ------------------------------------------------------------
  if (showVersion) {
    if (versionJson) { PrintVersionJson(effectivePlatform); } else { PrintVersionText(effectivePlatform); }
    recordUiAction("app.version", "version", "version output", MakeUnsupportedUiCommand("app.version", "handled on cli"));
    writeStatus("version_query");
    return 0;
  }

  // ---- --dump-config (A10) --------------------------------------------------
  if (dumpConfig) {
    std::cout << vkpt::config::SerializeRuntimeConfig(config) << "\n";
    recordUiAction("app.dump_config", "dump_config", "config dump", MakeUnsupportedUiCommand("app.dump_config", "handled on cli"));
    writeStatus("dump_config");
    return 0;
  }

  if (uiModelSmoke) {
    const bool ok = RunUiModelSmokeTests();
    writeStatus(ok ? "ui_model_smoke" : "ui_model_smoke_failed");
    return ok ? 0 : 1;
  }

  if (uiReleaseGate) {
    const bool ok = RunUiReleaseGateCheck(versionJson);
    writeStatus(ok ? "ui_release_gate" : "ui_release_gate_failed");
    return ok ? 0 : 1;
  }

  // ---- --doctor / --check-* (A09) -------------------------------------------
  if (doctorMode) {
    PrintNonGuiPlatformShellNotice("doctor", selectedPlatform, effectivePlatform);
    RunDoctor(checkBuild, checkCpu, checkBackends, checkAssets, checkShaders,
              checkJobSystem, checkSceneSchema, checkBenchmarkArtifact);
    recordUiAction("app.doctor", "doctor", "doctor complete", MakeUnsupportedUiCommand("app.doctor", "handled on cli"));
    writeStatus("doctor_ok");
    return 0;
  }

  if (listBackends) {
    PrintNonGuiPlatformShellNotice("list-backends", selectedPlatform, effectivePlatform);
    PrintBackendDiagnostics();
    recordUiAction("app.list_backends", "list_backends", "backend list", MakeUnsupportedUiCommand("app.list_backends", "handled on cli"));
    writeStatus("list_backends");
    return 0;
  }

  if (listAccelerators) {
    PrintNonGuiPlatformShellNotice("list-accelerators", selectedPlatform, effectivePlatform);
    PrintAcceleratorDiagnostics(config.render_width.value, config.render_height.value);
    recordUiAction("app.list_accelerators", "list_accelerators", "accelerator list",
                   MakeUnsupportedUiCommand("app.list_accelerators", "handled on cli"));
    writeStatus("list_accelerators");
    return 0;
  }

#ifdef PT_ENABLE_VULKAN
  if (listGpus) {
    const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
        PT_SHADER_SPV_PATH;
#else
        "shaders/pathtrace.spv";
#endif
    // Init a temporary tracer just for device enumeration (it logs devices in init_device)
    auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
    if (gpuTracer->is_valid()) {
      std::cout << "Selected GPU: " << gpuTracer->gpu_name() << "\n";
      std::cout << "  Type  : " << gpuTracer->gpu_type() << "\n";
      std::cout << "  VRAM  : " << gpuTracer->vram_mb() << " MB\n";
      std::cout << "  Vulkan: "
                << VK_VERSION_MAJOR(gpuTracer->vulkan_api()) << "."
                << VK_VERSION_MINOR(gpuTracer->vulkan_api()) << "\n";
    } else {
      std::cerr << "Vulkan device init failed: " << gpuTracer->last_error() << "\n";
    }
    writeStatus("list_gpus");
    return gpuTracer->is_valid() ? 0 : 1;
  }
#else
  if (listGpus) {
    std::cout << "PT_ENABLE_VULKAN not set in this build — no GPU info available.\n";
    writeStatus("list_gpus_no_vulkan");
    return 0;
  }
#endif

  // ---- --window (interactive shell placeholder) ----------------------------
  if (openWindow) {
    BootStep("window mode requested");
#ifdef _WIN32
    ScopedWindowsTimerResolution windowTimerResolution(1u);
    logger.log(vkpt::log::Severity::Info,
               "app",
               windowTimerResolution.active()
                   ? "Windows timer resolution set to 1 ms for window mode"
                   : "Windows timer resolution request failed for window mode");
#endif
    if (headless) {
      std::cerr << "--window and --headless are mutually exclusive\n";
      return 1;
    }
    if (selectedPlatform == vkpt::platform::RuntimePlatformKind::Headless) {
      std::cerr << "--window and --platform headless are mutually exclusive\n";
      return 1;
    }
    if (selectedPlatform == vkpt::platform::RuntimePlatformKind::Qt) {
      BootStep("creating qt platform");
      const std::string qtTitleBase = BuildWindowTitleBase("qt");
      auto platform = vkpt::platform::CreatePlatform(vkpt::platform::RuntimePlatformKind::Qt,
                                                     qtTitleBase);
      if (!platform) {
        std::cerr << "qt platform factory creation failed\n";
        writeStatus("error:qt_platform_create_failed", "qt_platform_create_failed");
        return 1;
      }
      BootStep("initializing qt platform");
      auto platformState = platform->initialize();
      if (!platformState) {
        std::cerr << "qt platform initialization failed\n";
        writeStatus("error:qt_platform_init_failed", "qt_platform_init_failed");
        return 1;
      }

      auto* window = platform->window();
      BootStep("qt platform initialized; acquiring window");
      if (!window) {
        std::cerr << "qt platform returned invalid window\n";
        platform->shutdown();
        writeStatus("error:qt_window_missing", "qt_window_missing");
        return 1;
      }

      std::function<void(std::string_view)> qtStartupStep = [&](std::string_view phase) {
        BootStep(phase);
      };

#ifdef PT_ENABLE_QT
      auto* qtWindow = dynamic_cast<vkpt::platform::QtWindow*>(window);
      std::unordered_map<NativeMenuId, std::string> qtMenuCommandLookup;
      auto rebuildQtMenuBar = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const auto uiMenu = vkpt::editor::BuildDefaultMenuBar(ui_selection_state);
        qtWindow->set_menu_bar(BuildQtMenuBarFromModel(uiMenu, qtMenuCommandLookup));
      };
      qtStartupStep = [&](std::string_view phase) {
        BootStep(phase);
        if (qtWindow == nullptr) {
          return;
        }
        std::ostringstream overlay;
        overlay << "vkPathTracer\n"
                << "Starting: " << phase << "\n"
                << "Scene: " << (config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value) << "\n"
                << "Backend: " << config.backend.value << "\n"
                << "Logs: artifacts/logs/ptapp.log";
        qtWindow->set_title(BuildWindowRuntimeTitle(qtTitleBase, ui_runtime_state, ui_layout_state));
        qtWindow->set_overlay_text(overlay.str());
        qtWindow->set_startup_splash_text(phase);
        vkpt::platform::QtStatusBarText status;
        status.message = "Starting: " + std::string(phase);
        status.fields.push_back(vkpt::platform::QtStatusBarField{
            "startup.backend", "Backend: " + config.backend.value, 0});
        status.fields.push_back(vkpt::platform::QtStatusBarField{
            "startup.scene",
            config.scene_path.value.empty() ? std::string("Scene: builtin") : "Scene: " + config.scene_path.value,
            0});
        qtWindow->set_status_bar_text(status);
        DrainQtQueuedWork(4);
      };
      if (qtWindow) {
        qtWindow->resize(std::max<uint32_t>(1u, windowWidth),
                         std::max<uint32_t>(1u, windowHeight));
        qtStartupStep("preparing Qt shell");
        rebuildQtMenuBar();
        logger.log(vkpt::log::Severity::Info,
                   "app",
                   std::string("Qt menu bar attached with ") +
                       std::to_string(qtMenuCommandLookup.size()) + " command mappings");
      }
#endif
      LogRuntimeMetadata(logger, "qt_platform_initialized", requestedPlatform,
                         selectedPlatform, effectivePlatform, openWindow, doRender,
                         autoExitWindow, windowFrameLimit, config.ui_present_hz.value);

      std::cout << "qt window open (" << window->metrics().width << "x"
                << window->metrics().height << ")\n";
      std::cout << "qt platform shell: " << QtPlatformShellString() << "\n";
      std::cout << "Close the Qt window to exit.\n";
      if (windowFrameLimit != 0u) {
        std::cout << "[ui] gui smoke frame limit: " << windowFrameLimit << "\n";
      }
      qtStartupStep("qt window opened");

      // ---- Runtime backend / path tracer setup ----
      struct QtRuntimeTracerInit {
        std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
        std::string requested_backend;
        std::string renderer_path;
        std::string error;
        bool background = false;
      };
      auto qtNormalizeRuntimeBackend = [](std::string_view backend) {
        auto normalized = vkpt::render::NormalizeBackendName(backend);
        if (normalized.empty()) {
          return std::string("auto");
        }
        if (normalized == "cputiled" || normalized == "cpu-tiled" || normalized == "cpu") {
          return std::string("cpu");
        }
        return normalized;
      };
      const auto qtRuntimeBackendOptions = [&]() {
        std::vector<std::string> options;
        const auto add = [&](std::string_view option) {
          const std::string normalized = qtNormalizeRuntimeBackend(option);
          if (std::find(options.begin(), options.end(), normalized) == options.end()) {
            options.push_back(normalized);
          }
        };
        add("auto");
        add("cpu");
#ifdef PT_ENABLE_D3D12
        add("d3d12");
        add("d3d12-dxr");
#endif
#ifdef PT_ENABLE_VULKAN
        add("vulkan");
#endif
        add("null");
        return options;
      }();
      auto qtCreateRuntimeTracer = [&](std::string_view requestedBackend) {
        QtRuntimeTracerInit out;
        out.requested_backend = qtNormalizeRuntimeBackend(requestedBackend);
        const std::string effectiveBackend =
            (out.requested_backend == "auto") ? std::string("cpu") : out.requested_backend;
        if (effectiveBackend == "cpu") {
          vkpt::cpu::TiledRenderConfig tiledConfig{};
          tiledConfig.worker_count = InteractiveCpuWorkerCount();
          out.tracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
          out.renderer_path = "cpu_tiled_background";
          out.background = true;
          return out;
        }
        if (effectiveBackend == "null") {
          out.tracer = std::make_unique<vkpt::pathtracer::NullPathTracer>();
          out.renderer_path = "null";
          return out;
        }
#ifdef PT_ENABLE_D3D12
        if (effectiveBackend == "d3d12" || effectiveBackend == "d3d12-dxr") {
          const bool requestDxr = (effectiveBackend == "d3d12-dxr");
          const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
              PT_SHADER_HLSL_PATH;
#else
              "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
          auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
          if (!gpuTracer->is_valid()) {
            out.error = "D3D12 tracer init failed: " + gpuTracer->last_error();
            return out;
          }
          gpuTracer->set_prefer_dxr(requestDxr);
          std::cout << "[gpu] D3D12 " << gpuTracer->gpu_name()
                    << "  " << gpuTracer->vram_mb() << " MB VRAM"
                    << "  DXR=" << (gpuTracer->dxr_supported() ? "yes" : "no") << "\n";
          out.renderer_path = requestDxr ? "d3d12_dxr" : "d3d12_compute";
          out.tracer = std::move(gpuTracer);
          return out;
        }
#endif
#ifdef PT_ENABLE_VULKAN
        if (effectiveBackend == "vulkan" || effectiveBackend == "vulkan-compute") {
          const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
              PT_SHADER_SPV_PATH;
#else
              "shaders/pathtrace.spv";
#endif
          auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
          if (!gpuTracer->is_valid()) {
            out.error = "Vulkan tracer init failed: " + gpuTracer->last_error();
            return out;
          }
          out.renderer_path = "vulkan_compute";
          out.tracer = std::move(gpuTracer);
          return out;
        }
#endif
        out.error = "runtime backend is not supported by the Qt path tracer: " + out.requested_backend;
        return out;
      };

      std::unique_ptr<vkpt::pathtracer::IPathTracer> qtTracer;
      std::string qtRendererPath = "unselected";
      qtStartupStep("initializing runtime backend");
      auto qtInitialTracer = qtCreateRuntimeTracer(config.backend.value);
      if (!qtInitialTracer.tracer) {
        const auto initialBackend = qtNormalizeRuntimeBackend(config.backend.value);
        const bool explicitGpuRequest =
            initialBackend == "d3d12" || initialBackend == "d3d12-dxr" ||
            initialBackend == "vulkan" || initialBackend == "vulkan-compute";
        if (explicitGpuRequest) {
          std::cerr << "[render] " << qtInitialTracer.error << "\n";
          writeStatus("error:qt_renderer_init_failed", qtInitialTracer.error);
          platform->shutdown();
          return 1;
        }
        std::cerr << "[render] " << qtInitialTracer.error << "; falling back to CPU\n";
        config.backend = {std::string("cpu"), vkpt::config::ConfigSource::Default};
        qtInitialTracer = qtCreateRuntimeTracer("cpu");
      }
      if (!qtInitialTracer.tracer) {
        std::cerr << "[render] CPU fallback failed: " << qtInitialTracer.error << "\n";
        writeStatus("error:qt_renderer_init_failed", qtInitialTracer.error);
        platform->shutdown();
        return 1;
      }
      qtTracer = std::move(qtInitialTracer.tracer);
      qtRendererPath = qtInitialTracer.renderer_path;
      updateStatusFile([&](auto& fileStatus) {
        fileStatus.selected_renderer_path = qtRendererPath;
      });
      config.backend = {qtInitialTracer.requested_backend, config.backend.source};
      if (qtInitialTracer.background) {
        std::cout << "[cpu] Using TiledCpuPathTracer workers="
                  << InteractiveCpuWorkerCount() << " (interactive)\n";
      }
      // ---- Scene loading ----
      vkpt::pathtracer::PathTracerSceneSnapshot qtScene;
      vkpt::scene::SceneDocument qtSceneDocument;
      RuntimeMeshStats qtStressSourceMeshStats;
      RuntimeMeshStats qtStressBallastMeshStats;
      RuntimeMeshStats qtStressRtMeshStats;
      qtStartupStep("loading scene snapshot");
      {
        bool sceneOk = false;
        if (!config.scene_path.value.empty()) {
          auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
          if (parseResult) {
            qtSceneDocument = parseResult.value();
            if (qtStressGate) {
              qtSceneDocument.performance_culling.enabled = true;
              qtSceneDocument.performance_culling.frustum = true;
              qtSceneDocument.performance_culling.distance = true;
              qtSceneDocument.performance_culling.cull_dynamic = true;
              qtSceneDocument.performance_culling.aspect_ratio =
                  static_cast<float>(std::max<uint32_t>(1u, config.render_width.value)) /
                  static_cast<float>(std::max<uint32_t>(1u, config.render_height.value));
              if (qtSceneDocument.performance_culling.max_distance <= 0.0f) {
                qtSceneDocument.performance_culling.max_distance = 180.0f;
              }
              qtStressBallastMeshStats = RuntimeAppendOffscreenCullingBallast(qtSceneDocument);
              qtStressSourceMeshStats = RuntimeCountAuthoredMeshStats(qtSceneDocument);
            }
#ifdef PT_ENABLE_QT
            EnsureQtFallbackLightingEntities(qtSceneDocument);
#endif
            auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
            if (sceneResult) {
              qtScene = std::move(sceneResult.value());
              sceneOk = true;
            }
          }
        } else {
          qtSceneDocument = LoadDefaultSceneDocument();
          if (qtStressGate) {
            qtSceneDocument.performance_culling.enabled = true;
            qtSceneDocument.performance_culling.frustum = true;
            qtSceneDocument.performance_culling.distance = true;
            qtSceneDocument.performance_culling.cull_dynamic = true;
            qtSceneDocument.performance_culling.aspect_ratio =
                static_cast<float>(std::max<uint32_t>(1u, config.render_width.value)) /
                static_cast<float>(std::max<uint32_t>(1u, config.render_height.value));
            if (qtSceneDocument.performance_culling.max_distance <= 0.0f) {
              qtSceneDocument.performance_culling.max_distance = 180.0f;
            }
            qtStressBallastMeshStats = RuntimeAppendOffscreenCullingBallast(qtSceneDocument);
            qtStressSourceMeshStats = RuntimeCountAuthoredMeshStats(qtSceneDocument);
          }
#ifdef PT_ENABLE_QT
          EnsureQtFallbackLightingEntities(qtSceneDocument);
#endif
          auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
          if (sceneResult) {
            qtScene = std::move(sceneResult.value());
            sceneOk = true;
          }
        }
        if (!sceneOk) {
          std::cerr << "qt window: scene load failed\n";
          writeStatus("error:qt_scene_load_failed", "qt_scene_load_failed");
          platform->shutdown();
          return 1;
        }
      }
      qtStressRtMeshStats.instances = qtScene.instances.size();
      qtStressRtMeshStats.vertices = qtScene.vertices.size();
      qtStressRtMeshStats.triangles = qtScene.indices.size() / 3u;
      qtStartupStep("scene loaded");
      vkpt::scene::SnapshotRing qtSnapshotRing;
      vkpt::scene::RenderSceneSnapshotRevisions qtSnapshotRevisions{};
      qtSnapshotRevisions.generation = 1u;
      qtSnapshotRevisions.topology_revision = 1u;
      qtSnapshotRevisions.transform_revision = 1u;
      qtSnapshotRevisions.camera_revision = 1u;
      qtSnapshotRevisions.material_revision = 1u;
      // Timestamp of the most recent publish that mutated scene OR camera
      // state. Used by the dock panel sync to defer its O(entities)
      // rebuild while mutation is in flight. Includes camera changes
      // because the editor camera orbit, Qt orbit, FPS walk, and
      // qtPublishRenderSnapshot lambda all bump camera_revision at 60Hz —
      // letting the dock panel rebuild during continuous camera motion
      // produces sub-second freezes on large scenes (Mona Lisa: 1054
      // entities → ~1.5s per BuildQtDockPanels). The dock panel content
      // is editor-facing and tolerates a 200ms post-stop delay.
      std::uint64_t qtLastMutationPublishNs = 0u;
      auto qtPublishRenderSnapshot =
          [&](bool topology_changed,
              bool transform_changed,
              bool camera_changed,
              bool material_changed) -> std::uint64_t {
        const auto _vkp_publish_t0_ns = vkpt::core::metrics::UiInputLatencyNowNs();
        const auto pending_input_ns =
            vkpt::core::metrics::ConsumePendingInputForPublish();
        auto previous = qtSnapshotRing.current();
        qtSnapshotRevisions.generation =
            std::max<std::uint64_t>(qtSnapshotRevisions.generation + 1u,
                                    previous ? previous->generation + 1u : 1u);
        if (topology_changed) {
          ++qtSnapshotRevisions.topology_revision;
          VKP_METRIC_INC("vkp.scene.publish_by_source.topology_total");
          qtLastMutationPublishNs = _vkp_publish_t0_ns;
        }
        if (transform_changed) {
          ++qtSnapshotRevisions.transform_revision;
          VKP_METRIC_INC("vkp.scene.publish_by_source.transform_total");
          qtLastMutationPublishNs = _vkp_publish_t0_ns;
        }
        if (camera_changed) {
          ++qtSnapshotRevisions.camera_revision;
          VKP_METRIC_INC("vkp.scene.publish_by_source.camera_total");
          qtLastMutationPublishNs = _vkp_publish_t0_ns;
        }
        if (material_changed) {
          ++qtSnapshotRevisions.material_revision;
          VKP_METRIC_INC("vkp.scene.publish_by_source.material_total");
          qtLastMutationPublishNs = _vkp_publish_t0_ns;
        }
        auto snapshot = vkpt::scene::BuildRenderSceneSnapshot(
            qtScene,
            previous.get(),
            qtSnapshotRevisions);
        const auto generation = snapshot ? snapshot->generation : 0u;
        if (generation != 0u) {
          vkpt::core::metrics::RegisterPublishInput(generation, pending_input_ns);
        }
        qtSnapshotRing.publish(std::move(snapshot));
        const auto _vkp_publish_t1_ns = vkpt::core::metrics::UiInputLatencyNowNs();
        VKP_METRIC_OBSERVE("vkp.scene.publish_us",
                           (_vkp_publish_t1_ns - _vkp_publish_t0_ns) / 1000u);
        return generation;
      };
      auto qtPublishInitialRenderSnapshot = [&]() {
        auto snapshot = vkpt::scene::BuildRenderSceneSnapshot(
            qtScene,
            nullptr,
            qtSnapshotRevisions);
        qtSnapshotRing.publish(std::move(snapshot));
      };
      qtPublishInitialRenderSnapshot();
      vkpt::audio::AudioSystemConfig qtAudioConfig{};
      qtAudioConfig.backend = audioBackend;
      qtAudioConfig.muted = audioMute;
      qtAudioConfig.sample_rate = 44100u;
      qtAudioConfig.channels = 2u;
      qtAudioConfig.buffer_frames = 1024u;
      qtAudioConfig.queued_buffers = 3u;
      if (startupDeterminismContext) {
        qtAudioConfig.set_determinism(*startupDeterminismContext);
      }
      auto qtAudioSystem = vkpt::audio::CreateAudioSystem(qtAudioConfig);
      if (qtAudioSystem && qtAudioSystem->initialize()) {
        vkpt::audio::SetGlobalAudioSystem(qtAudioSystem.get());
        qtAudioSystem->set_snapshot_ring(&qtSnapshotRing);
        qtAudioSystem->load_scene_audio(qtSceneDocument, RuntimeSceneDisplayName(config.scene_path.value));
      }
      std::unordered_map<vkpt::core::StableId, uint32_t> qtRtInstanceIndexByEntity;
      auto qtRebuildRtInstanceIndexCache = [&]() {
        qtRtInstanceIndexByEntity.clear();
        qtRtInstanceIndexByEntity.reserve(qtScene.instances.size());
        for (std::size_t index = 0; index < qtScene.instances.size(); ++index) {
          const auto entityId = qtScene.instances[index].entity_id;
          if (entityId != 0u) {
            qtRtInstanceIndexByEntity[entityId] = static_cast<uint32_t>(index);
          }
        }
      };
      qtRebuildRtInstanceIndexCache();
      const std::uint32_t qtUiSnapshotReader = qtSnapshotRing.register_reader("ui");
      auto qtCurrentUiSnapshot = [&]() -> vkpt::scene::RenderSceneSnapshot::Ptr {
        return qtSnapshotRing.current(qtUiSnapshotReader);
      };
      (void)qtCurrentUiSnapshot;
      vkpt::core::TraceExecution("qt_scene_ready", {
        {"renderer_path", qtRendererPath},
        {"scene", RuntimeSceneDisplayName(config.scene_path.value)},
        {"document_entities", std::to_string(qtSceneDocument.entities.size())},
        {"document_geometry", std::to_string(qtSceneDocument.geometry.size())},
        {"rt_vertices", std::to_string(qtScene.vertices.size())},
        {"rt_indices", std::to_string(qtScene.indices.size())},
        {"rt_instances", std::to_string(qtScene.instances.size())},
        {"rt_lights", std::to_string(qtScene.lights.size())}
      });

      // ---- Tracer configure ----
      vkpt::pathtracer::RenderSettings qtSettings{};
      qtSettings.width  = std::max<uint32_t>(1u, config.render_width.value);
      qtSettings.height = std::max<uint32_t>(1u, config.render_height.value);
      qtSettings.spp    = std::numeric_limits<uint32_t>::max();
      qtSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
      qtSettings.seed = 0xC001D00Dull;
      qtSettings.enable_nee = true;
      qtSettings.enable_mis = true;
      // Interactive preview follows the reference WebGL tracer loop: display the
      // latest path-traced sample immediately and let accumulation catch up.
      // Denoising/temporal AA are still exposed in Render Settings for explicit
      // quality work, but defaulting them on adds latency and ghosting while dragging.
      qtSettings.enable_denoiser = false;
      qtSettings.enable_temporal_aa = false;
      if (startupDeterminismContext) {
        qtSettings.set_determinism(*startupDeterminismContext);
      }

      qtStartupStep("configuring renderer and acceleration");
      bool qtTracerReady = (qtTracer->configure(qtSettings) &&
                            qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation());
      if (!qtTracerReady) {
        std::cerr << "qt window: tracer init failed\n";
        qtStartupStep("tracer init failed");
#ifdef PT_ENABLE_QT
        if (qtWindow != nullptr) {
          qtWindow->finish_startup_splash();
        }
#endif
      } else {
        qtStartupStep("tracer initialized");
      }
      vkpt::core::TraceExecution("qt_renderer_initialized", {
        {"renderer_path", qtRendererPath},
        {"ready", qtTracerReady ? "true" : "false"},
        {"resolution", std::to_string(qtSettings.width) + "x" + std::to_string(qtSettings.height)},
        {"max_depth", std::to_string(qtSettings.max_depth)}
      });

#ifdef PT_ENABLE_QT
      auto qtViewportPickablesSnapshot = qtCurrentUiSnapshot();
      std::uint64_t qtViewportPickablesSnapshotGeneration =
          qtViewportPickablesSnapshot ? qtViewportPickablesSnapshot->generation : 0u;
      std::uint64_t qtViewportPickablesTopologyRevision =
          qtViewportPickablesSnapshot ? qtViewportPickablesSnapshot->topology_revision : 0u;
      std::uint64_t qtViewportPickablesTransformRevision =
          qtViewportPickablesSnapshot ? qtViewportPickablesSnapshot->transform_revision : 0u;
      std::vector<ViewportPickable> qtPickables = qtViewportPickablesSnapshot
          ? BuildViewportPickables(qtSceneDocument, *qtViewportPickablesSnapshot)
          : BuildViewportPickables(qtSceneDocument, qtScene);
      FpsCollisionWorker qtFpsCollisionWorker;
      qtFpsCollisionWorker.set_pickables(qtPickables);
#endif
      auto qtBuildPhysicsBodies = [&](const vkpt::scene::SceneWorld* world) {
        std::vector<vkpt::physics::PhysicsBodySync> bodies;
        bodies.reserve(qtSceneDocument.entities.size());
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_physics_body) {
            continue;
          }
          vkpt::physics::PhysicsBodySync sync;
          sync.entity = entity.id;
          sync.body = entity.physics_body;
          sync.transform = ResolveEntityWorldTransform(entity, world);
          bodies.push_back(std::move(sync));
        }
        return bodies;
      };
      auto qtPhysics = vkpt::physics::CreatePhysicsWorld();
      auto qtSyncPhysicsSceneDocumentNow = [&]() {
        if (auto world = BuildSceneWorldSnapshot(qtSceneDocument)) {
          return qtPhysics->sync_from_scene_world(*world);
        }
        return qtPhysics->sync_from_bodies(qtBuildPhysicsBodies(nullptr),
                                           qtSceneDocument.entities.size());
      };
      auto qtPhysicsSummary = qtSyncPhysicsSceneDocumentNow();
      const auto qtPhysicsInfo = qtPhysics->engine_info();
      bool qtPhysicsRuntimeDirty = false;
      auto qtSyncPhysicsFromSceneDocument = [&]() {
        qtPhysicsSummary = qtSyncPhysicsSceneDocumentNow();
        qtPhysicsRuntimeDirty = true;
      };
#ifndef PT_ENABLE_QT
      (void)qtSyncPhysicsFromSceneDocument;
#endif
      const bool qtPhysicsRuntimeEnabled =
          qtPhysicsSummary.enabled_bodies > 0u && qtPhysicsSummary.dynamic_bodies > 0u;
      std::cout << "[physics] " << qtPhysicsInfo.engine_name
                << " bodies=" << qtPhysicsSummary.backend_bodies
                << " dynamic=" << qtPhysicsSummary.dynamic_bodies
                << " static=" << qtPhysicsSummary.static_bodies
                << " worker=" << (qtPhysicsInfo.runs_on_worker_thread ? "yes" : "no") << "\n";

      // ---- Background render coordinator ------------------------------------
      // Qt posts motion updates independently and consumes latest-wins display frames.
      uint32_t qtPreviewPublishHz = std::max<uint32_t>(1u, config.ui_present_hz.value);
      uint32_t qtFramebufferDisplayHz = qtPreviewPublishHz;
      auto qtFramebufferDisplayInterval =
          std::chrono::microseconds(1000000u / qtFramebufferDisplayHz);
      constexpr uint32_t kQtPreviewImmediatePublishes = 4u;
      std::atomic<uint32_t> qtPublishedSample{0u};
      std::atomic<uint32_t> qtPublishedWidth{qtSettings.width};
      std::atomic<uint32_t> qtPublishedHeight{qtSettings.height};
      std::atomic<std::uint64_t> qtPublishedRays{0u};
      std::atomic<std::uint64_t> qtPublishedFrames{0u};
      std::atomic<std::uint64_t> qtDroppedFrames{0u};
      std::unique_ptr<vkpt::render::RenderCoordinator> qtRenderCoordinator;
      auto qtNextFramebufferSubmit = std::chrono::steady_clock::time_point{};
      bool qtUseBg = qtInitialTracer.background;
      auto qtRefreshPreviewCadence = [&]() {
        qtPreviewPublishHz = std::max<uint32_t>(1u, config.ui_present_hz.value);
        qtFramebufferDisplayHz = qtPreviewPublishHz;
        qtFramebufferDisplayInterval =
            std::chrono::microseconds(1000000u / qtFramebufferDisplayHz);
        if (qtRenderCoordinator) {
          qtRenderCoordinator->set_publish_hz(qtPreviewPublishHz);
        }
      };
#ifdef PT_ENABLE_QT
      QtDockDeviceStats qtDeviceStats;
      auto qtRefreshDeviceStats = [&]() {
        auto retainedMetrics = std::move(qtDeviceStats.ray_metrics);
        qtDeviceStats = QtDockDeviceStats{};
        qtDeviceStats.ray_metrics = std::move(retainedMetrics);
        qtDeviceStats.runtime_backend_options = qtRuntimeBackendOptions;
        qtDeviceStats.selected_backend = config.backend.value.empty() ? "auto" : config.backend.value;
        qtDeviceStats.active_renderer_path = qtRendererPath;
        qtDeviceStats.backend_caps.backend_name = qtDeviceStats.selected_backend;
        const auto normalizedBackend = qtNormalizeRuntimeBackend(qtDeviceStats.selected_backend);
        if (normalizedBackend.find("d3d12") != std::string::npos ||
            normalizedBackend.find("dxr") != std::string::npos) {
          qtDeviceStats.accelerators = vkpt::render::EnumerateD3D12Accelerators(true, true);
        }
        const auto selectedIt = std::find_if(qtDeviceStats.accelerators.begin(),
                                             qtDeviceStats.accelerators.end(),
                                             [](const vkpt::render::AcceleratorCapabilities& accelerator) {
                                               return accelerator.selected_by_default;
                                             });
        if (selectedIt != qtDeviceStats.accelerators.end()) {
          qtDeviceStats.has_selected_accelerator = true;
          qtDeviceStats.selected_accelerator = *selectedIt;
          qtDeviceStats.backend_caps = selectedIt->backend_caps;
        } else if (!qtDeviceStats.accelerators.empty()) {
          qtDeviceStats.has_selected_accelerator = true;
          qtDeviceStats.selected_accelerator = qtDeviceStats.accelerators.front();
          qtDeviceStats.backend_caps = qtDeviceStats.accelerators.front().backend_caps;
        } else if (auto probeBackend = vkpt::render::CreateBackend(qtDeviceStats.selected_backend)) {
          if (probeBackend->initialize().is_ok()) {
            qtDeviceStats.backend_caps = probeBackend->capabilities();
            (void)probeBackend->shutdown();
          }
        }
      };
      qtRefreshDeviceStats();
#endif
      if (qtTracerReady && qtUseBg) {
        qtStartupStep("starting background render coordinator");
        vkpt::render::RenderCoordinatorConfig coordinatorConfig{};
        coordinatorConfig.publish_hz = qtPreviewPublishHz;
        coordinatorConfig.immediate_publish_count = kQtPreviewImmediatePublishes;
        coordinatorConfig.snapshot_ring = &qtSnapshotRing;
        qtRenderCoordinator = std::make_unique<vkpt::render::RenderCoordinator>(
            std::move(qtTracer),
            qtSettings,
            qtScene,
            coordinatorConfig);
        if (!qtRenderCoordinator->start()) {
          qtTracerReady = false;
          qtStartupStep("background cpu render coordinator start failed");
        }
      }

      uint32_t qtSampleIndex = 0;
      std::string qtPreviewStatus = (windowFrameLimit != 0u)
          ? "smoke frame limit"
          : (qtTracerReady ? "rendering" : "tracer init failed");

      // ---- Orbit camera setup ----
      bool qtEnableAutoOrbit = (windowFrameLimit == 0u) && !qtUseBg && !qtPhysicsRuntimeEnabled && !qtStressGate;
      const float kQtOrbitDegPerSec = 7.5f;
      const float kQtOrbitMinStepDeg = 0.1f;
      vkpt::pathtracer::Vec3 qtOrbitCenter{};
      {
        float bminX = FLT_MAX, bminZ = FLT_MAX;
        float bmaxX = -FLT_MAX, bmaxZ = -FLT_MAX;
        float bminY = FLT_MAX, bmaxY = -FLT_MAX;
        for (const auto& v : qtScene.vertices) {
          bminX = std::min(bminX, v.x); bmaxX = std::max(bmaxX, v.x);
          bminY = std::min(bminY, v.y); bmaxY = std::max(bmaxY, v.y);
          bminZ = std::min(bminZ, v.z); bmaxZ = std::max(bmaxZ, v.z);
        }
        if (bminX < bmaxX) {
          qtOrbitCenter.x = (bminX + bmaxX) * 0.5f;
          qtOrbitCenter.y = (bminY + bmaxY) * 0.5f;
          qtOrbitCenter.z = (bminZ + bmaxZ) * 0.5f;
        } else {
          qtOrbitCenter = qtScene.camera_target;
        }
      }
      const float qtOrbitDx = qtScene.camera_position.x - qtOrbitCenter.x;
      const float qtOrbitDz = qtScene.camera_position.z - qtOrbitCenter.z;
      const float kQtOrbitRadius = std::sqrt(qtOrbitDx * qtOrbitDx + qtOrbitDz * qtOrbitDz);
      const float qtOrbitInitialAngleDeg = std::atan2(qtOrbitDx, qtOrbitDz) * (180.0f / 3.14159265f);
      float qtOrbitLastAngleDeg = qtOrbitInitialAngleDeg;
      const auto qtOrbitStartTime = std::chrono::steady_clock::now();
      if (qtEnableAutoOrbit) {
        std::cout << "[orbit] setup: center=(" << qtOrbitCenter.x << "," << qtOrbitCenter.y << "," << qtOrbitCenter.z
                  << ") radius=" << kQtOrbitRadius << " initial_angle=" << qtOrbitInitialAngleDeg << "\n";
      } else if (qtPhysicsRuntimeEnabled) {
        std::cout << "[orbit] disabled for dynamic physics scene; using authored camera\n";
      } else if (qtUseBg) {
        std::cout << "[orbit] disabled for background tracer to preserve throughput\n";
      } else {
        std::cout << "[orbit] disabled by window frame limit\n";
      }

      // ---- Main Qt event loop with rendering ----
      qtStartupStep("entering qt render loop");
      uint32_t qtFrameCount = 0u;
      bool qtUserCameraActive = false;
      bool qtPlayModeFramePacingActive = false;
      auto qtInteractiveFrameTarget = [&]() {
        const uint32_t publishHz = std::max<uint32_t>(1u, qtPreviewPublishHz);
        if (qtPlayModeFramePacingActive && publishHz == 60u) {
          // The framebuffer handoff remains capped at 60 Hz. Waking the app
          // loop slightly ahead of the display interval keeps Play mode from
          // missing handoff slots when Windows/Qt sleep granularity lands late.
          return std::chrono::microseconds(15000u);
        }
        return std::chrono::microseconds(
            1000000u / publishHz);
      };
      double qtProfilePollMs = 0.0;
      double qtProfileInputMs = 0.0;
      double qtProfilePhysicsMs = 0.0;
      double qtProfileSelectionMs = 0.0;
      double qtProfileRenderMs = 0.0;
      double qtProfileUiMs = 0.0;
      double qtProfileFrameMs = 0.0;
      double qtProfileMaxFrameMs = 0.0;
      uint32_t qtProfileFrames = 0u;
      double qtProfilePaceWaitMs = 0.0;
      double qtProfileMaxLateMs = 0.0;
      uint32_t qtProfileLateFrames = 0u;
      uint32_t qtLastGpuBatchesPerTick = 0u;
      double qtSmoothedGpuBatchMs = 0.0;
      std::uint64_t qtGizmoDragInputSamples = 0u;
      std::uint64_t qtGizmoTransformImmediatePublishes = 0u;
      std::uint64_t qtGizmoTransformPublishedEntityCount = 0u;
      std::uint64_t qtGizmoTransformPublishFailures = 0u;
      std::uint64_t qtOverlayRequiredRenderGeneration = 0u;
      std::uint64_t qtOverlayStaleSkips = 0u;
      auto qtLatestPaintedRenderGeneration = [&]() -> std::uint64_t {
#ifdef PT_ENABLE_QT
        if (qtWindow == nullptr) {
          return 0u;
        }
        return qtWindow->framebuffer_stats().latestPaintedGeneration;
#else
        return 0u;
#endif
      };
      auto qtMarkOverlayRequiresRenderCatchup = [&]() {
        if (!qtUseBg || !qtRenderCoordinator) {
          qtOverlayRequiredRenderGeneration = 0u;
          return;
        }
        const auto renderStats = qtRenderCoordinator->stats();
        qtOverlayRequiredRenderGeneration =
            std::max<std::uint64_t>(qtOverlayRequiredRenderGeneration,
                                    renderStats.generation + 1u);
      };
      bool qtDeferBackgroundSnapshotPublishes = false;
      bool qtDeferredBackgroundSnapshotPending = false;
      bool qtDeferredBackgroundTopologyChanged = false;
      bool qtDeferredBackgroundTransformChanged = false;
      bool qtDeferredBackgroundCameraChanged = false;
      bool qtDeferredBackgroundMaterialChanged = false;
      auto qtFlushDeferredBackgroundSnapshot = [&]() -> std::uint64_t {
        if (!qtDeferredBackgroundSnapshotPending) {
          return 0u;
        }
        const bool topologyChanged = qtDeferredBackgroundTopologyChanged;
        const bool transformChanged = qtDeferredBackgroundTransformChanged;
        const bool cameraChanged = qtDeferredBackgroundCameraChanged;
        const bool materialChanged = qtDeferredBackgroundMaterialChanged;
        qtDeferredBackgroundSnapshotPending = false;
        qtDeferredBackgroundTopologyChanged = false;
        qtDeferredBackgroundTransformChanged = false;
        qtDeferredBackgroundCameraChanged = false;
        qtDeferredBackgroundMaterialChanged = false;

        const auto generation = qtPublishRenderSnapshot(topologyChanged,
                                                        transformChanged,
                                                        cameraChanged,
                                                        materialChanged);
        if (qtUseBg && qtRenderCoordinator && generation != 0u) {
          qtOverlayRequiredRenderGeneration =
              std::max(qtOverlayRequiredRenderGeneration, generation);
        }
        return generation;
      };
      auto qtPublishRenderSnapshotForBackground =
          [&](bool topology_changed,
              bool transform_changed,
              bool camera_changed,
              bool material_changed) -> std::uint64_t {
        if (qtDeferBackgroundSnapshotPublishes && qtUseBg && qtRenderCoordinator) {
          qtDeferredBackgroundSnapshotPending = true;
          qtDeferredBackgroundTopologyChanged =
              qtDeferredBackgroundTopologyChanged || topology_changed;
          qtDeferredBackgroundTransformChanged =
              qtDeferredBackgroundTransformChanged || transform_changed;
          qtDeferredBackgroundCameraChanged =
              qtDeferredBackgroundCameraChanged || camera_changed;
          qtDeferredBackgroundMaterialChanged =
              qtDeferredBackgroundMaterialChanged || material_changed;
          return 0u;
        }
        const auto generation = qtPublishRenderSnapshot(topology_changed,
                                                        transform_changed,
                                                        camera_changed,
                                                        material_changed);
        if (qtUseBg && qtRenderCoordinator && generation != 0u) {
          qtOverlayRequiredRenderGeneration =
              std::max(qtOverlayRequiredRenderGeneration, generation);
        }
        return generation;
      };
      auto qtRenderImageCaughtUpForOverlay = [&]() -> bool {
        if (qtOverlayRequiredRenderGeneration == 0u) {
          return true;
        }
        if (qtLatestPaintedRenderGeneration() >= qtOverlayRequiredRenderGeneration) {
          qtOverlayRequiredRenderGeneration = 0u;
          return true;
        }
        ++qtOverlayStaleSkips;
        return false;
      };
#ifndef PT_ENABLE_QT
      (void)qtFramebufferDisplayInterval;
      (void)qtPublishedSample;
      (void)qtPublishedRays;
      (void)qtPublishedFrames;
      (void)qtDroppedFrames;
      (void)qtNextFramebufferSubmit;
      (void)qtSampleIndex;
      (void)kQtOrbitDegPerSec;
      (void)kQtOrbitMinStepDeg;
      (void)qtOrbitLastAngleDeg;
      (void)qtOrbitStartTime;
      (void)qtFrameCount;
      (void)qtUserCameraActive;
      (void)qtInteractiveFrameTarget;
      (void)qtProfilePollMs;
      (void)qtProfileInputMs;
      (void)qtProfilePhysicsMs;
      (void)qtProfileSelectionMs;
      (void)qtProfileRenderMs;
      (void)qtProfileUiMs;
      (void)qtProfileFrameMs;
      (void)qtProfileMaxFrameMs;
      (void)qtProfileFrames;
      (void)qtProfilePaceWaitMs;
      (void)qtProfileMaxLateMs;
      (void)qtProfileLateFrames;
      (void)qtLastGpuBatchesPerTick;
      (void)qtSmoothedGpuBatchMs;
      (void)qtGizmoDragInputSamples;
      (void)qtGizmoTransformImmediatePublishes;
      (void)qtGizmoTransformPublishedEntityCount;
      (void)qtGizmoTransformPublishFailures;
      (void)qtOverlayRequiredRenderGeneration;
      (void)qtOverlayStaleSkips;
      (void)qtLatestPaintedRenderGeneration;
      (void)qtMarkOverlayRequiresRenderCatchup;
      (void)qtFlushDeferredBackgroundSnapshot;
      (void)qtPublishRenderSnapshotForBackground;
      (void)qtRenderImageCaughtUpForOverlay;
#endif
#ifdef PT_ENABLE_QT
#include "AppRuntimeQtCameraAndScene.inc"
#include "AppRuntimeQtBackendSwitch.inc"
#include "AppRuntimeQtReloadAndSceneGraph.inc"
#include "AppRuntimeQtAssetsAndPlayback.inc"
#include "AppRuntimeQtDockPropertyEdits.inc"
#include "AppRuntimeQtViewportInteraction.inc"
#include "AppRuntimeQtDockSyncAndPhysics.inc"
#endif
#include "AppRuntimeQtStressGate.inc"
#ifdef PT_ENABLE_QT
#include "AppRuntimeQtRenderLoop.inc"
#else
    }
    LogUiWindowStartup(windowWidth, windowHeight, ui_layout_state, ui_runtime_state);
    logger.log(vkpt::log::Severity::Info, "app",
               "ui window mode selected; attempting to create desktop platform");
    recordUiAction("app.window", "window", "window init",
                   MakeUnsupportedUiCommand("app.window", "window mode requested"));

    if (windowWidth == 0u || windowHeight == 0u) {
      std::cout << "invalid ui window size (" << windowWidth << "x" << windowHeight
                << "); falling back to 1280x720\n";
      logger.log(vkpt::log::Severity::Warning, "app",
                 "invalid ui window size, using default fallback 1280x720");
      windowWidth = 1280;
      windowHeight = 720;
    }
#endif
#if !defined(PT_ENABLE_RAW_DESKTOP)
#include "AppRuntimeRawDesktopUnavailable.inc"
#else
#include "AppRuntimeRawDesktopSetup.inc"
#include "AppRuntimeRawDesktopFrameLoop.inc"
#endif
#include "AppRuntimeRenderAndHeadless.inc"
}
}  // namespace vkpt::app
