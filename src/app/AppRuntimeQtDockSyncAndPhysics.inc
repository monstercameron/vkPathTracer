      double qtCanvasFps = 0.0;
      double qtCanvasFrameMs = 0.0;
      std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
          qtCanvasRateSamples;
      auto qtLastStatusBarSync = std::chrono::steady_clock::time_point{};
      constexpr auto kQtCanvasRateWindow = std::chrono::milliseconds(1500);
      constexpr auto kQtCanvasRateMinWindow = std::chrono::milliseconds(750);
      constexpr auto kQtStatusBarRefreshInterval = std::chrono::milliseconds(250);
      constexpr auto kQtDockPanelLiveScriptRefreshInterval = std::chrono::milliseconds(100);
      constexpr auto kQtDockPanelDirtyRefreshInterval = std::chrono::milliseconds(500);
      constexpr auto kQtDockPanelIdleRefreshInterval = std::chrono::milliseconds(5000);
      auto qtUpdateCanvasFrameRate =
          [&](const vkpt::platform::QtFramebufferStats& windowStats,
              std::chrono::steady_clock::time_point now) {
        if (qtCanvasRateSamples.empty() ||
            qtCanvasRateSamples.back().second != windowStats.painted ||
            now - qtCanvasRateSamples.back().first >= kQtStatusBarRefreshInterval) {
          qtCanvasRateSamples.emplace_back(now, windowStats.painted);
        }

        while (qtCanvasRateSamples.size() > 2u &&
               now - qtCanvasRateSamples.front().first > kQtCanvasRateWindow) {
          qtCanvasRateSamples.pop_front();
        }

        if (qtCanvasRateSamples.size() < 2u) {
          return;
        }
        const auto base = qtCanvasRateSamples.front();
        const auto elapsed = now - base.first;
        if (elapsed < kQtCanvasRateMinWindow) {
          return;
        }

        const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
        if (elapsedSeconds <= 0.0) {
          return;
        }
        const std::uint64_t paintedDelta =
            windowStats.painted >= base.second
                ? windowStats.painted - base.second
                : 0u;
        qtCanvasFps = static_cast<double>(paintedDelta) / elapsedSeconds;
        qtCanvasFrameMs = qtCanvasFps > 0.0 ? 1000.0 / qtCanvasFps : 0.0;
      };
      auto qtBuildFrameStats = [&]() {
        QtDockFrameStats stats;
        const auto now = std::chrono::steady_clock::now();
        stats.sample_count = qtUseBg
            ? qtPublishedSample.load(std::memory_order_relaxed)
            : qtSampleIndex;
        stats.frame_width = qtPublishedWidth.load(std::memory_order_relaxed);
        stats.frame_height = qtPublishedHeight.load(std::memory_order_relaxed);
        if (qtWindow != nullptr) {
          const auto metrics = qtWindow->metrics();
          stats.canvas_width = static_cast<std::uint32_t>(std::max(0, metrics.width));
          stats.canvas_height = static_cast<std::uint32_t>(std::max(0, metrics.height));
          const auto imageRect = qtViewportImageRect();
          stats.displayed_image_width = static_cast<std::uint32_t>(
              std::max(0, static_cast<int>(std::round(imageRect.width))));
          stats.displayed_image_height = static_cast<std::uint32_t>(
              std::max(0, static_cast<int>(std::round(imageRect.height))));
        }
        stats.preview_publish_hz = qtPreviewPublishHz;
        stats.gpu_batches_per_tick = qtLastGpuBatchesPerTick;
        stats.gpu_batch_ms = qtSmoothedGpuBatchMs;
        stats.ui_frame_ms = qtLastUiFrameMs;
        stats.canvas_fps = qtCanvasFps;
        stats.canvas_frame_ms = qtCanvasFrameMs;
        stats.total_rays = qtPublishedRays.load(std::memory_order_relaxed);
        const auto computerMetric = qtComputerRayMetrics.update(
            "computer",
            "Computer",
            stats.total_rays,
            stats.sample_count,
            now);
        stats.instant_rays_per_second = computerMetric.instant_rays_per_second;
        stats.rolling_rays_per_second = computerMetric.rolling_rays_per_second;
        stats.accumulated_rays_per_second = computerMetric.accumulated_rays_per_second;
        const std::string activeDeviceKey = QtDockActiveRayDeviceKey(qtDeviceStats);
        QtDockRayDeviceMetric activeMetric;
        if (qtActiveRenderAverageRps > 0.0) {
          stats.instant_rays_per_second = qtActiveRenderInstantRps;
          stats.rolling_rays_per_second = qtActiveRenderRollingRps;
          stats.accumulated_rays_per_second = qtActiveRenderAverageRps;
          activeMetric.device_key = activeDeviceKey;
          activeMetric.device_name = QtDockActiveRayDeviceName(qtDeviceStats);
          activeMetric.sample_count = stats.sample_count;
          activeMetric.total_rays = stats.total_rays;
          activeMetric.instant_rays_per_second = qtActiveRenderInstantRps;
          activeMetric.rolling_rays_per_second = qtActiveRenderRollingRps;
          activeMetric.accumulated_rays_per_second = qtActiveRenderAverageRps;
          activeMetric.measured = true;
        } else {
          auto& activeAccumulator = qtRayMetricsByDevice[activeDeviceKey];
          activeMetric = activeAccumulator.update(
              activeDeviceKey,
              QtDockActiveRayDeviceName(qtDeviceStats),
              stats.total_rays,
              stats.sample_count,
              now);
        }
        qtDeviceStats.active_device_key = activeDeviceKey;
        QtDockUpsertRayMetric(qtDeviceStats.ray_metrics, std::move(activeMetric));
        stats.render_published = qtPublishedFrames.load(std::memory_order_relaxed);
        stats.render_dropped = qtDroppedFrames.load(std::memory_order_relaxed);
        stats.background_thread = qtUseBg;
        stats.tracer_ready = qtTracerReady;
        stats.preview_status = qtPreviewStatus;
        stats.render_mode = qtTracerReady
            ? (qtScriptsRunning()
                   ? (qtUseBg
                          ? std::string(qtIsPlayMode() ? "play (background thread)" : "live edit (background thread)")
                          : std::string(qtIsPlayMode() ? "play (event loop)" : "live edit (event loop)"))
                   : (qtUseBg ? std::string("editor (background thread)") : std::string("editor (event loop)")))
            : std::string("off");
        stats.publish_cap = qtUseBg
            ? (std::to_string(qtPreviewPublishHz) + " fps")
            : std::string("event loop");
        stats.camera_mode = qtIsPlayMode()
            ? std::string("play")
            : (qtIsLiveEditMode()
            ? std::string("live edit")
            : (qtFpsMode
            ? std::string("fps")
            : (qtEnableAutoOrbit ? std::string("orbit") : std::string("authored"))));
        stats.fps_player_grounded = qtFpsPlayer.grounded;
        stats.fps_player_crouching = qtFpsPlayer.crouching;
        stats.fps_player_running = qtFpsPlayer.running;
        stats.fps_player_speed = qtFpsPlayer.current_speed;
        stats.fps_player_eye_height = qtFpsPlayer.eye_height;
        if (qtWindow != nullptr) {
          const auto windowStats = qtWindow->framebuffer_stats();
          qtUpdateCanvasFrameRate(windowStats, now);
          stats.window_received = windowStats.received;
          stats.window_presented = windowStats.presented;
          stats.window_painted = windowStats.painted;
          stats.window_dropped = windowStats.dropped;
          stats.canvas_fps = qtCanvasFps;
          stats.canvas_frame_ms = qtCanvasFrameMs;
        }
        return stats;
      };
      auto qtBuildBenchmarkPanel = [&](const QtDockFrameStats& frameStats) {
        const std::string scenePath =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        const std::string rendererPath = qtRendererPath.empty()
            ? (qtUseBg ? std::string("cpu_tiled_background")
                       : (config.backend.value.empty() ? std::string("event_loop") : config.backend.value))
            : qtRendererPath;
        auto desc = vkpt::editor::MakeDefaultBenchmarkRunDesc(
            scenePath,
            config.backend.value.empty() ? "cpu" : config.backend.value,
            rendererPath,
            std::max<std::uint32_t>(1u, config.spp.value),
            std::max<std::uint32_t>(1u, config.max_depth.value),
            42u,
            std::max<std::uint32_t>(1u, frameStats.frame_width),
            std::max<std::uint32_t>(1u, frameStats.frame_height));
        vkpt::editor::BenchmarkRawMetricsModel raw;
        raw.fps = ui_runtime_state.fps;
        raw.frame_ms = ui_runtime_state.frame_ms;
        raw.spp_accumulated = frameStats.sample_count;
        raw.samples_per_second = frameStats.ui_frame_ms > 0.0
            ? (static_cast<double>(std::max<std::uint32_t>(1u, frameStats.sample_count)) * 1000.0 /
               frameStats.ui_frame_ms)
            : 0.0;
        raw.paths_per_second = raw.samples_per_second;
        raw.path_vertices_per_second = raw.samples_per_second *
            static_cast<double>(std::max<std::size_t>(1u, qtScene.indices.size() / 3u));
        raw.memory_estimate_bytes =
            static_cast<std::uint64_t>(qtScene.vertices.size() * sizeof(vkpt::pathtracer::Vec3)) +
            static_cast<std::uint64_t>(qtScene.indices.size() * sizeof(std::uint32_t)) +
            static_cast<std::uint64_t>(qtScene.materials.size() * sizeof(vkpt::pathtracer::RTMaterial));
        const auto workload = vkpt::editor::EstimateWorkloadComplexity(
            desc,
            static_cast<std::uint32_t>(qtScene.lights.size()),
            static_cast<std::uint64_t>(qtScene.indices.size() / 3u),
            static_cast<std::uint64_t>(qtScene.instances.size()),
            0u,
            false);
        const double expected = raw.samples_per_second > 0.0 ? raw.samples_per_second : 1.0;
        auto score = vkpt::editor::ComputeBenchmarkScore(
            raw.samples_per_second,
            expected,
            raw.samples_per_second,
            workload.normalized_cost_units,
            false);
        score.raw_paths_per_second = raw.paths_per_second;
        return vkpt::editor::BuildBenchmarkPanelModel(
            desc,
            raw,
            score,
            workload,
            "benchmarks/",
            "Live preview defaults; benchmark not running",
            frameStats.tracer_ready,
            frameStats.tracer_ready ? std::string_view{} : std::string_view{"tracer is not ready"});
      };
      auto qtApplyRuntimeFrameStats = [&](const QtDockFrameStats& frameStats) {
        ui_runtime_state.active_scene =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        ui_runtime_state.active_camera = qtActiveCameraObjectName ? qtActiveCameraObjectName() : "runtime camera";
        ui_runtime_state.active_renderer_backend = config.backend.value;
        ui_runtime_state.active_renderer_path = qtRendererPath.empty()
            ? (qtUseBg ? std::string("cpu_tiled_background") : config.backend.value)
            : qtRendererPath;
        ui_runtime_state.spp_accumulated = frameStats.sample_count;
        ui_runtime_state.frame_ms = frameStats.canvas_frame_ms;
        ui_runtime_state.fps = frameStats.canvas_fps;
        ui_runtime_state.background_job_count =
            (qtUseBg ? 1u : 0u) +
            ((qtSceneLoadJob && qtSceneLoadJob->active.load(std::memory_order_relaxed)) ? 1u : 0u);
      };
      auto syncQtStatusBar = [&](bool force) {
        if (qtWindow == nullptr) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            qtLastStatusBarSync != std::chrono::steady_clock::time_point{} &&
            now - qtLastStatusBarSync < kQtStatusBarRefreshInterval) {
          return;
        }
        const auto frameStats = qtBuildFrameStats();
        qtApplyRuntimeFrameStats(frameStats);
        const auto statusBar = vkpt::editor::BuildStatusBarModel(
            ui_runtime_state,
            ui_selection_state,
            nullptr);
        ApplyQtStatusBarToWindow(qtWindow, BuildQtStatusBarText(statusBar));
        qtLastStatusBarSync = now;
      };
      auto syncQtDockPanels = [&](bool force) {
        if (qtWindow == nullptr) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        // Skip dock panel rebuild during any rapid transform mutation —
        // gizmo drag, physics-driven motion (game-mode walking), or Lua
        // script motion. BuildQtDockPanels walks every entity in the scene
        // document (1000+ in the louvre scene) to populate scene tree,
        // properties, materials, lights, and diagnostics docks; that
        // single rebuild costs hundreds of milliseconds to seconds. While
        // mutation is in flight the rebuild's results are stale before
        // they're presented anyway. The qtLastTransformPublishNs timestamp
        // is bumped by qtPublishRenderSnapshot whenever transform_revision
        // increments; we treat anything within kQtDockPanelMutationGraceNs
        // as still-mutating and defer the sync. Status bar still refreshes
        // because it is O(1).
        constexpr std::uint64_t kQtDockPanelMutationGraceNs =
            200u * 1000000ull;  // 200ms
        const auto now_ns = vkpt::core::metrics::UiInputLatencyNowNs();
        const bool rapidMutationActive =
            qtGizmoDrag.active ||
            (qtLastTransformPublishNs != 0u &&
             now_ns > qtLastTransformPublishNs &&
             (now_ns - qtLastTransformPublishNs) < kQtDockPanelMutationGraceNs);
        if (!force && rapidMutationActive) {
          qtDockPanelsDirty = true;
          return;
        }
        if (!force && qtLastDockPanelSync != std::chrono::steady_clock::time_point{}) {
          auto minInterval = qtDockPanelsDirty
              ? kQtDockPanelDirtyRefreshInterval
              : kQtDockPanelIdleRefreshInterval;
          if (qtScriptsRunning()) {
            minInterval = std::min(minInterval, kQtDockPanelLiveScriptRefreshInterval);
          }
          if (now - qtLastDockPanelSync < minInterval) {
            return;
          }
        }
        const auto frameStats = qtBuildFrameStats();
        qtApplyRuntimeFrameStats(frameStats);
        const auto benchmarkPanel = qtBuildBenchmarkPanel(frameStats);
        const auto statusBar = vkpt::editor::BuildStatusBarModel(
            ui_runtime_state,
            ui_selection_state,
            &benchmarkPanel.score);
        ApplyQtStatusBarToWindow(qtWindow, BuildQtStatusBarText(statusBar));
        qtLastStatusBarSync = now;
        if (qtAudioSystem) {
          qtScriptState.audio = qtAudioSystem->diagnostics();
        } else {
          qtScriptState.audio = {};
        }
        qtLastDockPanels = BuildQtDockPanels(qtSceneDocument,
                                             qtScene,
                                             qtSettings,
                                             ui_runtime_state,
                                             ui_selection_state,
                                             ui_layout_state,
                                             benchmarkPanel,
                                             frameStats,
                                             qtDeviceStats,
                                             qtActiveCameraShotSlot,
                                             std::array<bool, 4>{
                                                 qtSavedCameraShots[0].valid,
                                                 qtSavedCameraShots[1].valid,
                                                 qtSavedCameraShots[2].valid,
                                                 qtSavedCameraShots[3].valid},
                                             &qtScriptState);
        ApplyQtDockPanelsToWindow(qtWindow, qtLastDockPanels);
        qtLastDockPanelSync = now;
        qtDockPanelsDirty = false;
      };
      auto updateQtWindowTitle = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        std::string nextTitle =
            BuildWindowRuntimeTitle(qtTitleBase, ui_runtime_state, ui_layout_state);
        if (nextTitle == qtLastWindowTitle) {
          return;
        }
        qtWindow->set_title(nextTitle);
        qtLastWindowTitle = std::move(nextTitle);
      };
      auto emitQtShellReadyMarker = [&]() {
        constexpr std::string_view requiredDockIds[] = {
            "scene_graph",
            "inspector",
            "materials",
            "lights",
            "camera",
            "render_settings",
            "diagnostics",
            "performance",
            "device",
        };
        std::ostringstream docks;
        for (std::size_t i = 0; i < qtLastDockPanels.size(); ++i) {
          if (i > 0u) {
            docks << ",";
          }
          docks << qtLastDockPanels[i].id;
        }
        bool requiredDocksPresent = true;
        for (const auto required : requiredDockIds) {
          const auto found = std::find_if(qtLastDockPanels.begin(),
                                          qtLastDockPanels.end(),
                                          [required](const QtDockPanelContent& panel) {
                                            return panel.id == required;
                                          });
          requiredDocksPresent = requiredDocksPresent && found != qtLastDockPanels.end();
        }
        const std::string message =
            "qt shell ready menu_bar=true status_bar=true dock_count=" +
            std::to_string(qtLastDockPanels.size()) +
            " required_docks=" + (requiredDocksPresent ? std::string("true") : std::string("false")) +
            " docks=" + docks.str();
        std::cout << message << "\n";
        logger.log(vkpt::log::Severity::Info, "app", message);
      };
      auto qtColorByte = [](float value) -> std::uint8_t {
        const float clamped = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(std::round(clamped * 255.0f));
      };
      auto qtBuildScriptOverlayPanels = [&]() {
        std::vector<vkpt::platform::QtScriptOverlayPanel> panels;
        panels.reserve(qtSceneDocument.entities.size());
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_ui_panel || !entity.ui_panel.enabled || !entity.ui_panel.visible) {
            continue;
          }
          const auto& source = entity.ui_panel;
          vkpt::platform::QtScriptOverlayPanel panel;
          panel.id = source.panel_id.empty() ? std::to_string(entity.id) : source.panel_id;
          panel.title = source.title;
          panel.anchor = source.anchor;
          panel.x = source.x;
          panel.y = source.y;
          panel.width = source.width;
          panel.height = source.height;
          panel.opacity = source.opacity;
          panel.font_size = source.font_size;
          panel.background_r = qtColorByte(source.background.x);
          panel.background_g = qtColorByte(source.background.y);
          panel.background_b = qtColorByte(source.background.z);
          panel.foreground_r = qtColorByte(source.foreground.x);
          panel.foreground_g = qtColorByte(source.foreground.y);
          panel.foreground_b = qtColorByte(source.foreground.z);
          panel.accent_r = qtColorByte(source.accent.x);
          panel.accent_g = qtColorByte(source.accent.y);
          panel.accent_b = qtColorByte(source.accent.z);
          panel.lines = source.lines;
          panels.push_back(std::move(panel));
        }
        return panels;
      };
      auto syncQtScriptOverlayPanels = [&]() {
        if (qtWindow != nullptr) {
          qtWindow->set_script_overlay_panels(qtBuildScriptOverlayPanels());
        }
      };
      auto updateQtPreviewOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        std::ostringstream overlay;
        overlay << "Accumulated: "
                << qtPublishedSample.load(std::memory_order_relaxed)
                << " spp\nFrame: " << qtFrameCount
                << "\nRays: " << qtPublishedRays.load(std::memory_order_relaxed);
        qtWindow->set_overlay_text(overlay.str());
        syncQtScriptOverlayPanels();
        qtMaybeRefreshPendingViewportPickables(std::chrono::steady_clock::now());
        syncQtStatusBar(false);
        syncQtDockPanels(false);
        updateQtWindowTitle();
      };
      vkpt::physics::PhysicsStepConfig qtPhysicsStepConfig;
      qtPhysicsStepConfig.fixed_dt = 1.0f / 60.0f;
      qtPhysicsStepConfig.collision_steps = 2;
      qtPhysicsStepConfig.deterministic = true;
      if (startupDeterminismContext) {
        qtPhysicsStepConfig.set_determinism(*startupDeterminismContext);
      }
      bool qtPhysicsCollisionDetectionEnabled = true;
      float qtPhysicsAccumulatorSeconds = 0.0f;
      auto qtPhysicsPrevTime = std::chrono::steady_clock::now();
      auto qtLastPhysicsScenePublish = std::chrono::steady_clock::time_point{};
      bool qtPhysicsScenePublishPending = false;
      bool qtPhysicsTransformUpdateSupported = true;
      uint32_t qtPhysicsTransformRevision = 1u;
      std::unordered_map<vkpt::core::StableId, vkpt::pathtracer::RTInstanceTransformUpdate>
          qtPendingPhysicsInstanceUpdates;
      struct QtPhysicsRenderPose {
        vkpt::scene::TransformComponent previous;
        vkpt::scene::TransformComponent current;
        bool valid = false;
      };
      std::unordered_map<vkpt::core::StableId, QtPhysicsRenderPose> qtPhysicsRenderPoses;
      double qtPhysicsSceneReloadMs = 0.0;
      auto qtLastPhysicsSlowLog = std::chrono::steady_clock::time_point{};
      auto qtLerpSceneVec3 = [](const vkpt::scene::Vec3& lhs,
                                const vkpt::scene::Vec3& rhs,
                                float alpha) {
        return vkpt::scene::Vec3{
            lhs.x + (rhs.x - lhs.x) * alpha,
            lhs.y + (rhs.y - lhs.y) * alpha,
            lhs.z + (rhs.z - lhs.z) * alpha};
      };
      auto qtNlerpSceneQuat = [](const vkpt::scene::Quat& lhs,
                                 const vkpt::scene::Quat& rhs,
                                 float alpha) {
        vkpt::scene::Quat out{
            lhs.x + (rhs.x - lhs.x) * alpha,
            lhs.y + (rhs.y - lhs.y) * alpha,
            lhs.z + (rhs.z - lhs.z) * alpha,
            lhs.w + (rhs.w - lhs.w) * alpha};
        const float len2 = out.x*out.x + out.y*out.y + out.z*out.z + out.w*out.w;
        if (len2 > 1.0e-12f) {
          const float invLen = 1.0f / std::sqrt(len2);
          out.x *= invLen;
          out.y *= invLen;
          out.z *= invLen;
          out.w *= invLen;
        } else {
          out = {};
        }
        return out;
      };
      auto qtPhysicsInterpolationAlpha = [&]() {
        if (qtPhysicsStepConfig.fixed_dt <= 0.0f) {
          return 1.0f;
        }
        return ClampFloat(qtPhysicsAccumulatorSeconds / qtPhysicsStepConfig.fixed_dt,
                          0.0f,
                          1.0f);
      };
      auto qtFallbackTransformFromUpdate =
          [](const vkpt::pathtracer::RTInstanceTransformUpdate& update) {
        vkpt::scene::TransformComponent transform;
        transform.translation = {update.translation.x, update.translation.y, update.translation.z};
        transform.rotation = {update.rotation.x, update.rotation.y, update.rotation.z, update.rotation.w};
        transform.scale = {update.scale.x, update.scale.y, update.scale.z};
        transform.dirty = true;
        return transform;
      };
      auto qtInterpolatedPhysicsTransform = [&](vkpt::core::StableId entityId,
                                                const vkpt::pathtracer::RTInstanceTransformUpdate& fallback) {
        const auto found = qtPhysicsRenderPoses.find(entityId);
        if (found == qtPhysicsRenderPoses.end() || !found->second.valid) {
          return qtFallbackTransformFromUpdate(fallback);
        }
        const float alpha = qtPhysicsInterpolationAlpha();
        vkpt::scene::TransformComponent transform;
        transform.translation = qtLerpSceneVec3(found->second.previous.translation,
                                                found->second.current.translation,
                                                alpha);
        transform.rotation = qtNlerpSceneQuat(found->second.previous.rotation,
                                              found->second.current.rotation,
                                              alpha);
        transform.scale = qtLerpSceneVec3(found->second.previous.scale,
                                          found->second.current.scale,
                                          alpha);
        transform.dirty = true;
        return transform;
      };
      auto qtPhysicsPublishInterval = [&](bool cameraRecentlyActive) {
        if (qtPhysicsTransformUpdateSupported) {
          (void)cameraRecentlyActive;
          return std::chrono::duration_cast<std::chrono::microseconds>(
              qtInteractiveFrameTarget());
        }
        if (cameraRecentlyActive) {
          const double targetMs = qtPhysicsSceneReloadMs > 0.0
              ? qtPhysicsSceneReloadMs * 1.25
              : 80.0;
          return std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::milliseconds(static_cast<int>(
                  std::round(std::clamp(targetMs, 50.0, 240.0)))));
        }
        const double targetMs = qtPhysicsSceneReloadMs > 0.0
            ? qtPhysicsSceneReloadMs * 2.5
            : 250.0;
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::milliseconds(static_cast<int>(
                std::round(std::clamp(targetMs, 180.0, 1000.0)))));
      };
      auto qtBuildPendingPhysicsInstanceUpdates = [&]() {
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        updates.reserve(qtPendingPhysicsInstanceUpdates.size());
        for (auto& [entityId, update] : qtPendingPhysicsInstanceUpdates) {
          if (update.instance_index == vkpt::pathtracer::kInvalidRTInstanceIndex) {
            update.instance_index = qtFindRtInstanceIndex(entityId);
          }
          if (update.instance_index != vkpt::pathtracer::kInvalidRTInstanceIndex) {
            const auto renderTransform = qtInterpolatedPhysicsTransform(entityId, update);
            update.translation = ToPtVec3(renderTransform.translation);
            update.rotation = ToPtQuat4(renderTransform.rotation);
            update.scale = ToPtVec3(renderTransform.scale);
            updates.push_back(update);
          }
        }
        std::sort(updates.begin(), updates.end(),
                  [](const vkpt::pathtracer::RTInstanceTransformUpdate& lhs,
                     const vkpt::pathtracer::RTInstanceTransformUpdate& rhs) {
                    if (lhs.instance_index != rhs.instance_index) {
                      return lhs.instance_index < rhs.instance_index;
                    }
                    return lhs.entity_id < rhs.entity_id;
                  });
        return updates;
      };
      auto qtQueuePhysicsRenderPoseUpdates = [&]() {
        for (const auto& [entityId, pose] : qtPhysicsRenderPoses) {
          if (!pose.valid) {
            continue;
          }
          auto& pending = qtPendingPhysicsInstanceUpdates[entityId];
          pending.entity_id = entityId;
          pending.instance_index = qtFindRtInstanceIndex(entityId);
          pending.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                          vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                          vkpt::pathtracer::kRTInstanceFlagTransformDirty;
          pending.transform_revision = qtPhysicsTransformRevision++;
          pending.translation = ToPtVec3(pose.current.translation);
          pending.rotation = ToPtQuat4(pose.current.rotation);
          pending.scale = ToPtVec3(pose.current.scale);
        }
        qtPhysicsScenePublishPending =
            qtPhysicsScenePublishPending || !qtPendingPhysicsInstanceUpdates.empty();
      };
      auto qtTryPublishPhysicsInstanceUpdates = [&]() {
        if (!qtTracerReady || qtPendingPhysicsInstanceUpdates.empty()) {
          return false;
        }
        const auto updates = qtBuildPendingPhysicsInstanceUpdates();
        if (updates.empty() ||
            !qtPublishInstanceTransformUpdates(
                updates,
                vkpt::pathtracer::MakeStandardTransformUpdateOptions(
                    vkpt::pathtracer::RenderUpdateReason::PhysicsMotion,
                    0u,
                    "qt-physics"))) {
          qtPhysicsTransformUpdateSupported = false;
          return false;
        }
        qtPendingPhysicsInstanceUpdates.clear();
        qtPhysicsTransformUpdateSupported = true;
        return true;
      };
      auto qtApplyPhysicsSimulation = [&](std::chrono::steady_clock::time_point now) {
        auto nearlyEqual = [](float lhs, float rhs, float epsilon) {
          return std::abs(lhs - rhs) <= epsilon;
        };
        if (qtPhysicsRuntimeDirty) {
          qtPhysicsRenderPoses.clear();
          qtPendingPhysicsInstanceUpdates.clear();
          qtPhysicsTransformUpdateSupported = true;
          qtPhysicsTransformRevision++;
          qtPhysicsAccumulatorSeconds = 0.0f;
          qtPhysicsPrevTime = now;
          qtLastPhysicsScenePublish = std::chrono::steady_clock::time_point{};
          qtPhysicsRuntimeDirty = false;
        }
        if (!qtScriptsRunning()) {
          qtPhysicsAccumulatorSeconds = 0.0f;
          qtPhysicsPrevTime = now;
          qtPendingPhysicsInstanceUpdates.clear();
          qtPhysicsScenePublishPending = false;
          return;
        }
        const bool physicsOn =
            qtPhysicsSummary.enabled_bodies > 0u && qtPhysicsSummary.dynamic_bodies > 0u;
        if (!physicsOn || qtGizmoDrag.active) {
          qtPhysicsPrevTime = now;
          return;
        }

        const float dt = ClampFloat(
            std::chrono::duration<float>(now - qtPhysicsPrevTime).count(),
            0.0f,
            0.05f);
        qtPhysicsPrevTime = now;
        qtPhysicsAccumulatorSeconds =
            std::min(0.25f, qtPhysicsAccumulatorSeconds + dt);

        bool stepped = false;
        int steps = 0;
        double physicsStepMs = 0.0;
        constexpr int kMaxPhysicsStepsPerUiFrame = 4;
        qtPhysicsStepConfig.collision_detection_enabled = qtPhysicsCollisionDetectionEnabled;
        while (qtPhysicsAccumulatorSeconds >= qtPhysicsStepConfig.fixed_dt &&
               steps < kMaxPhysicsStepsPerUiFrame) {
          const auto stepStart = std::chrono::steady_clock::now();
          const auto stepResult = qtPhysics->step_fixed(qtPhysicsStepConfig);
          physicsStepMs += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - stepStart)
                               .count();
          if (!stepResult) {
            qtPreviewStatus = "physics step failed";
            ui_runtime_state.status_message = "physics step failed";
            qtPhysicsAccumulatorSeconds = 0.0f;
            return;
          }
          qtPhysicsAccumulatorSeconds -= qtPhysicsStepConfig.fixed_dt;
          stepped = true;
          ++steps;
        }
        if (steps == kMaxPhysicsStepsPerUiFrame) {
          qtPhysicsAccumulatorSeconds = 0.0f;
        }
        if (!stepped) {
          if (!qtPhysicsRenderPoses.empty() &&
              qtPhysicsTransformUpdateSupported &&
              !qtUseBg &&
              qtTracerReady) {
            qtQueuePhysicsRenderPoseUpdates();
          } else {
            return;
          }
        }

        double physicsExtractMs = 0.0;
        std::vector<vkpt::physics::PhysicsTransformWrite> physicsWrites;
        if (stepped) {
          bool sceneChanged = false;
          const auto extractStart = std::chrono::steady_clock::now();
          physicsWrites = qtPhysics->step_snapshot().transform_writes;
          physicsExtractMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - extractStart)
                                 .count();
          std::optional<vkpt::scene::SceneWorld> preWriteWorldSnapshot;
          auto preWriteWorldFor = [&](const vkpt::scene::SceneEntityDefinition& entity)
              -> const vkpt::scene::SceneWorld* {
            if (!entity.has_hierarchy || entity.hierarchy.parent == 0u) {
              return nullptr;
            }
            if (!preWriteWorldSnapshot) {
              preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
            }
            return preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
          };
          for (const auto& write : physicsWrites) {
            auto* entity = qtFindEntity(write.entity);
            if (entity == nullptr) {
              continue;
            }
            const auto* preWriteWorld = preWriteWorldFor(*entity);
            const auto currentWorld = ResolveEntityWorldTransform(*entity, preWriteWorld);
            const auto& next = write.transform;
            const bool transformChanged =
                !nearlyEqual(currentWorld.translation.x, next.translation.x, 0.0005f) ||
                !nearlyEqual(currentWorld.translation.y, next.translation.y, 0.0005f) ||
                !nearlyEqual(currentWorld.translation.z, next.translation.z, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.x, next.rotation.x, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.y, next.rotation.y, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.z, next.rotation.z, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.w, next.rotation.w, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.x, next.scale.x, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.y, next.scale.y, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.z, next.scale.z, 0.0005f);
            if (!transformChanged) {
              continue;
            }
            auto& renderPose = qtPhysicsRenderPoses[write.entity];
            renderPose.previous = currentWorld;
            renderPose.current = next;
            renderPose.valid = true;
            entity->has_transform = true;
            entity->transform = ConvertWorldTransformToDocumentLocal(*entity, preWriteWorld, next);
            entity->transform.dirty = true;
            auto& pending = qtPendingPhysicsInstanceUpdates[write.entity];
            pending.entity_id = write.entity;
            pending.instance_index = qtFindRtInstanceIndex(write.entity);
            pending.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                            vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                            vkpt::pathtracer::kRTInstanceFlagTransformDirty;
            pending.transform_revision = qtPhysicsTransformRevision++;
            pending.translation = ToPtVec3(next.translation);
            pending.rotation = ToPtQuat4(next.rotation);
            pending.scale = ToPtVec3(next.scale);
            sceneChanged = true;
          }
          if (!sceneChanged) {
            qtPhysicsRenderPoses.clear();
          }
          qtPhysicsScenePublishPending = qtPhysicsScenePublishPending || sceneChanged;
          if (!qtPhysicsScenePublishPending) {
            return;
          }
        } else if (!qtPhysicsScenePublishPending) {
          return;
        }

        const bool cameraRecentlyActive =
            qtLastCameraInputTime != std::chrono::steady_clock::time_point{} &&
            now - qtLastCameraInputTime < std::chrono::milliseconds(250);

        const auto publishInterval = qtPhysicsPublishInterval(cameraRecentlyActive);
        if (qtLastPhysicsScenePublish == std::chrono::steady_clock::time_point{} ||
            now - qtLastPhysicsScenePublish >= publishInterval) {
          const auto publishStart = std::chrono::steady_clock::now();
          qtLastPhysicsScenePublish = now;
          const bool transformUpdatePublished = qtTryPublishPhysicsInstanceUpdates();
          if (transformUpdatePublished) {
            const double publishMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart).count();
            qtPhysicsSceneReloadMs = qtPhysicsSceneReloadMs <= 0.0
                ? publishMs
                : (qtPhysicsSceneReloadMs * 0.75 + publishMs * 0.25);
            qtPhysicsScenePublishPending = false;
            qtPreviewStatus = "physics transform update";
            ui_runtime_state.status_message = "physics transform update";
          } else {
            const double publishMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart).count();
            qtPhysicsSceneReloadMs = qtPhysicsSceneReloadMs <= 0.0
                ? publishMs
                : (qtPhysicsSceneReloadMs * 0.75 + publishMs * 0.25);
            qtPhysicsTransformUpdateSupported = false;
            qtPreviewStatus = "physics transform update blocked";
            ui_runtime_state.status_message = "physics transform update blocked";
          }
          const auto slowNow = std::chrono::steady_clock::now();
          if ((physicsStepMs > 4.0 || physicsExtractMs > 2.0 || physicsWrites.size() > 64u) &&
              (qtLastPhysicsSlowLog == std::chrono::steady_clock::time_point{} ||
               slowNow - qtLastPhysicsSlowLog > std::chrono::seconds(1))) {
            logger.log(vkpt::log::Severity::Warning,
                       "physics",
                       "slow qt physics simulation slice",
                       {
                           {"stack", "qtApplyPhysicsSimulation -> ThreadedPhysicsWorld::step_fixed/step_snapshot -> qtPublishInstanceTransformUpdates"},
                           {"steps", std::to_string(steps)},
                           {"step_ms", std::to_string(physicsStepMs)},
                           {"extract_ms", std::to_string(physicsExtractMs)},
                           {"writes", std::to_string(physicsWrites.size())},
                           {"collision_steps", std::to_string(qtPhysicsStepConfig.collision_steps)}
                       });
            qtLastPhysicsSlowLog = slowNow;
          }
        }
      };
      syncQtDockPanels(true);
      emitQtShellReadyMarker();
      updateQtSelectionOverlay();
      updateQtPreviewOverlay();
