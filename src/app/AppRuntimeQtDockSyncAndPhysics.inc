      auto qtBuildFrameStats = [&]() {
        QtDockFrameStats stats;
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
        stats.total_rays = qtPublishedRays.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
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
            ? (qtUseBg ? std::string("on (background thread)") : std::string("on (event loop)"))
            : std::string("off");
        stats.publish_cap = qtUseBg
            ? (std::to_string(qtPreviewPublishHz) + " fps")
            : std::string("event loop");
        stats.camera_mode = qtFpsMode
            ? std::string("fps")
            : (qtEnableAutoOrbit ? std::string("orbit") : std::string("authored"));
        stats.fps_player_grounded = qtFpsPlayer.grounded;
        stats.fps_player_crouching = qtFpsPlayer.crouching;
        stats.fps_player_running = qtFpsPlayer.running;
        stats.fps_player_speed = qtFpsPlayer.current_speed;
        stats.fps_player_eye_height = qtFpsPlayer.eye_height;
        if (qtWindow != nullptr) {
          const auto windowStats = qtWindow->framebuffer_stats();
          stats.window_received = windowStats.received;
          stats.window_presented = windowStats.presented;
          stats.window_dropped = windowStats.dropped;
        }
        return stats;
      };
      auto qtBuildBenchmarkPanel = [&](const QtDockFrameStats& frameStats) {
        const std::string scenePath =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        const std::string rendererPath = qtUseBg
            ? "cpu_tiled_background"
            : (config.backend.value.empty() ? std::string("event_loop") : config.backend.value);
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
      auto syncQtDockPanels = [&](bool force) {
        if (qtWindow == nullptr) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            !qtDockPanelsDirty &&
            qtLastDockPanelSync != std::chrono::steady_clock::time_point{} &&
            now - qtLastDockPanelSync < std::chrono::milliseconds(5000)) {
          return;
        }
        const auto frameStats = qtBuildFrameStats();
        ui_runtime_state.active_scene =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        ui_runtime_state.active_camera = qtActiveCameraObjectName ? qtActiveCameraObjectName() : "runtime camera";
        ui_runtime_state.active_renderer_backend = config.backend.value;
        ui_runtime_state.active_renderer_path = qtUseBg ? "cpu_tiled_background" : config.backend.value;
        ui_runtime_state.spp_accumulated = frameStats.sample_count;
        ui_runtime_state.frame_ms = frameStats.ui_frame_ms;
        ui_runtime_state.fps = frameStats.ui_frame_ms > 0.0 ? 1000.0 / frameStats.ui_frame_ms : 0.0;
        ui_runtime_state.background_job_count = qtUseBg ? 1u : 0u;
        const auto benchmarkPanel = qtBuildBenchmarkPanel(frameStats);
        const auto statusBar = vkpt::editor::BuildStatusBarModel(
            ui_runtime_state,
            ui_selection_state,
            &benchmarkPanel.score);
        ApplyQtStatusBarToWindow(qtWindow, BuildQtStatusBarText(statusBar));
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
      auto updateQtPreviewOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        qtWindow->set_overlay_text(std::string_view{});
        syncQtDockPanels(false);
        updateQtWindowTitle();
      };
      vkpt::physics::PhysicsStepConfig qtPhysicsStepConfig;
      qtPhysicsStepConfig.fixed_dt = 1.0f / 60.0f;
      qtPhysicsStepConfig.collision_steps = 6;
      qtPhysicsStepConfig.deterministic = true;
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
          return qtUseBg ? std::chrono::milliseconds(33)
                         : std::chrono::milliseconds(16);
        }
        if (cameraRecentlyActive) {
          const double targetMs = qtPhysicsSceneReloadMs > 0.0
              ? qtPhysicsSceneReloadMs * 1.25
              : 80.0;
          return std::chrono::milliseconds(static_cast<int>(
              std::round(std::clamp(targetMs, 50.0, 240.0))));
        }
        const double targetMs = qtPhysicsSceneReloadMs > 0.0
            ? qtPhysicsSceneReloadMs * 2.5
            : 250.0;
        return std::chrono::milliseconds(static_cast<int>(
            std::round(std::clamp(targetMs, 180.0, 1000.0))));
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
        if (updates.empty() || !qtPublishInstanceTransformUpdates(updates)) {
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
        constexpr int kMaxPhysicsStepsPerUiFrame = 4;
        qtPhysicsStepConfig.collision_detection_enabled = qtPhysicsCollisionDetectionEnabled;
        while (qtPhysicsAccumulatorSeconds >= qtPhysicsStepConfig.fixed_dt &&
               steps < kMaxPhysicsStepsPerUiFrame) {
          const auto stepResult = qtPhysics->step_fixed(qtPhysicsStepConfig);
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

        if (stepped) {
          bool sceneChanged = false;
          const auto preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
          const auto* preWriteWorld =
              preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
          for (const auto& write : qtPhysics->extract_transform_writes()) {
            auto* entity = qtFindEntity(write.entity);
            if (entity == nullptr) {
              continue;
            }
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
          } else if (qtReloadEditedScene("physics simulation")) {
            const double publishMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart).count();
            qtPhysicsSceneReloadMs = qtPhysicsSceneReloadMs <= 0.0
                ? publishMs
                : (qtPhysicsSceneReloadMs * 0.75 + publishMs * 0.25);
            qtPhysicsScenePublishPending = false;
            qtPendingPhysicsInstanceUpdates.clear();
            qtPhysicsTransformUpdateSupported = false;
            qtPreviewStatus = "physics simulation";
            ui_runtime_state.status_message = "physics simulation";
          }
        }
      };
      syncQtDockPanels(true);
      emitQtShellReadyMarker();
      updateQtSelectionOverlay();
      updateQtPreviewOverlay();
