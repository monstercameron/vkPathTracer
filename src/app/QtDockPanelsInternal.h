#pragma once

#ifdef PT_ENABLE_QT

#include "app/QtDockPanels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::app {

std::string QtDockBool(bool value);
std::string QtDockNumber(double value, int precision = 2);
std::string QtDockBytes(std::uint64_t bytes);
std::string QtDockRate(double raysPerSecond);
int QtDockPreferredPixels(float value);
std::uint64_t EstimateQtSceneMemoryBytes(const vkpt::pathtracer::PathTracerSceneSnapshot& scene);
std::string QtDockFeatureSummary(const vkpt::render::RenderBackendCapabilities& caps);
std::string QtDockAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel);
std::string QtDockMemoryUsage(std::uint64_t usage,
                              std::uint64_t budget,
                              std::string_view unavailable_reason);
std::string QtDockJoin(const std::vector<std::string>& parts,
                       std::string_view separator = " | ");
std::string QtDockToLower(std::string text);
std::string QtDockPathString(const std::filesystem::path& path);
std::filesystem::path QtDockAbsoluteNormalizedPath(std::string_view path);
std::string QtDockDisplayPath(const std::filesystem::path& path);
std::filesystem::path QtDockFindRepoRelativePath(const std::filesystem::path& relativePath);
bool QtDockSamePath(std::string_view lhs, std::string_view rhs);
bool QtDockHasExtension(const std::filesystem::path& path,
                        std::initializer_list<std::string_view> extensions);
std::vector<std::filesystem::path> QtDockFindAssetFiles(
    const std::filesystem::path& root,
    std::initializer_list<std::string_view> extensions,
    bool recursive);
std::string QtDockPrettyStem(const std::filesystem::path& path);
std::string QtDockReadSceneDisplayName(const std::filesystem::path& path);
QtDockTreeRow QtDockAssetFileRow(std::string idPrefix,
                                 const std::filesystem::path& path,
                                 std::string label,
                                 std::string icon,
                                 bool selected);
std::string QtDockFallbackRayDeviceKey(std::string_view selected_backend,
                                       std::string_view renderer_path);
const QtDockRayDeviceMetric* QtDockFindRayMetric(
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::string_view device_key);
bool QtDockRayMetricHasRate(const QtDockRayDeviceMetric* metric);
void QtDockAppendRayMetricTags(std::vector<std::string>& tags,
                               const QtDockRayDeviceMetric* metric);
std::string QtDockShortAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel);
bool QtDockSameAccelerator(const vkpt::render::AcceleratorCapabilities& lhs,
                           const vkpt::render::AcceleratorCapabilities& rhs);
bool QtDockRayDeviceEligible(const vkpt::render::AcceleratorCapabilities& accel);
std::uint64_t QtDockAcceleratorMemoryBytes(const vkpt::render::AcceleratorCapabilities& accel);
std::string QtDockAcceleratorSummary(const vkpt::render::AcceleratorCapabilities& accel,
                                     bool active,
                                     const QtDockRayDeviceMetric* metric = nullptr);
std::string QtDockAcceleratorGroupSummary(
    const std::vector<const vkpt::render::AcceleratorCapabilities*>& accelerators,
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::size_t maxRows = 3u);
std::string QtDockVec3(float x, float y, float z);
std::string QtDockVec3(const vkpt::scene::Vec3& v);
std::string QtDockVec3(const vkpt::pathtracer::Vec3& v);
std::string QtDockVec3(const vkpt::editor::Vec3& v);
std::string QtDockBounds(const vkpt::editor::Bounds& bounds);
void QtDockAddProperty(QtDockPanelContent& panel,
                       std::string_view label,
                       std::string value);
void QtDockAddEditableGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::string unit = {});
void QtDockAddTextGroupedProperty(QtDockPanelContent& panel,
                                  std::string id,
                                  std::string_view group,
                                  std::string_view label,
                                  std::string value);
void QtDockAddDropdownGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::vector<std::string> options);
void QtDockAddToggleGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    bool value);
void QtDockAddButtonGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    std::string value);
void QtDockAddSliderGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    double value,
                                    double minimum,
                                    double maximum,
                                    double step,
                                    double default_value,
                                    std::string unit = {});
void QtDockAddSliderProperty(QtDockPanelContent& panel,
                             std::string id,
                             std::string_view label,
                             double value,
                             double minimum,
                             double maximum,
                             double step,
                             double default_value);
std::vector<std::string> QtMaterialFamilyOptions();
std::string QtStableIdDisplayLabel(std::string label, vkpt::core::StableId id);
std::string QtMaterialDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id);
std::string QtGeometryDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id);
std::vector<std::string> QtMaterialIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current);
std::vector<std::string> QtGeometryIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current);
std::vector<std::string> QtLightTypeOptions();
std::vector<std::string> QtSdfShapeOptions();
std::vector<std::string> QtPhysicsBodyTypeOptions();
std::vector<std::string> QtPhysicsShapeOptions();
std::vector<std::string> QtBoolOptions();
std::vector<std::string> QtToneMapOptions();
std::vector<std::string> QtOutputTransformOptions();
std::string QtToneMapName(vkpt::pathtracer::ToneMapMode mode);
std::string QtOutputTransformName(vkpt::pathtracer::OutputTransformMode mode);
void QtDockAddVec3Sliders(QtDockPanelContent& panel,
                          std::string_view prefix,
                          std::string_view group,
                          std::string_view label,
                          const vkpt::scene::Vec3& value,
                          const vkpt::scene::Vec3& defaults,
                          double minimum,
                          double maximum,
                          double step);
void QtDockAddInspectorTransformControls(QtDockPanelContent& panel,
                                         std::string_view prefix,
                                         const vkpt::scene::TransformComponent& transform);
void QtDockAddPrimaryCameraControls(QtDockPanelContent& panel,
                                    std::string_view prefix,
                                    const vkpt::scene::CameraComponent& camera);
void QtDockAddRow(QtDockPanelContent& panel, std::string row);
void QtDockAddTreeRow(QtDockPanelContent& panel, QtDockTreeRow row);
const vkpt::editor::UiPanelState* FindQtLayoutPanel(
    const vkpt::editor::UiLayoutDocument& layout,
    std::string_view panel_id);
QtDockPanelContent MakeQtDockPanel(const vkpt::editor::UiLayoutDocument& layout,
                                   std::string_view id,
                                   std::string_view title,
                                   bool default_visible,
                                   float default_width = 320.0f,
                                   float default_height = 240.0f);
const vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
const vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
std::string QtEntityComponentSummary(const vkpt::scene::SceneEntityDefinition& entity);
vkpt::core::StableId QtPrimarySelectionId(const vkpt::editor::SelectionState& selection);
const vkpt::editor::Bounds* FindQtSelectionBounds(
    const vkpt::editor::SelectionState& selection,
    vkpt::core::StableId entity_id);
void QtDockLimitRows(QtDockPanelContent& panel, std::size_t max_rows);

QtDockPanelContent BuildQtInspectorDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiRuntimeState& runtime,
                                        const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtMaterialsDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                                        const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtLightsDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                                     const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtCameraDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     int active_shot_slot,
                                     const std::array<bool, 4>& saved_shot_slots);
QtDockPanelContent BuildQtBenchmarkDock(const vkpt::editor::BenchmarkPanelModel& benchmark,
                                        const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtDiagnosticsDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::SelectionState& selection,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats);
QtDockPanelContent BuildQtPerformanceDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats);
QtDockPanelContent BuildQtMetricsDock(const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtEventsDock(const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtHealthDock(const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtDebugViewsDock(const vkpt::editor::UiRuntimeState& runtime,
                                         const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtTimelineDock(const vkpt::scene::SceneDocument& document,
                                       const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtScriptDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::editor::SelectionState& selection,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockScriptRuntimeState* runtime);
QtDockPanelContent BuildQtPhysicsDock(const vkpt::scene::SceneDocument& document,
                                      const vkpt::editor::UiLayoutDocument& layout);

vkpt::platform::QtDockArea QtDockAreaForPanel(std::string_view panel_id);
vkpt::platform::QtDockRow ToQtPlatformDockRow(const QtDockTreeRow& row);
std::vector<vkpt::platform::QtDockPanel> ToQtPlatformDockPanels(
    const std::vector<QtDockPanelContent>& panels);

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
