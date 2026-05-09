      enum class QtInputRouteResult {
        Pass,
        Handled,
        Capture,
      };

      enum class QtInputMode {
        Editor,
        FpsCamera,
        GameMode,
      };

      auto qtCurrentInputMode = [&]() -> QtInputMode {
        if (qtIsPlayMode() || qtLiveEditViewportInputArmed()) {
          return QtInputMode::GameMode;
        }
        if (qtFpsMode) {
          return QtInputMode::FpsCamera;
        }
        return QtInputMode::Editor;
      };
      auto qtViewportMouseInputMode = [](QtInputMode mode) -> ViewportMouseInputMode {
        switch (mode) {
          case QtInputMode::FpsCamera:
            return ViewportMouseInputMode::FpsCamera;
          case QtInputMode::GameMode:
            return ViewportMouseInputMode::GameModeScript;
          case QtInputMode::Editor:
          default:
            return ViewportMouseInputMode::Editor;
        }
      };
      auto qtResetViewportMouseClick = [&]() {
        ResetViewportMouseClick(qtMouseClickState);
        qtPotentialClick = false;
        qtClickDragPixels = 0.0f;
      };

      // Ordered input layers give Escape a single reusable cancel path before
      // mode-specific shortcuts consume the same key event.
      auto qtHandleEscapeCancelLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (key != kQtKeyEscape && rawKey != 27) {
          return QtInputRouteResult::Pass;
        }

        const bool wasRuntimeMode = qtScriptsRunning();
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
        if (wasRuntimeMode) {
          qtSetRuntimeMode(RuntimeMode::Edit, qtFrameCount, "keyboard");
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

      auto qtHandleGameModeKeyLayer = [&](int key, int rawKey) -> QtInputRouteResult {
        if (key == kQtKeyF1 || rawKey == kQtKeyF1) {
          qtSetRuntimeMode(qtIsPlayMode() ? RuntimeMode::Edit : RuntimeMode::Play,
                           qtFrameCount,
                           "keyboard");
          return QtInputRouteResult::Handled;
        }
        if (key == kQtKeyF2 || rawKey == kQtKeyF2) {
          qtToggleLiveEditMode(qtFrameCount, "keyboard");
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
        result = qtHandleGameModeKeyLayer(key, rawKey);
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

      constexpr uint32_t kQtMaxGpuBatchesPerTick = 1u;
      const auto qtMeasuredLoopStart = std::chrono::steady_clock::now();
      // Auto-enter Play mode if the CLI requested it (--play). Goes through
      // the same code path F1 uses, so all downstream subsystems (script
      // tick, ragdoll attach, animation tick, framebuffer pacing) wake up
      // identically. Fires once before the loop body so the very first frame
      // is already in Play mode.
      if (parsedOptions.start_in_play_mode) {
        qtStartupStep("auto-entering play mode (cli --play)");
        qtSetRuntimeMode(RuntimeMode::Play, qtFrameCount, "cli --play");
      }
      while (window->is_open()) {
        const auto qtFrameStart = std::chrono::steady_clock::now();

        if (qtStressGate) {
          if (qtStressAdvance() && !qtStressReportWritten) {
            qtStressWriteReport();
            qtStressReportWritten = true;
            window->close();
            break;
          }
        }

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
        const auto qtAfterPoll = std::chrono::steady_clock::now();
        qtPollSceneLoadJob(qtFrameCount);

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
                          {"is_f1_key", (key == kQtKeyF1 || rawKey == kQtKeyF1) ? "true" : "false"},
                          {"is_f2_key", (key == kQtKeyF2 || rawKey == kQtKeyF2) ? "true" : "false"},
                          {"is_w_key", (key == 'W' || rawKey == 'W') ? "true" : "false"},
                          {"is_a_key", (key == 'A' || rawKey == 'A') ? "true" : "false"},
                          {"is_s_key", (key == 'S' || rawKey == 'S') ? "true" : "false"},
                          {"is_d_key", (key == 'D' || rawKey == 'D') ? "true" : "false"},
                          {"keys_down", std::to_string(qtKeysDown.size())},
                          {"runtime_mode", std::string(qtRuntimeModeName())},
                          {"play_mode", qtIsPlayMode() ? "true" : "false"},
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
                          {"is_f1_key", (key == kQtKeyF1 || rawKey == kQtKeyF1) ? "true" : "false"},
                          {"is_f2_key", (key == kQtKeyF2 || rawKey == kQtKeyF2) ? "true" : "false"},
                          {"is_w_key", (key == 'W' || rawKey == 'W') ? "true" : "false"},
                          {"is_a_key", (key == 'A' || rawKey == 'A') ? "true" : "false"},
                          {"is_s_key", (key == 'S' || rawKey == 'S') ? "true" : "false"},
                          {"is_d_key", (key == 'D' || rawKey == 'D') ? "true" : "false"},
                          {"keys_down", std::to_string(qtKeysDown.size())},
                          {"runtime_mode", std::string(qtRuntimeModeName())},
                          {"play_mode", qtIsPlayMode() ? "true" : "false"}},
                         qtFrameCount);
              break;
            }
            case vkpt::platform::InputEventType::MouseButtonDown:
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              {
                const auto inputMode = qtCurrentInputMode();
                BeginViewportMouseClick(qtMouseClickState,
                                        qtViewportMouseInputMode(inputMode),
                                        event.code,
                                        event.x,
                                        event.y);
                qtPotentialClick =
                    qtMouseClickState.owner == ViewportMouseClickOwner::EditorSelection;
                if (inputMode == QtInputMode::FpsCamera) {
                  qtUserCameraActive = true;
                  qtLeftMouseDown = false;
                  qtRightMouseDown = false;
                  qtMiddleMouseDown = false;
                  qtPotentialClick = false;
                  qtGizmoDrag = {};
                  break;
                }
              }
              if (qtCurrentInputMode() == QtInputMode::GameMode) {
                qtUserCameraActive = true;
                if (event.code == 0) {
                  qtLeftMouseDown = true;
                } else if (event.code == 1) {
                  qtRightMouseDown = true;
                } else if (event.code == 2) {
                  qtMiddleMouseDown = true;
                }
                qtPotentialClick = false;
                qtGizmoDrag = {};
                break;
              }
              if (event.code == 0) {
                qtUserCameraActive = true;
                qtLeftMouseDown = true;
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
                    if (hit) {
                      qtCaptureGizmoDrag(*hit, event.x, event.y);
                      if (qtGizmoDrag.active) {
                        MarkViewportMouseClickDrag(qtMouseClickState);
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
              {
                const QtInputMode inputMode = qtCurrentInputMode();
                const auto mouseInputMode = qtViewportMouseInputMode(inputMode);
                if (inputMode == QtInputMode::FpsCamera) {
                  qtLeftMouseDown = false;
                  qtRightMouseDown = false;
                  qtMiddleMouseDown = false;
                  qtResetViewportMouseClick();
                  qtGizmoDrag = {};
                  break;
                }
                if (event.code == 0) {
                  qtLeftMouseDown = false;
                  const auto clickResult =
                      EndViewportMouseClick(qtMouseClickState,
                                            mouseInputMode,
                                            event.code,
                                            event.x,
                                            event.y,
                                            6.0f);
                  if (qtGizmoDrag.active) {
                    qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                    qtCommitGizmoDrag(qtFrameCount);
                    qtGizmoDrag = {};
                    ui_runtime_state.status_message = "gizmo drag committed";
                    qtDockPanelsDirty = true;
                  } else if (clickResult.editor_pick_allowed) {
                    qtApplyViewportPick(event.x, event.y, qtFrameCount);
                  } else if (clickResult.suppressed_editor_pick) {
                    ui_runtime_state.status_message =
                        clickResult.game_mode_click
                            ? "game mode consumed viewport click"
                            : "viewport click consumed by camera mode";
                    PushUiEvent(ui_event_log,
                                "viewport_pick_suppressed",
                                "viewport",
                                clickResult.game_mode_click ? "game_mode" : "camera",
                                qtFrameCount,
                                {},
                                clickResult.game_mode_click ? "game_mode" : "camera",
                                ui_runtime_state.status_message);
                  }
                  qtPotentialClick = false;
                  if (inputMode == QtInputMode::Editor) {
                    qtUpdateGizmoHoverCursor(event.x, event.y);
                  }
                } else if (event.code == 1) {
                  qtRightMouseDown = false;
                  (void)EndViewportMouseClick(qtMouseClickState,
                                              mouseInputMode,
                                              event.code,
                                              event.x,
                                              event.y,
                                              6.0f);
                  if (inputMode == QtInputMode::Editor) {
                    qtUpdateGizmoHoverCursor(event.x, event.y);
                  }
                } else if (event.code == 2) {
                  qtMiddleMouseDown = false;
                  (void)EndViewportMouseClick(qtMouseClickState,
                                              mouseInputMode,
                                              event.code,
                                              event.x,
                                              event.y,
                                              6.0f);
                  if (inputMode == QtInputMode::Editor) {
                    qtUpdateGizmoHoverCursor(event.x, event.y);
                  }
                }
              }
              break;
            case vkpt::platform::InputEventType::MouseMove: {
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              const QtInputMode inputMode = qtCurrentInputMode();
              if (qtLeftMouseDown || qtRightMouseDown || qtMiddleMouseDown) {
                qtClickDragPixels =
                    UpdateViewportMouseClickDrag(qtMouseClickState, event.x, event.y);
              }
              if (inputMode == QtInputMode::FpsCamera) {
                qtUserCameraActive = true;
                qtPendingFpsLookDx += event.delta_x;
                qtPendingFpsLookDy += event.delta_y;
                break;
              }
              if (inputMode == QtInputMode::GameMode) {
                qtUserCameraActive = true;
                qtPendingFpsLookDx += event.delta_x;
                qtPendingFpsLookDy += event.delta_y;
                break;
              }
              if (qtLeftMouseDown && qtGizmoDrag.active) {
                qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                qtClickDragPixels = UpdateViewportMouseClickDrag(qtMouseClickState, event.x, event.y);
              } else if (qtLeftMouseDown && qtPotentialClick) {
                qtClickDragPixels = UpdateViewportMouseClickDrag(qtMouseClickState, event.x, event.y);
              }
              if ((qtRightMouseDown || inputMode == QtInputMode::FpsCamera) && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                if (inputMode == QtInputMode::FpsCamera) {
                  qtApplyFpsLookDelta(event.delta_x, event.delta_y);
                } else {
                  qtPendingOrbitDx += event.delta_x;
                  qtPendingOrbitDy += event.delta_y;
                }
              } else if (qtMiddleMouseDown && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                qtPendingPanDx += event.delta_x;
                qtPendingPanDy += event.delta_y;
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
              if (qtLiveEditViewportInputArmed()) {
                qtScriptState.viewport_input_forwarding = false;
                qtDockPanelsDirty = true;
              }
              if (qtSetFpsMode && qtFpsMode) {
                qtSetFpsMode(false, qtFrameCount, "focus");
              } else {
                qtSetViewportMouseLocked(false);
              }
              qtSyncViewportAssetDropMode();
              qtPendingFpsLookDx = 0.0f;
              qtPendingFpsLookDy = 0.0f;
              qtKeysDown.clear();
              qtLeftMouseDown = false;
              qtRightMouseDown = false;
              qtMiddleMouseDown = false;
              qtResetViewportMouseClick();
              if (qtGizmoDrag.active) {
                qtCommitGizmoDrag(qtFrameCount);
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
        qtPollSceneLoadJob(qtFrameCount);
        if (!qtInputEvents.empty() || !qtPropertyEdits.empty() || !qtDockRowActivations.empty()) {
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
        }
        const auto qtAfterInputHandling = std::chrono::steady_clock::now();
        // Coalesce all per-event drag transforms into a single render-thread
        // post per frame: BuildSceneWorldSnapshot + transform-update post is
        // O(scene size), so we only want to pay it once per frame regardless
        // of how many MouseMove events arrived.
        if (qtGizmoDrag.active && qtGizmoDrag.pending_publish &&
            !qtGizmoDrag.publishable_entity_ids.empty()) {
          qtGizmoDrag.pending_publish = false;
          ++qtGizmoTransformImmediatePublishes;
          qtGizmoTransformPublishedEntityCount +=
              qtGizmoDrag.publishable_entity_ids.size();
          if (!qtPublishGizmoTransformUpdates(qtGizmoDrag.publishable_entity_ids,
                                              qtFrameCount)) {
            ++qtGizmoTransformPublishFailures;
          }
        }
        if (qtFpsMode &&
            (std::fabs(qtPendingFpsLookDx) > 0.0f || std::fabs(qtPendingFpsLookDy) > 0.0f)) {
          qtApplyFpsLookDelta(qtPendingFpsLookDx, qtPendingFpsLookDy);
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
        }
        if (std::fabs(qtPendingOrbitDx) > 0.0f || std::fabs(qtPendingOrbitDy) > 0.0f) {
          qtApplyOrbitDrag(qtPendingOrbitDx, qtPendingOrbitDy);
          qtPendingOrbitDx = 0.0f;
          qtPendingOrbitDy = 0.0f;
        }
        if (std::fabs(qtPendingPanDx) > 0.0f || std::fabs(qtPendingPanDy) > 0.0f) {
          qtApplyPanDrag(qtPendingPanDx, qtPendingPanDy);
          qtPendingPanDx = 0.0f;
          qtPendingPanDy = 0.0f;
        }
        const bool qtCoalesceBackgroundPublishesThisFrame =
            qtUseBg && qtRenderCoordinator && (qtIsPlayMode() || qtFpsMode);
        qtDeferBackgroundSnapshotPublishes = qtCoalesceBackgroundPublishesThisFrame;
        qtApplyContinuousOrbitCameraMovement(qtInputDt);
        const auto _vkp_after_orbit = std::chrono::steady_clock::now();
        qtApplyContinuousFpsMovement(qtInputDt);
        const auto _vkp_after_fps = std::chrono::steady_clock::now();
        const bool qtScriptDispatched = qtApplyGameModeScripts(qtFrameStart, qtFrameCount, qtInputDt);
        const auto _vkp_after_scripts = std::chrono::steady_clock::now();
        if (qtScriptDispatched && qtScriptsRunning() && !qtFpsMode) {
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
        }
        // Phase 5 RAG05+: tick animation sampler + ragdoll activation
        // handling against the cached script runtime world. Cheap when no
        // skeletal entities are present (early-outs inside tick_animations).
        if (qtScriptsRunning()) {
          qtTickAnimationsAndRagdolls(qtInputDt, qtFrameCount);
        }
        qtApplyPhysicsSimulation(qtFrameStart);
        const auto qtAfterPhysics = std::chrono::steady_clock::now();
        qtFlushDeferredBackgroundSnapshot();
        qtDeferBackgroundSnapshotPublishes = false;
        const auto qtAfterSnapshotFlush = std::chrono::steady_clock::now();
        VKP_METRIC_OBSERVE("vkp.qt.fps_movement_us",
            std::chrono::duration_cast<std::chrono::microseconds>(_vkp_after_fps - _vkp_after_orbit).count());
        VKP_METRIC_OBSERVE("vkp.qt.scripts_us",
            std::chrono::duration_cast<std::chrono::microseconds>(_vkp_after_scripts - _vkp_after_fps).count());
        VKP_METRIC_OBSERVE("vkp.qt.physics_apply_us",
            std::chrono::duration_cast<std::chrono::microseconds>(qtAfterPhysics - _vkp_after_scripts).count());
        VKP_METRIC_OBSERVE("vkp.qt.snapshot_flush_us",
            std::chrono::duration_cast<std::chrono::microseconds>(qtAfterSnapshotFlush - qtAfterPhysics).count());
        if (qtAudioSystem) {
          const auto audioForward = qtCameraForward();
          vkpt::audio::AudioListenerState listener;
          listener.position = {qtCameraPose.position.x, qtCameraPose.position.y, qtCameraPose.position.z};
          listener.forward = {audioForward.x, audioForward.y, audioForward.z};
          listener.up = {qtCameraPose.up.x, qtCameraPose.up.y, qtCameraPose.up.z};
          qtAudioSystem->set_listener(listener);
          qtAudioSystem->update();
        }
        if (!qtGizmoDrag.active) {
          qtRefreshViewportPickablesIfSnapshotChanged();
        }
        updateQtSelectionOverlay();
        const auto qtAfterSelectionOverlay = std::chrono::steady_clock::now();
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
              qtPublishRenderSnapshotForBackground(false, false, true, false);
            } else {
              // Synchronous (GPU) path — update directly on main thread.
              bool camOk = qtTracer->update_camera(
                  qtScene.camera_position, qtScene.camera_target,
                  qtScene.camera_up, qtScene.camera_fov_deg);
              if (!camOk) {
                camOk = qtTracer->load_scene_snapshot(qtScene) &&
                        qtTracer->build_or_update_acceleration();
              }
              const bool resetOk = camOk && qtTracer->reset_accumulation();
              if (camOk && resetOk) {
                (void)qtPublishRenderSnapshot(false, false, true, false);
              }
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
          const auto qtNow = std::chrono::steady_clock::now();
          if (qtNextFramebufferSubmit == std::chrono::steady_clock::time_point{}) {
            qtNextFramebufferSubmit = qtNow;
          }
          const bool qtCanSubmitFramebuffer = qtNow >= qtNextFramebufferSubmit;
          if (qtCanSubmitFramebuffer) {
            do {
              qtNextFramebufferSubmit += qtFramebufferDisplayInterval;
            } while (qtNextFramebufferSubmit <= qtNow);
            if (auto frame = qtRenderCoordinator->acquire_latest_frame()) {
              qtPublishedSample.store(frame->sample_count, std::memory_order_relaxed);
              qtPublishedWidth.store(frame->width, std::memory_order_relaxed);
              qtPublishedHeight.store(frame->height, std::memory_order_relaxed);
              qtPublishedRays.store(frame->counters.rays, std::memory_order_relaxed);
#ifdef PT_ENABLE_QT
              if (qtWindow != nullptr) {
                const auto frameGeneration = frame->generation;
                qtWindow->set_framebuffer_rgba(std::move(frame->rgba8),
                                                frame->width,
                                                frame->height,
                                                frameGeneration);
                if (qtOverlayRequiredRenderGeneration != 0u &&
                    frameGeneration >= qtOverlayRequiredRenderGeneration) {
                  updateQtSelectionOverlay();
                }
              }
#endif
            }
          }
        } else if (!qtUseBg && qtTracerReady) {
          bool renderedThisTick = false;
          qtLastGpuBatchesPerTick = 0u;
          while (qtTracerReady && qtLastGpuBatchesPerTick < kQtMaxGpuBatchesPerTick) {
            const auto now = std::chrono::steady_clock::now();
            const auto frameTarget = qtInteractiveFrameTarget();
            if (qtLastGpuBatchesPerTick > 0u) {
              if (now >= qtFrameStart + frameTarget) {
                break;
              }
              if (qtSmoothedGpuBatchMs > 0.0) {
                const auto estimatedNextBatch =
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(qtSmoothedGpuBatchMs));
                if (now + estimatedNextBatch > qtFrameStart + frameTarget) {
                  break;
                }
              }
            }

#ifdef PT_ENABLE_QT
            const auto countersBefore = qtTracer->read_counters();
#endif
            const auto batchStart = std::chrono::steady_clock::now();
            vkpt::pathtracer::RenderTile tile;
            tile.x = 0u;
            tile.y = 0u;
            tile.width = qtSettings.width;
            tile.height = qtSettings.height;
            tile.sample_index = qtSampleIndex;
            if (!qtTracer->render_tile(tile, qtFrameCount)) {
              qtTracerReady = false;
              qtPreviewStatus = "render sample failed";
              std::cerr << "[qt] render_tile failed at sample " << qtSampleIndex << "\n";
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
        const auto qtAfterRender = std::chrono::steady_clock::now();

#ifdef PT_ENABLE_QT
        const double qtFrameWorkMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - qtFrameStart).count();
        qtLastUiFrameMs = qtInputDt > 0.0f
            ? static_cast<double>(qtInputDt) * 1000.0
            : qtFrameWorkMs;
        updateQtPreviewOverlay();
        const auto qtAfterUi = std::chrono::steady_clock::now();
        const double qtProfileCurrentFrameMs =
            std::chrono::duration<double, std::milli>(qtAfterUi - qtFrameStart).count();
        const double qtThisFramePollMs = std::chrono::duration<double, std::milli>(qtAfterPoll - qtFrameStart).count();
        const double qtThisFrameInputMs = std::chrono::duration<double, std::milli>(qtAfterInputHandling - qtAfterPoll).count();
        const double qtThisFramePhysicsMs = std::chrono::duration<double, std::milli>(qtAfterPhysics - qtAfterInputHandling).count();
        const double qtThisFrameSelectionMs = std::chrono::duration<double, std::milli>(qtAfterSelectionOverlay - qtAfterPhysics).count();
        const double qtThisFrameRenderMs = std::chrono::duration<double, std::milli>(qtAfterRender - qtAfterSelectionOverlay).count();
        const double qtThisFrameUiMs = std::chrono::duration<double, std::milli>(qtAfterUi - qtAfterRender).count();
        const auto qtRepaintUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(qtAfterUi - qtFrameStart).count());
        const double qtRepaintHz = qtProfileCurrentFrameMs > 0.0
            ? 1000.0 / qtProfileCurrentFrameMs
            : 0.0;
        vkpt::platform::RecordUiRepaint(qtRepaintUs, qtRepaintHz, qtThisFrameRenderMs + qtThisFrameUiMs);
        if (!qtInputEvents.empty()) {
          const auto inputToPixelUs = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(qtAfterUi - qtAfterPoll).count());
          VKP_METRIC_OBSERVE("vkp.ui.input_to_pixel_us", inputToPixelUs);
        }
        qtProfilePollMs += qtThisFramePollMs;
        qtProfileInputMs += qtThisFrameInputMs;
        qtProfilePhysicsMs += qtThisFramePhysicsMs;
        qtProfileSelectionMs += qtThisFrameSelectionMs;
        qtProfileRenderMs += qtThisFrameRenderMs;
        qtProfileUiMs += qtThisFrameUiMs;
        qtProfileFrameMs += qtProfileCurrentFrameMs;
        qtProfileMaxFrameMs = std::max(qtProfileMaxFrameMs, qtProfileCurrentFrameMs);
        ++qtProfileFrames;
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_poll_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFramePollMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_input_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFrameInputMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_physics_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFramePhysicsMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_selection_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFrameSelectionMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_render_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFrameRenderMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_ui_us",
            static_cast<std::uint64_t>(std::max(0.0, qtThisFrameUiMs * 1000.0)));
        VKP_METRIC_OBSERVE(
            "vkp.qt.loop_frame_work_us",
            static_cast<std::uint64_t>(std::max(0.0, qtProfileCurrentFrameMs * 1000.0)));
        // Live drag-fps diagnostic. While a gizmo drag is active, log the
        // per-phase breakdown for any frame that overruns 1.5x the target. The
        // user can drag once and the log will show which phase ate the budget
        // (poll = Qt event drain, input = gizmo math + transform publish,
        // selection = updateQtSelectionOverlay, render = frame acquire +
        // set_framebuffer_rgba, ui = updateQtPreviewOverlay).
        if (qtGizmoDrag.active) {
          const auto frameTargetMs =
              std::chrono::duration<double, std::milli>(qtInteractiveFrameTarget()).count();
          if (qtProfileCurrentFrameMs > frameTargetMs * 1.5) {
            logger.log(vkpt::log::Severity::Info, "app", "Qt drag frame profile", {
              {"frame", std::to_string(qtFrameCount)},
              {"frame_ms", std::to_string(qtProfileCurrentFrameMs)},
              {"target_ms", std::to_string(frameTargetMs)},
              {"poll_ms", std::to_string(qtThisFramePollMs)},
              {"input_ms", std::to_string(qtThisFrameInputMs)},
              {"physics_ms", std::to_string(qtThisFramePhysicsMs)},
              {"selection_ms", std::to_string(qtThisFrameSelectionMs)},
              {"render_ms", std::to_string(qtThisFrameRenderMs)},
              {"ui_ms", std::to_string(qtThisFrameUiMs)}
            });
          }
        }
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
        const auto frameTarget = qtInteractiveFrameTarget();
        if (qtFrameDuration < frameTarget) {
          const auto paceStart = std::chrono::steady_clock::now();
          SleepUntilFrameTarget(qtFrameStart + frameTarget);
          const double qtPaceWaitMs =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - paceStart).count();
          qtProfilePaceWaitMs += qtPaceWaitMs;
          VKP_METRIC_OBSERVE(
              "vkp.qt.loop_pace_wait_us",
              static_cast<std::uint64_t>(std::max(0.0, qtPaceWaitMs * 1000.0)));
        } else {
          ++qtProfileLateFrames;
          const double qtLateMs =
              std::chrono::duration<double, std::milli>(qtFrameDuration - frameTarget).count();
          qtProfileMaxLateMs = std::max(qtProfileMaxLateMs, qtLateMs);
          VKP_METRIC_OBSERVE(
              "vkp.qt.loop_late_us",
              static_cast<std::uint64_t>(std::max(0.0, qtLateMs * 1000.0)));
        }
        // Pace the UI/event loop so input and overlay updates do not busy-spin.
      }

      if (qtProfileFrames > 0u) {
        const double invFrames = 1.0 / static_cast<double>(qtProfileFrames);
        const double elapsedLoopMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - qtMeasuredLoopStart).count();
        logger.log(vkpt::log::Severity::Info, "app", "Qt render loop frame profile", {
          {"frames", std::to_string(qtProfileFrames)},
          {"elapsed_loop_ms", std::to_string(elapsedLoopMs)},
          {"avg_total_frame_ms", std::to_string(elapsedLoopMs * invFrames)},
          {"avg_frame_ms", std::to_string(qtProfileFrameMs * invFrames)},
          {"max_frame_ms", std::to_string(qtProfileMaxFrameMs)},
          {"avg_poll_ms", std::to_string(qtProfilePollMs * invFrames)},
          {"avg_input_ms", std::to_string(qtProfileInputMs * invFrames)},
          {"avg_physics_ms", std::to_string(qtProfilePhysicsMs * invFrames)},
          {"avg_selection_ms", std::to_string(qtProfileSelectionMs * invFrames)},
          {"avg_render_ms", std::to_string(qtProfileRenderMs * invFrames)},
          {"avg_ui_ms", std::to_string(qtProfileUiMs * invFrames)},
          {"avg_pace_wait_ms", std::to_string(qtProfilePaceWaitMs * invFrames)},
          {"late_frames", std::to_string(qtProfileLateFrames)},
          {"max_late_ms", std::to_string(qtProfileMaxLateMs)},
          {"canvas_fps", std::to_string(qtCanvasFps)},
          {"canvas_frame_ms", std::to_string(qtCanvasFrameMs)},
          {"gizmo_drag_input_samples", std::to_string(qtGizmoDragInputSamples)},
          {"gizmo_transform_immediate_publishes", std::to_string(qtGizmoTransformImmediatePublishes)},
          {"gizmo_transform_published_entities", std::to_string(qtGizmoTransformPublishedEntityCount)},
          {"gizmo_transform_publish_failures", std::to_string(qtGizmoTransformPublishFailures)},
          {"overlay_required_generation", std::to_string(qtOverlayRequiredRenderGeneration)},
          {"overlay_stale_skips", std::to_string(qtOverlayStaleSkips)}
        });
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
                      << " window_painted=" << qtStats.painted
                      << " canvas_fps=" << qtCanvasFps
                      << " canvas_frame_ms=" << qtCanvasFrameMs
                      << " gizmo_drag_input_samples=" << qtGizmoDragInputSamples
                      << " gizmo_transform_immediate_publishes=" << qtGizmoTransformImmediatePublishes
                      << " gizmo_transform_publish_failures=" << qtGizmoTransformPublishFailures
                      << " latest_presented_generation=" << qtStats.latestPresentedGeneration
                      << " latest_published_generation=" << qtStats.latestPublishedGeneration
                      << " window_dropped=" << qtStats.dropped;
        updateStatusFile([&](auto& fileStatus) {
          fileStatus.performance_summary = qtPerfSummary.str();
        });
        logger.log(vkpt::log::Severity::Info, "app", "Qt preview shutdown metadata", {
          {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
          {"render_published", std::to_string(qtPublishedFrames.load(std::memory_order_relaxed))},
          {"render_dropped", std::to_string(qtDroppedFrames.load(std::memory_order_relaxed))},
          {"window_received", std::to_string(qtStats.received)},
          {"window_presented", std::to_string(qtStats.presented)},
          {"window_painted", std::to_string(qtStats.painted)},
          {"window_dropped", std::to_string(qtStats.dropped)},
          {"latest_published_generation", std::to_string(qtStats.latestPublishedGeneration)},
          {"latest_presented_generation", std::to_string(qtStats.latestPresentedGeneration)},
          {"canvas_fps", std::to_string(qtCanvasFps)},
          {"canvas_frame_ms", std::to_string(qtCanvasFrameMs)},
          {"gizmo_drag_input_samples", std::to_string(qtGizmoDragInputSamples)},
          {"gizmo_transform_immediate_publishes", std::to_string(qtGizmoTransformImmediatePublishes)},
          {"gizmo_transform_published_entities", std::to_string(qtGizmoTransformPublishedEntityCount)},
          {"gizmo_transform_publish_failures", std::to_string(qtGizmoTransformPublishFailures)},
          {"overlay_required_generation", std::to_string(qtOverlayRequiredRenderGeneration)},
          {"overlay_stale_skips", std::to_string(qtOverlayStaleSkips)},
          {"latest_published_id", std::to_string(qtStats.latestPublishedId)},
          {"latest_presented_id", std::to_string(qtStats.latestPresentedId)}
        });
      }
      DrainQtQueuedWork();
#endif
      BootStep("qt render loop exited; shutting down platform");
      vkpt::audio::SetGlobalAudioSystem(nullptr);
      if (qtAudioSystem) {
        qtAudioSystem->shutdown();
      }

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
