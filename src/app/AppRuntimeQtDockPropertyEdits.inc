      auto qtApplyDockPropertyEdit = [&](const vkpt::platform::QtDockPropertyEdit& edit,
                                         vkpt::core::FrameIndex frameIndex) {
        const auto parts = QtSplitPropertyPath(edit.property_id);
        bool changed = false;
        bool renderAffecting = false;
        bool settingsAffecting = false;
        bool cameraAffecting = false;
        bool cameraPoseAffecting = false;
        bool entityTransformAffecting = false;
        vkpt::core::StableId cameraAffectingEntityId = 0u;
        std::unordered_set<vkpt::core::StableId> transformTouchedIds;

        auto failEdit = [&](std::string message) {
          ui_runtime_state.status_message = "property edit rejected: " + std::move(message);
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "dock_property_edit_rejected",
                      edit.panel_id,
                      "dock",
                      frameIndex,
                      {},
                      edit.property_id,
                      ui_runtime_state.status_message);
          return false;
        };

        if (parts.size() >= 2u && parts[0] == "script") {
          if (parts.size() >= 3u && parts[1] == "runtime") {
            if (parts[2] == "enabled") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              qtScriptState.scripts_enabled = value;
              qtScriptState.status = value ? "scripts enabled" : "scripts disabled";
            } else if (parts[2] == "playing") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              qtSetGameMode(value, frameIndex, "dock");
            } else if (parts[2] == "play") {
              qtSetGameMode(true, frameIndex, "dock");
            } else if (parts[2] == "pause") {
              qtSetGameMode(false, frameIndex, "dock");
            } else if (parts[2] == "step") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate, frameIndex, 1.0 / 60.0, "dock step");
            } else if (parts[2] == "reload") {
              qtReloadScriptsFromDocument("dock");
            } else if (parts[2] == "dispatch_on_load") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLoad, frameIndex, 0.0, "dock on_load");
            } else if (parts[2] == "dispatch_fixed_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnFixedUpdate, frameIndex, 1.0 / 60.0, "dock fixed_update");
            } else if (parts[2] == "dispatch_late_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLateUpdate, frameIndex, 1.0 / 60.0, "dock late_update");
            } else {
              return failEdit("unknown script runtime property");
            }
          } else {
            return failEdit("unknown script property");
          }
          changed = true;
          qtDockPanelsDirty = true;
        } else if (parts.size() >= 3u && parts[0] == "scene_tree" && parts[1] == "filter") {
          auto filterBitForName = [](const std::string& name) -> std::uint32_t {
            if (name == "camera") return kQtSceneTreeFilterCamera;
            if (name == "light") return kQtSceneTreeFilterLight;
            if (name == "model") return kQtSceneTreeFilterModel;
            if (name == "sdf") return kQtSceneTreeFilterSdf;
            if (name == "physics") return kQtSceneTreeFilterPhysics;
            if (name == "script") return kQtSceneTreeFilterScript;
            if (name == "entity") return kQtSceneTreeFilterEntity;
            if (name == "particle") return kQtSceneTreeFilterParticle;
            return 0u;
          };
          if (parts[2] == "name") {
            ui_runtime_state.scene_tree_name_filter = QtTrim(edit.value);
          } else if (parts[2] == "clear") {
            ui_runtime_state.scene_tree_name_filter.clear();
            ui_runtime_state.scene_tree_type_filter_mask = 0u;
          } else {
            const std::uint32_t bit = filterBitForName(parts[2]);
            if (bit == 0u) {
              return failEdit("unknown scene tree filter");
            }
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            if (value) {
              ui_runtime_state.scene_tree_type_filter_mask |= bit;
            } else {
              ui_runtime_state.scene_tree_type_filter_mask &= ~bit;
            }
          }
          changed = true;
          qtDockPanelsDirty = true;
        } else if (parts.size() >= 2u && parts[0] == "camera") {
          if (parts.size() == 3u && parts[1] == "mode" && parts[2] == "fps_toggle") {
            if (qtSetFpsMode) {
              qtSetFpsMode(!qtFpsMode, frameIndex, "dock");
            } else {
              qtFpsMode = !qtFpsMode;
              qtSyncViewportAssetDropMode();
            }
          } else if (parts.size() == 3u && parts[1] == "preview" && parts[2] == "fps") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric preview fps");
            }
            const auto hz = vkpt::config::ClampUiPresentHz(
                static_cast<std::uint32_t>(ClampFloat(std::round(value), 1.0f, 120.0f)));
            config.ui_present_hz = {hz, vkpt::config::ConfigSource::CliFlag};
            qtRefreshPreviewCadence();
            qtNextFramebufferSubmit = std::chrono::steady_clock::time_point{};
            ui_runtime_state.status_message =
                "motion preview target set to " + std::to_string(hz) + " fps";
            qtDockPanelsDirty = true;
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "slot") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric camera shot slot");
            }
            qtActiveCameraShotSlot = static_cast<int>(ClampFloat(std::round(value), 1.0f, 4.0f)) - 1;
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "save") {
            bool value = true;
            if (edit.value != "clicked" && !QtParseBool(edit.value, value)) {
              return failEdit("expected true, false, or clicked");
            }
            if (value) {
              qtSaveCameraShot(qtActiveCameraShotSlot);
            }
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "recall") {
            bool value = true;
            if (edit.value != "clicked" && !QtParseBool(edit.value, value)) {
              return failEdit("expected true, false, or clicked");
            }
            if (value && !qtRecallCameraShot(qtActiveCameraShotSlot)) {
              return failEdit("camera shot slot is empty");
            }
          } else if (parts.size() == 3u && parts[1] == "focus" && parts[2] == "pick") {
            if (!qtDockAutoFocus || !qtDockAutoFocus(frameIndex)) {
              return failEdit("auto focus missed");
            }
          } else if (parts.size() == 3u && parts[1] == "focus" && parts[2] == "selected") {
            if (!qtDockFocusSelected || !qtDockFocusSelected(frameIndex)) {
              return failEdit("no active selection to focus");
            }
          } else {
            return failEdit("unknown camera command property");
          }
          changed = true;
        } else if (parts.size() >= 2u && parts[0] == "render") {
          if (parts.size() == 2u && parts[1] == "backend") {
            const std::string requestedBackend = QtTrim(edit.value);
            if (requestedBackend.empty()) {
              return failEdit("expected backend name");
            }
            const bool switched = qtSwitchRuntimeBackend
                ? qtSwitchRuntimeBackend(requestedBackend, "dock")
                : false;
            PushUiEvent(ui_event_log,
                        switched ? "dock_backend_switch" : "dock_backend_switch_failed",
                        edit.panel_id,
                        "dock",
                        frameIndex,
                        {},
                        edit.property_id,
                        requestedBackend);
            UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                           ui_selection_state,
                                           ui_layout_state,
                                           ui_event_log,
                                           ui_command_history);
            updateQtSelectionOverlay();
            return switched;
          } else if (parts.size() == 3u && parts[1] == "resolution") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric render resolution");
            }
            const auto dimension =
                static_cast<uint32_t>(ClampFloat(std::round(value), 16.0f, 8192.0f));
            if (parts[2] == "width") {
              qtSettings.width = dimension;
              config.render_width = {dimension, vkpt::config::ConfigSource::CliFlag};
            } else if (parts[2] == "height") {
              qtSettings.height = dimension;
              config.render_height = {dimension, vkpt::config::ConfigSource::CliFlag};
            } else {
              return failEdit("unknown render resolution dimension");
            }
          } else if (parts.size() == 2u && parts[1] == "max_depth") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric max depth");
            }
            qtSettings.max_depth =
                static_cast<uint32_t>(ClampFloat(std::round(value), 1.0f, 256.0f));
          } else if (parts.size() == 2u && parts[1] == "nee") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_nee = value;
          } else if (parts.size() == 2u && parts[1] == "mis") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_mis = value;
          } else if (parts.size() == 2u && parts[1] == "denoiser") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_denoiser = value;
          } else if (parts.size() == 2u && parts[1] == "temporal_aa") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_temporal_aa = value;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "exposure") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric exposure");
            }
            qtSettings.film_resolve.exposure = ClampFloat(value, 0.0f, 64.0f);
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "tone_map") {
            vkpt::pathtracer::ToneMapMode mode{};
            if (!QtParseToneMapMode(edit.value, mode)) {
              return failEdit("unknown tone mapper");
            }
            qtSettings.film_resolve.tone_map = mode;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "output_transform") {
            vkpt::pathtracer::OutputTransformMode mode{};
            if (!QtParseOutputTransformMode(edit.value, mode)) {
              return failEdit("unknown output transform");
            }
            qtSettings.film_resolve.output_transform = mode;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "gamma") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric gamma");
            }
            qtSettings.film_resolve.gamma = ClampFloat(value, 0.01f, 8.0f);
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "clamp_output") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.film_resolve.clamp_output = value;
          } else {
            return failEdit("unknown render settings property");
          }
          changed = true;
          settingsAffecting = true;
        } else if (parts.size() >= 3u && parts[0] == "entity") {
          vkpt::core::StableId entityId = 0u;
          if (!QtParseStableId(parts[1], entityId)) {
            return failEdit("invalid entity id");
          }
          auto* entity = qtFindEntity(entityId);
          if (entity == nullptr) {
            return failEdit("entity not found");
          }
          auto ensurePhysicsBody = [&]() -> vkpt::scene::PhysicsBodyComponent& {
            if (!entity->has_physics_body) {
              entity->has_physics_body = true;
              entity->physics_body = QtDefaultDynamicPhysicsBody();
            }
            return entity->physics_body;
          };

          if (parts.size() == 3u && parts[2] == "name") {
            const std::string value = QtTrim(edit.value);
            if (entity->name != value) {
              entity->name = value;
              changed = true;
            }
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "transform") {
            if (!entity->has_transform) {
              entity->has_transform = true;
              entity->transform = {};
            }
            if (parts.size() == 5u) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric transform value");
              }
              const auto& field = parts[3];
              const auto& component = parts[4];
              if (field == "translation") {
                if (component == "x") {
                  entity->transform.translation.x = value;
                } else if (component == "y") {
                  entity->transform.translation.y = value;
                } else if (component == "z") {
                  entity->transform.translation.z = value;
                } else {
                  return failEdit("unknown translation component");
                }
              } else if (field == "rotation") {
                if (component == "x") {
                  entity->transform.rotation.x = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "y") {
                  entity->transform.rotation.y = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "z") {
                  entity->transform.rotation.z = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "w") {
                  entity->transform.rotation.w = ClampFloat(value, -1.0f, 1.0f);
                } else {
                  return failEdit("unknown rotation component");
                }
                const float lenSq =
                    entity->transform.rotation.x * entity->transform.rotation.x +
                    entity->transform.rotation.y * entity->transform.rotation.y +
                    entity->transform.rotation.z * entity->transform.rotation.z +
                    entity->transform.rotation.w * entity->transform.rotation.w;
                if (lenSq <= 1.0e-8f) {
                  entity->transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                } else {
                  const float invLen = 1.0f / std::sqrt(lenSq);
                  entity->transform.rotation.x *= invLen;
                  entity->transform.rotation.y *= invLen;
                  entity->transform.rotation.z *= invLen;
                  entity->transform.rotation.w *= invLen;
                }
              } else if (field == "scale") {
                value = std::fabs(value) <= 1.0e-5f ? 1.0e-5f : value;
                if (component == "x") {
                  entity->transform.scale.x = value;
                } else if (component == "y") {
                  entity->transform.scale.y = value;
                } else if (component == "z") {
                  entity->transform.scale.z = value;
                } else {
                  return failEdit("unknown scale component");
                }
              } else {
                return failEdit("unknown transform field");
              }
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "translation") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for position");
              }
              entity->transform.translation = value;
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "rotation") {
              vkpt::scene::Quat value{};
              if (!QtParseQuat(edit.value, value)) {
                return failEdit("expected four numbers for rotation quaternion");
              }
              entity->transform.rotation = value;
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "scale") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for scale");
              }
              value.x = std::fabs(value.x) <= 1.0e-5f ? 1.0e-5f : value.x;
              value.y = std::fabs(value.y) <= 1.0e-5f ? 1.0e-5f : value.y;
              value.z = std::fabs(value.z) <= 1.0e-5f ? 1.0e-5f : value.z;
              entity->transform.scale = value;
              changed = true;
              renderAffecting = true;
            } else if (!qtUseBg) {
              return failEdit("unknown transform field");
            }
            entity->transform.dirty = true;
            entityTransformAffecting = !entity->has_light &&
                                       !entity->has_camera &&
                                       !entity->has_sdf_primitive;
            if (entityTransformAffecting) {
              transformTouchedIds.insert(entityId);
            }
            if (entity->has_camera) {
              cameraAffecting = true;
              cameraPoseAffecting = true;
              cameraAffectingEntityId = entityId;
            }
          } else if (parts.size() == 4u && parts[2] == "mesh" && parts[3] == "material_id") {
            if (!entity->has_mesh) {
              return failEdit("entity has no mesh renderer");
            }
            vkpt::core::StableId materialId = 0u;
            if (!QtParseStableId(edit.value, materialId)) {
              return failEdit("invalid material id");
            }
            if (materialId != 0u && FindQtSceneMaterial(qtSceneDocument, materialId) == nullptr) {
              return failEdit("material id does not exist");
            }
            entity->mesh.material_id = materialId;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "mesh" && parts[3] == "mesh_id") {
            if (!entity->has_mesh) {
              return failEdit("entity has no mesh renderer");
            }
            vkpt::core::StableId geometryId = 0u;
            if (!QtParseStableId(edit.value, geometryId)) {
              return failEdit("invalid mesh id");
            }
            if (geometryId != 0u && FindQtSceneGeometry(qtSceneDocument, geometryId) == nullptr) {
              return failEdit("mesh id does not exist");
            }
            entity->mesh.mesh_id = geometryId;
            changed = true;
            renderAffecting = true;
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "sdf_primitive") {
            if (!entity->has_sdf_primitive) {
              return failEdit("entity has no SDF primitive");
            }
            const auto& field = parts[3];
            if (parts.size() == 5u) {
              return failEdit("unknown SDF property component");
            }
            if (field == "shape") {
              entity->sdf_primitive.shape = QtTrim(edit.value);
              if (entity->sdf_primitive.shape.empty()) {
                entity->sdf_primitive.shape = "sphere";
              }
            } else if (field == "radius" || field == "param_a" || field == "param_b") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF value");
              }
              if (field == "radius") {
                entity->sdf_primitive.radius = std::max(0.01f, value);
              } else if (field == "param_a") {
                entity->sdf_primitive.param_a = value;
              } else {
                entity->sdf_primitive.param_b = value;
              }
            } else {
              return failEdit("unknown SDF property");
            }
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "material" && parts[3] == "material_id") {
            vkpt::core::StableId materialId = 0u;
            if (!QtParseStableId(edit.value, materialId)) {
              return failEdit("invalid material id");
            }
            if (materialId != 0u && FindQtSceneMaterial(qtSceneDocument, materialId) == nullptr) {
              return failEdit("material id does not exist");
            }
            entity->material.material_id = materialId;
            changed = true;
            renderAffecting = true;
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "light") {
            if (!entity->has_light) {
              return failEdit("entity has no light");
            }
            const auto& field = parts[3];
            if (parts.size() == 5u && (field == "color" || field == "direction")) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light vector component");
              }
              value = field == "color" ? ClampFloat(value, 0.0f, 10.0f)
                                       : ClampFloat(value, -1.0f, 1.0f);
              auto& target = field == "color" ? entity->light.color : entity->light.direction;
              if (parts[4] == "x") {
                target.x = value;
              } else if (parts[4] == "y") {
                target.y = value;
              } else if (parts[4] == "z") {
                target.z = value;
              } else {
                return failEdit("unknown light vector component");
              }
            } else if (parts.size() != 4u) {
              return failEdit("unknown light property component");
            } else if (field == "type") {
              entity->light.type = QtTrim(edit.value);
              if (entity->light.type.empty()) {
                entity->light.type = "point";
              }
            } else if (field == "color") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for light color");
              }
              entity->light.color = {
                  ClampFloat(value.x, 0.0f, 10.0f),
                  ClampFloat(value.y, 0.0f, 10.0f),
                  ClampFloat(value.z, 0.0f, 10.0f)};
            } else if (field == "direction") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for light direction");
              }
              entity->light.direction = {
                  ClampFloat(value.x, -1.0f, 1.0f),
                  ClampFloat(value.y, -1.0f, 1.0f),
                  ClampFloat(value.z, -1.0f, 1.0f)};
            } else if (field == "intensity" || field == "radius" ||
                       field == "beam_angle" || field == "blend") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light value");
              }
              if (field == "intensity") {
                entity->light.intensity = std::max(0.0f, value);
              } else if (field == "radius") {
                entity->light.radius = std::max(0.0f, value);
              } else if (field == "beam_angle") {
                entity->light.beam_angle_degrees = ClampFloat(value, 1.0f, 120.0f);
              } else {
                entity->light.blend = ClampFloat(value, 0.0f, 1.0f);
              }
            } else {
              return failEdit("unknown light property");
            }
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "camera") {
            if (!entity->has_camera) {
              return failEdit("entity has no camera");
            }
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric camera value");
            }
            const auto& field = parts[3];
            if (field == "fov") {
              entity->camera.fov = ClampFloat(value, 1.0f, 179.0f);
            } else if (field == "near_plane") {
              entity->camera.near_plane = std::max(0.001f, value);
              if (entity->camera.far_plane <= entity->camera.near_plane) {
                entity->camera.far_plane = entity->camera.near_plane + 1.0f;
              }
            } else if (field == "far_plane") {
              entity->camera.far_plane = std::max(value, entity->camera.near_plane + 0.001f);
            } else if (field == "focal_length_mm") {
              entity->camera.focal_length_mm = ClampFloat(value, 1.0f, 1000.0f);
            } else if (field == "sensor_width_mm") {
              entity->camera.sensor_width_mm = ClampFloat(value, 1.0f, 200.0f);
            } else if (field == "sensor_height_mm") {
              entity->camera.sensor_height_mm = ClampFloat(value, 1.0f, 200.0f);
            } else if (field == "aperture_radius") {
              entity->camera.aperture_radius = ClampFloat(value, 0.0f, 100.0f);
            } else if (field == "focus_distance") {
              entity->camera.focus_distance = ClampFloat(value, 0.0f, 100000.0f);
            } else if (field == "f_stop") {
              entity->camera.f_stop = ClampFloat(value, 0.0f, 256.0f);
            } else if (field == "shutter_seconds") {
              entity->camera.shutter_seconds = ClampFloat(value, 0.000001f, 60.0f);
            } else if (field == "iso") {
              entity->camera.iso = ClampFloat(value, 1.0f, 1048576.0f);
            } else if (field == "exposure_compensation") {
              entity->camera.exposure_compensation = ClampFloat(value, -32.0f, 32.0f);
            } else if (field == "white_balance_kelvin") {
              entity->camera.white_balance_kelvin = ClampFloat(value, 1000.0f, 40000.0f);
            } else if (field == "iris_blade_count") {
              entity->camera.iris_blade_count =
                  static_cast<std::uint32_t>(ClampFloat(std::round(value), 0.0f, 64.0f));
            } else if (field == "iris_rotation_degrees") {
              entity->camera.iris_rotation_degrees = ClampFloat(value, -360.0f, 360.0f);
            } else if (field == "iris_roundness") {
              entity->camera.iris_roundness = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "anamorphic_squeeze") {
              entity->camera.anamorphic_squeeze = ClampFloat(value, 0.01f, 100.0f);
            } else {
              return failEdit("unknown camera property");
            }
            changed = true;
            cameraAffecting = true;
            cameraAffectingEntityId = entityId;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "physics") {
            const auto& field = parts[3];
            if (field == "enabled") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              if (value || entity->has_physics_body) {
                auto& body = ensurePhysicsBody();
                body.enabled = value;
                changed = true;
              }
            } else if (field == "body_type") {
              auto& body = ensurePhysicsBody();
              std::string value = QtTrim(edit.value);
              if (value.empty()) {
                value = "static";
              }
              body.dynamic = value == "dynamic";
              body.body_type = value;
              changed = true;
            } else if (field == "shape") {
              auto& body = ensurePhysicsBody();
              body.shape = QtTrim(edit.value);
              if (body.shape.empty()) {
                body.shape = "box";
              }
              changed = true;
            } else if (field == "mass" ||
                       field == "friction" ||
                       field == "restitution" ||
                       field == "gravity_scale") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric physics value");
              }
              auto& body = ensurePhysicsBody();
              if (field == "mass") {
                body.mass = std::max(0.01f, value);
              } else if (field == "friction") {
                body.friction = std::max(0.0f, value);
              } else if (field == "restitution") {
                body.restitution = std::max(0.0f, value);
              } else {
                body.gravity_scale = value;
              }
              changed = true;
            } else if (field == "trigger" ||
                       field == "allow_sleeping" ||
                       field == "continuous_collision") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              auto& body = ensurePhysicsBody();
              if (field == "trigger") {
                body.trigger = value;
              } else if (field == "allow_sleeping") {
                body.allow_sleeping = value;
              } else {
                body.continuous_collision = value;
              }
              changed = true;
            } else {
              return failEdit("unknown physics property");
            }
            renderAffecting = changed;
          } else {
            return failEdit("unknown entity property");
          }
        } else if (parts.size() >= 3u && parts[0] == "sdf") {
          vkpt::core::StableId sdfId = 0u;
          if (!QtParseStableId(parts[1], sdfId)) {
            return failEdit("invalid SDF id");
          }
          auto* primitive = qtFindSdfPrimitive(sdfId);
          if (primitive == nullptr) {
            return failEdit("SDF primitive not found");
          }

          if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "transform") {
            if (parts.size() == 5u) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF transform value");
              }
              const auto& field = parts[3];
              const auto& component = parts[4];
              if (field == "translation") {
                if (component == "x") {
                  primitive->transform.translation.x = value;
                } else if (component == "y") {
                  primitive->transform.translation.y = value;
                } else if (component == "z") {
                  primitive->transform.translation.z = value;
                } else {
                  return failEdit("unknown SDF translation component");
                }
              } else if (field == "rotation") {
                if (component == "x") {
                  primitive->transform.rotation.x = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "y") {
                  primitive->transform.rotation.y = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "z") {
                  primitive->transform.rotation.z = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "w") {
                  primitive->transform.rotation.w = ClampFloat(value, -1.0f, 1.0f);
                } else {
                  return failEdit("unknown SDF rotation component");
                }
                const float lenSq =
                    primitive->transform.rotation.x * primitive->transform.rotation.x +
                    primitive->transform.rotation.y * primitive->transform.rotation.y +
                    primitive->transform.rotation.z * primitive->transform.rotation.z +
                    primitive->transform.rotation.w * primitive->transform.rotation.w;
                if (lenSq <= 1.0e-8f) {
                  primitive->transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                } else {
                  const float invLen = 1.0f / std::sqrt(lenSq);
                  primitive->transform.rotation.x *= invLen;
                  primitive->transform.rotation.y *= invLen;
                  primitive->transform.rotation.z *= invLen;
                  primitive->transform.rotation.w *= invLen;
                }
              } else if (field == "scale") {
                value = std::fabs(value) <= 1.0e-5f ? 1.0e-5f : value;
                if (component == "x") {
                  primitive->transform.scale.x = value;
                } else if (component == "y") {
                  primitive->transform.scale.y = value;
                } else if (component == "z") {
                  primitive->transform.scale.z = value;
                } else {
                  return failEdit("unknown SDF scale component");
                }
              } else {
                return failEdit("unknown SDF transform field");
              }
            } else if (parts[3] == "translation") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for SDF position");
              }
              primitive->transform.translation = value;
            } else if (parts[3] == "rotation") {
              vkpt::scene::Quat value{};
              if (!QtParseQuat(edit.value, value)) {
                return failEdit("expected four numbers for SDF rotation quaternion");
              }
              primitive->transform.rotation = value;
            } else if (parts[3] == "scale") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for SDF scale");
              }
              value.x = std::fabs(value.x) <= 1.0e-5f ? 1.0e-5f : value.x;
              value.y = std::fabs(value.y) <= 1.0e-5f ? 1.0e-5f : value.y;
              value.z = std::fabs(value.z) <= 1.0e-5f ? 1.0e-5f : value.z;
              primitive->transform.scale = value;
            } else {
              return failEdit("unknown SDF transform field");
            }
            primitive->transform.dirty = true;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 3u && parts[2] == "shape") {
            primitive->shape = QtTrim(edit.value);
            if (primitive->shape.empty()) {
              primitive->shape = "sphere";
            }
            primitive->primitive.shape = primitive->shape;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "primitive") {
            const auto& field = parts[3];
            if (field == "shape") {
              primitive->primitive.shape = QtTrim(edit.value);
              if (primitive->primitive.shape.empty()) {
                primitive->primitive.shape = "sphere";
              }
              primitive->shape = primitive->primitive.shape;
            } else if (field == "radius" || field == "param_a" || field == "param_b") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF value");
              }
              if (field == "radius") {
                primitive->primitive.radius = std::max(0.01f, value);
              } else if (field == "param_a") {
                primitive->primitive.param_a = value;
              } else {
                primitive->primitive.param_b = value;
              }
            } else {
              return failEdit("unknown SDF primitive property");
            }
            changed = true;
            renderAffecting = true;
          } else {
            return failEdit("unknown SDF property");
          }
        } else if ((parts.size() == 3u || parts.size() == 4u) && parts[0] == "material") {
          vkpt::core::StableId materialId = 0u;
          if (!QtParseStableId(parts[1], materialId)) {
            return failEdit("invalid material id");
          }
          auto* material = FindQtMutableSceneMaterial(qtSceneDocument, materialId);
          if (material == nullptr) {
            return failEdit("material not found");
          }

          const auto& field = parts[2];
          if (parts.size() == 4u && (field == "albedo" || field == "emission")) {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric material component");
            }
            auto& color = field == "albedo" ? material->albedo : material->emission;
            value = field == "albedo" ? ClampFloat(value, 0.0f, 1.0f) : std::max(0.0f, value);
            if (parts[3] == "x") {
              color.x = value;
            } else if (parts[3] == "y") {
              color.y = value;
            } else if (parts[3] == "z") {
              color.z = value;
            } else {
              return failEdit("unknown material color component");
            }
          } else if (parts.size() != 3u) {
            return failEdit("unknown material property component");
          } else if (field == "name") {
            material->name = QtTrim(edit.value);
          } else if (field == "family") {
            material->family = QtTrim(edit.value);
            if (material->family.empty()) {
              material->family = "diffuse";
            }
            vkpt::scene::ApplyMaterialFamilyPreset(
                *material,
                vkpt::scene::SceneMaterialPresetPolicy::Override);
          } else if (field == "albedo") {
            vkpt::scene::Vec3 value{};
            if (!QtParseVec3(edit.value, value)) {
              return failEdit("expected three numbers for base color");
            }
            material->albedo = value;
          } else if (field == "emission") {
            vkpt::scene::Vec3 value{};
            if (!QtParseVec3(edit.value, value)) {
              return failEdit("expected three numbers for emission");
            }
            material->emission = value;
          } else if (field == "roughness" ||
                     field == "metallic" ||
                     field == "ior" ||
                     field == "transmission" ||
                     field == "clearcoat" ||
                     field == "sheen" ||
                     field == "anisotropy" ||
                     field == "alpha" ||
                     field == "emission_intensity") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric material value");
            }
            if (field == "roughness") {
              material->roughness = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "metallic") {
              material->metallic = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "ior") {
              material->ior = std::max(1.01f, value);
            } else if (field == "transmission") {
              material->transmission = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "clearcoat") {
              material->clearcoat = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "sheen") {
              material->sheen = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "anisotropy") {
              material->anisotropy = ClampFloat(value, -1.0f, 1.0f);
            } else if (field == "alpha") {
              material->alpha = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "emission_intensity") {
              material->emission_intensity = std::max(0.0f, value);
            }
          } else if (field == "double_sided") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            material->double_sided = value;
          } else {
            return failEdit("unknown material property");
          }
          changed = true;
          renderAffecting = true;
        } else {
          return failEdit("unknown property id");
        }

        if (!changed) {
          qtDockPanelsDirty = true;
          return true;
        }

        bool reloadOk = true;
        if (settingsAffecting) {
          reloadOk = qtReloadRenderSettings("render settings edited");
        } else if (cameraAffecting) {
          reloadOk = qtReloadCameraSettings(
              cameraAffectingEntityId,
              "camera settings edited",
              cameraPoseAffecting);
          if (!reloadOk && renderAffecting) {
            reloadOk = qtReloadEditedScene("property edit");
          }
        } else if (renderAffecting && entityTransformAffecting &&
                   !transformTouchedIds.empty()) {
          auto transformBuild = qtBuildInstanceTransformUpdatesForTouchedEntities(
              transformTouchedIds,
              0u,
              qtScriptTransformRevision);
          if (!transformBuild.unsupported_instances &&
              !transformBuild.updates.empty() &&
              qtPublishInstanceTransformUpdates &&
              qtPublishInstanceTransformUpdates(
                  transformBuild.updates,
                  vkpt::pathtracer::MakeStandardTransformUpdateOptions(
                      vkpt::pathtracer::RenderUpdateReason::EditorGizmoMotion,
                      frameIndex,
                      "qt-property-edit"))) {
            reloadOk = true;
            qtPreviewStatus = "instance transform update";
          } else {
            reloadOk = qtReloadEditedScene("property edit");
          }
        } else if (renderAffecting) {
          reloadOk = qtReloadEditedScene("property edit");
        }
        qtDockPanelsDirty = true;
        if (reloadOk) {
          ui_runtime_state.status_message = "property edited: " + edit.property_id;
          ui_runtime_state.last_warning_or_error.clear();
        }
        PushUiEvent(ui_event_log,
                    reloadOk ? "dock_property_edit" : "dock_property_edit_failed",
                    edit.panel_id,
                    "dock",
                    frameIndex,
                    {},
                    edit.property_id,
                    edit.value);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        updateQtSelectionOverlay();
        return reloadOk;
      };
