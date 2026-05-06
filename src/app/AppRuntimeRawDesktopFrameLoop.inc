    // ---- Camera orbit state ---------------------------------------------------
    // Orbit center = bounding-box center of scene geometry so the camera always
    // looks at the actual scene content regardless of the camera_target stored
    // in the scene file (which may just be "1 unit in front of camera").
    const float kOrbitDegPerSec = 7.5f;
    const float kOrbitMinStepDeg = 0.1f;  // min angle change before reset
    vkpt::pathtracer::Vec3 orbitCenter{};
    {
      float bminX = FLT_MAX, bminZ = FLT_MAX;
      float bmaxX = -FLT_MAX, bmaxZ = -FLT_MAX;
      float bminY = FLT_MAX, bmaxY = -FLT_MAX;
      for (const auto& v : previewScene.vertices) {
        bminX = std::min(bminX, v.x); bmaxX = std::max(bmaxX, v.x);
        bminY = std::min(bminY, v.y); bmaxY = std::max(bmaxY, v.y);
        bminZ = std::min(bminZ, v.z); bmaxZ = std::max(bmaxZ, v.z);
      }
      if (bminX < bmaxX) {
        orbitCenter.x = (bminX + bmaxX) * 0.5f;
        orbitCenter.y = (bminY + bmaxY) * 0.5f;
        orbitCenter.z = (bminZ + bmaxZ) * 0.5f;
      } else {
        orbitCenter = previewScene.camera_target;
      }
    }
    const float orbitDx = previewScene.camera_position.x - orbitCenter.x;
    const float orbitDz = previewScene.camera_position.z - orbitCenter.z;
    const float kOrbitRadius = std::sqrt(orbitDx * orbitDx + orbitDz * orbitDz);
    const float orbitInitialAngleDeg = std::atan2(orbitDx, orbitDz) * (180.0f / 3.14159265f);
    float orbitLastAngleDeg = orbitInitialAngleDeg;
    const auto orbitStartTime = std::chrono::steady_clock::now();
    std::cout << "[orbit] setup: center=(" << orbitCenter.x << "," << orbitCenter.y << "," << orbitCenter.z
              << ") radius=" << kOrbitRadius
              << " initial_angle=" << orbitInitialAngleDeg << "\n";

    auto frame = static_cast<vkpt::core::FrameIndex>(0);
    bool running = true;
    std::size_t loggedStartupFrames = 0;
    auto perfPrevTime = std::chrono::steady_clock::now();
    std::uint64_t perfPrevRays = 0u;
    std::deque<double> perfRpsWindow;
    double perfRpsWindowSum = 0.0;
    constexpr std::size_t kPerfWindowSize = 12u;
    constexpr double kRpsBenchmarkTarget = 30.0e6;
    std::string titlePerfText = "rays/s: 0";
    while (running && window->is_open()) {
      const auto frameStart = std::chrono::steady_clock::now();

      // ---- Camera orbit update -------------------------------------------
      if (previewTracerReady &&
          ((useBgRender && bgRenderCoordinator) || previewTracer) &&
          kOrbitRadius > 1e-4f) {
        const float elapsedSec = std::chrono::duration<float>(
            frameStart - orbitStartTime).count();
        const float rawAngleDeg = orbitInitialAngleDeg + elapsedSec * kOrbitDegPerSec;
        const float angleDeg = std::fmod(rawAngleDeg, 360.0f);
        const float angleDiff = std::fabs(angleDeg - orbitLastAngleDeg);
        const float wrappedDiff = std::min(angleDiff, 360.0f - angleDiff);
        if (wrappedDiff >= kOrbitMinStepDeg) {
          const float rad = angleDeg * (3.14159265f / 180.0f);
          previewScene.camera_position.x = orbitCenter.x + kOrbitRadius * std::sin(rad);
          previewScene.camera_position.z = orbitCenter.z + kOrbitRadius * std::cos(rad);
          // Camera always looks at the orbit center.
          previewScene.camera_target = orbitCenter;
          if (useBgRender && bgRenderCoordinator) {
            bgRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
                previewScene.camera_position,
                previewScene.camera_target,
                previewScene.camera_up,
                previewScene.camera_fov_deg});
          } else {
            // Use lightweight camera update so geometry upload state is preserved.
            // Fall back to full load_scene_snapshot only if update_camera is unsupported.
            const bool camOk = previewTracer->update_camera(
                previewScene.camera_position,
                previewScene.camera_target,
                previewScene.camera_up,
                previewScene.camera_fov_deg);
            if (!camOk) {
              previewTracer->load_scene_snapshot(previewScene);
              previewTracer->build_or_update_acceleration();
            }
            previewTracer->reset_accumulation();
          }
          previewSampleIndex = 0u;
          orbitLastAngleDeg = angleDeg;
          std::cout << "[orbit] angle=" << angleDeg
                    << " pos=(" << previewScene.camera_position.x
                    << "," << previewScene.camera_position.y
                    << "," << previewScene.camera_position.z
                    << ") elapsed=" << elapsedSec << "s\n";
        }
      }

      // ---- Render dispatch -----------------------------------------------
      // TiledCpuPathTracer runs behind RenderCoordinator to keep the Win32
      // message pump unblocked. The main loop only consumes latest-wins frames.
      // ScalarCpuPathTracer keeps the original incremental pixel-batch path.
      if (useBgRender && bgRenderCoordinator) {
        const auto coordinatorStats = bgRenderCoordinator->stats();
        if (coordinatorStats.failed) {
          previewTracerReady = false;
          previewError = coordinatorStats.error.empty()
              ? "window preview background render failed"
              : coordinatorStats.error;
          logger.log(vkpt::log::Severity::Error,
                     "traceprobe",
                     previewError);
        }
        if (auto bgFrame = bgRenderCoordinator->acquire_latest_frame()) {
          desktopWindow->set_framebuffer_rgba(bgFrame->rgba8, bgFrame->width, bgFrame->height);
          previewRendered  = true;
          previewNonBlack  = false;
          for (std::size_t i = 0; i + 3u < bgFrame->rgba8.size(); i += 4u) {
            if (bgFrame->rgba8[i] || bgFrame->rgba8[i + 1u] || bgFrame->rgba8[i + 2u]) {
              previewNonBlack = true;
              break;
            }
          }
          previewSampleIndex = bgFrame->sample_count;
          // Log every 16 samples to show convergence progress
          if (previewSampleIndex == 1u || (previewSampleIndex % 16u) == 0u) {
            std::cout << "[ui] preview accumulate: samples=" << previewSampleIndex
                      << " non_black=" << (previewNonBlack ? "yes" : "no") << "\n";
            logger.log(vkpt::log::Severity::Info, "traceprobe",
                       "window preview accumulate",
                       {
                         {"samples",   std::to_string(previewSampleIndex)},
                         {"rendered",  previewRendered ? "yes" : "no"},
                         {"non_black", previewNonBlack ? "yes" : "no"}
                       },
                       frame);
          }
        }
      } else if (windowFrameLimit == 0u && previewTracerReady && previewSampleIndex < previewSettings.spp) {
        // Original ScalarCpuPathTracer incremental pixel-batch path.
        const auto traceFrameStart = std::chrono::steady_clock::now();
        bool tracedAnyPixelsThisFrame = false;
        bool completedSampleThisFrame = false;
        uint32_t completedSampleCount = 0u;
        uint32_t completedSampleNumber = 0u;

        while (previewTracerReady && previewSampleIndex < previewSettings.spp) {
          const auto elapsed = std::chrono::steady_clock::now() - traceFrameStart;
          if (elapsed >= previewTraceBudgetPerFrame) {
            break;
          }

          bool render_ok = false;
          if (auto* scalar_tracer = dynamic_cast<vkpt::pathtracer::ScalarCpuPathTracer*>(previewTracer.get())) {
            const uint32_t remainingPixels = previewPixelCount - previewPixelCursor;
            const uint32_t batchCount = std::min(previewPixelsPerBatch, remainingPixels);
            render_ok = scalar_tracer->render_sample_pixels(previewPixelOrder.data() + previewPixelCursor,
                                                            batchCount,
                                                            previewSampleIndex,
                                                            frame);
            if (render_ok) {
              tracedAnyPixelsThisFrame = true;
              ++previewChunkCounter;
              previewPixelCursor += batchCount;

              if (previewPixelCursor >= previewPixelCount) {
                previewPixelCursor = 0u;
                ++previewSampleIndex;
                completedSampleThisFrame = true;
                ++completedSampleCount;
                completedSampleNumber = previewSampleIndex;
                if (previewSampleIndex < previewSettings.spp) {
                  resetPreviewPixelOrder(previewSampleIndex);
                }
              }
            }
          } else {
            render_ok = previewTracer->render_sample_batch(0, previewSettings.height, previewSampleIndex, frame);
            if (render_ok) {
              tracedAnyPixelsThisFrame = true;
              ++previewSampleIndex;
              completedSampleThisFrame = true;
              ++completedSampleCount;
              completedSampleNumber = previewSampleIndex;
            }
          }

          if (!render_ok) {
            previewTracerReady = false;
            previewError = "window preview sample failed";
            std::string sampleFailureReason;
            bool d3d12Failure = false;
#ifdef PT_ENABLE_D3D12
            if (const auto* d3d12Tracer =
                    dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(previewTracer.get())) {
              sampleFailureReason = d3d12Tracer->last_error();
              d3d12Failure = true;
            }
#endif
            const auto counters = previewTracer->read_counters();
            logger.log(vkpt::log::Severity::Error, "traceprobe",
                       "window preview sample failed",
                       {
                         {"sample", std::to_string(previewSampleIndex)},
                         {"frame", std::to_string(frame)},
                         {"accum_samples", std::to_string(counters.samples)},
                         {"accum_rays", std::to_string(counters.rays)},
                         {"error", sampleFailureReason.empty() ? "unavailable" : sampleFailureReason}
                       },
                       frame);
            std::cout << "[ui] preview sample failed: sample=" << previewSampleIndex
                      << " frame=" << frame
                      << " accum_samples=" << counters.samples
                      << " accum_rays=" << counters.rays
                      << " error=" << (sampleFailureReason.empty() ? "unavailable" : sampleFailureReason)
                      << "\n";
            if (d3d12Failure && previewD3D12RetryCount < 2u) {
              if (previewD3D12RetryCount < 2u && previewTracer->reset_accumulation()) {
                std::cout << "[ui] preview d3d12 sample failed; retrying sample 0\n";
                logger.log(vkpt::log::Severity::Warning, "traceprobe",
                           "window preview d3d12 sample retry",
                           {
                             {"attempt", std::to_string(previewD3D12RetryCount + 1u)},
                             {"accum_samples", std::to_string(counters.samples)},
                             {"accum_rays", std::to_string(counters.rays)}
                           },
                           frame);
                ++previewD3D12RetryCount;
                previewSampleIndex = 0u;
                previewPixelCursor = 0u;
                resetPreviewPixelOrder(previewSampleIndex);
                previewError.clear();
                previewTracerReady = true;
                continue;
              }
            }
            break;
          }
        }

        if (previewTracerReady && tracedAnyPixelsThisFrame) {
          const auto ldr = previewTracer->resolve_ldr();
          desktopWindow->set_framebuffer_rgba(ldr.rgba8, ldr.width, ldr.height);
          previewRendered = true;
          previewNonBlack = false;
          std::uint8_t rgbMax = 0u;
          std::uint64_t rgbSum = 0u;
          std::uint64_t rgbCount = 0u;
          for (std::size_t i = 0; i + 3u < ldr.rgba8.size(); i += 4u) {
            const std::uint8_t r = ldr.rgba8[i + 0u];
            const std::uint8_t g = ldr.rgba8[i + 1u];
            const std::uint8_t b = ldr.rgba8[i + 2u];
            rgbMax = std::max(rgbMax, std::max(r, std::max(g, b)));
            rgbSum += static_cast<std::uint64_t>(r) + static_cast<std::uint64_t>(g) + static_cast<std::uint64_t>(b);
            rgbCount += 3u;
            if (r != 0u || g != 0u || b != 0u) {
              previewNonBlack = true;
            }
          }
          const float rgbAvg = (rgbCount == 0u) ? 0.0f
              : static_cast<float>(rgbSum) / static_cast<float>(rgbCount);
          if (completedSampleThisFrame &&
              (completedSampleNumber == 1u || (completedSampleNumber % 16u) == 0u)) {
            std::cout << "[ui] preview accumulate: samples=" << completedSampleNumber
                      << " non_black=" << (previewNonBlack ? "yes" : "no") << "\n";
            logger.log(vkpt::log::Severity::Info, "traceprobe",
                       "window preview accumulate",
                       {
                         {"samples",   std::to_string(completedSampleNumber)},
                         {"samples_completed_this_frame", std::to_string(completedSampleCount)},
                         {"chunks_traced_total", std::to_string(previewChunkCounter)},
                         {"rendered",   previewRendered ? "yes" : "no"},
                         {"rgb_max",    std::to_string(static_cast<unsigned>(rgbMax))},
                         {"rgb_avg",    std::to_string(rgbAvg)},
                         {"non_black",  previewNonBlack ? "yes" : "no"}
                       },
                       frame);
          }
        }
      }

      window->poll_events();
      const auto events = desktopWindow->drain_events();
      if (loggedStartupFrames < 6) {
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string("window loop frame=") + std::to_string(frame) +
                     " open=" + (window->is_open() ? "true" : "false") +
                     " events=" + std::to_string(events.size()));
      }
      bool closeRequested = false;
      for (const auto& e : events) {
        if (loggedStartupFrames < 3) {
          std::cout << "[ui] event: " << ToString(e.type)
                    << " x=" << e.x << " y=" << e.y
                    << " raw=" << e.raw_code
                    << " dx=" << e.delta_x << " dy=" << e.delta_y << "\n";
        }
        LogWindowInput(ui_event_log, ui_runtime_state, e, frame);

        const auto logMenuCommand = [&](const std::string& action_id,
                                       const vkpt::editor::EditorCommand& menuCommand,
                                       const std::string& status_text) {
          auto logged = menuCommand;
          logged.command_id = action_id;
          logged.frame_index = frame;
          if (logged.source_widget.empty()) {
            logged.source_widget = "menu";
          }
          ui_command_history.push(logged);
          ui_runtime_state.status_message = status_text;
          ui_runtime_state.last_menu_action = action_id;
          ui_runtime_state.focused_panel = logged.source_widget;
          PushUiEvent(ui_event_log, "app_action", logged.source_widget, "menu",
                      frame, {}, status_text, status_text);
          UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                         ui_event_log, ui_command_history);
        };

        switch (e.type) {
          case vkpt::platform::InputEventType::MenuCommand: {
            const auto nativeCommandId = static_cast<NativeMenuId>(e.raw_code);
            auto mappedAction = menuCommandLookup.find(nativeCommandId);
            if (mappedAction == menuCommandLookup.end()) {
              auto unsupported = MakeUnsupportedUiCommand("app.menu.unknown_command", "unknown menu command id");
              logMenuCommand(std::string("app.menu.unknown_command") + "_" + std::to_string(nativeCommandId),
                             unsupported,
                             std::string("unknown menu action: ") + std::to_string(nativeCommandId));
              break;
            }

            const std::string actionId = mappedAction->second;
            auto menuAction = vkpt::editor::MakeMenuCommand(actionId, "menu", frame);
            menuAction.command_id = actionId;

            if (actionId == "file.exit") {
              closeRequested = true;
              logMenuCommand(actionId,
                             MakeUnsupportedUiCommand("file.exit", "user selected Exit"),
                             "menu action executed: file.exit");
              break;
            }

            if (actionId == "benchmark.open_artifacts" ||
                actionId == "benchmark.history") {
              const std::string target = "artifacts/benchmarks";
              std::string menuStatus = "menu action failed: benchmark folder open failed";
              if (OpenPathInExplorer(target)) {
                menuStatus = std::string("menu action opened: ") + target;
              }
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            if (actionId == "benchmark.export_csv_json") {
              auto latest = FindLatestBenchmarkResultJson();
              std::string menuStatus;
              bool opened = false;
              if (latest && std::filesystem::exists(*latest)) {
                const auto csvPath = latest->parent_path() / "results.csv";
                if (std::filesystem::exists(csvPath)) {
                  opened = OpenPathInExplorer(csvPath);
                  menuStatus = opened ? "menu action opened: " + csvPath.string()
                                  : "menu action failed: unable to open results.csv";
                } else {
                  opened = OpenPathInExplorer(latest->parent_path());
                  menuStatus = opened ? "menu action opened: " + latest->parent_path().string()
                                  : "menu action failed: no results.csv in latest benchmark result";
                }
              } else {
                opened = false;
                menuStatus = "menu action failed: no benchmark results found";
              }
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            if (menuAction.kind == vkpt::editor::EditorCommandKind::kRunBenchmark &&
                std::holds_alternative<vkpt::editor::RunBenchmarkCommand>(menuAction.payload)) {
              auto resolved = std::get<vkpt::editor::RunBenchmarkCommand>(menuAction.payload);
              resolved = ResolveBenchmarkCommand(resolved, actionId,
                                                ui_runtime_state.active_scene,
                                                config.scene_path.value);
              const auto artifactDir = MakeMenuActionArtifactPath(actionId);
              std::filesystem::create_directories(artifactDir);
              std::string resultPath;
              const bool ok = LaunchBenchmarkRun(resolved,
                                                resolved.desc.backend.empty()
                                                  ? std::string(BenchmarkRunBackendFromAction(actionId))
                                                  : resolved.desc.backend,
                                                resolved.desc.renderer_path.empty()
                                                  ? std::string(BenchmarkRendererFromBackend(
                                                    resolved.desc.backend))
                                                  : resolved.desc.renderer_path,
                                                resolved.desc.scene_path,
                                                artifactDir,
                                                &resultPath);
              std::string menuStatus = "menu action executed: " + actionId;
              if (!std::filesystem::exists(resolved.desc.scene_path)) {
                menuStatus = "menu action failed: benchmark scene not found: " +
                         resolved.desc.scene_path;
              } else if (!ok) {
                menuStatus = "menu action failed: benchmark command exited non-zero";
              } else {
                menuStatus = "menu action completed: " + actionId;
                ui_runtime_state.last_benchmark_command = actionId;
              }
              auto completion = menuAction;
              completion.payload = resolved;
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, completion, menuStatus);
              break;
            }

            if (menuAction.kind != vkpt::editor::EditorCommandKind::kUnsupportedUiAction) {
              const std::string menuStatus =
                  std::string("menu action routed: ") + vkpt::editor::ToString(menuAction.kind) +
                  " (command queued)";
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            std::string reason = "unsupported in menu runtime";
            if (std::holds_alternative<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload)) {
              reason = std::get<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload).reason;
            }
            std::string menuStatus =
                std::string("menu action unsupported: ") + actionId + " (" + reason + ")";
            logger.log(vkpt::log::Severity::Info, "app",
                       std::string("menu command received: ") + menuStatus);
            logMenuCommand(actionId, menuAction, menuStatus);
            break;
          }
          case vkpt::platform::InputEventType::WindowResize:
            if (loggedStartupFrames < 3) {
              std::cout << "[ui] applying resize from event: " << e.x << "x" << e.y << "\n";
            }
            if (e.x > 0.0f && e.y > 0.0f) {
              ApplyWindowMetricsToLayout(ui_layout_state,
                                        static_cast<std::size_t>(e.x),
                                        static_cast<std::size_t>(e.y));
              ui_runtime_state.status_message = "window resize";
              {
                auto cmd = MakeUnsupportedUiCommand("app.window.resize", "window resized",
                                                   "window", frame);
                cmd.command_id = "app.window.resize";
                ui_command_history.push(cmd);
              }
              SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
            } else {
              logger.log(vkpt::log::Severity::Warning, "app",
                         std::string("ignoring zero-size resize event: x=") +
                           std::to_string(static_cast<int>(e.x)) + " y=" + std::to_string(static_cast<int>(e.y)));
            }
            break;
          case vkpt::platform::InputEventType::FocusGained:
            ui_runtime_state.status_message = "window focus gained";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.focus_gained", "focus gained",
                                                 "window", frame);
              cmd.command_id = "app.window.focus_gained";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::FocusLost:
            ui_runtime_state.status_message = "window focus lost";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.focus_lost", "focus lost",
                                                 "window", frame);
              cmd.command_id = "app.window.focus_lost";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::CloseRequested:
            closeRequested = true;
            ui_runtime_state.last_menu_action = "app.window.close_requested";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.close_requested", "close requested",
                                                 "window", frame);
              cmd.command_id = "app.window.close_requested";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::KeyDown:
          case vkpt::platform::InputEventType::KeyUp:
            ui_runtime_state.last_clicked_entity = static_cast<vkpt::core::StableId>(e.raw_code);
            break;
          default:
            break;
        }
      }

      if (!events.empty()) {
        UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                       ui_event_log, ui_command_history);
      }
#ifdef _WIN32
      if (previewTracerReady && previewTracer && !useBgRender) {
        const auto counters = previewTracer->read_counters();
        const auto now = std::chrono::steady_clock::now();
        const double dtSec = std::chrono::duration<double>(now - perfPrevTime).count();
        if (dtSec > 0.0) {
          const std::uint64_t currRays = counters.rays;
          const std::uint64_t deltaRays = (currRays >= perfPrevRays) ? (currRays - perfPrevRays) : 0u;
          const double instantRps = static_cast<double>(deltaRays) / dtSec;
          perfRpsWindow.push_back(instantRps);
          perfRpsWindowSum += instantRps;
          if (perfRpsWindow.size() > kPerfWindowSize) {
            perfRpsWindowSum -= perfRpsWindow.front();
            perfRpsWindow.pop_front();
          }
          const double avgRps = (perfRpsWindow.empty())
              ? instantRps
              : (perfRpsWindowSum / static_cast<double>(perfRpsWindow.size()));
          const bool benchmarkPass = (avgRps >= kRpsBenchmarkTarget);
          titlePerfText = std::string("rays/s: ") + FormatThroughputKmb(avgRps)
              + "  target>30M:" + (benchmarkPass ? "PASS" : "FAIL");
          perfPrevRays = currRays;
          perfPrevTime = now;
        }
      }
      SetWindowFrameStatus(desktopWindow, rawTitleBase, ui_runtime_state, ui_layout_state, frame, titlePerfText);
      std::ostringstream statusText;
      statusText << "UI shell ready\n"
                 << "backend: " << ui_runtime_state.active_renderer_backend;
      if (!previewGpuName.empty()) {
        statusText << "  [" << previewGpuName << "]";
      }
      statusText << "\n"
                 << "scene: "   << ui_runtime_state.active_scene << "\n"
                 << "layout: "  << ui_runtime_state.active_layout_name << "\n"
                 << "path tracing: " << (previewTracerReady ? "on" : "failed")
                 << "  samples=" << previewSampleIndex << "\n"
                 << "image: " << (previewNonBlack ? "non-black" : "dark/empty") << "\n"
                 << "status: "  << ui_runtime_state.status_message << "\n"
                 << "events: "  << ui_event_log.events().size();
      desktopWindow->set_overlay_text(statusText.str());
#endif
      ++loggedStartupFrames;

      if (windowFrameLimit != 0u && static_cast<uint32_t>(frame + 1u) >= windowFrameLimit) {
        BootStep("raw gui smoke frame limit reached");
        closeRequested = true;
      }

      if (closeRequested) {
        window->close();
        running = false;
      } else {
        ++frame;
        // Sleep only the remaining time to hit the 60fps (16ms) frame budget.
        const auto frameEnd = std::chrono::steady_clock::now();
        const auto frameDuration = frameEnd - frameStart;
        const auto frameTarget = std::chrono::milliseconds(16);
        if (frameDuration < frameTarget) {
          std::this_thread::sleep_for(frameTarget - frameDuration);
        }
      }
    }

    // Stop the background coordinator before tearing down platform resources.
    if (bgRenderCoordinator) {
      bgRenderCoordinator->stop();
    }

    platform.shutdown();
    recordUiAction("app.window", "window", "window closed",
                   MakeUnsupportedUiCommand("app.window", "window_closed"));
    writeStatus("window_closed");
    return 0;
  }
