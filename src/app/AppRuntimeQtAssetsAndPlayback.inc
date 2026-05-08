      struct QtSceneLoadJobResult {
        std::string display_path;
        std::string error;
        vkpt::scene::SceneDocument document;
        vkpt::pathtracer::PathTracerSceneSnapshot scene;
        bool ok = false;
        double parse_ms = 0.0;
        double convert_ms = 0.0;
        double total_ms = 0.0;
      };
      struct QtSceneLoadJob {
        std::mutex mutex;
        std::string phase;
        std::optional<QtSceneLoadJobResult> result;
        std::atomic_bool active{false};
        std::jthread worker;
      };
      std::unique_ptr<QtSceneLoadJob> qtSceneLoadJob;
      std::uint64_t qtSceneLoadWaitingForPaintGeneration = 0u;
      auto qtSceneLoadErrorCodeName = [](vkpt::core::ErrorCode code) {
        switch (code) {
          case vkpt::core::ErrorCode::Ok: return "ok";
          case vkpt::core::ErrorCode::InvalidArgument: return "invalid_argument";
          case vkpt::core::ErrorCode::NotFound: return "not_found";
          case vkpt::core::ErrorCode::IOError: return "io_error";
          case vkpt::core::ErrorCode::Unsupported: return "unsupported";
          case vkpt::core::ErrorCode::Timeout: return "timeout";
          case vkpt::core::ErrorCode::Internal: return "internal";
          case vkpt::core::ErrorCode::Cancelled: return "cancelled";
          default: return "unknown";
        }
      };
      auto qtPumpSceneLoadDialog = [&](std::string_view phase) {
        if (qtWindow == nullptr) {
          return;
        }
        qtWindow->set_runtime_loading_text(phase);
        qtWindow->set_status_bar_text(phase);
      };
      auto qtApplyLoadedSceneResult =
          [&](QtSceneLoadJobResult result, vkpt::core::FrameIndex frameIndex) {
        if (!result.ok) {
          ui_runtime_state.status_message = "scene load failed: " + result.display_path;
          ui_runtime_state.last_warning_or_error =
              result.error.empty() ? ui_runtime_state.status_message : result.error;
          ui_runtime_state.focused_panel = "asset_browser";
          qtPreviewStatus = "scene load failed";
          qtDockPanelsDirty = true;
          if (qtWindow != nullptr) {
            qtWindow->set_overlay_text("Scene load failed\n" + result.display_path +
                                       "\n" + ui_runtime_state.last_warning_or_error);
            qtWindow->set_runtime_loading_text("Scene load failed");
            qtWindow->finish_runtime_loading_dialog();
          }
          PushUiEvent(ui_event_log,
                      "asset_browser_scene_load_failed",
                      "asset_browser",
                      "scene",
                      frameIndex,
                      {},
                      result.display_path,
                      ui_runtime_state.status_message);
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
          logger.log(vkpt::log::Severity::Error,
                     "app",
                     "Qt asset browser scene load failed",
                     {{"scene", result.display_path},
                      {"error", ui_runtime_state.last_warning_or_error}});
          return false;
        }

        qtPumpSceneLoadDialog("Installing scene document");
        qtFpsMode = false;
        qtSetViewportMouseLocked(false);
        qtSyncViewportAssetDropMode();
        qtDiscardFpsMovementResults();
        qtGizmoDrag = {};
        qtHoveredGizmoHit = std::nullopt;
        qtSceneDocument = std::move(result.document);
        qtScene = std::move(result.scene);
        qtRebuildRtInstanceIndexCache();
        ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
        config.scene_path.value = result.display_path;
        config.scene_path.source = vkpt::config::ConfigSource::CliFlag;
        vkpt::diagnostics::CrashRecorder::instance().update_scene(result.display_path);

        qtPumpSceneLoadDialog("Refreshing camera and viewport data");
        qtEnsureRuntimeLightObject();
        if (qtSyncActiveCameraObjectFromPose) {
          qtSyncActiveCameraObjectFromPose();
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
        if (!qtUseBg) {
          (void)qtPublishRenderSnapshot(true, true, true, true);
        }
        qtSetViewportPickablesFromSnapshot(qtCurrentUiSnapshot());
        qtViewportPickablesRefreshPending = false;
        qtLastViewportPickablesRefresh = std::chrono::steady_clock::now();
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);

        qtPumpSceneLoadDialog(qtUseBg ? "Posting renderer scene rebuild"
                                      : "Rebuilding renderer acceleration");
        bool loaded = false;
        bool waitingForBackgroundPaint = false;
        if (qtUseBg && qtRenderCoordinator) {
          qtOverlayRequiredRenderGeneration =
              qtPublishRenderSnapshotForBackground(true, true, true, true);
          loaded = qtTracerReady;
          waitingForBackgroundPaint = loaded && qtOverlayRequiredRenderGeneration != 0u;
          qtSceneLoadWaitingForPaintGeneration =
              waitingForBackgroundPaint ? qtOverlayRequiredRenderGeneration : 0u;
        } else if (!qtUseBg && qtTracer) {
          const bool configured = qtTracerReady || qtTracer->configure(qtSettings);
          qtTracerReady = configured &&
                          qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
          loaded = qtTracerReady;
          qtSceneLoadWaitingForPaintGeneration = 0u;
        }
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);

        qtPumpSceneLoadDialog("Refreshing audio, scripts, physics, and panels");
        if (qtAudioSystem) {
          qtAudioSystem->load_scene_audio(qtSceneDocument, result.display_path);
        }
        qtSyncPhysicsFromSceneDocument();
        qtReloadScriptsFromDocument("scene loaded");
        qtPreviewStatus = loaded ? "scene loaded" : "scene renderer reload failed";
        ui_runtime_state.active_scene = result.display_path;
        ui_runtime_state.status_message = loaded
            ? (waitingForBackgroundPaint ? "asset browser scene load; renderer rebuilding"
                                         : "asset browser scene load")
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
        command.payload = vkpt::editor::ImportAssetCommand{result.display_path, "scene"};
        ui_command_history.push(command);
        PushUiEvent(ui_event_log,
                    loaded ? "asset_browser_scene_loaded" : "asset_browser_scene_load_failed",
                    "asset_browser",
                    "scene",
                    frameIndex,
                    {},
                    result.display_path,
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        if (qtWindow != nullptr && loaded && !waitingForBackgroundPaint) {
          qtWindow->set_overlay_text(std::string_view{});
          qtWindow->finish_runtime_loading_dialog();
        } else if (qtWindow != nullptr && waitingForBackgroundPaint) {
          qtWindow->set_runtime_loading_text("Renderer building acceleration and first preview");
        }
        logger.log(loaded ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   loaded ? "Qt asset browser scene loaded"
                          : "Qt asset browser scene renderer reload failed",
                   {{"scene", result.display_path},
                    {"parse_ms", std::to_string(result.parse_ms)},
                    {"convert_ms", std::to_string(result.convert_ms)},
                    {"total_ms", std::to_string(result.total_ms)},
                    {"background_renderer", qtUseBg ? "true" : "false"}});
        return loaded;
      };
      auto qtPollSceneLoadJob = [&](vkpt::core::FrameIndex frameIndex) {
        if (qtSceneLoadWaitingForPaintGeneration != 0u) {
          if (qtUseBg && qtRenderCoordinator && qtRenderCoordinator->stats().failed) {
            qtSceneLoadWaitingForPaintGeneration = 0u;
            ui_runtime_state.status_message = "scene renderer reload failed";
            ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
            qtPreviewStatus = ui_runtime_state.status_message;
            qtDockPanelsDirty = true;
            if (qtWindow != nullptr) {
              qtWindow->set_runtime_loading_text("Scene renderer reload failed");
              qtWindow->finish_runtime_loading_dialog();
            }
          } else if (qtLatestPaintedRenderGeneration() >= qtSceneLoadWaitingForPaintGeneration) {
            qtSceneLoadWaitingForPaintGeneration = 0u;
            ui_runtime_state.status_message = "asset browser scene load";
            qtPreviewStatus = "scene loaded";
            qtDockPanelsDirty = true;
            if (qtWindow != nullptr) {
              qtWindow->set_overlay_text(std::string_view{});
              qtWindow->set_runtime_loading_text("Scene loaded");
              qtWindow->finish_runtime_loading_dialog();
            }
          } else if (qtWindow != nullptr) {
            qtWindow->set_runtime_loading_text("Renderer building acceleration and first preview");
          }
        }

        if (!qtSceneLoadJob) {
          return false;
        }
        {
          std::scoped_lock lock(qtSceneLoadJob->mutex);
          if (!qtSceneLoadJob->phase.empty() && qtWindow != nullptr) {
            qtWindow->set_runtime_loading_text(qtSceneLoadJob->phase);
          }
        }
        if (qtSceneLoadJob->active.load(std::memory_order_acquire)) {
          return true;
        }

        std::optional<QtSceneLoadJobResult> result;
        {
          std::scoped_lock lock(qtSceneLoadJob->mutex);
          result = std::move(qtSceneLoadJob->result);
        }
        qtSceneLoadJob.reset();
        if (!result) {
          return false;
        }
        qtApplyLoadedSceneResult(std::move(*result), frameIndex);
        return true;
      };
      auto qtLoadSceneFromAssetBrowser = [&](std::string scenePath,
                                             vkpt::core::FrameIndex frameIndex) {
        if (scenePath.empty()) {
          return false;
        }
        if (qtSceneLoadJob || qtSceneLoadWaitingForPaintGeneration != 0u) {
          ui_runtime_state.status_message = "scene load already in progress";
          ui_runtime_state.focused_panel = "asset_browser";
          qtDockPanelsDirty = true;
          if (qtWindow != nullptr) {
            qtWindow->set_runtime_loading_text(ui_runtime_state.status_message);
          }
          return true;
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
          qtWindow->set_overlay_text("Loading scene...\n" + displayPath +
                                     "\nParsing and converting scene data in the background.");
          qtWindow->set_status_bar_text("Loading scene: " + displayPath);
          qtWindow->show_runtime_loading_dialog("Loading Scene", "Queueing scene load\n" + displayPath);
        }
        ui_runtime_state.status_message = "scene load queued: " + displayPath;
        ui_runtime_state.focused_panel = "asset_browser";
        qtPreviewStatus = "loading scene";
        qtDockPanelsDirty = true;

        qtSceneLoadJob = std::make_unique<QtSceneLoadJob>();
        auto* job = qtSceneLoadJob.get();
        job->active.store(true, std::memory_order_release);
        {
          std::scoped_lock lock(job->mutex);
          job->phase = "Reading scene document\n" + displayPath;
        }
        job->worker = std::jthread([job, displayPath, qtSceneLoadErrorCodeName](std::stop_token stop) {
          QtSceneLoadJobResult out;
          out.display_path = displayPath;
          const auto loadStart = std::chrono::steady_clock::now();
          auto publishPhase = [&](std::string phase) {
            std::scoped_lock lock(job->mutex);
            job->phase = std::move(phase);
          };
          auto elapsedMs = [](auto begin, auto end) {
            return std::chrono::duration<double, std::milli>(end - begin).count();
          };
          try {
            publishPhase("Reading scene document\n" + displayPath);
            auto parseResult = vkpt::scene::SceneDocument::load_from_file(displayPath);
            const auto parseEnd = std::chrono::steady_clock::now();
            out.parse_ms = elapsedMs(loadStart, parseEnd);
            if (!parseResult) {
              out.error = std::string("scene document load failed: ") +
                          qtSceneLoadErrorCodeName(parseResult.error());
            } else if (stop.stop_requested()) {
              out.error = "scene load canceled";
            } else {
              publishPhase("Validating scene components");
              auto document = std::move(parseResult.value());
              PromoteQtSceneObjectCamerasAndLights(document);
              EnsureQtFallbackLightingEntities(document);
              publishPhase("Building render scene");
              auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
              const auto convertEnd = std::chrono::steady_clock::now();
              out.convert_ms = elapsedMs(parseEnd, convertEnd);
              if (!sceneResult) {
                out.error = std::string("render scene conversion failed: ") +
                            qtSceneLoadErrorCodeName(sceneResult.error());
              } else if (stop.stop_requested()) {
                out.error = "scene load canceled";
              } else {
                publishPhase("Preparing renderer handoff");
                out.document = std::move(document);
                out.scene = std::move(sceneResult.value());
                out.ok = true;
              }
            }
          } catch (const std::exception& ex) {
            out.error = std::string("scene load exception: ") + ex.what();
          } catch (...) {
            out.error = "scene load exception";
          }
          out.total_ms = elapsedMs(loadStart, std::chrono::steady_clock::now());
          {
            std::scoped_lock lock(job->mutex);
            job->phase = out.ok ? std::string("Scene data ready")
                                : std::string("Scene load failed");
            job->result = std::move(out);
          }
          job->active.store(false, std::memory_order_release);
        });
        PushUiEvent(ui_event_log,
                    "asset_browser_scene_load_queued",
                    "asset_browser",
                    "scene",
                    frameIndex,
                    {},
                    displayPath,
                    ui_runtime_state.status_message);
        return true;
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
                                          importedRoot->script.script.empty();
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
        const bool loaded = qtReloadEditedScene("asset browser model spawn");
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

        const bool loaded = qtReloadEditedScene("asset browser model spawn");
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
          qtMarkOverlayRequiresRenderCatchup();
          qtRenderCoordinator->set_render_settings(qtSettings);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->update_render_settings(qtSettings) &&
                          qtTracer->reset_accumulation();
          if (!qtTracerReady) {
            qtTracerReady = qtTracer->configure(qtSettings) &&
                            qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation();
          }
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        qtPreviewStatus = qtTracerReady ? std::string(reason) : "render settings reload failed";
        return qtTracerReady;
      };
      std::string qtLastRenderMenuStatus;
      auto qtStartRenderToFile = [&](uint32_t frame) {
        qtLastRenderMenuStatus.clear();
        if (qtWindow == nullptr) {
          qtLastRenderMenuStatus = "Qt render dialog unavailable";
          return false;
        }
        if (qtUseBg || !qtTracer) {
          qtLastRenderMenuStatus = "start render requires the synchronous GPU preview renderer";
          qtPreviewStatus = qtLastRenderMenuStatus;
          return false;
        }
        if (!qtTracerReady) {
          qtLastRenderMenuStatus = "renderer is not ready";
          qtPreviewStatus = qtLastRenderMenuStatus;
          return false;
        }

        vkpt::platform::QtRenderDialogSettings dialogSettings{};
        dialogSettings.width = qtSettings.width;
        dialogSettings.height = qtSettings.height;
        dialogSettings.max_rays = std::max<std::uint64_t>(
            1ull,
            static_cast<std::uint64_t>(qtSettings.width) *
                static_cast<std::uint64_t>(qtSettings.height) * 128ull);
        dialogSettings.output_path = "artifacts/renders/render.png";
        if (!qtWindow->request_render_settings(dialogSettings)) {
          qtLastRenderMenuStatus = "start render canceled";
          return false;
        }

        const std::uint64_t pixelCount =
            std::max<std::uint64_t>(1ull,
                                    static_cast<std::uint64_t>(dialogSettings.width) *
                                        static_cast<std::uint64_t>(dialogSettings.height));
        const std::uint64_t requestedSamples64 =
            std::max<std::uint64_t>(1ull, (dialogSettings.max_rays + pixelCount - 1ull) / pixelCount);
        const auto renderSamples = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(requestedSamples64, std::numeric_limits<std::uint32_t>::max()));

        auto outputPath = std::filesystem::path(dialogSettings.output_path);
        const std::string format = dialogSettings.format == "exr" ? "exr" : "png";
        const std::string expectedExtension = "." + format;
        if (outputPath.extension().string() != expectedExtension) {
          outputPath.replace_extension(format);
        }
        const auto parent = outputPath.parent_path();
        if (!parent.empty()) {
          std::filesystem::create_directories(parent);
        }

        auto renderSettings = qtSettings;
        const auto previousSettings = qtSettings;
        renderSettings.width = dialogSettings.width;
        renderSettings.height = dialogSettings.height;
        renderSettings.spp = renderSamples;
        if (dialogSettings.budget == "Quick") {
          renderSettings.max_depth = std::min<std::uint32_t>(renderSettings.max_depth, 3u);
          renderSettings.enable_denoiser = false;
          renderSettings.enable_temporal_aa = false;
        } else if (dialogSettings.budget == "Quality") {
          renderSettings.max_depth = std::max<std::uint32_t>(renderSettings.max_depth, 8u);
          renderSettings.enable_denoiser = true;
          renderSettings.enable_temporal_aa = true;
        } else {
          renderSettings.max_depth = std::max<std::uint32_t>(renderSettings.max_depth, 6u);
          renderSettings.enable_denoiser = true;
        }

        qtPreviewStatus = "rendering still image";
        qtWindow->set_status_bar_text("Rendering still image...");
        qtTracerReady = qtTracer->configure(renderSettings) &&
                        qtTracer->load_scene_snapshot(qtScene) &&
                        qtTracer->build_or_update_acceleration() &&
                        qtTracer->reset_accumulation();
        if (!qtTracerReady) {
          qtSettings = previousSettings;
          qtReloadRenderSettings("render restore after failed still setup");
          qtLastRenderMenuStatus = "still render setup failed";
          return false;
        }

        bool rendered = true;
        for (std::uint32_t sample = 0u; sample < renderSamples; ++sample) {
          vkpt::pathtracer::RenderTile tile;
          tile.x = 0u;
          tile.y = 0u;
          tile.width = renderSettings.width;
          tile.height = renderSettings.height;
          tile.sample_index = sample;
          if (!qtTracer->render_tile(tile, frame)) {
            rendered = false;
            qtLastRenderMenuStatus = "still render sample failed";
            break;
          }

          const bool publishFrame = sample < 4u || ((sample + 1u) % 4u) == 0u || (sample + 1u) == renderSamples;
          if (publishFrame) {
            auto ldr = qtTracer->resolve_ldr();
            qtPublishedSample.store(sample + 1u, std::memory_order_relaxed);
            qtPublishedWidth.store(ldr.width, std::memory_order_relaxed);
            qtPublishedHeight.store(ldr.height, std::memory_order_relaxed);
            qtPublishedRays.store(qtTracer->read_counters().rays, std::memory_order_relaxed);
            qtWindow->set_framebuffer_rgba(std::move(ldr.rgba8), ldr.width, ldr.height);
            qtPublishedFrames.fetch_add(1u, std::memory_order_relaxed);
            qtWindow->set_overlay_text("Still render\nAccumulated: " +
                                        std::to_string(sample + 1u) + "/" +
                                        std::to_string(renderSamples) + " spp\nBudget: " +
                                        dialogSettings.budget);
            qtWindow->set_status_bar_text("Rendering still image " +
                                          std::to_string(sample + 1u) + "/" +
                                          std::to_string(renderSamples) + " spp");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            if (!window->is_open()) {
              rendered = false;
              qtLastRenderMenuStatus = "window closed during still render";
              break;
            }
          }
        }

        if (rendered) {
          std::string saveError;
          const auto ldr = qtTracer->resolve_ldr();
          const auto hdr = qtTracer->resolve_hdr();
          if (format == "exr") {
            rendered = vkpt::pathtracer::SaveExrCompat(outputPath.string(), hdr, &saveError);
          } else {
            rendered = vkpt::pathtracer::SavePngCompat(outputPath.string(), ldr, &saveError);
          }
          if (!rendered) {
            qtLastRenderMenuStatus = "still render save failed: " + saveError;
          } else {
            qtLastRenderMenuStatus = "still render saved: " + outputPath.string();
          }
        }

        qtSettings = previousSettings;
        qtReloadRenderSettings(rendered ? "still render complete" : "still render failed");
        qtDockPanelsDirty = true;
        qtPreviewStatus = qtLastRenderMenuStatus;
        qtWindow->set_status_bar_text(qtLastRenderMenuStatus);
        return rendered;
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
          qtPublishRenderSnapshotForBackground(false, false, true, false);
        } else if (!qtUseBg && qtTracerReady) {
          bool cameraOk = qtTracer->update_camera_state(cameraState);
          if (!cameraOk) {
            cameraOk = qtTracer->load_scene_snapshot(qtScene) &&
                       qtTracer->build_or_update_acceleration();
          }
          qtTracerReady = cameraOk && qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
          if (qtTracerReady) {
            (void)qtPublishRenderSnapshot(false, false, true, false);
          }
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
          qtPublishRenderSnapshotForBackground(false, false, true, false);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
          if (qtTracerReady) {
            (void)qtPublishRenderSnapshot(false, false, true, false);
          }
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
      auto qtSetRuntimeMode = [&](RuntimeMode mode,
                                  vkpt::core::FrameIndex frameIndex,
                                  std::string_view source) {
        const auto capabilities = GetRuntimeModeCapabilities(mode);
        const bool requestedScriptsRunning = capabilities.scripts_running;
        const bool requestedPlayMode = mode == RuntimeMode::Play;
        const bool wasScriptsRunning = qtScriptsRunning();
        const bool wasPlayMode = qtIsPlayMode();
        const std::string requestedModeName(RuntimeModeStableName(mode));
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "runtime mode toggle requested",
                   {{"requested_mode", requestedModeName},
                    {"source", std::string(source)},
                    {"runtime_mode_before", std::string(qtRuntimeModeName())},
                    {"play_mode_before", wasPlayMode ? "true" : "false"},
                    {"scripts_running_before", wasScriptsRunning ? "true" : "false"},
                    {"scripts_enabled_before", qtScriptState.scripts_enabled ? "true" : "false"},
                    {"fps_mode_before", qtFpsMode ? "true" : "false"},
                    {"lua_compiled_in", qtScriptRuntime ? (qtScriptRuntime->lua_compiled_in() ? "true" : "false") : "missing_runtime"},
                    {"execution_available", qtScriptRuntime ? (qtScriptRuntime->execution_available() ? "true" : "false") : "missing_runtime"},
                    {"bindings", std::to_string(qtScriptState.binding_summary.binding_count)},
                    {"runnable", std::to_string(qtScriptState.binding_summary.runnable_count)}},
                   frameIndex);
        if (requestedScriptsRunning && qtScriptRuntime && !qtScriptRuntime->execution_available()) {
          qtScriptState.scripts_enabled = true;
          qtScriptState.playing = false;
          qtScriptState.mode = std::string(kRuntimeModeEditName);
          qtScriptState.status = "runtime mode unavailable: Lua execution is not compiled in";
          ui_runtime_state.status_message = qtScriptState.status;
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtSyncViewportAssetDropMode();
          qtDockPanelsDirty = true;
          logger.log(vkpt::log::Severity::Error,
                     "scripts",
                     "runtime mode unavailable in this executable",
                     {{"requested_mode", requestedModeName},
                      {"lua_compiled_in", qtScriptRuntime->lua_compiled_in() ? "true" : "false"},
                      {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"},
                      {"active_scene", ui_runtime_state.active_scene},
                      {"status", qtScriptState.status}},
                     frameIndex);
          PushUiEvent(ui_event_log,
                      "runtime_mode_toggle",
                      "viewport",
                      source,
                      frameIndex,
                      {},
                      "unavailable",
                      qtScriptState.status);
          return;
        }
        if (requestedScriptsRunning && qtFpsMode) {
          if (qtSetFpsMode) {
            qtSetFpsMode(false, frameIndex, source);
          } else {
            qtFpsMode = false;
          }
        }
        if (!requestedScriptsRunning && wasScriptsRunning) {
          qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnDisable,
                               frameIndex,
                               0.0,
                               "runtime mode exit");
        }
        const bool modeChanged = qtRuntimeModeName() != requestedModeName;
        qtScriptState.mode = requestedModeName;
        qtScriptState.playing = requestedPlayMode;
        if (mode != RuntimeMode::LiveEdit || modeChanged) {
          qtScriptState.viewport_input_forwarding = false;
        }
        if (requestedScriptsRunning) {
          qtScriptState.scripts_enabled = true;
        }
        qtScriptState.benchmark_scripts_allowed = requestedScriptsRunning;
        if (requestedScriptsRunning && !wasScriptsRunning) {
          qtSyncPhysicsFromSceneDocument();
          qtReloadScriptsFromDocument(requestedPlayMode ? "play mode enter" : "live edit enter");
          qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLoad,
                               frameIndex,
                               0.0,
                               requestedPlayMode ? "play mode load" : "live edit load");
          qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnEnable,
                               frameIndex,
                               0.0,
                               requestedPlayMode ? "play mode enter" : "live edit enter");
        }
        qtPendingFpsLookDx = 0.0f;
        qtPendingFpsLookDy = 0.0f;
        qtLeftMouseDown = false;
        qtRightMouseDown = false;
        qtMiddleMouseDown = false;
        qtPotentialClick = false;
        ResetViewportMouseClick(qtMouseClickState);
        qtClickDragPixels = 0.0f;
        qtGizmoDrag = {};
        qtHoveredGizmoHit = std::nullopt;
        qtSetViewportMouseLocked(requestedPlayMode || qtLiveEditViewportInputArmed());
        qtSyncViewportAssetDropMode();
        if (!requestedScriptsRunning) {
          if (qtAudioSystem) {
            qtAudioSystem->stop_all();
          }
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        }
        qtPreviewStatus = requestedPlayMode
            ? "play mode"
            : (mode == RuntimeMode::LiveEdit ? "live edit" : "editor mode");
        qtScriptState.status = requestedPlayMode
            ? "play mode running"
            : (mode == RuntimeMode::LiveEdit ? "live edit running" : "runtime stopped");
        ui_runtime_state.status_message = qtScriptState.status;
        qtDockPanelsDirty = true;
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "runtime mode toggle applied",
                   {{"runtime_mode", std::string(qtRuntimeModeName())},
                    {"play_mode", qtScriptState.playing ? "true" : "false"},
                    {"scripts_running", qtScriptsRunning() ? "true" : "false"},
                    {"scripts_enabled", qtScriptState.scripts_enabled ? "true" : "false"},
                    {"fps_mode", qtFpsMode ? "true" : "false"},
                    {"status", qtScriptState.status}},
                   frameIndex);
        PushUiEvent(ui_event_log,
                    "runtime_mode_toggle",
                    "viewport",
                    source,
                    frameIndex,
                    {},
                    requestedModeName,
                    qtScriptState.status);
      };
      auto qtSetGameMode = [&](bool enabled,
                               vkpt::core::FrameIndex frameIndex,
                               std::string_view source) {
        qtSetRuntimeMode(enabled ? RuntimeMode::Play : RuntimeMode::Edit,
                         frameIndex,
                         source);
      };
      auto qtToggleLiveEditMode = [&](vkpt::core::FrameIndex frameIndex,
                                      std::string_view source) {
        if (!qtIsLiveEditMode()) {
          qtSetRuntimeMode(RuntimeMode::LiveEdit, frameIndex, source);
          return;
        }
        qtScriptState.viewport_input_forwarding = !qtScriptState.viewport_input_forwarding;
        qtPendingFpsLookDx = 0.0f;
        qtPendingFpsLookDy = 0.0f;
        qtLeftMouseDown = false;
        qtRightMouseDown = false;
        qtMiddleMouseDown = false;
        qtPotentialClick = false;
        ResetViewportMouseClick(qtMouseClickState);
        qtClickDragPixels = 0.0f;
        qtGizmoDrag = {};
        qtHoveredGizmoHit = std::nullopt;
        qtSetViewportMouseLocked(qtScriptState.viewport_input_forwarding);
        qtSyncViewportAssetDropMode();
        qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        qtScriptState.status = qtScriptState.viewport_input_forwarding
            ? "live edit mouse locked"
            : "live edit mouse unlocked";
        ui_runtime_state.status_message = qtScriptState.status;
        qtDockPanelsDirty = true;
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "live edit mouse lock toggled",
                   {{"locked", qtScriptState.viewport_input_forwarding ? "true" : "false"},
                    {"source", std::string(source)},
                    {"runtime_mode", std::string(qtRuntimeModeName())}},
                   frameIndex);
        PushUiEvent(ui_event_log,
                    "live_edit_mouse_lock_toggle",
                    "viewport",
                    source,
                    frameIndex,
                    {},
                    qtScriptState.viewport_input_forwarding ? "locked" : "unlocked",
                    qtScriptState.status);
      };
