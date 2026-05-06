      ViewportCameraPose qtCameraPose{qtScene.camera_position,
                                      qtScene.camera_target,
                                      qtScene.camera_up,
                                      qtScene.camera_fov_deg};
      auto syncQtCameraPoseFromScene = [&]() {
        qtCameraPose.position = qtScene.camera_position;
        qtCameraPose.target = qtScene.camera_target;
        qtCameraPose.up = qtScene.camera_up;
        qtCameraPose.fov_deg = qtScene.camera_fov_deg;
      };
      auto qtCameraForward = [&]() {
        return PtNormalize(PtSub(qtCameraPose.target, qtCameraPose.position));
      };
      std::function<void()> qtSyncActiveCameraObjectFromPose;
      std::function<std::string()> qtActiveCameraObjectName;
      std::chrono::steady_clock::time_point qtLastFpsCameraObjectSync{};
      float qtFpsYaw = 0.0f;
      float qtFpsPitch = 0.0f;
      auto syncQtFpsAnglesFromPose = [&]() {
        const auto forward = qtCameraForward();
        qtFpsYaw = std::atan2(forward.x, forward.z);
        qtFpsPitch = std::asin(ClampFloat(forward.y, -0.98f, 0.98f));
      };
      auto qtFpsForwardFromAngles = [&]() {
        const float cosPitch = std::cos(qtFpsPitch);
        return PtNormalize(vkpt::pathtracer::Vec3{
            std::sin(qtFpsYaw) * cosPitch,
            std::sin(qtFpsPitch),
            std::cos(qtFpsYaw) * cosPitch});
      };
      auto qtLastCameraInputTime = std::chrono::steady_clock::time_point{};
      auto applyQtCameraPose = [&](std::string_view reason, bool syncSceneCameraObject = true) {
        qtLastCameraInputTime = std::chrono::steady_clock::now();
        if (syncSceneCameraObject && qtSyncActiveCameraObjectFromPose) {
          qtSyncActiveCameraObjectFromPose();
        }
        qtScene.camera_position = qtCameraPose.position;
        qtScene.camera_target = qtCameraPose.target;
        qtScene.camera_up = qtCameraPose.up;
        qtScene.camera_fov_deg = qtCameraPose.fov_deg;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
              qtScene.camera_position,
              qtScene.camera_target,
              qtScene.camera_up,
              qtScene.camera_fov_deg});
        } else if (!qtUseBg && qtTracerReady) {
          const bool camOk = qtTracer->update_camera(
              qtScene.camera_position, qtScene.camera_target,
              qtScene.camera_up, qtScene.camera_fov_deg);
          if (!camOk) {
            qtTracer->load_scene_snapshot(qtScene);
            qtTracer->build_or_update_acceleration();
          }
          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        ui_runtime_state.status_message = std::string("camera ") + std::string(reason);
      };
      auto qtRenderAspect = [&]() {
        const float renderWidth = static_cast<float>(
            std::max(1u, qtPublishedWidth.load(std::memory_order_relaxed)));
        const float renderHeight = static_cast<float>(
            std::max(1u, qtPublishedHeight.load(std::memory_order_relaxed)));
        return renderWidth / renderHeight;
      };
      vkpt::pathtracer::Vec3 qtCameraFocusPoint = qtCameraPose.target;
      float qtCameraFocusDistance =
          std::max(0.25f, PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
      struct QtSavedCameraShot {
        bool valid = false;
        ViewportCameraPose pose{};
        float focus_distance = 1.0f;
      };
      std::array<QtSavedCameraShot, 4> qtSavedCameraShots{};
      int qtActiveCameraShotSlot = 0;
      struct QtViewportImageRect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
      };
      auto qtViewportImageRect = [&]() {
        const auto metrics = window->metrics();
        const float viewportWidth = static_cast<float>(std::max(1, metrics.width));
        const float viewportHeight = static_cast<float>(std::max(1, metrics.height));
        const float aspect = std::max(0.01f, qtRenderAspect());
        QtViewportImageRect out{};
        const float viewportAspect = viewportWidth / viewportHeight;
        if (viewportAspect > aspect) {
          out.height = viewportHeight;
          out.width = viewportHeight * aspect;
          out.x = (viewportWidth - out.width) * 0.5f;
          out.y = 0.0f;
        } else {
          out.width = viewportWidth;
          out.height = viewportWidth / aspect;
          out.x = 0.0f;
          out.y = (viewportHeight - out.height) * 0.5f;
        }
        out.width = std::max(1.0f, out.width);
        out.height = std::max(1.0f, out.height);
        return out;
      };
      auto qtViewportLocalPoint = [&](float x, float y) -> std::optional<std::pair<float, float>> {
        const auto frameRect = qtViewportImageRect();
        if (x < frameRect.x || y < frameRect.y ||
            x > frameRect.x + frameRect.width ||
            y > frameRect.y + frameRect.height) {
          return std::nullopt;
        }
        return std::pair<float, float>{x - frameRect.x, y - frameRect.y};
      };
      auto qtOffsetOverlayBoxes = [](std::vector<vkpt::platform::QtSelectionOverlayBox> boxes,
                                     const QtViewportImageRect& frameRect) {
        for (auto& box : boxes) {
          box.x += frameRect.x;
          box.y += frameRect.y;
          for (auto& line : box.lines) {
            line.x0 += frameRect.x;
            line.y0 += frameRect.y;
            line.x1 += frameRect.x;
            line.y1 += frameRect.y;
          }
          for (auto& point : box.points) {
            point.x += frameRect.x;
            point.y += frameRect.y;
          }
        }
        return boxes;
      };
      std::optional<ViewportGizmoHit> qtHoveredGizmoHit;
      auto updateQtSelectionOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const auto frameRect = qtViewportImageRect();
        auto boxes = BuildSelectionOverlayBoxes(
            ui_selection_state,
            qtPickables,
            qtCameraPose,
            frameRect.width,
            frameRect.height,
            qtRenderAspect(),
            ui_runtime_state.active_gizmo_mode,
            qtHoveredGizmoHit);
        if (qtCameraFocusDistance > 0.0f &&
            (qtScene.camera_focus_distance > 0.0f || qtScene.camera_aperture_radius > 0.0f)) {
          vkpt::platform::QtSelectionOverlayBox focusBox{};
          focusBox.label = "focus";
          focusBox.primary = false;
          const auto forward = PtNormalize(PtSub(qtCameraPose.target, qtCameraPose.position));
          const auto right = PtNormalize(PtCross(forward, qtCameraPose.up), {1.0f, 0.0f, 0.0f});
          const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
          const float half = std::max(0.05f, qtCameraFocusDistance * 0.08f);
          constexpr OverlayColor kFocus{80u, 220u, 255u, 170u};
          AddWorldOverlayLine(focusBox,
                              qtCameraPose,
                              frameRect.width,
                              frameRect.height,
                              qtRenderAspect(),
                              PtSub(qtCameraFocusPoint, PtMul(right, half)),
                              PtAdd(qtCameraFocusPoint, PtMul(right, half)),
                              kFocus,
                              1.5f);
          AddWorldOverlayLine(focusBox,
                              qtCameraPose,
                              frameRect.width,
                              frameRect.height,
                              qtRenderAspect(),
                              PtSub(qtCameraFocusPoint, PtMul(up, half)),
                              PtAdd(qtCameraFocusPoint, PtMul(up, half)),
                              kFocus,
                              1.5f);
          AddWorldOverlayPoint(focusBox,
                               qtCameraPose,
                               frameRect.width,
                               frameRect.height,
                               qtRenderAspect(),
                               qtCameraFocusPoint,
                               kFocus,
                               3.0f);
          if (!focusBox.lines.empty() || !focusBox.points.empty()) {
            boxes.push_back(std::move(focusBox));
          }
        }
        qtWindow->set_selection_overlay_boxes(qtOffsetOverlayBoxes(std::move(boxes), frameRect));
      };
      syncQtFpsAnglesFromPose();
      bool qtFpsMode = false;
      FpsPlayerState qtFpsPlayer{};
      bool qtLeftMouseDown = false;
      bool qtRightMouseDown = false;
      bool qtMiddleMouseDown = false;
      bool qtPotentialClick = false;
      float qtClickX = 0.0f;
      float qtClickY = 0.0f;
      float qtClickDragPixels = 0.0f;
      float qtLastMouseX = 0.0f;
      float qtLastMouseY = 0.0f;
      float qtPendingFpsLookDx = 0.0f;
      float qtPendingFpsLookDy = 0.0f;
      float qtPendingFpsMoveDt = 0.0f;
      std::uint64_t qtFpsMoveSequence = 0u;
      std::uint64_t qtFpsAppliedMoveSequence = 0u;
      bool qtDockPanelsDirty = true;
      auto qtLastFpsDockRefresh = std::chrono::steady_clock::time_point{};
      constexpr auto kQtFpsDockRefreshInterval = std::chrono::milliseconds(250);
      auto qtMarkFpsDockPanelsDirty = [&](bool immediate) {
        const auto now = std::chrono::steady_clock::now();
        if (immediate ||
            qtLastFpsDockRefresh == std::chrono::steady_clock::time_point{} ||
            now - qtLastFpsDockRefresh >= kQtFpsDockRefreshInterval) {
          qtDockPanelsDirty = true;
          qtLastFpsDockRefresh = now;
        }
      };
      auto qtLastTransformDockRefresh = std::chrono::steady_clock::time_point{};
      constexpr auto kQtTransformDockRefreshInterval = std::chrono::milliseconds(125);
      auto qtMarkTransformDockPanelsDirty = [&](bool immediate) {
        const auto now = std::chrono::steady_clock::now();
        if (immediate ||
            qtLastTransformDockRefresh == std::chrono::steady_clock::time_point{} ||
            now - qtLastTransformDockRefresh >= kQtTransformDockRefreshInterval) {
          qtDockPanelsDirty = true;
          qtLastTransformDockRefresh = now;
        }
      };
      auto qtDiscardFpsMovementResults = [&]() {
        qtPendingFpsMoveDt = 0.0f;
        qtFpsAppliedMoveSequence = qtFpsMoveSequence;
        qtFpsCollisionWorker.discard_pending_results();
      };
      struct QtGizmoDragEntityStart {
        vkpt::core::StableId entity_id = 0;
        bool sdf_primitive = false;
        vkpt::scene::TransformComponent transform;
        vkpt::pathtracer::Vec3 local_pivot{};
      };
      struct QtGizmoDragState {
        bool active = false;
        ViewportGizmoHit hit;
        float start_x = 0.0f;
        float start_y = 0.0f;
        std::vector<QtGizmoDragEntityStart> entities;
        bool scene_reload_pending = false;
      } qtGizmoDrag;
      std::unordered_set<int> qtKeysDown;
      auto qtNormalizeKey = [](int key) {
        if (key >= 'a' && key <= 'z') {
          return static_cast<int>(std::toupper(static_cast<unsigned char>(key)));
        }
        return key;
      };
      constexpr int kQtKeyEscape = 0x01000000;
      constexpr int kQtKeyShift = 0x01000020;
      constexpr int kQtKeyControl = 0x01000021;
      constexpr int kQtKeyLeft = 0x01000012;
      constexpr int kQtKeyUp = 0x01000013;
      constexpr int kQtKeyRight = 0x01000014;
      constexpr int kQtKeyDown = 0x01000015;
      constexpr int kQtKeySpace = 0x20;
      constexpr int kQtKeyBackspace = 0x01000003;
      constexpr int kQtKeyDelete = 0x01000007;
      auto qtKeyActive = [&](int key) {
        return qtKeysDown.find(key) != qtKeysDown.end() ||
               qtKeysDown.find(qtNormalizeKey(key)) != qtKeysDown.end();
      };
      auto qtAppendSelection = [&]() {
        return qtKeyActive(kQtKeyShift) || qtKeyActive(kQtKeyControl) ||
               qtKeyActive(16) || qtKeyActive(17) || IsShiftDown() || IsCtrlDown();
      };
      auto qtActiveSelectionBounds = [&]() -> std::optional<vkpt::editor::Bounds> {
        if (ui_selection_state.active_primary_entity == 0u) {
          return std::nullopt;
        }
        for (const auto& item : ui_selection_state.per_item_bounds) {
          if (item.entity_id == ui_selection_state.active_primary_entity && item.bounds.valid) {
            return item.bounds;
          }
        }
        return std::nullopt;
      };
      auto qtSetViewportCursor = [&](vkpt::platform::QtViewportCursor cursor) {
        if (qtWindow != nullptr) {
          qtWindow->set_viewport_cursor(cursor);
        }
      };
      auto qtSetViewportMouseLocked = [&](bool locked) {
        if (qtWindow != nullptr) {
          qtWindow->set_viewport_mouse_locked(locked);
        }
      };
      auto qtUpdateGizmoHoverCursor = [&](float x, float y) {
        auto clearHover = [&]() {
          if (qtHoveredGizmoHit) {
            qtHoveredGizmoHit = std::nullopt;
            updateQtSelectionOverlay();
          }
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        };
        if (qtFpsMode ||
            qtLeftMouseDown ||
            qtRightMouseDown ||
            qtMiddleMouseDown ||
            ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          clearHover();
          return;
        }
        const auto activeBounds = qtActiveSelectionBounds();
        if (!activeBounds) {
          clearHover();
          return;
        }
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        if (!localPoint) {
          clearHover();
          return;
        }
        auto hit = PickSelectionGizmoHandle(
            *activeBounds,
            qtCameraPose,
            frameRect.width,
            frameRect.height,
            qtRenderAspect(),
            ui_runtime_state.active_gizmo_mode,
            localPoint->first,
            localPoint->second);
        if (!hit) {
          hit = PickSelectionBoundsFreeform(
              *activeBounds,
              qtCameraPose,
              frameRect.width,
              frameRect.height,
              qtRenderAspect(),
              ui_runtime_state.active_gizmo_mode,
              localPoint->first,
              localPoint->second);
        }
        qtSetViewportCursor(hit ? CursorForGizmoHit(*hit)
                                : vkpt::platform::QtViewportCursor::Default);
        if (!SameGizmoHandle(qtHoveredGizmoHit, hit)) {
          qtHoveredGizmoHit = hit;
          updateQtSelectionOverlay();
        }
      };
      auto qtFindEntity = [&](vkpt::core::StableId id) -> vkpt::scene::SceneEntityDefinition* {
        return FindQtSceneEntity(qtSceneDocument, id);
      };
      auto qtFindRtInstanceIndex = [&](vkpt::core::StableId entityId) -> uint32_t {
        for (std::size_t index = 0; index < qtScene.instances.size(); ++index) {
          if (qtScene.instances[index].entity_id == entityId) {
            return static_cast<uint32_t>(index);
          }
        }
        return vkpt::pathtracer::kInvalidRTInstanceIndex;
      };
      auto qtFindSdfPrimitive = [&](vkpt::core::StableId id) -> vkpt::scene::SceneSdfPrimitiveDefinition* {
        return FindQtSceneSdfPrimitive(qtSceneDocument, id);
      };
      auto qtFindTransformEntry = [&](vkpt::core::StableId id) -> const vkpt::scene::SceneTransformEntry* {
        return FindQtSceneTransformEntry(qtSceneDocument, id);
      };
      auto qtNextSceneObjectId = [&]() {
        return NextQtSceneObjectId(qtSceneDocument);
      };
      auto qtRemoveLegacyTransformEntry = [&](vkpt::core::StableId id) {
        RemoveQtLegacyTransformEntry(qtSceneDocument, id);
      };
      auto qtEnsureSceneObjectEntity = [&](vkpt::core::StableId requestedId,
                                           std::string_view fallbackName)
          -> vkpt::scene::SceneEntityDefinition& {
        return EnsureQtSceneObjectEntity(qtSceneDocument, requestedId, fallbackName);
      };
      auto qtEntityParentId = [&](const vkpt::scene::SceneEntityDefinition& entity) {
        return QtSceneEntityParentId(qtSceneDocument, entity);
      };
      auto qtFindSceneRootEntityId = [&]() {
        return FindQtSceneRootEntityId(qtSceneDocument);
      };
      auto qtNextSiblingOrder = [&](vkpt::core::StableId parentId) {
        return NextQtSceneSiblingOrder(qtSceneDocument, parentId);
      };
      auto qtWouldCreateParentCycle = [&](vkpt::core::StableId childId,
                                          vkpt::core::StableId newParentId) {
        return WouldCreateQtSceneParentCycle(qtSceneDocument, childId, newParentId);
      };
      auto qtSetEntityParentPreserveWorld =
          [&](vkpt::core::StableId entityId,
              vkpt::core::StableId newParentId,
              const vkpt::scene::SceneWorld* preWriteWorld,
              bool preserveWorld) {
        return SetQtSceneEntityParentPreserveWorld(
            qtSceneDocument, entityId, newParentId, preWriteWorld, preserveWorld);
      };
      auto qtPromoteSceneObjectCamerasAndLights = [&]() {
        PromoteQtSceneObjectCamerasAndLights(qtSceneDocument);
      };
      auto qtDocumentHasLightObject = [&]() {
        return std::any_of(qtSceneDocument.entities.begin(),
                           qtSceneDocument.entities.end(),
                           [](const vkpt::scene::SceneEntityDefinition& entity) {
                             return entity.has_light;
                           });
      };
      auto qtRuntimeSceneHasEmissiveMaterial = [&]() {
        return std::any_of(qtScene.materials.begin(),
                           qtScene.materials.end(),
                           [](const vkpt::pathtracer::RTMaterial& material) {
                             return material.is_emissive();
                           });
      };
      auto qtEnsureRuntimeLightObject = [&]() {
        qtPromoteSceneObjectCamerasAndLights();
        if (qtDocumentHasLightObject()) {
          return;
        }
        vkpt::scene::SceneEntityDefinition* entity = nullptr;
        if (!qtScene.lights.empty()) {
          const auto& runtimeLight = qtScene.lights.front();
          auto& lightEntity = qtEnsureSceneObjectEntity(0u, "Key Light");
          lightEntity.has_transform = true;
          lightEntity.transform.translation = ToSceneVec3(runtimeLight.position);
          lightEntity.transform.dirty = true;
          lightEntity.has_light = true;
          lightEntity.light.type = "point";
          lightEntity.light.color = ToSceneVec3(runtimeLight.color);
          lightEntity.light.intensity = runtimeLight.intensity;
          lightEntity.light.radius = runtimeLight.radius;
          lightEntity.light.direction = ToSceneVec3(runtimeLight.direction);
          entity = &lightEntity;
        } else if (!qtRuntimeSceneHasEmissiveMaterial() &&
                   (PtLength(qtScene.environment_color) > 1.0e-6f)) {
          auto& lightEntity = qtEnsureSceneObjectEntity(0u, "Environment Light");
          lightEntity.has_light = true;
          lightEntity.light.type = "environment";
          lightEntity.light.color = ToSceneVec3(qtScene.environment_color);
          lightEntity.light.intensity = 1.0f;
          entity = &lightEntity;
        }
        if (entity != nullptr) {
          qtRemoveLegacyTransformEntry(entity->id);
        }
      };
      auto qtFindActiveCameraEntity = [&]() -> vkpt::scene::SceneEntityDefinition* {
        const auto it = std::find_if(qtSceneDocument.entities.begin(),
                                     qtSceneDocument.entities.end(),
                                     [](const vkpt::scene::SceneEntityDefinition& entity) {
                                       return entity.has_camera;
                                     });
        return it == qtSceneDocument.entities.end() ? nullptr : &*it;
      };
      auto qtEnsureActiveCameraEntity = [&]() -> vkpt::scene::SceneEntityDefinition* {
        qtPromoteSceneObjectCamerasAndLights();
        if (auto* entity = qtFindActiveCameraEntity()) {
          if (entity->name.empty()) {
            entity->name = "Camera";
          }
          return entity;
        }
        auto& entity = qtEnsureSceneObjectEntity(0u, "Camera");
        entity.has_camera = true;
        entity.camera.fov = qtCameraPose.fov_deg;
        entity.camera.focus_distance = qtCameraFocusDistance;
        return &entity;
      };
      auto qtWriteActiveCameraObjectFromPose = [&]() {
        auto* entity = qtEnsureActiveCameraEntity();
        if (entity == nullptr) {
          return;
        }
        entity->has_camera = true;
        entity->camera.fov = qtCameraPose.fov_deg;
        entity->camera.focus_distance = qtCameraFocusDistance;
        entity->has_transform = true;
        entity->transform.translation = ToSceneVec3(qtCameraPose.position);
        entity->transform.rotation = QuatFromCameraForwardUp(qtCameraForward(), qtCameraPose.up);
        entity->transform.dirty = true;
        qtRemoveLegacyTransformEntry(entity->id);
      };
      qtSyncActiveCameraObjectFromPose = qtWriteActiveCameraObjectFromPose;
      qtActiveCameraObjectName = [&]() {
        qtPromoteSceneObjectCamerasAndLights();
        if (const auto* entity = qtFindActiveCameraEntity()) {
          return QtEntityDisplayName(*entity) + " #" + std::to_string(entity->id);
        }
        return std::string("runtime camera");
      };
      auto qtMaybeSyncFpsCameraObject = [&](bool force) {
        if (!qtSyncActiveCameraObjectFromPose) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            qtLastFpsCameraObjectSync != std::chrono::steady_clock::time_point{} &&
            now - qtLastFpsCameraObjectSync < std::chrono::milliseconds(250)) {
          return;
        }
        qtSyncActiveCameraObjectFromPose();
        qtLastFpsCameraObjectSync = now;
      };
      qtPromoteSceneObjectCamerasAndLights();
      qtEnsureRuntimeLightObject();
      qtSyncActiveCameraObjectFromPose();
      auto qtApplySceneLightingFallbackTo = [](vkpt::pathtracer::RTSceneData& scene) {
        bool hasLight = !scene.lights.empty();
        bool hasEmissive = false;
        for (const auto& mat : scene.materials) {
          if (mat.is_emissive()) {
            hasEmissive = true;
            break;
          }
        }
        if (!hasLight && !hasEmissive) {
          scene.environment_color = {0.35f, 0.4f, 0.5f};
        }
      };
      auto qtScriptRuntime = vkpt::scripting::CreateScriptRuntime();
      QtDockScriptRuntimeState qtScriptState;
      const auto qtScriptRuntimeStartTime = std::chrono::steady_clock::now();
