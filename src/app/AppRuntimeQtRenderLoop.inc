      enum class QtInputRouteResult {
        Pass,
        Handled,
        Capture,
      };

      enum class QtInputMode {
        Editor,
        FpsCamera,
        ScriptPlayback,
      };

      auto qtCurrentInputMode = [&]() -> QtInputMode {
        if (qtFpsMode) {
          return QtInputMode::FpsCamera;
        }
        if (qtScriptState.playing) {
          return QtInputMode::ScriptPlayback;
        }
        return QtInputMode::Editor;
      };

      // Ordered input layers give Escape a single reusable cancel path before
      // mode-specific shortcuts consume the same key event.
      auto qtHandleEscapeCancelLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (key != kQtKeyEscape && rawKey != 27) {
          return QtInputRouteResult::Pass;
        }

        const bool wasLuaPlayable = qtScriptState.playing;
        const bool hadSelection =
            !ui_selection_state.selected_entity_ids.empty() ||
            ui_selection_state.active_primary_entity != 0u ||
            ui_selection_state.hovered_entity != 0u;
        if (qtSetFpsMode) {
          qtSetFpsMode(false, qtFrameCount, "keyboard");
        } else {
          qtFpsMode = false;
        }
        ui_runtime_state.active_viewport_tool = vkpt::editor::ViewportTool::Select;
        ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        qtHoveredGizmoHit = std::nullopt;
        if (wasLuaPlayable) {
          qtSetLuaPlayableMode(false, qtFrameCount, "keyboard");
        } else if (hadSelection) {
          vkpt::editor::EditorCommand clearCommand;
          clearCommand.command_id = "keyboard.escape.clear_selection";
          clearCommand.kind = vkpt::editor::EditorCommandKind::kClearSelection;
          clearCommand.source_widget = "keyboard";
          clearCommand.frame_index = qtFrameCount;
          clearCommand.payload = vkpt::editor::ClearSelectionCommand{};
          ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, clearCommand);
          RebuildSelectionBounds(ui_selection_state, qtPickables);
          ui_command_history.push(clearCommand);
          rebuildQtMenuBar();
          qtDockPanelsDirty = true;
          ui_runtime_state.status_message = "selection cleared";
          PushUiEvent(ui_event_log,
                      "keyboard_clear_selection",
                      "viewport",
                      "keyboard",
                      qtFrameCount,
                      {},
                      "escape",
                      ui_runtime_state.status_message);
        } else {
          ui_runtime_state.status_message = "select camera mode";
        }
        updateQtSelectionOverlay();
        qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        return QtInputRouteResult::Handled;
      };

      auto qtHandleCameraKeyLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (key == 'F' || rawKey == 'F') {
          qtFocusSelected(qtFrameCount, "keyboard");
          return QtInputRouteResult::Handled;
        }
        if (key == 'P' || rawKey == 'P') {
          qtPickFocusAt(qtLastMouseX, qtLastMouseY, qtFrameCount, "keyboard", true);
          return QtInputRouteResult::Handled;
        }
        if (key == 'V' || rawKey == 'V') {
          if (qtSetFpsMode) {
            qtSetFpsMode(!qtFpsMode, qtFrameCount, "keyboard");
          }
          return QtInputRouteResult::Handled;
        }
        return QtInputRouteResult::Pass;
      };

      auto qtHandleScriptKeyLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (key == 'B' || rawKey == 'B') {
          qtSetLuaPlayableMode(!qtScriptState.playing, qtFrameCount, "keyboard");
          return QtInputRouteResult::Handled;
        }
        return QtInputRouteResult::Pass;
      };

      auto qtHandleFpsKeyLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (qtCurrentInputMode() == QtInputMode::FpsCamera &&
            (key == kQtKeySpace || rawKey == kQtKeySpace)) {
          qtFpsPlayer.jump_queued = true;
          qtUserCameraActive = true;
          return QtInputRouteResult::Handled;
        }
        return QtInputRouteResult::Pass;
      };

      auto qtHandleViewportToolKeyLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (qtCurrentInputMode() != QtInputMode::Editor) {
          return QtInputRouteResult::Pass;
        }
        if (key == 'C' || rawKey == 'C') {
          qtPhysicsCollisionDetectionEnabled = !qtPhysicsCollisionDetectionEnabled;
          qtPhysicsStepConfig.collision_detection_enabled = qtPhysicsCollisionDetectionEnabled;
          qtDockPanelsDirty = true;
          ui_runtime_state.status_message = qtPhysicsCollisionDetectionEnabled
              ? "physics collision detection on"
              : "physics collision detection off";
          PushUiEvent(ui_event_log,
                      "physics_collision_detection_toggle",
                      "viewport",
                      "keyboard",
                      qtFrameCount,
                      {},
                      qtPhysicsCollisionDetectionEnabled ? "on" : "off",
                      ui_runtime_state.status_message);
          return QtInputRouteResult::Handled;
        }
        if (key == 'T' || rawKey == 'T') {
          qtSetGizmoMode(vkpt::editor::GizmoMode::Translate, "keyboard", qtFrameCount);
          return QtInputRouteResult::Handled;
        }
        if (key == 'R' || rawKey == 'R') {
          qtSetGizmoMode(vkpt::editor::GizmoMode::Rotate, "keyboard", qtFrameCount);
          return QtInputRouteResult::Handled;
        }
        if (key == 'G' || rawKey == 'G') {
          qtSetGizmoMode(vkpt::editor::GizmoMode::Universal, "keyboard", qtFrameCount);
          return QtInputRouteResult::Handled;
        }
        if (key == kQtKeyDelete || rawKey == kQtKeyDelete ||
            key == kQtKeyBackspace || rawKey == kQtKeyBackspace ||
            rawKey == 46 || rawKey == 8) {
          auto deleteIds = ui_selection_state.selected_entity_ids;
          if (deleteIds.empty() && ui_selection_state.active_primary_entity != 0u) {
            deleteIds.push_back(ui_selection_state.active_primary_entity);
          }
          if (!deleteIds.empty()) {
            qtDeleteSceneGraphTargets(std::move(deleteIds),
                                      qtFrameCount,
                                      "keyboard",
                                      "delete");
          } else {
            ui_runtime_state.status_message = "delete failed: no scene graph item selected";
            ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
            qtDockPanelsDirty = true;
          }
          return QtInputRouteResult::Handled;
        }
        return QtInputRouteResult::Pass;
      };

      auto qtRouteKeyDown = [&](int key, int rawKey) -> QtInputRouteResult {
        QtInputRouteResult result = qtHandleEscapeCancelLayer(key, rawKey);
        if (result != QtInputRouteResult::Pass) {
          return result;
        }
        result = qtHandleScriptKeyLayer(key, rawKey);
        if (result != QtInputRouteResult::Pass) {
          return result;
        }
        result = qtHandleFpsKeyLayer(key, rawKey);
        if (result != QtInputRouteResult::Pass) {
          return result;
        }
        result = qtHandleCameraKeyLayer(key, rawKey);
        if (result != QtInputRouteResult::Pass) {
          return result;
        }
        return qtHandleViewportToolKeyLayer(key, rawKey);
      };

      constexpr uint32_t kQtMaxGpuBatchesPerTick = 64u;
      while (window->is_open()) {
        const auto qtFrameStart = std::chrono::steady_clock::now();

        const float qtInputDt = ClampFloat(
            std::chrono::duration<float>(qtFrameStart - qtInputPrevTime).count(),
            0.0f,
            0.05f);
        qtInputPrevTime = qtFrameStart;

        std::vector<vkpt::platform::InputEvent> qtInputEvents;
        if (auto* input = platform->input()) {
          input->consume(qtInputEvents);
        } else if (!window->poll_events()) {
          break;
        }
        if (!window->is_open()) {
          break;
        }

        for (const auto& event : qtInputEvents) {
          LogWindowInput(ui_event_log, ui_runtime_state, event, qtFrameCount);
          switch (event.type) {
            case vkpt::platform::InputEventType::MenuCommand: {
              const auto nativeCommandId = static_cast<NativeMenuId>(event.raw_code);
              const auto mappedAction = qtMenuCommandLookup.find(nativeCommandId);
              if (mappedAction == qtMenuCommandLookup.end()) {
                const auto actionId =
                    std::string("app.menu.unknown_command_") + std::to_string(nativeCommandId);
                qtLogMenuCommand(actionId,
                                 MakeUnsupportedUiCommand(actionId, "unknown Qt menu command id", "menu", qtFrameCount),
                                 std::string("unknown menu action: ") + std::to_string(nativeCommandId),
                                 qtFrameCount);
                break;
              }

              const std::string actionId = mappedAction->second;
              auto menuAction = vkpt::editor::MakeMenuCommand(actionId, "menu", qtFrameCount);
              menuAction.command_id = actionId;

              if (actionId == "file.exit") {
                qtLogMenuCommand(actionId,
                                 MakeUnsupportedUiCommand("file.exit", "user selected Exit", "menu", qtFrameCount),
                                 "menu action executed: file.exit",
                                 qtFrameCount);
                window->close();
                break;
              }

              if (actionId == "view.focus_selected") {
                const bool focused = qtFocusSelected(qtFrameCount, "menu");
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 focused ? "menu action executed: view.focus_selected"
                                         : "menu action failed: no active selection",
                                 qtFrameCount);
                break;
              }

              if (actionId == "render.start_render") {
                const bool rendered = qtStartRenderToFile(qtFrameCount);
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 rendered ? "menu action executed: render.start_render"
                                          : "menu action failed: " + qtLastRenderMenuStatus,
                                 qtFrameCount);
                break;
              }

              if (menuAction.kind == vkpt::editor::EditorCommandKind::kDeleteEntity) {
                const auto ids = ResolveCommandEntityIds(menuAction, ui_selection_state);
                const bool deleted = qtDeleteSceneGraphTargets(ids, qtFrameCount, "menu", actionId);
                if (deleted) {
                  ui_runtime_state.last_menu_action = actionId;
                  break;
                }
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 "menu action failed: " + ui_runtime_state.status_message,
                                 qtFrameCount);
                break;
              }

              if (menuAction.kind == vkpt::editor::EditorCommandKind::kClearSelection) {
                ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, menuAction);
                RebuildSelectionBounds(ui_selection_state, qtPickables);
                ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
                qtHoveredGizmoHit = std::nullopt;
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 "menu action executed: " + actionId,
                                 qtFrameCount);
                rebuildQtMenuBar();
                updateQtSelectionOverlay();
                qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
                break;
              }

              if (menuAction.kind != vkpt::editor::EditorCommandKind::kUnsupportedUiAction) {
                const std::string menuStatus =
                    std::string("menu action routed: ") +
                    vkpt::editor::ToString(menuAction.kind) +
                    " (command queued)";
                qtLogMenuCommand(actionId, menuAction, menuStatus, qtFrameCount);
                break;
              }

              std::string reason = "unsupported in Qt menu runtime";
              if (std::holds_alternative<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload)) {
                reason = std::get<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload).reason;
              }
              qtLogMenuCommand(actionId,
                               menuAction,
                               "menu action unsupported: " + actionId + " (" + reason + ")",
                               qtFrameCount);
              break;
            }
            case vkpt::platform::InputEventType::KeyDown: {
              const int key = qtNormalizeKey(event.code);
              const int rawKey = qtNormalizeKey(event.raw_code);
              qtKeysDown.insert(key);
              if (rawKey != 0) {
                qtKeysDown.insert(rawKey);
              }
              logger.log(vkpt::log::Severity::Info,
                         "input",
                         "Qt key down consumed by app runtime",
                         {{"key", std::to_string(key)},
                          {"raw_key", std::to_string(rawKey)},
                          {"is_b_key", (key == 'B' || rawKey == 'B') ? "true" : "false"},
                          {"is_w_key", (key == 'W' || rawKey == 'W') ? "true" : "false"},
                          {"is_a_key", (key == 'A' || rawKey == 'A') ? "true" : "false"},
                          {"is_s_key", (key == 'S' || rawKey == 'S') ? "true" : "false"},
                          {"is_d_key", (key == 'D' || rawKey == 'D') ? "true" : "false"},
                          {"keys_down", std::to_string(qtKeysDown.size())},
                          {"lua_playing", qtScriptState.playing ? "true" : "false"},
                          {"lua_compiled_in", qtScriptRuntime->lua_compiled_in() ? "true" : "false"},
                          {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"}},
                         qtFrameCount);
              (void)qtRouteKeyDown(key, rawKey);
              break;
            }
            case vkpt::platform::InputEventType::KeyUp: {
              const int key = qtNormalizeKey(event.code);
              const int rawKey = qtNormalizeKey(event.raw_code);
              qtKeysDown.erase(key);
              qtKeysDown.erase(rawKey);
              logger.log(vkpt::log::Severity::Debug,
                         "input",
                         "Qt key up consumed by app runtime",
                         {{"key", std::to_string(key)},
                          {"raw_key", std::to_string(rawKey)},
                          {"is_b_key", (key == 'B' || rawKey == 'B') ? "true" : "false"},
                          {"is_w_key", (key == 'W' || rawKey == 'W') ? "true" : "false"},
                          {"is_a_key", (key == 'A' || rawKey == 'A') ? "true" : "false"},
                          {"is_s_key", (key == 'S' || rawKey == 'S') ? "true" : "false"},
                          {"is_d_key", (key == 'D' || rawKey == 'D') ? "true" : "false"},
                          {"keys_down", std::to_string(qtKeysDown.size())},
                          {"lua_playing", qtScriptState.playing ? "true" : "false"}},
                         qtFrameCount);
              break;
            }
            case vkpt::platform::InputEventType::MouseButtonDown:
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              if (qtCurrentInputMode() == QtInputMode::FpsCamera) {
                qtUserCameraActive = true;
                qtLeftMouseDown = false;
                qtRightMouseDown = false;
                qtMiddleMouseDown = false;
                qtPotentialClick = false;
                qtGizmoDrag = {};
                break;
              }
              if (event.code == 0) {
                qtUserCameraActive = true;
                qtLeftMouseDown = true;
                qtClickX = event.x;
                qtClickY = event.y;
                qtClickDragPixels = 0.0f;
                qtPotentialClick = true;
                if (qtCurrentInputMode() == QtInputMode::Editor &&
                    ui_runtime_state.active_gizmo_mode != vkpt::editor::GizmoMode::None &&
                    ui_selection_state.active_primary_entity != 0u) {
                  const auto activeBounds = qtActiveSelectionBounds();
                  if (activeBounds) {
                    const auto frameRect = qtViewportImageRect();
                    const auto localPoint = qtViewportLocalPoint(event.x, event.y);
                    if (localPoint) {
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
                      if (hit) {
                        qtCaptureGizmoDrag(*hit, event.x, event.y);
                        if (qtGizmoDrag.active) {
                          qtPotentialClick = false;
                          ui_runtime_state.status_message = "gizmo drag started";
                        }
                      }
                    }
                  }
                }
              } else if (event.code == 1) {
                qtRightMouseDown = true;
                syncQtFpsAnglesFromPose();
              } else if (event.code == 2) {
                qtMiddleMouseDown = true;
              }
              break;
            case vkpt::platform::InputEventType::MouseButtonUp:
              if (qtCurrentInputMode() == QtInputMode::FpsCamera) {
                qtLeftMouseDown = false;
                qtRightMouseDown = false;
                qtMiddleMouseDown = false;
                qtPotentialClick = false;
                qtGizmoDrag = {};
                break;
              }
              if (event.code == 0) {
                qtLeftMouseDown = false;
                if (qtGizmoDrag.active) {
                  qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                  qtCommitGizmoDrag();
                  qtGizmoDrag = {};
                  ui_runtime_state.status_message = "gizmo drag committed";
                  qtDockPanelsDirty = true;
                } else if (qtPotentialClick && qtClickDragPixels <= 6.0f) {
                  qtApplyViewportPick(event.x, event.y, qtFrameCount);
                }
                qtPotentialClick = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              } else if (event.code == 1) {
                qtRightMouseDown = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              } else if (event.code == 2) {
                qtMiddleMouseDown = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              }
              break;
            case vkpt::platform::InputEventType::MouseMove: {
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              const QtInputMode inputMode = qtCurrentInputMode();
              if (inputMode == QtInputMode::FpsCamera) {
                qtUserCameraActive = true;
                qtPendingFpsLookDx += event.delta_x;
                qtPendingFpsLookDy += event.delta_y;
                break;
              }
              if (inputMode == QtInputMode::ScriptPlayback) {
                qtUserCameraActive = true;
                qtPendingFpsLookDx += event.delta_x;
                qtPendingFpsLookDy += event.delta_y;
                break;
              }
              if (qtLeftMouseDown && qtGizmoDrag.active) {
                qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                qtClickDragPixels = std::max(qtClickDragPixels,
                                             ScreenDistance(event.x, event.y, qtClickX, qtClickY));
              } else if (qtLeftMouseDown && qtPotentialClick) {
                const float dx = event.x - qtClickX;
                const float dy = event.y - qtClickY;
                qtClickDragPixels = std::max(qtClickDragPixels, std::sqrt(dx * dx + dy * dy));
              }
              if ((qtRightMouseDown || inputMode == QtInputMode::FpsCamera) && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                if (inputMode == QtInputMode::FpsCamera) {
                  qtApplyFpsLookDelta(event.delta_x, event.delta_y);
                } else {
                  qtApplyOrbitDrag(event.delta_x, event.delta_y);
                }
              } else if (qtMiddleMouseDown && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                qtApplyPanDrag(event.delta_x, event.delta_y);
              }
              if (!qtGizmoDrag.active && !qtRightMouseDown && !qtMiddleMouseDown) {
                qtUpdateGizmoHoverCursor(event.x, event.y);
              }
              break;
            }
            case vkpt::platform::InputEventType::MouseWheel:
              if (qtCurrentInputMode() != QtInputMode::Editor) {
                break;
              }
              qtUserCameraActive = true;
              qtApplyDolly(event.delta_z);
              break;
            case vkpt::platform::InputEventType::WindowResize:
              if (event.x > 0.0f && event.y > 0.0f) {
                ApplyWindowMetricsToLayout(ui_layout_state,
                                           static_cast<std::size_t>(event.x),
                                           static_cast<std::size_t>(event.y));
                SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
                updateQtSelectionOverlay();
              }
              break;
            case vkpt::platform::InputEventType::FocusLost:
              if (qtSetFpsMode && qtFpsMode) {
                qtSetFpsMode(false, qtFrameCount, "focus");
              } else {
                qtSetViewportMouseLocked(false);
              }
              qtPendingFpsLookDx = 0.0f;
              qtPendingFpsLookDy = 0.0f;
              qtKeysDown.clear();
              qtLeftMouseDown = false;
              qtRightMouseDown = false;
              qtMiddleMouseDown = false;
              qtPotentialClick = false;
              if (qtGizmoDrag.active) {
                qtCommitGizmoDrag();
              }
              qtGizmoDrag = {};
              qtHoveredGizmoHit = std::nullopt;
              qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
              ui_runtime_state.status_message = "window focus lost";
              break;
            case vkpt::platform::InputEventType::FocusGained:
              ui_runtime_state.status_message = "window focus gained";
              break;
            case vkpt::platform::InputEventType::CloseRequested:
              window->close();
              break;
            default:
              break;
          }
        }
        if (qtGizmoDrag.active) {
          qtFlushPendingGizmoTransformUpdates(false);
        }
        std::vector<vkpt::platform::QtDockPropertyEdit> qtPropertyEdits;
        if (qtWindow != nullptr) {
          qtPropertyEdits = qtWindow->drain_dock_property_edits();
        }
        for (const auto& edit : qtPropertyEdits) {
          qtApplyDockPropertyEdit(edit, qtFrameCount);
        }
        std::vector<vkpt::platform::QtDockRowActivation> qtDockRowActivations;
        if (qtWindow != nullptr) {
          qtDockRowActivations = qtWindow->drain_dock_row_activations();
        }
        for (const auto& activation : qtDockRowActivations) {
          if (!qtApplyAssetBrowserActivation(activation, qtFrameCount)) {
            qtApplySceneGraphActivation(activation, qtFrameCount);
          }
        }
        if (!qtInputEvents.empty() || !qtPropertyEdits.empty() || !qtDockRowActivations.empty()) {
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
        }
        if (qtFpsMode &&
            (std::fabs(qtPendingFpsLookDx) > 0.0f || std::fabs(qtPendingFpsLookDy) > 0.0f)) {
          qtApplyFpsLookDelta(qtPendingFpsLookDx, qtPendingFpsLookDy);
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
        }
        qtApplyPhysicsSimulation(qtFrameStart);
        const bool qtScriptDispatched = qtApplyScriptPlayback(qtFrameStart, qtFrameCount, qtInputDt);
        if (qtScriptDispatched && qtScriptState.playing && !qtFpsMode) {
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
        }
        updateQtSelectionOverlay();
        qtEnableAutoOrbit = false;

        // ---- Camera orbit update — post to bg thread, never join ----
        if (qtTracerReady && qtEnableAutoOrbit && !qtUserCameraActive && kQtOrbitRadius > 1e-4f) {
          const float elapsedSec = std::chrono::duration<float>(qtFrameStart - qtOrbitStartTime).count();
          const float rawAngleDeg = qtOrbitInitialAngleDeg + elapsedSec * kQtOrbitDegPerSec;
          const float angleDeg = std::fmod(rawAngleDeg, 360.0f);
          const float angleDiff = std::fabs(angleDeg - qtOrbitLastAngleDeg);
          const float wrappedDiff = std::min(angleDiff, 360.0f - angleDiff);
          if (wrappedDiff >= kQtOrbitMinStepDeg) {
            const float rad = angleDeg * (3.14159265f / 180.0f);
            qtScene.camera_position.x = qtOrbitCenter.x + kQtOrbitRadius * std::sin(rad);
            qtScene.camera_position.z = qtOrbitCenter.z + kQtOrbitRadius * std::cos(rad);
            qtScene.camera_target = qtOrbitCenter;
#ifdef PT_ENABLE_QT
            syncQtCameraPoseFromScene();
            qtRefreshCameraFocusFromPose();
            syncQtFpsAnglesFromPose();
#endif
            qtOrbitLastAngleDeg = angleDeg;
            if (qtUseBg && qtRenderCoordinator) {
              // Post camera update; coordinator will apply it between samples.
              qtRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
                  qtScene.camera_position,
                  qtScene.camera_target,
                  qtScene.camera_up,
                  qtScene.camera_fov_deg});
            } else {
              // Synchronous (GPU) path — update directly on main thread.
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
          }
        }

        if (qtUseBg && qtRenderCoordinator) {
          const auto coordinatorStats = qtRenderCoordinator->stats();
          qtPublishedFrames.store(coordinatorStats.handoff.published, std::memory_order_relaxed);
          qtDroppedFrames.store(coordinatorStats.handoff.dropped, std::memory_order_relaxed);
          qtPublishedRays.store(coordinatorStats.counters.rays, std::memory_order_relaxed);
          if (coordinatorStats.failed) {
            qtTracerReady = false;
            qtPreviewStatus = coordinatorStats.error.empty() ? "render sample failed" : coordinatorStats.error;
            std::cerr << "[qt] background render coordinator failed\n";
            logger.log(vkpt::log::Severity::Error, "app", "Qt background render coordinator failed", {
              {"sample", std::to_string(coordinatorStats.sample_count)},
              {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
            });
          }
          if (auto frame = qtRenderCoordinator->acquire_latest_frame()) {
            qtPublishedSample.store(frame->sample_count, std::memory_order_relaxed);
            qtPublishedWidth.store(frame->width, std::memory_order_relaxed);
            qtPublishedHeight.store(frame->height, std::memory_order_relaxed);
            qtPublishedRays.store(frame->counters.rays, std::memory_order_relaxed);
#ifdef PT_ENABLE_QT
            if (qtWindow != nullptr) {
              qtWindow->set_framebuffer_rgba(std::move(frame->rgba8), frame->width, frame->height);
            }
#endif
          }
        } else if (!qtUseBg && windowFrameLimit == 0u && qtTracerReady) {
          bool renderedThisTick = false;
          qtLastGpuBatchesPerTick = 0u;
          while (qtTracerReady && qtLastGpuBatchesPerTick < kQtMaxGpuBatchesPerTick) {
            const auto now = std::chrono::steady_clock::now();
            if (qtLastGpuBatchesPerTick > 0u) {
              if (now >= qtFrameStart + kQtInteractiveFrameTarget) {
                break;
              }
              if (qtSmoothedGpuBatchMs > 0.0) {
                const auto estimatedNextBatch =
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(qtSmoothedGpuBatchMs));
                if (now + estimatedNextBatch > qtFrameStart + kQtInteractiveFrameTarget) {
                  break;
                }
              }
            }

#ifdef PT_ENABLE_QT
            const auto countersBefore = qtTracer->read_counters();
#endif
            const auto batchStart = std::chrono::steady_clock::now();
            if (!qtTracer->render_sample_batch(0, qtSettings.height, qtSampleIndex, qtFrameCount)) {
              qtTracerReady = false;
              qtPreviewStatus = "render sample failed";
              std::cerr << "[qt] render_sample_batch failed at sample " << qtSampleIndex << "\n";
              logger.log(vkpt::log::Severity::Error, "app", "Qt render sample failed", {
                {"sample", std::to_string(qtSampleIndex)},
                {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
              });
              break;
            }

            const auto batchMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batchStart).count();
#ifdef PT_ENABLE_QT
            const auto countersAfter = qtTracer->read_counters();
            qtRecordActiveRenderBatch(countersBefore.rays, countersAfter.rays, batchMs);
#endif
            qtSmoothedGpuBatchMs = (qtSmoothedGpuBatchMs <= 0.0)
                ? batchMs
                : (qtSmoothedGpuBatchMs * 0.75 + batchMs * 0.25);
            ++qtSampleIndex;
            ++qtLastGpuBatchesPerTick;
            renderedThisTick = true;
          }

          if (renderedThisTick && qtTracerReady) {
            qtPublishedRays.store(qtTracer->read_counters().rays, std::memory_order_relaxed);
            auto ldr = qtTracer->resolve_ldr();
            qtPublishedSample.store(qtSampleIndex, std::memory_order_relaxed);
            qtPublishedWidth.store(ldr.width, std::memory_order_relaxed);
            qtPublishedHeight.store(ldr.height, std::memory_order_relaxed);
#ifdef PT_ENABLE_QT
            if (qtWindow) {
              qtWindow->set_framebuffer_rgba(std::move(ldr.rgba8), ldr.width, ldr.height);
              qtPublishedFrames.fetch_add(1u, std::memory_order_relaxed);
            }
#endif
          }
        }

#ifdef PT_ENABLE_QT
        qtLastUiFrameMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - qtFrameStart).count();
        updateQtPreviewOverlay();
#endif
        if (!window->is_open()) break;
        ++qtFrameCount;
        if (windowFrameLimit != 0u && qtFrameCount >= windowFrameLimit) {
          BootStep("qt gui smoke frame limit reached");
          window->close();
          break;
        }
        const auto qtFrameEnd = std::chrono::steady_clock::now();
        const auto qtFrameDuration = qtFrameEnd - qtFrameStart;
        if (qtFrameDuration < kQtInteractiveFrameTarget) {
          std::this_thread::sleep_for(kQtInteractiveFrameTarget - qtFrameDuration);
        }
        // Pace the UI/event loop so input and overlay updates do not busy-spin.
      }

      logger.log(vkpt::log::Severity::Info, "app", "Qt render loop closing", {
        {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
        {"frames", std::to_string(qtFrameCount)},
        {"status", qtPreviewStatus}
      });
      if (qtRenderCoordinator) {
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render coordinator stop begin");
        qtRenderCoordinator->stop();
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render coordinator stopped");
      }
#ifdef PT_ENABLE_QT
      DrainQtQueuedWork();
      if (qtWindow != nullptr) {
        const auto qtStats = qtWindow->framebuffer_stats();
        std::ostringstream qtPerfSummary;
        qtPerfSummary << "qt_preview_hz=" << qtPreviewPublishHz
                      << " render_published=" << qtPublishedFrames.load(std::memory_order_relaxed)
                      << " render_dropped=" << qtDroppedFrames.load(std::memory_order_relaxed)
                      << " window_received=" << qtStats.received
                      << " window_presented=" << qtStats.presented
                      << " window_dropped=" << qtStats.dropped;
        status.performance_summary = qtPerfSummary.str();
        logger.log(vkpt::log::Severity::Info, "app", "Qt preview shutdown metadata", {
          {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
          {"render_published", std::to_string(qtPublishedFrames.load(std::memory_order_relaxed))},
          {"render_dropped", std::to_string(qtDroppedFrames.load(std::memory_order_relaxed))},
          {"window_received", std::to_string(qtStats.received)},
          {"window_presented", std::to_string(qtStats.presented)},
          {"window_dropped", std::to_string(qtStats.dropped)},
          {"latest_published_id", std::to_string(qtStats.latestPublishedId)},
          {"latest_presented_id", std::to_string(qtStats.latestPresentedId)}
        });
      }
      DrainQtQueuedWork();
#endif
      BootStep("qt render loop exited; shutting down platform");

      platform->shutdown();
      recordUiAction("app.window.qt", "window", "qt window closed",
                     MakeUnsupportedUiCommand("app.window.qt", "window_closed"));
      writeStatus("window_closed");
      return 0;
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
