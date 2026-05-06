      auto qtLoadSceneFromAssetBrowser = [&](std::string scenePath,
                                             vkpt::core::FrameIndex frameIndex) {
        if (scenePath.empty()) {
          return false;
        }
        const std::filesystem::path requestedPath(scenePath);
        const std::string displayPath = requestedPath.lexically_normal().generic_string();
        if (qtWindow != nullptr && !qtWindow->confirm_scene_replacement(displayPath)) {
          ui_runtime_state.status_message = "scene load canceled";
          ui_runtime_state.focused_panel = "asset_browser";
          qtDockPanelsDirty = true;
          return false;
        }
        if (qtWindow != nullptr) {
          qtWindow->clear_framebuffer();
          qtWindow->set_overlay_text("Loading scene...\n" + displayPath + "\nRebuilding renderer and UI panels.");
          qtWindow->set_status_bar_text("Loading scene: " + displayPath);
          QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 16);
        }
        auto parseResult = vkpt::scene::SceneDocument::load_from_file(displayPath);
        if (!parseResult) {
          ui_runtime_state.status_message = "scene load failed: " + displayPath;
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtPreviewStatus = "scene load failed";
          if (qtWindow != nullptr) {
            qtWindow->set_overlay_text("Scene load failed\n" + displayPath);
          }
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "asset_browser_scene_load_failed",
                      "asset_browser",
                      "scene",
                      frameIndex,
                      {},
                      displayPath,
                      ui_runtime_state.status_message);
          return false;
        }

        qtFpsMode = false;
        qtSetViewportMouseLocked(false);
        qtDiscardFpsMovementResults();
        qtGizmoDrag = {};
        qtHoveredGizmoHit = std::nullopt;
        qtSceneDocument = std::move(parseResult.value());
        qtPromoteSceneObjectCamerasAndLights();
        ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
        qtAnimationBaseTransforms.clear();
        config.scene_path.value = displayPath;
        config.scene_path.source = vkpt::config::ConfigSource::CliFlag;
        vkpt::diagnostics::CrashRecorder::instance().update_scene(displayPath);

        const bool loaded = qtReloadEditedScene("scene loaded");
        qtEnsureRuntimeLightObject();
        if (qtSyncActiveCameraObjectFromPose) {
          qtSyncActiveCameraObjectFromPose();
        }
        qtRefreshAnimationBaseTransforms(true);
        syncQtCameraPoseFromScene();
        syncQtFpsAnglesFromPose();
        qtCameraFocusDistance = std::max(
            0.25f,
            qtScene.camera_focus_distance > 0.0f
                ? qtScene.camera_focus_distance
                : PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
        qtCameraFocusPoint =
            PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        qtPreviewStatus = loaded ? "scene loaded" : "scene renderer reload failed";
        ui_runtime_state.active_scene = displayPath;
        ui_runtime_state.status_message = loaded
            ? "asset browser scene load"
            : "asset browser scene load failed";
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();

        vkpt::editor::EditorCommand command;
        command.command_id = "asset_browser.open_scene";
        command.kind = vkpt::editor::EditorCommandKind::kImportAsset;
        command.source_widget = "asset_browser";
        command.frame_index = frameIndex;
        command.undoable = false;
        command.redoable = false;
        command.validated = loaded;
        command.payload = vkpt::editor::ImportAssetCommand{displayPath, "scene"};
        ui_command_history.push(command);
        PushUiEvent(ui_event_log,
                    loaded ? "asset_browser_scene_loaded" : "asset_browser_scene_load_failed",
                    "asset_browser",
                    "scene",
                    frameIndex,
                    {},
                    displayPath,
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        if (qtWindow != nullptr && loaded) {
          qtWindow->set_overlay_text(std::string_view{});
        }
        logger.log(loaded ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   loaded ? "Qt asset browser scene loaded"
                          : "Qt asset browser scene renderer reload failed",
                   {{"scene", displayPath}});
        return loaded;
      };
      auto qtAssetBrowserSpawnPoint =
          [&](const vkpt::platform::QtDockRowActivation& activation) {
        if (activation.viewport_drop) {
          const auto frameRect = qtViewportImageRect();
          if (const auto local = qtViewportLocalPoint(activation.x, activation.y)) {
            if (const auto point = ScreenPointOnCameraPlane(qtCameraPose,
                                                            local->first,
                                                            local->second,
                                                            frameRect.width,
                                                            frameRect.height,
                                                            qtRenderAspect(),
                                                            qtCameraFocusPoint)) {
              return *point;
            }
          }
        }
        return qtCameraFocusPoint;
      };
      auto qtSpawnModelFromAssetBrowser =
          [&](std::string modelPath,
              const vkpt::platform::QtDockRowActivation& activation,
              vkpt::core::FrameIndex frameIndex) {
        if (modelPath.empty()) {
          return false;
        }
        const std::filesystem::path requestedPath(modelPath);
        const std::string displayPath = requestedPath.lexically_normal().generic_string();
        auto ext = std::filesystem::path(displayPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
        if (ext == ".glb") {
          ui_runtime_state.status_message = "model import failed: GLB is not supported yet";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          ui_runtime_state.focused_panel = "asset_browser";
          ui_runtime_state.last_file_drop_path = displayPath;
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "asset_browser_model_spawn_failed",
                      "asset_browser",
                      activation.row_id,
                      frameIndex,
                      {},
                      displayPath,
                      ui_runtime_state.status_message);
          return false;
        }

        vkpt::scene::TransformComponent placement;
        placement.translation = ToSceneVec3(qtAssetBrowserSpawnPoint(activation));
        placement.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        placement.scale = {1.0f, 1.0f, 1.0f};
        placement.dirty = true;

        vkpt::assets::SceneAssetExpansionStats importStats{};
        std::vector<std::string> importDiagnostics;
        const bool imported = vkpt::assets::ImportSceneModelAsset(qtSceneDocument,
                                                                  config.scene_path.value,
                                                                  displayPath,
                                                                  placement,
                                                                  &importStats,
                                                                  &importDiagnostics);
        if (!imported) {
          ui_runtime_state.status_message = "model import failed: " + displayPath;
          ui_runtime_state.last_warning_or_error = importDiagnostics.empty()
              ? ui_runtime_state.status_message
              : importDiagnostics.back();
          ui_runtime_state.focused_panel = "asset_browser";
          ui_runtime_state.last_file_drop_path = displayPath;
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "asset_browser_model_spawn_failed",
                      "asset_browser",
                      activation.row_id,
                      frameIndex,
                      {},
                      displayPath,
                      ui_runtime_state.last_warning_or_error);
          return false;
        }

        auto importedSelectionEntity = importStats.imported_root_entity;
        if (importStats.imported_root_entity != 0u) {
          if (const auto sceneRootId = qtFindSceneRootEntityId(); sceneRootId != 0u) {
            const auto preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
            const auto* preWriteWorld = preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
            qtSetEntityParentPreserveWorld(importStats.imported_root_entity,
                                           sceneRootId,
                                           preWriteWorld,
                                           true);

            auto* importedRoot = qtFindEntity(importStats.imported_root_entity);
            const bool rootIsOnlyAGroup = importedRoot != nullptr &&
                                          !importedRoot->has_camera &&
                                          !importedRoot->has_light &&
                                          !importedRoot->has_mesh &&
                                          !importedRoot->has_physics_body &&
                                          !importedRoot->has_benchmark_tag &&
                                          !importedRoot->has_sdf_primitive &&
                                          importedRoot->script.script.empty() &&
                                          importedRoot->animation.clip.empty();
            std::vector<vkpt::core::StableId> rootChildren;
            for (const auto& entity : qtSceneDocument.entities) {
              if (qtEntityParentId(entity) == importStats.imported_root_entity) {
                rootChildren.push_back(entity.id);
              }
            }
            if (rootIsOnlyAGroup && rootChildren.size() == 1u) {
              const auto childId = rootChildren.front();
              const auto childWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
              const auto* childWorld = childWorldSnapshot ? &childWorldSnapshot.value() : nullptr;
              if (qtSetEntityParentPreserveWorld(childId, sceneRootId, childWorld, true)) {
                qtSceneDocument.entities.erase(
                    std::remove_if(qtSceneDocument.entities.begin(),
                                   qtSceneDocument.entities.end(),
                                   [&](const vkpt::scene::SceneEntityDefinition& entity) {
                                     return entity.id == importStats.imported_root_entity;
                                   }),
                    qtSceneDocument.entities.end());
                qtRemoveLegacyTransformEntry(importStats.imported_root_entity);
                importedSelectionEntity = childId;
              }
            }
          }
        }

        ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
        if (importedSelectionEntity != 0u) {
          vkpt::editor::EditorCommand selectCommand;
          selectCommand.command_id = "asset_browser.select_imported_model";
          selectCommand.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
          selectCommand.source_widget = "asset_browser";
          selectCommand.frame_index = frameIndex;
          selectCommand.payload = vkpt::editor::SelectEntityCommand{
              importedSelectionEntity,
              false,
              false};
          ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, selectCommand);
          ui_command_history.push(selectCommand);
        }
        qtAnimationBaseTransforms.clear();
        const bool loaded = qtReloadEditedScene("asset browser model spawn");
        qtRefreshAnimationBaseTransforms(true);
        qtPreviewStatus = loaded ? "model spawned" : "model spawned; renderer reload failed";
        ui_runtime_state.focused_panel = "asset_browser";
        ui_runtime_state.last_file_drop_path = displayPath;
        ui_runtime_state.status_message = loaded
            ? "spawned model " + displayPath
            : "model spawned but renderer reload failed";
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();

        vkpt::editor::EditorCommand command;
        command.command_id = "asset_browser.spawn_model";
        command.kind = vkpt::editor::EditorCommandKind::kImportAsset;
        command.source_widget = "asset_browser";
        command.frame_index = frameIndex;
        command.undoable = false;
        command.redoable = false;
        command.validated = loaded;
        command.payload = vkpt::editor::ImportAssetCommand{displayPath, "model"};
        ui_command_history.push(command);
        PushUiEvent(ui_event_log,
                    loaded ? "asset_browser_model_spawned" : "asset_browser_model_spawn_failed",
                    "asset_browser",
                    activation.viewport_drop ? "viewport_drop" : "click",
                    frameIndex,
                    {},
                    displayPath,
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        logger.log(loaded ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   loaded ? "Qt asset browser model spawned"
                          : "Qt asset browser model renderer reload failed",
                   {{"model", displayPath},
                    {"root_entity", std::to_string(importedSelectionEntity)},
                    {"geometry", std::to_string(importStats.imported_geometry)}});
        return loaded;
      };
      auto qtSpawnModelGroupFromAssetBrowser =
          [&](std::vector<std::string> modelPaths,
              const vkpt::platform::QtDockRowActivation& activation,
              vkpt::core::FrameIndex frameIndex) {
        std::vector<std::string> displayPaths;
        for (const auto& modelPath : modelPaths) {
          if (modelPath.empty()) {
            continue;
          }
          const auto displayPath = std::filesystem::path(modelPath).lexically_normal().generic_string();
          if (std::find(displayPaths.begin(), displayPaths.end(), displayPath) == displayPaths.end()) {
            displayPaths.push_back(displayPath);
          }
        }
        if (displayPaths.empty()) {
          return false;
        }
        if (displayPaths.size() == 1u) {
          return qtSpawnModelFromAssetBrowser(displayPaths.front(), activation, frameIndex);
        }

        vkpt::scene::TransformComponent groupTransform;
        groupTransform.translation = ToSceneVec3(qtAssetBrowserSpawnPoint(activation));
        groupTransform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        groupTransform.scale = {1.0f, 1.0f, 1.0f};
        groupTransform.dirty = true;

        const auto sceneRootId = qtFindSceneRootEntityId();
        vkpt::scene::SceneEntityDefinition groupEntity;
        groupEntity.id = qtNextSceneObjectId();
        groupEntity.name = "Imported " + std::to_string(displayPaths.size()) + " models";
        groupEntity.has_transform = true;
        groupEntity.transform = groupTransform;
        groupEntity.has_hierarchy = true;
        groupEntity.hierarchy.parent = sceneRootId;
        groupEntity.hierarchy.sibling_order = qtNextSiblingOrder(sceneRootId);
        const auto groupEntityId = groupEntity.id;
        qtSceneDocument.entities.push_back(std::move(groupEntity));

        std::vector<vkpt::core::StableId> importedRoots;
        vkpt::assets::SceneAssetExpansionStats totalStats{};
        const float center = (static_cast<float>(displayPaths.size()) - 1.0f) * 0.5f;
        for (std::size_t index = 0; index < displayPaths.size(); ++index) {
          vkpt::scene::TransformComponent placement = groupTransform;
          placement.translation.x += (static_cast<float>(index) - center) * 1.5f;
          vkpt::assets::SceneAssetExpansionStats importStats{};
          std::vector<std::string> diagnostics;
          if (!vkpt::assets::ImportSceneModelAsset(qtSceneDocument,
                                                   config.scene_path.value,
                                                   displayPaths[index],
                                                   placement,
                                                   &importStats,
                                                   &diagnostics) ||
              importStats.imported_root_entity == 0u) {
            continue;
          }
          totalStats.imported_geometry += importStats.imported_geometry;
          totalStats.imported_entities += importStats.imported_entities;
          importedRoots.push_back(importStats.imported_root_entity);
          const auto preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
          const auto* preWriteWorld = preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
          qtSetEntityParentPreserveWorld(importStats.imported_root_entity,
                                         groupEntityId,
                                         preWriteWorld,
                                         true);
        }
        if (importedRoots.empty()) {
          qtSceneDocument.entities.erase(
              std::remove_if(qtSceneDocument.entities.begin(),
                             qtSceneDocument.entities.end(),
                             [groupEntityId](const vkpt::scene::SceneEntityDefinition& entity) {
                               return entity.id == groupEntityId;
                             }),
              qtSceneDocument.entities.end());
          ui_runtime_state.status_message = "model import failed: selected models";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          ui_runtime_state.focused_panel = "asset_browser";
          qtDockPanelsDirty = true;
          return false;
        }

        ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
        vkpt::editor::EditorCommand selectCommand;
        selectCommand.command_id = "asset_browser.select_imported_group";
        selectCommand.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
        selectCommand.source_widget = "asset_browser";
        selectCommand.frame_index = frameIndex;
        selectCommand.payload = vkpt::editor::SelectEntityCommand{groupEntityId, false, false};
        ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, selectCommand);
        ui_command_history.push(selectCommand);

        qtAnimationBaseTransforms.clear();
        const bool loaded = qtReloadEditedScene("asset browser model spawn");
        qtRefreshAnimationBaseTransforms(true);
        ui_runtime_state.focused_panel = "asset_browser";
        ui_runtime_state.last_file_drop_path = displayPaths.front();
        ui_runtime_state.status_message = loaded
            ? "spawned " + std::to_string(importedRoots.size()) + " models"
            : "models spawned but renderer reload failed";
        qtPreviewStatus = loaded ? "model group spawned" : "model group spawned; renderer reload failed";
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();

        std::ostringstream joinedPaths;
        for (std::size_t index = 0; index < displayPaths.size(); ++index) {
          if (index > 0u) {
            joinedPaths << ";";
          }
          joinedPaths << displayPaths[index];
        }
        vkpt::editor::EditorCommand command;
        command.command_id = "asset_browser.spawn_model_group";
        command.kind = vkpt::editor::EditorCommandKind::kImportAsset;
        command.source_widget = "asset_browser";
        command.frame_index = frameIndex;
        command.undoable = false;
        command.redoable = false;
        command.validated = loaded;
        command.payload = vkpt::editor::ImportAssetCommand{joinedPaths.str(), "model_group"};
        ui_command_history.push(command);
        PushUiEvent(ui_event_log,
                    loaded ? "asset_browser_model_group_spawned" : "asset_browser_model_group_spawn_failed",
                    "asset_browser",
                    activation.viewport_drop ? "viewport_drop" : "click",
                    frameIndex,
                    std::to_string(displayPaths.size()),
                    std::to_string(groupEntityId),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        logger.log(loaded ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   loaded ? "Qt asset browser model group spawned"
                          : "Qt asset browser model group renderer reload failed",
                   {{"models", std::to_string(importedRoots.size())},
                    {"group_entity", std::to_string(groupEntityId)},
                    {"geometry", std::to_string(totalStats.imported_geometry)}});
        return loaded;
      };
      auto qtReloadRenderSettings = [&](std::string_view reason) {
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtPublishedWidth.store(qtSettings.width, std::memory_order_relaxed);
        qtPublishedHeight.store(qtSettings.height, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_settings(qtSettings);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->configure(qtSettings) &&
                          qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        qtPreviewStatus = qtTracerReady ? std::string(reason) : "render settings reload failed";
        return qtTracerReady;
      };
      auto qtReloadCameraSettings =
          [&](vkpt::core::StableId entityId,
              std::string_view reason,
              bool refreshPoseFromTransform = false) {
        qtPromoteSceneObjectCamerasAndLights();
        const auto* activeCamera = qtFindActiveCameraEntity();
        if (activeCamera == nullptr || activeCamera->id != entityId || !activeCamera->has_camera) {
          return false;
        }

        const auto& camera = activeCamera->camera;
        qtScene.camera_fov_deg = camera.fov;
        qtScene.camera_focal_length_mm = camera.focal_length_mm;
        qtScene.camera_sensor_width_mm = camera.sensor_width_mm;
        qtScene.camera_sensor_height_mm = camera.sensor_height_mm;
        qtScene.camera_aperture_radius = camera.aperture_radius;
        qtScene.camera_focus_distance = camera.focus_distance;
        qtScene.camera_f_stop = camera.f_stop;
        qtScene.camera_shutter_seconds = camera.shutter_seconds;
        qtScene.camera_iso = camera.iso;
        qtScene.camera_exposure_compensation = camera.exposure_compensation;
        qtScene.camera_white_balance_kelvin = camera.white_balance_kelvin;
        qtScene.camera_iris_blade_count = camera.iris_blade_count;
        qtScene.camera_iris_rotation_degrees = camera.iris_rotation_degrees;
        qtScene.camera_iris_roundness = camera.iris_roundness;
        qtScene.camera_anamorphic_squeeze = camera.anamorphic_squeeze;
        if (refreshPoseFromTransform && activeCamera->has_transform) {
          const auto worldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
          const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
          const auto transform = ResolveEntityWorldTransform(*activeCamera, world);
          qtScene.camera_position = ToPtVec3(transform.translation);
          const auto forward = PtNormalize(
              RotatePointByQuat({0.0f, 0.0f, -1.0f}, transform.rotation),
              {0.0f, 0.0f, -1.0f});
          qtScene.camera_target = PtAdd(qtScene.camera_position, forward);
          qtScene.camera_up = PtNormalize(
              RotatePointByQuat({0.0f, 1.0f, 0.0f}, transform.rotation),
              {0.0f, 1.0f, 0.0f});
        }

        syncQtCameraPoseFromScene();
        syncQtFpsAnglesFromPose();
        qtCameraFocusDistance = std::max(
            0.25f,
            qtScene.camera_focus_distance > 0.0f
                ? qtScene.camera_focus_distance
                : PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
        qtCameraFocusPoint =
            PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);

        const auto cameraState = vkpt::pathtracer::ExtractCameraState(qtScene);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_camera_state(cameraState);
        } else if (!qtUseBg && qtTracerReady) {
          bool cameraOk = qtTracer->update_camera_state(cameraState);
          if (!cameraOk) {
            cameraOk = qtTracer->load_scene_snapshot(qtScene) &&
                       qtTracer->build_or_update_acceleration();
          }
          qtTracerReady = cameraOk && qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }

        qtDockPanelsDirty = true;
        qtPreviewStatus = qtTracerReady ? std::string(reason) : "camera settings reload failed";
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      auto qtSetAuthoredCameraFocusDistance = [&](float distance, std::string_view reason) {
        const float focusDistance = ClampFloat(distance, 0.0f, 100000.0f);
        bool wroteAuthoredCamera = false;
        vkpt::core::StableId authoredCameraEntityId = 0u;
        if (auto* entity = qtEnsureActiveCameraEntity()) {
          entity->camera.focus_distance = focusDistance;
          wroteAuthoredCamera = true;
          authoredCameraEntityId = entity->id;
        }
        qtCameraFocusDistance = std::max(0.25f, focusDistance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        if (wroteAuthoredCamera) {
          return qtReloadCameraSettings(authoredCameraEntityId, reason);
        }

        qtScene.camera_focus_distance = focusDistance;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_scene(qtScene);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      auto qtSaveCameraShot = [&](int slot) {
        const int clampedSlot = std::clamp(slot, 0, 3);
        qtSavedCameraShots[static_cast<std::size_t>(clampedSlot)] =
            QtSavedCameraShot{true, qtCameraPose, qtCameraFocusDistance};
        qtActiveCameraShotSlot = clampedSlot;
        qtDockPanelsDirty = true;
        ui_runtime_state.status_message = "camera shot saved";
      };
      auto qtRecallCameraShot = [&](int slot) {
        const int clampedSlot = std::clamp(slot, 0, 3);
        const auto& shot = qtSavedCameraShots[static_cast<std::size_t>(clampedSlot)];
        if (!shot.valid) {
          ui_runtime_state.status_message = "camera shot empty";
          qtDockPanelsDirty = true;
          return false;
        }
        qtActiveCameraShotSlot = clampedSlot;
        qtCameraPose = shot.pose;
        qtCameraFocusDistance = std::max(0.25f, shot.focus_distance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("shot recall");
        qtSetAuthoredCameraFocusDistance(qtCameraFocusDistance, "shot recall");
        qtDockPanelsDirty = true;
        return true;
      };
      std::function<bool(vkpt::core::FrameIndex)> qtDockAutoFocus;
      std::function<bool(vkpt::core::FrameIndex)> qtDockFocusSelected;
      std::function<void(bool, vkpt::core::FrameIndex, std::string_view)> qtSetFpsMode;
      auto qtSetLuaPlayableMode = [&](bool enabled,
                                      vkpt::core::FrameIndex frameIndex,
                                      std::string_view source) {
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "Lua playable mode toggle requested",
                   {{"requested", enabled ? "on" : "off"},
                    {"source", std::string(source)},
                    {"playing_before", qtScriptState.playing ? "true" : "false"},
                    {"scripts_enabled_before", qtScriptState.scripts_enabled ? "true" : "false"},
                    {"fps_mode_before", qtFpsMode ? "true" : "false"},
                    {"lua_compiled_in", qtScriptRuntime ? (qtScriptRuntime->lua_compiled_in() ? "true" : "false") : "missing_runtime"},
                    {"execution_available", qtScriptRuntime ? (qtScriptRuntime->execution_available() ? "true" : "false") : "missing_runtime"},
                    {"bindings", std::to_string(qtScriptState.binding_summary.binding_count)},
                    {"runnable", std::to_string(qtScriptState.binding_summary.runnable_count)}},
                   frameIndex);
        if (enabled && qtScriptRuntime && !qtScriptRuntime->execution_available()) {
          qtScriptState.scripts_enabled = true;
          qtScriptState.playing = false;
          qtScriptState.status = "lua playable mode unavailable: Lua execution is not compiled in";
          ui_runtime_state.status_message = qtScriptState.status;
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          logger.log(vkpt::log::Severity::Error,
                     "scripts",
                     "Lua playable mode unavailable in this executable",
                     {{"requested", "on"},
                      {"lua_compiled_in", qtScriptRuntime->lua_compiled_in() ? "true" : "false"},
                      {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"},
                      {"active_scene", ui_runtime_state.active_scene},
                      {"status", qtScriptState.status}},
                     frameIndex);
          PushUiEvent(ui_event_log,
                      "lua_playable_mode_toggle",
                      "viewport",
                      source,
                      frameIndex,
                      {},
                      "unavailable",
                      qtScriptState.status);
          return;
        }
        if (enabled && qtFpsMode) {
          if (qtSetFpsMode) {
            qtSetFpsMode(false, frameIndex, source);
          } else {
            qtFpsMode = false;
          }
        }
        qtScriptState.playing = enabled;
        if (enabled) {
          qtScriptState.scripts_enabled = true;
        }
        qtScriptState.status = enabled ? "lua playable mode running" : "lua playable mode paused";
        ui_runtime_state.status_message = qtScriptState.status;
        qtDockPanelsDirty = true;
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "Lua playable mode toggle applied",
                   {{"playing", qtScriptState.playing ? "true" : "false"},
                    {"scripts_enabled", qtScriptState.scripts_enabled ? "true" : "false"},
                    {"fps_mode", qtFpsMode ? "true" : "false"},
                    {"status", qtScriptState.status}},
                   frameIndex);
        PushUiEvent(ui_event_log,
                    "lua_playable_mode_toggle",
                    "viewport",
                    source,
                    frameIndex,
                    {},
                    enabled ? "on" : "off",
                    qtScriptState.status);
      };
