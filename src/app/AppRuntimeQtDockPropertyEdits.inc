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
        bool hasExplicitOldNew = false;
        std::string explicitOldValue;
        std::string explicitNewValue;

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
            } else if (parts[2] == "run_live") {
              qtSetRuntimeMode(RuntimeMode::LiveEdit, frameIndex, "dock");
            } else if (parts[2] == "play") {
              qtSetRuntimeMode(RuntimeMode::Play, frameIndex, "dock");
            } else if (parts[2] == "pause") {
              qtSetRuntimeMode(RuntimeMode::Edit, frameIndex, "dock");
            } else if (parts[2] == "stop") {
              qtSetRuntimeMode(RuntimeMode::Edit, frameIndex, "dock");
            } else if (parts[2] == "send_viewport_input") {
              if (!qtIsLiveEditMode()) {
                return failEdit("viewport input can only be toggled in live edit");
              }
              qtToggleLiveEditMode(frameIndex, "dock");
            } else if (parts[2] == "step") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate, frameIndex, 1.0 / 60.0, "dock step");
            } else if (parts[2] == "reload") {
              qtReloadScriptsFromDocument("dock");
            } else if (parts[2] == "dispatch_on_load") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLoad, frameIndex, 0.0, "dock on_load");
            } else if (parts[2] == "dispatch_on_spawn") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnSpawn, frameIndex, 0.0, "dock on_spawn");
            } else if (parts[2] == "dispatch_on_enable") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnEnable, frameIndex, 0.0, "dock on_enable");
            } else if (parts[2] == "dispatch_on_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate, frameIndex, 1.0 / 60.0, "dock on_update");
            } else if (parts[2] == "dispatch_fixed_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnFixedUpdate, frameIndex, 1.0 / 60.0, "dock fixed_update");
            } else if (parts[2] == "dispatch_late_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLateUpdate, frameIndex, 1.0 / 60.0, "dock late_update");
            } else if (parts[2] == "dispatch_on_disable") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnDisable, frameIndex, 0.0, "dock on_disable");
            } else if (parts[2] == "dispatch_on_destroy") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnDestroy, frameIndex, 0.0, "dock on_destroy");
            } else if (parts[2] == "dispatch_on_unload") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUnload, frameIndex, 0.0, "dock on_unload");
            } else if (parts[2] == "variable" && parts.size() >= 6u) {
              if (!qtScriptRuntime) {
                return failEdit("script runtime unavailable");
              }
              vkpt::core::StableId entityId = 0u;
              if (!QtParseStableId(parts[3], entityId)) {
                return failEdit("invalid script variable entity");
              }
              const std::string scope = parts[4];
              std::string name = parts[5];
              for (std::size_t i = 6u; i < parts.size(); ++i) {
                name += "." + parts[i];
              }
              if (!qtScriptRuntime->set_variable_override(entityId, scope, name, edit.value)) {
                return failEdit("could not edit runtime variable");
              }
              for (auto& variable : qtScriptState.variables) {
                if (variable.entity == entityId && variable.scope == scope && variable.name == name) {
                  variable.value = edit.value;
                }
              }
              qtScriptState.status = "runtime variable edited: " + name;
              ui_runtime_state.status_message = qtScriptState.status;
            } else {
              return failEdit("unknown script runtime property");
            }
          } else if (parts.size() >= 3u && parts[1] == "audio") {
            if (!qtAudioSystem) {
              return failEdit("audio system unavailable");
            }
            if (parts[2] == "play_scene") {
              if (!qtScriptsRunning()) {
                qtSetGameMode(true, frameIndex, "script audio");
              } else {
                qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnEnable,
                                     frameIndex,
                                     0.0,
                                     "script audio");
              }
              qtScriptState.status = "scripted scene audio playing";
              ui_runtime_state.status_message = qtScriptState.status;
            } else if (parts[2] == "pause_scene") {
              qtAudioSystem->stop_all();
              qtScriptState.status = "scripted scene audio paused";
              ui_runtime_state.status_message = qtScriptState.status;
            } else if ((parts[2] == "volume" || parts[2] == "muted") && parts.size() >= 4u) {
              std::string bus = parts[3];
              for (std::size_t i = 4u; i < parts.size(); ++i) {
                bus += "." + parts[i];
              }
              if (parts[2] == "volume") {
                float value = 1.0f;
                if (!QtParseFloat(edit.value, value)) {
                  return failEdit("expected audio volume number");
                }
                qtAudioSystem->set_bus_volume(bus, value);
                qtScriptState.status = "audio volume set: " + bus;
              } else {
                bool value = false;
                if (!QtParseBool(edit.value, value)) {
                  return failEdit("expected true or false");
                }
                qtAudioSystem->set_bus_muted(bus, value);
                qtScriptState.status = value ? ("audio muted: " + bus) : ("audio unmuted: " + bus);
              }
              ui_runtime_state.status_message = qtScriptState.status;
            } else {
              return failEdit("unknown script audio property");
            }
            qtScriptState.audio = qtAudioSystem->diagnostics();
          } else if (parts.size() >= 3u && parts[1] == "selection") {
            const auto selectedId = ui_selection_state.active_primary_entity != 0u
                ? ui_selection_state.active_primary_entity
                : (ui_selection_state.selected_entity_ids.empty()
                       ? 0u
                       : ui_selection_state.selected_entity_ids.front());
            if (selectedId == 0u) {
              return failEdit("no selected entity");
            }
            auto* entity = qtFindEntity(selectedId);
            if (entity == nullptr) {
              return failEdit("selected entity not found");
            }
            if (parts[2] == "attach") {
              if (entity->script.script.empty()) {
                entity->script.script = "assets/scripts/new_script.lua";
              }
              entity->script.language = entity->script.language.empty() ? "lua" : entity->script.language;
              entity->script.entry = entity->script.entry.empty() ? "default" : entity->script.entry;
              entity->script.enabled = true;
              qtReloadScriptsFromDocument("script attached");
            } else if (parts[2] == "detach") {
              entity->script = {};
              qtReloadScriptsFromDocument("script detached");
            } else if (parts[2] == "open") {
              if (entity->script.script.empty()) {
                return failEdit("selected entity has no script path");
              }
              const auto target = std::filesystem::path(entity->script.script);
              if (!OpenPathInExplorer(target)) {
                return failEdit("could not open selected script");
              }
              ui_runtime_state.status_message = "opened script: " + entity->script.script;
            } else {
              return failEdit("unknown script selection action");
            }
          } else if (parts.size() >= 3u && parts[1] == "scene_init") {
            auto sceneInitId = [&]() {
              std::string raw = qtSceneDocument.metadata.scene_name;
              if (raw.empty()) {
                raw = std::filesystem::path(ui_runtime_state.active_scene).stem().generic_string();
              }
              if (raw.empty() || raw == "none") {
                raw = "current_scene";
              }
              std::string out;
              out.reserve(raw.size());
              for (const unsigned char ch : raw) {
                if (std::isalnum(ch) != 0) {
                  out.push_back(static_cast<char>(std::tolower(ch)));
                } else if (!out.empty() && out.back() != '_') {
                  out.push_back('_');
                }
              }
              while (!out.empty() && out.back() == '_') {
                out.pop_back();
              }
              return out.empty() ? std::string("current_scene") : out;
            };
            auto sceneInitPath = [&]() {
              return std::filesystem::path("assets/scripts/scenes") / sceneInitId() / "init.lua";
            };
            auto ensureSceneInitFile = [&](const std::filesystem::path& scriptPath) {
              std::error_code ec;
              std::filesystem::create_directories(scriptPath.parent_path(), ec);
              if (ec) {
                return false;
              }
              if (std::filesystem::exists(scriptPath)) {
                return true;
              }
              std::ofstream out(scriptPath, std::ios::binary);
              if (!out) {
                return false;
              }
              out << "local script = {}\n\n"
                     "function script.on_load(self, ctx)\n"
                     "  if ctx ~= nil and type(ctx.diagnostic) == \"function\" then\n"
                     "    ctx:diagnostic(\"info\", \"scene init loaded\")\n"
                     "  end\n"
                     "end\n\n"
                     "return script\n";
              return true;
            };
            auto assignSceneInit = [&](const std::filesystem::path& scriptPath, bool enabled) {
              qtSceneDocument.has_scene_script = true;
              qtSceneDocument.scene_script.script = scriptPath.generic_string();
              qtSceneDocument.scene_script.language = "lua";
              qtSceneDocument.scene_script.entry = "default";
              qtSceneDocument.scene_script.module_id = sceneInitId();
              qtSceneDocument.scene_script.enabled = enabled;
              qtSceneDocument.scene_script.reload_on_save = true;
            };
            if (parts[2] == "create") {
              const auto scriptPath = sceneInitPath();
              if (!ensureSceneInitFile(scriptPath)) {
                return failEdit("could not create scene init script");
              }
              assignSceneInit(scriptPath, true);
              qtReloadScriptsFromDocument("scene init created");
              ui_runtime_state.status_message = "created scene init: " + scriptPath.generic_string();
            } else if (parts[2] == "open") {
              if (qtSceneDocument.scene_script.script.empty()) {
                return failEdit("scene has no init script");
              }
              if (!OpenPathInExplorer(std::filesystem::path(qtSceneDocument.scene_script.script))) {
                return failEdit("could not open scene init script");
              }
              ui_runtime_state.status_message = "opened scene init: " + qtSceneDocument.scene_script.script;
            } else if (parts[2] == "bake_default_fps") {
              const auto decision = vkpt::scene::DecideSceneScriptBootstrap(qtSceneDocument);
              vkpt::scene::ScriptComponent script;
              script.script = "assets/scripts/systems/generic_fps_camera.lua";
              script.language = "lua";
              script.entry = "default";
              script.module_id = "systems.generic_fps_camera";
              script.enabled = true;
              script.reload_on_save = true;
              script.params["baked_from"] = "default_fps_fallback";
              if (decision.fallback.operation ==
                      vkpt::scene::SceneScriptBootstrapFallbackOperation::AttachDefaultFpsToMainCamera &&
                  decision.fallback.target_entity_id != 0u) {
                auto* entity = qtFindEntity(decision.fallback.target_entity_id);
                if (entity == nullptr) {
                  return failEdit("fallback target camera is not an authored entity");
                }
                entity->script = script;
              } else if (decision.fallback.operation ==
                         vkpt::scene::SceneScriptBootstrapFallbackOperation::SpawnDefaultFpsCamera) {
                vkpt::scene::SceneEntityDefinition entity;
                entity.id = 9'000'000'000ull;
                for (const auto& existing : qtSceneDocument.entities) {
                  if (existing.id >= entity.id) {
                    entity.id = existing.id + 1u;
                  }
                }
                entity.name = "Runtime FPS Camera";
                entity.has_transform = true;
                entity.transform.translation = {0.0f, 1.7f, 4.0f};
                entity.has_camera = true;
                entity.camera.fov = 65.0f;
                entity.camera.focus_distance = 4.0f;
                entity.script = script;
                qtSceneDocument.entities.push_back(std::move(entity));
              } else {
                return failEdit("default FPS fallback is not active for this scene");
              }
              qtReloadScriptsFromDocument("default FPS baked");
              ui_runtime_state.status_message = "baked default FPS script into scene";
            } else if (parts[2] == "disable_fallback") {
              const auto scriptPath = sceneInitPath();
              if (!ensureSceneInitFile(scriptPath)) {
                return failEdit("could not create disabled scene init script");
              }
              assignSceneInit(scriptPath, false);
              qtReloadScriptsFromDocument("default fallback disabled");
              ui_runtime_state.status_message = "disabled default bootstrap fallback";
            } else {
              return failEdit("unknown scene init action");
            }
          } else if (parts.size() >= 3u && parts[1] == "actions") {
            if (parts[2] == "open_folder") {
              if (!OpenPathInExplorer(std::filesystem::path("assets/scripts"))) {
                return failEdit("could not open script folder");
              }
              ui_runtime_state.status_message = "opened script folder";
            } else if (parts[2] == "new_lua_script") {
              const auto scriptDir = std::filesystem::path("assets/scripts");
              std::error_code ec;
              std::filesystem::create_directories(scriptDir, ec);
              if (ec) {
                return failEdit("could not create assets/scripts");
              }
              std::filesystem::path scriptPath = scriptDir / "new_lua_script.lua";
              for (int suffix = 2; std::filesystem::exists(scriptPath) && suffix < 1000; ++suffix) {
                scriptPath = scriptDir / ("new_lua_script_" + std::to_string(suffix) + ".lua");
              }
              if (std::filesystem::exists(scriptPath)) {
                return failEdit("could not allocate script filename");
              }
              {
                std::ofstream out(scriptPath, std::ios::binary);
                if (!out) {
                  return failEdit("could not create Lua script file");
                }
                out << "local script = {}\n\n"
                       "function script.on_update(self, ctx)\n"
                       "  local transform = self:get_transform()\n"
                       "  if transform == nil then\n"
                       "    return\n"
                       "  end\n"
                       "\n"
                       "  -- Add command-buffered gameplay writes here.\n"
                       "end\n\n"
                       "return script\n";
              }
              const auto selectedId = ui_selection_state.active_primary_entity != 0u
                  ? ui_selection_state.active_primary_entity
                  : (ui_selection_state.selected_entity_ids.empty()
                         ? 0u
                         : ui_selection_state.selected_entity_ids.front());
              if (selectedId != 0u) {
                if (auto* entity = qtFindEntity(selectedId); entity != nullptr) {
                  entity->script.script = scriptPath.generic_string();
                  entity->script.language = "lua";
                  entity->script.entry = entity->script.entry.empty() ? "default" : entity->script.entry;
                  entity->script.module_id = entity->script.module_id.empty() ? "default" : entity->script.module_id;
                  entity->script.enabled = true;
                  entity->script.reload_on_save = true;
                  qtReloadScriptsFromDocument("new Lua script");
                }
              }
              ui_runtime_state.status_message = "created Lua script: " + scriptPath.generic_string();
            } else {
              return failEdit("unknown script action");
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
          } else if (parts.size() >= 4u && parts[2] == "script") {
            const auto& field = parts[3];
            if (field == "path") {
              hasExplicitOldNew = true;
              explicitOldValue = entity->script.script;
              entity->script.script = QtTrim(edit.value);
              explicitNewValue = entity->script.script;
            } else if (field == "language") {
              hasExplicitOldNew = true;
              explicitOldValue = entity->script.language;
              entity->script.language = QtTrim(edit.value);
              if (entity->script.language.empty()) {
                entity->script.language = "lua";
              }
              explicitNewValue = entity->script.language;
            } else if (field == "entry") {
              hasExplicitOldNew = true;
              explicitOldValue = entity->script.entry;
              entity->script.entry = QtTrim(edit.value);
              if (entity->script.entry.empty()) {
                entity->script.entry = "default";
              }
              explicitNewValue = entity->script.entry;
            } else if (field == "module_id") {
              hasExplicitOldNew = true;
              explicitOldValue = entity->script.module_id;
              entity->script.module_id = QtTrim(edit.value);
              if (entity->script.module_id.empty()) {
                entity->script.module_id = "default";
              }
              explicitNewValue = entity->script.module_id;
            } else if (field == "enabled" || field == "reload_on_save") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              hasExplicitOldNew = true;
              if (field == "enabled") {
                explicitOldValue = entity->script.enabled ? "true" : "false";
                entity->script.enabled = value;
              } else {
                explicitOldValue = entity->script.reload_on_save ? "true" : "false";
                entity->script.reload_on_save = value;
              }
              explicitNewValue = value ? "true" : "false";
            } else if (field == "param" && parts.size() >= 5u) {
              std::string key = parts[4];
              for (std::size_t i = 5u; i < parts.size(); ++i) {
                key += "." + parts[i];
              }
              if (key.empty()) {
                return failEdit("empty script param name");
              }
              hasExplicitOldNew = true;
              if (const auto existing = entity->script.params.find(key); existing != entity->script.params.end()) {
                explicitOldValue = existing->second;
              }
              entity->script.params[key] = QtTrim(edit.value);
              explicitNewValue = entity->script.params[key];
            } else {
              return failEdit("unknown script property");
            }
            changed = true;
            qtReloadScriptsFromDocument("script property edited");
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
                    hasExplicitOldNew ? std::string_view(explicitOldValue) : std::string_view{},
                    hasExplicitOldNew ? std::string_view(explicitNewValue) : std::string_view(edit.property_id),
                    hasExplicitOldNew ? edit.property_id : edit.value);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        updateQtSelectionOverlay();
        return reloadOk;
      };
