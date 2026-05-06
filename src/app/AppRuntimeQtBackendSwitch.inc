      auto qtResetPublishedRenderState = [&]() {
        qtSampleIndex = 0u;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedWidth.store(qtSettings.width, std::memory_order_relaxed);
        qtPublishedHeight.store(qtSettings.height, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtPublishedFrames.store(0u, std::memory_order_relaxed);
        qtDroppedFrames.store(0u, std::memory_order_relaxed);
      };

      auto qtShutdownActiveRenderer = [&]() {
        if (qtRenderCoordinator) {
          qtRenderCoordinator->stop();
          qtRenderCoordinator.reset();
        }
        if (qtTracer) {
          qtTracer->shutdown();
          qtTracer.reset();
        }
      };

      auto qtConfigureTracerForCurrentScene =
          [&](vkpt::pathtracer::IPathTracer& tracer) {
        return tracer.configure(qtSettings) &&
               tracer.load_scene_snapshot(qtScene) &&
               tracer.build_or_update_acceleration() &&
               tracer.reset_accumulation();
      };

      std::function<bool(std::string_view, std::string_view)> qtSwitchRuntimeBackend;
      qtSwitchRuntimeBackend = [&](std::string_view requestedBackend,
                                   std::string_view reason) {
        const std::string normalized = qtNormalizeRuntimeBackend(requestedBackend);
        if (normalized.empty()) {
          ui_runtime_state.status_message = "backend switch failed: empty backend";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          return false;
        }

        const std::string current = qtNormalizeRuntimeBackend(config.backend.value);
        if (normalized == current && qtTracerReady) {
          ui_runtime_state.status_message = "backend already active: " + normalized;
          ui_runtime_state.last_warning_or_error.clear();
          qtDockPanelsDirty = true;
          return true;
        }

        auto next = qtCreateRuntimeTracer(normalized);
        if (!next.tracer) {
          ui_runtime_state.status_message =
              "backend switch failed: " + (next.error.empty() ? normalized : next.error);
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          logger.log(vkpt::log::Severity::Warning,
                     "app",
                     "Qt runtime backend switch failed before teardown",
                     {{"requested_backend", normalized},
                      {"reason", next.error}});
          return false;
        }

        if (!qtConfigureTracerForCurrentScene(*next.tracer)) {
          next.tracer->shutdown();
          ui_runtime_state.status_message = "backend switch failed: renderer initialization";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          logger.log(vkpt::log::Severity::Warning,
                     "app",
                     "Qt runtime backend switch failed during renderer initialization",
                     {{"requested_backend", normalized},
                      {"renderer_path", next.renderer_path}});
          return false;
        }

        qtShutdownActiveRenderer();

        config.backend = {next.requested_backend, vkpt::config::ConfigSource::CliFlag};
        qtRendererPath = next.renderer_path;
        qtUseBg = next.background;
        qtEnableAutoOrbit = (windowFrameLimit == 0u) && !qtUseBg && !qtPhysicsRuntimeEnabled;
        qtTracerReady = true;
        qtResetPublishedRenderState();

        if (qtUseBg) {
          vkpt::render::RenderCoordinatorConfig coordinatorConfig{};
          coordinatorConfig.publish_hz = qtPreviewPublishHz;
          coordinatorConfig.immediate_publish_count = kQtPreviewImmediatePublishes;
          qtRenderCoordinator = std::make_unique<vkpt::render::RenderCoordinator>(
              std::move(next.tracer),
              qtSettings,
              qtScene,
              coordinatorConfig);
          if (!qtRenderCoordinator->start()) {
            qtRenderCoordinator.reset();
            qtTracerReady = false;
          }
        } else {
          qtTracer = std::move(next.tracer);
        }

        qtRefreshDeviceStats();
        qtPreviewStatus = qtTracerReady
            ? "backend switched: " + config.backend.value
            : "backend switch failed: coordinator start";
        ui_runtime_state.active_renderer_backend = config.backend.value;
        ui_runtime_state.active_renderer_path = qtRendererPath;
        ui_runtime_state.background_job_count = qtUseBg ? 1u : 0u;
        ui_runtime_state.status_message = std::string("runtime backend ") +
            (qtTracerReady ? "switched" : "failed") +
            (reason.empty() ? std::string{} : " (" + std::string(reason) + ")");
        if (qtTracerReady) {
          ui_runtime_state.last_warning_or_error.clear();
        } else {
          ui_runtime_state.last_warning_or_error = qtPreviewStatus;
        }
        qtDockPanelsDirty = true;
        logger.log(qtTracerReady ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   "Qt runtime backend switched",
                   {{"requested_backend", config.backend.value},
                    {"renderer_path", qtRendererPath},
                    {"background", qtUseBg ? "true" : "false"},
                    {"ready", qtTracerReady ? "true" : "false"}});
        return qtTracerReady;
      };
