// Stage B Qt orchestration stress gate. Drives 4 deterministic phases
// (idle, camera_pan, transform_drag, lua_motion) on the polled Qt render loop
// and captures Stage A metrics deltas, then writes a JSON report and closes
// the window. This file is included once inside the Qt branch of AppRuntime.
#ifdef PT_ENABLE_QT

      struct QtStressPhaseRecord {
        std::string name;
        double duration_ms = 0.0;
        std::vector<vkpt::core::metrics::MetricSnapshot> before;
        std::vector<vkpt::core::metrics::MetricSnapshot> after;
      };

      enum class QtStressPhaseId : int {
        Idle = 0,
        CameraPan = 1,
        TransformDrag = 2,
        LuaMotion = 3,
        Done = 4,
      };

      const auto qtStressPhaseDuration = std::chrono::milliseconds(
          static_cast<std::int64_t>(qtStressPhaseSeconds) * 1000);
      const auto qtStressTickInterval = std::chrono::milliseconds(16); // ~60Hz

      QtStressPhaseId qtStressPhase = QtStressPhaseId::Idle;
      std::array<QtStressPhaseRecord, 4> qtStressRecords{};
      qtStressRecords[0].name = "idle";
      qtStressRecords[1].name = "camera_pan";
      qtStressRecords[2].name = "transform_drag";
      qtStressRecords[3].name = "lua_motion";

      auto qtStressNow = std::chrono::steady_clock::now();
      auto qtStressPhaseStart = qtStressNow;
      auto qtStressLastTick = qtStressNow - qtStressTickInterval;
      bool qtStressBeforeCaptured = false;

      // Camera pan state — sweep azimuth around qtOrbitCenter.
      const float qtStressCamPanRadius = (kQtOrbitRadius > 1e-3f) ? kQtOrbitRadius : 4.0f;
      float qtStressCamPanAngleDeg = qtOrbitInitialAngleDeg;

      // Transform drag state — pick first entity with a transform.
      vkpt::core::StableId qtStressDragEntityId = 0;
      vkpt::scene::Vec3 qtStressDragBaseTranslation{0.0f, 0.0f, 0.0f};
      for (const auto& e : qtSceneDocument.entities) {
        if (e.has_transform) {
          qtStressDragEntityId = e.id;
          qtStressDragBaseTranslation = e.transform.translation;
          break;
        }
      }
      float qtStressDragPhaseSec = 0.0f;

      // Lua-motion fallback: animate a second entity (or the same one if only
      // one exists) on a different sinusoid. The harness documents this as the
      // fallback because wiring a fresh embedded Lua source through
      // ScriptComponent during the running Qt loop is out of scope for Stage B.
      vkpt::core::StableId qtStressLuaEntityId = 0;
      vkpt::scene::Vec3 qtStressLuaBaseTranslation{0.0f, 0.0f, 0.0f};
      for (const auto& e : qtSceneDocument.entities) {
        if (e.has_transform && e.id != qtStressDragEntityId) {
          qtStressLuaEntityId = e.id;
          qtStressLuaBaseTranslation = e.transform.translation;
          break;
        }
      }
      if (qtStressLuaEntityId == 0u) {
        qtStressLuaEntityId = qtStressDragEntityId;
        qtStressLuaBaseTranslation = qtStressDragBaseTranslation;
      }
      float qtStressLuaPhaseSec = 0.0f;

      auto qtStressCaptureBefore = [&](QtStressPhaseRecord& rec) {
        rec.before = vkpt::core::metrics::MetricsRegistry::instance().snapshot_all();
      };
      auto qtStressCaptureAfter = [&](QtStressPhaseRecord& rec,
                                      std::chrono::steady_clock::time_point start_tp,
                                      std::chrono::steady_clock::time_point end_tp) {
        rec.after = vkpt::core::metrics::MetricsRegistry::instance().snapshot_all();
        rec.duration_ms = std::chrono::duration<double, std::milli>(end_tp - start_tp).count();
      };

      // Real Qt mouse moves so the input → pixel correlation gets exercised.
      // Without this, emit_mouse_move never fires and input_event_total /
      // input_to_pixel_us are null. We synthesize a small jitter inside the
      // viewport so the path runs all the way through Qt's input chain.
      auto qtStressSynthesizeMouseMove = [&](int phase_seq) {
        if (qtWindow == nullptr) return;
        const int x = 320 + (phase_seq % 7) * 13;
        const int y = 180 + ((phase_seq / 7) % 5) * 11;
        qtWindow->emit_mouse_move(x, y);
      };

      int qtStressTickSeq = 0;

      auto qtStressTickCameraPan = [&](float dt_seconds) {
        ++qtStressTickSeq;
        qtStressSynthesizeMouseMove(qtStressTickSeq);
        qtStressCamPanAngleDeg += 30.0f * dt_seconds; // 30 deg/s
        const float rad = qtStressCamPanAngleDeg * (3.14159265f / 180.0f);
        qtScene.camera_position.x = qtOrbitCenter.x + qtStressCamPanRadius * std::sin(rad);
        qtScene.camera_position.z = qtOrbitCenter.z + qtStressCamPanRadius * std::cos(rad);
        qtScene.camera_target = qtOrbitCenter;
        syncQtCameraPoseFromScene();
        qtRefreshCameraFocusFromPose();
        syncQtFpsAnglesFromPose();
        qtPublishRenderSnapshotForBackground(false, false, true, false);
      };

      auto qtStressTickTransformDrag = [&](float t_seconds) {
        ++qtStressTickSeq;
        qtStressSynthesizeMouseMove(qtStressTickSeq);
        if (qtStressDragEntityId == 0u) return;
        auto* entity = vkpt::app::FindQtSceneEntity(qtSceneDocument, qtStressDragEntityId);
        if (entity == nullptr || !entity->has_transform) return;
        // Mirror the real editor gizmo drag path: ui_selection_state primary
        // pointing at the entity, qtGizmoDrag.active = true with the entity
        // populated so every gated check (`if (!qtGizmoDrag.active)` etc.)
        // takes the same branch as a hand-driven drag. Without this, the
        // harness measures the physics/Lua mutation path instead of the
        // editor drag path the user is actually testing.
        ui_selection_state.active_primary_entity = qtStressDragEntityId;
        if (std::find(ui_selection_state.selected_entity_ids.begin(),
                      ui_selection_state.selected_entity_ids.end(),
                      qtStressDragEntityId) ==
            ui_selection_state.selected_entity_ids.end()) {
          ui_selection_state.selected_entity_ids.push_back(qtStressDragEntityId);
        }
        if (!qtGizmoDrag.active) {
          qtGizmoDrag = {};
          qtGizmoDrag.active = true;
          QtGizmoDragEntityStart start{};
          start.entity_id = qtStressDragEntityId;
          start.sdf_primitive = false;
          start.transform = entity->transform;
          qtGizmoDrag.entities.push_back(start);
          qtGizmoDrag.publishable_entity_ids.insert(qtStressDragEntityId);
        }
        const float jitter = 0.05f * std::sin(t_seconds * 6.283f * 2.0f);
        entity->transform.translation = qtStressDragBaseTranslation;
        entity->transform.translation.x += jitter;
        entity->transform.dirty = true;
        vkpt::app::ApplyQtLegacyTransformToEntity(qtSceneDocument, *entity);
        qtPublishRenderSnapshotForBackground(false, true, false, false);
      };

      auto qtStressTickLuaMotion = [&](float t_seconds) {
        ++qtStressTickSeq;
        // Lua phase intentionally does NOT synthesize a mouse move — it models
        // script-driven mutation, which should produce publishes with no
        // pending input. input_to_pixel_us samples in this phase will stay 0
        // by design; that is the "non-input-driven" baseline for comparison.
        if (qtStressLuaEntityId == 0u) return;
        auto* entity = vkpt::app::FindQtSceneEntity(qtSceneDocument, qtStressLuaEntityId);
        if (entity == nullptr || !entity->has_transform) return;
        const float dy = 0.04f * std::sin(t_seconds * 6.283f * 1.5f);
        const float dx = 0.04f * std::cos(t_seconds * 6.283f * 1.0f);
        entity->transform.translation = qtStressLuaBaseTranslation;
        entity->transform.translation.x += dx;
        entity->transform.translation.y += dy;
        entity->transform.dirty = true;
        vkpt::app::ApplyQtLegacyTransformToEntity(qtSceneDocument, *entity);
        qtPublishRenderSnapshotForBackground(false, true, false, false);
      };

      auto qtStressAdvance = [&]() -> bool {
        if (qtStressPhase == QtStressPhaseId::Done) return false;
        const auto now = std::chrono::steady_clock::now();
        auto& rec = qtStressRecords[static_cast<int>(qtStressPhase)];
        if (!qtStressBeforeCaptured) {
          qtStressCaptureBefore(rec);
          qtStressBeforeCaptured = true;
          qtStressPhaseStart = now;
          qtStressLastTick = now - qtStressTickInterval;
        }

        const bool tickReady = (now - qtStressLastTick) >= qtStressTickInterval;
        if (tickReady) {
          const float t_phase = std::chrono::duration<float>(now - qtStressPhaseStart).count();
          const float dt = std::chrono::duration<float>(now - qtStressLastTick).count();
          switch (qtStressPhase) {
            case QtStressPhaseId::Idle: break;
            case QtStressPhaseId::CameraPan: qtStressTickCameraPan(dt); break;
            case QtStressPhaseId::TransformDrag:
              qtStressDragPhaseSec = t_phase;
              qtStressTickTransformDrag(t_phase);
              break;
            case QtStressPhaseId::LuaMotion:
              qtStressLuaPhaseSec = t_phase;
              qtStressTickLuaMotion(t_phase);
              break;
            case QtStressPhaseId::Done: break;
          }
          qtStressLastTick = now;
        }

        if ((now - qtStressPhaseStart) >= qtStressPhaseDuration) {
          qtStressCaptureAfter(rec, qtStressPhaseStart, now);
          qtStressBeforeCaptured = false;
          // End the simulated gizmo drag when leaving TransformDrag — leaves
          // the editor in a clean state for the LuaMotion phase, which models
          // a different mutation source (script tick) and should NOT have an
          // active gizmo so we measure the non-drag path for comparison.
          if (qtStressPhase == QtStressPhaseId::TransformDrag) {
            qtGizmoDrag = {};
          }
          qtStressPhase = static_cast<QtStressPhaseId>(static_cast<int>(qtStressPhase) + 1);
          if (qtStressPhase == QtStressPhaseId::Done) {
            return true;
          }
        }
        return false;
      };

      // ---- Report writer (called once after phases complete) -----------------
      auto qtStressFindMetric = [&](const std::vector<vkpt::core::metrics::MetricSnapshot>& list,
                                    std::string_view name)
          -> const vkpt::core::metrics::MetricSnapshot* {
        for (const auto& m : list) {
          if (m.name == name) return &m;
        }
        return nullptr;
      };

      auto qtStressJsonEscape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
          switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
              if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
              } else {
                out += c;
              }
          }
        }
        return out;
      };

      auto qtStressMetricDelta =
          [&](const QtStressPhaseRecord& rec, std::string_view name, std::ostringstream& os) {
        const auto* before = qtStressFindMetric(rec.before, name);
        const auto* after = qtStressFindMetric(rec.after, name);
        if (after == nullptr) {
          os << "null";
          return;
        }
        switch (after->kind) {
          case vkpt::core::metrics::Kind::CounterKind: {
            std::uint64_t delta = after->counter_value -
                (before ? before->counter_value : 0ull);
            os << "{\"counter_delta\":" << delta << "}";
            break;
          }
          case vkpt::core::metrics::Kind::GaugeKind: {
            os << "{\"gauge\":" << after->gauge_value << "}";
            break;
          }
          case vkpt::core::metrics::Kind::HistogramKind: {
            std::uint64_t count_delta = after->hist.count -
                (before ? before->hist.count : 0ull);
            os << "{\"count_delta\":" << count_delta
               << ",\"p50\":" << after->hist.p50
               << ",\"p95\":" << after->hist.p95
               << ",\"p99\":" << after->hist.p99
               << ",\"max\":" << after->hist.max_val
               << "}";
            break;
          }
        }
      };

      const std::array<std::string_view, 18> qtStressTrackedMetrics = {
        "vkp.ui.paint_us",
        "vkp.ui.repaint_total",
        "vkp.ui.input_event_total",
        "vkp.ui.input_dispatch_us",
        "vkp.ui.frame_deliver_us",
        "vkp.ui.frame_delivered_total",
        "vkp.ui.frame_dropped_by_ui_total",
        "vkp.ui.input_to_pixel_us",
        "vkp.scene.publish_us",
        "vkp.scene.snapshot_published_total",
        "vkp.scene.snapshot_dropped_total",
        "vkp.scene.snapshot_build_us",
        "vkp.snapshot.publish_total",
        "vkp.snapshot.dropped_total",
        "vkp.tracer.gen_lag",
        "vkp.tracer.tile_latency_us",
        "vkp.render.handoff.published_total",
        "vkp.render.handoff.dropped_total",
      };

      const std::array<std::string_view, 4> qtStressPublishBySource = {
        "vkp.scene.publish_by_source.camera_total",
        "vkp.scene.publish_by_source.transform_total",
        "vkp.scene.publish_by_source.topology_total",
        "vkp.scene.publish_by_source.material_total",
      };

      auto qtStressWriteReport = [&]() {
        std::error_code ec;
        std::filesystem::create_directories(qtStressOutput, ec);

        std::ostringstream js;
        js << "{\n";
        js << "  \"scene\": \"" << qtStressJsonEscape(config.scene_path.value) << "\",\n";
        js << "  \"phase_seconds\": " << qtStressPhaseSeconds << ",\n";
        js << "  \"phases\": [\n";
        std::uint64_t worstP99 = 0u;
        std::string worstName;
        std::uint64_t headlineP95 = 0u;
        for (std::size_t i = 0; i < qtStressRecords.size(); ++i) {
          const auto& rec = qtStressRecords[i];
          js << "    {\n";
          js << "      \"name\": \"" << rec.name << "\",\n";
          js << "      \"duration_ms\": " << rec.duration_ms << ",\n";
          js << "      \"metrics\": {\n";
          bool first = true;
          for (auto name : qtStressTrackedMetrics) {
            if (!first) js << ",\n";
            first = false;
            js << "        \"" << name << "\": ";
            qtStressMetricDelta(rec, name, js);
          }
          for (auto name : qtStressPublishBySource) {
            js << ",\n        \"" << name << "\": ";
            qtStressMetricDelta(rec, name, js);
          }
          js << "\n      }\n";
          js << "    }";
          if (i + 1 < qtStressRecords.size()) js << ",";
          js << "\n";

          if (const auto* m = qtStressFindMetric(rec.after, "vkp.ui.input_to_pixel_us")) {
            if (m->kind == vkpt::core::metrics::Kind::HistogramKind) {
              if (m->hist.p99 > worstP99) { worstP99 = m->hist.p99; worstName = rec.name; }
              if (rec.name == "camera_pan") headlineP95 = m->hist.p95;
            }
          }
        }
        js << "  ],\n";
        js << "  \"summary\": {\n";
        js << "    \"headline_input_to_pixel_p95_us\": " << headlineP95 << ",\n";
        js << "    \"phase_with_worst_p99\": \"" << worstName << "\"\n";
        js << "  }\n";
        js << "}\n";

        const auto reportPath =
            std::filesystem::path(qtStressOutput) / "qt_stress_gate_report.json";
        std::ofstream out(reportPath, std::ios::binary | std::ios::trunc);
        if (out) {
          out << js.str();
          out.close();
        }
        const auto metricsPath =
            std::filesystem::path(qtStressOutput) / "metrics_final.json";
        std::ofstream m(metricsPath, std::ios::binary | std::ios::trunc);
        if (m) {
          m << vkpt::core::metrics::MetricsRegistry::instance().dump_json();
          m.close();
        }
        logger.log(vkpt::log::Severity::Info, "app", "qt_stress_gate report written", {
          {"report", reportPath.string()},
          {"metrics", metricsPath.string()}
        });
      };

      bool qtStressReportWritten = false;

#endif  // PT_ENABLE_QT
