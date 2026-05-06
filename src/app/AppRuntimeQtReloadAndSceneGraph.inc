      std::function<bool(std::string_view)> qtReloadEditedScene;
      std::function<bool(const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>&)>
          qtPublishInstanceTransformUpdates;
      uint32_t qtScriptTransformRevision = 1u;
      struct QtInstanceTransformBuildResult {
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        bool unsupported_instances = false;
      };
      auto qtBuildInstanceTransformUpdatesForTouchedEntities =
          [&](const std::unordered_set<vkpt::core::StableId>& transformTouchedIds,
              uint32_t baseFlags,
              uint32_t& transformRevision) {
        QtInstanceTransformBuildResult result;
        if (transformTouchedIds.empty()) {
          return result;
        }

        std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneEntityDefinition*>
            entitiesById;
        entitiesById.reserve(qtSceneDocument.entities.size());
        for (const auto& entity : qtSceneDocument.entities) {
          entitiesById.emplace(entity.id, &entity);
        }

        auto hasTouchedTransformAncestor =
            [&](const vkpt::scene::SceneEntityDefinition& entity) {
          if (transformTouchedIds.contains(entity.id)) {
            return true;
          }
          std::unordered_set<vkpt::core::StableId> visited;
          vkpt::core::StableId parent =
              entity.has_hierarchy ? entity.hierarchy.parent : 0u;
          while (parent != 0u && visited.insert(parent).second) {
            if (transformTouchedIds.contains(parent)) {
              return true;
            }
            const auto parentIt = entitiesById.find(parent);
            if (parentIt == entitiesById.end()) {
              break;
            }
            const auto* parentEntity = parentIt->second;
            parent = parentEntity->has_hierarchy ? parentEntity->hierarchy.parent : 0u;
          }
          return false;
        };

        result.updates.reserve(qtScene.instances.size());
        const auto worldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
        const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
        for (std::size_t index = 0; index < qtScene.instances.size(); ++index) {
          const auto& instance = qtScene.instances[index];
          const auto entityIt = entitiesById.find(instance.entity_id);
          if (entityIt == entitiesById.end() || entityIt->second == nullptr) {
            continue;
          }
          if (!hasTouchedTransformAncestor(*entityIt->second)) {
            continue;
          }
          const bool dynamicCompatible =
              instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform) &&
              instance.local_vertex_count > 0u &&
              instance.local_index_count >= 3u;
          if (!dynamicCompatible) {
            result.unsupported_instances = true;
            continue;
          }
          const auto worldTransform =
              ResolveEntityWorldTransform(*entityIt->second, world);
          vkpt::pathtracer::RTInstanceTransformUpdate update;
          update.entity_id = instance.entity_id;
          update.instance_index = static_cast<uint32_t>(index);
          update.flags = baseFlags |
                         vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                         vkpt::pathtracer::kRTInstanceFlagTransformDirty;
          update.transform_revision = transformRevision++;
          update.translation = ToPtVec3(worldTransform.translation);
          update.rotation = ToPtQuat4(worldTransform.rotation);
          update.scale = ToPtVec3(worldTransform.scale);
          result.updates.push_back(update);
        }
        std::sort(result.updates.begin(),
                  result.updates.end(),
                  [](const vkpt::pathtracer::RTInstanceTransformUpdate& lhs,
                     const vkpt::pathtracer::RTInstanceTransformUpdate& rhs) {
                    if (lhs.instance_index != rhs.instance_index) {
                      return lhs.instance_index < rhs.instance_index;
                    }
                    return lhs.entity_id < rhs.entity_id;
                  });
        return result;
      };
      auto qtRefreshSelectionBoundsFromRtScene = [&]() {
        ui_selection_state.per_item_bounds.clear();
        ui_selection_state.per_item_bounds.reserve(
            ui_selection_state.selected_entity_ids.size());
        ui_selection_state.aggregate_bounds = {};
        for (const auto selectedId : ui_selection_state.selected_entity_ids) {
          auto bounds = vkpt::editor::Bounds{};
          bool found = false;
          for (const auto& instance : qtScene.instances) {
            if (instance.entity_id != selectedId) {
              continue;
            }
            const uint64_t firstIndex =
                static_cast<uint64_t>(instance.first_triangle) * 3ull;
            const uint64_t indexCount =
                static_cast<uint64_t>(instance.triangle_count) * 3ull;
            if (firstIndex > qtScene.indices.size() ||
                indexCount > qtScene.indices.size() - firstIndex) {
              continue;
            }
            for (uint64_t offset = 0u; offset < indexCount; ++offset) {
              const uint32_t vertexIndex =
                  qtScene.indices[static_cast<std::size_t>(firstIndex + offset)];
              if (vertexIndex >= qtScene.vertices.size()) {
                continue;
              }
              ExpandBounds(bounds, qtScene.vertices[vertexIndex]);
              ExpandBounds(ui_selection_state.aggregate_bounds,
                           qtScene.vertices[vertexIndex]);
              found = true;
            }
          }
          if (found && bounds.valid) {
            ui_selection_state.per_item_bounds.push_back({selectedId, bounds});
          }
        }
      };
      auto qtLastViewportPickablesRefresh = std::chrono::steady_clock::time_point{};
      bool qtViewportPickablesRefreshPending = false;
      constexpr auto kQtTransformPickablesRefreshInterval =
          std::chrono::milliseconds(100);
      auto qtRefreshViewportPickablesNow = [&]() {
        qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtViewportPickablesRefreshPending = false;
        qtLastViewportPickablesRefresh = std::chrono::steady_clock::now();
        updateQtSelectionOverlay();
      };
      auto qtScheduleViewportPickablesRefresh = [&](bool immediate) {
        if (immediate) {
          qtRefreshViewportPickablesNow();
          return;
        }
        qtViewportPickablesRefreshPending = true;
        qtRefreshSelectionBoundsFromRtScene();
        updateQtSelectionOverlay();
      };
      auto qtMaybeRefreshPendingViewportPickables =
          [&](std::chrono::steady_clock::time_point now) {
        if (!qtViewportPickablesRefreshPending || qtGizmoDrag.active) {
          return;
        }
        if (qtLastViewportPickablesRefresh == std::chrono::steady_clock::time_point{} ||
            now - qtLastViewportPickablesRefresh >=
                kQtTransformPickablesRefreshInterval) {
          qtRefreshViewportPickablesNow();
        }
      };
      auto qtReloadScriptsFromDocument = [&](std::string_view reason) {
        auto worldResult = qtSceneDocument.to_world();
        if (!worldResult) {
          qtScriptState.status = "world build failed";
          ui_runtime_state.last_warning_or_error = "script reload failed: world build";
          qtDockPanelsDirty = true;
          return false;
        }
        auto world = std::move(worldResult.value());
        world.recompute_world_transforms();
        qtScriptState.binding_summary = qtScriptRuntime->reload_bindings(world);
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.status = std::string("bindings reloaded") +
            (reason.empty() ? std::string{} : " (" + std::string(reason) + ")");
        qtScriptState.last_hook = "reload";
        qtScriptState.last_frame = qtFrameCount;
        qtDockPanelsDirty = true;
        return true;
      };
      auto qtApplyScriptCommandsToDocument =
          [&](const vkpt::scene::WorldCommandBuffer& commands) {
        if (commands.commands().empty()) {
          return false;
        }
        std::size_t createCount = 0u;
        std::size_t destroyCount = 0u;
        std::size_t setTransformCount = 0u;
        std::size_t assignCameraCount = 0u;
        std::size_t assignLightCount = 0u;
        std::unordered_set<vkpt::core::StableId> transformTouchedIds;
        bool cameraStateTouched = false;
        bool cameraTransformTouched = false;
        bool fullSceneReloadNeeded = false;
        bool scriptBindingsDirty = false;
        bool physicsTransformTouched = false;
        bool touchesHeroRoot = false;
        bool touchesActionCamera = false;
        bool touchesActionLight = false;
        auto markTransformTouched = [&](vkpt::core::StableId id) {
          transformTouchedIds.insert(id);
          touchesHeroRoot = touchesHeroRoot || id == 9110u;
          touchesActionCamera = touchesActionCamera || id == 9101u;
          if (auto* entity = qtFindEntity(id)) {
            cameraTransformTouched = cameraTransformTouched || entity->has_camera;
            fullSceneReloadNeeded = fullSceneReloadNeeded ||
                                    entity->has_light ||
                                    entity->has_sdf_primitive;
          }
        };
        auto classifyComponentMutation =
            [&](vkpt::core::StableId id, vkpt::scene::ComponentKind kind, bool removing) {
          switch (kind) {
            case vkpt::scene::ComponentKind::Identity:
              scriptBindingsDirty = true;
              break;
            case vkpt::scene::ComponentKind::Transform:
              if (removing) {
                fullSceneReloadNeeded = true;
              } else {
                markTransformTouched(id);
              }
              break;
            case vkpt::scene::ComponentKind::Camera:
              cameraStateTouched = cameraStateTouched || !removing;
              fullSceneReloadNeeded = fullSceneReloadNeeded || removing;
              break;
            case vkpt::scene::ComponentKind::Script:
              scriptBindingsDirty = true;
              break;
            case vkpt::scene::ComponentKind::Hierarchy:
            case vkpt::scene::ComponentKind::Light:
            case vkpt::scene::ComponentKind::MeshRenderer:
            case vkpt::scene::ComponentKind::SdfPrimitive:
            case vkpt::scene::ComponentKind::MaterialOverride:
            case vkpt::scene::ComponentKind::PhysicsBody:
            case vkpt::scene::ComponentKind::Animation:
              fullSceneReloadNeeded = true;
              break;
            case vkpt::scene::ComponentKind::BenchmarkTag:
            case vkpt::scene::ComponentKind::Count:
              break;
          }
        };
        for (const auto& command : commands.commands()) {
          std::visit([&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::CreateEntityCommand>) {
              ++createCount;
              fullSceneReloadNeeded = true;
              scriptBindingsDirty = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::DestroyEntityCommand>) {
              ++destroyCount;
              fullSceneReloadNeeded = true;
              scriptBindingsDirty = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetComponentCommand> ||
                                 std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AddComponentCommand>) {
              classifyComponentMutation(payload.id, payload.kind, false);
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::RemoveComponentCommand>) {
              classifyComponentMutation(payload.id, payload.kind, true);
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetTransformCommand>) {
              ++setTransformCount;
              markTransformTouched(payload.id);
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReparentEntityCommand>) {
              fullSceneReloadNeeded = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignMaterialCommand>) {
              fullSceneReloadNeeded = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignCameraCommand>) {
              ++assignCameraCount;
              cameraStateTouched = true;
              touchesActionCamera = touchesActionCamera || payload.id == 9101u;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignLightCommand>) {
              ++assignLightCount;
              fullSceneReloadNeeded = true;
              touchesActionLight = touchesActionLight || payload.id == 9103u;
            }
          }, command.payload);
        }
        auto hasTouchedTransformAncestor =
            [&](const vkpt::scene::SceneEntityDefinition& entity) {
          if (transformTouchedIds.contains(entity.id)) {
            return true;
          }
          std::unordered_set<vkpt::core::StableId> visited;
          vkpt::core::StableId parent =
              entity.has_hierarchy ? entity.hierarchy.parent : 0u;
          while (parent != 0u && visited.insert(parent).second) {
            if (transformTouchedIds.contains(parent)) {
              return true;
            }
            const auto parentIt = std::find_if(
                qtSceneDocument.entities.begin(),
                qtSceneDocument.entities.end(),
                [&](const vkpt::scene::SceneEntityDefinition& candidate) {
                  return candidate.id == parent;
                });
            if (parentIt == qtSceneDocument.entities.end()) {
              break;
            }
            parent = parentIt->has_hierarchy ? parentIt->hierarchy.parent : 0u;
          }
          return false;
        };
        if (!transformTouchedIds.empty()) {
          for (const auto& entity : qtSceneDocument.entities) {
            if ((entity.has_light || entity.has_sdf_primitive) &&
                hasTouchedTransformAncestor(entity)) {
              fullSceneReloadNeeded = true;
            }
            if (entity.has_camera && hasTouchedTransformAncestor(entity)) {
              cameraTransformTouched = true;
            }
            if (entity.has_physics_body && hasTouchedTransformAncestor(entity)) {
              physicsTransformTouched = true;
            }
          }
        }
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "applying Lua script commands to scene document",
                   {{"commands", std::to_string(commands.commands().size())},
                    {"create", std::to_string(createCount)},
                    {"destroy", std::to_string(destroyCount)},
                    {"set_transform", std::to_string(setTransformCount)},
                    {"assign_camera", std::to_string(assignCameraCount)},
                    {"assign_light", std::to_string(assignLightCount)},
                    {"transform_entities", std::to_string(transformTouchedIds.size())},
                    {"camera_state_touched", cameraStateTouched ? "true" : "false"},
                    {"camera_transform_touched", cameraTransformTouched ? "true" : "false"},
                    {"physics_transform_touched", physicsTransformTouched ? "true" : "false"},
                    {"full_scene_reload_needed", fullSceneReloadNeeded ? "true" : "false"},
                    {"script_bindings_dirty", scriptBindingsDirty ? "true" : "false"},
                    {"touches_hero_root_9110", touchesHeroRoot ? "true" : "false"},
                    {"touches_action_camera_9101", touchesActionCamera ? "true" : "false"},
                    {"touches_action_light_9103", touchesActionLight ? "true" : "false"}},
                   qtFrameCount);

        auto removeEntitySet = [&](const std::unordered_set<vkpt::core::StableId>& ids) {
          qtSceneDocument.entities.erase(
              std::remove_if(qtSceneDocument.entities.begin(),
                             qtSceneDocument.entities.end(),
                             [&](const vkpt::scene::SceneEntityDefinition& entity) {
                               return ids.contains(entity.id);
                             }),
              qtSceneDocument.entities.end());
        };
        auto collectSubtree = [&](vkpt::core::StableId root) {
          std::unordered_set<vkpt::core::StableId> ids;
          std::function<void(vkpt::core::StableId)> visit =
              [&](vkpt::core::StableId id) {
            if (!ids.insert(id).second) {
              return;
            }
            for (const auto& entity : qtSceneDocument.entities) {
              if (entity.has_hierarchy && entity.hierarchy.parent == id) {
                visit(entity.id);
              }
            }
          };
          visit(root);
          return ids;
        };
        auto applyComponent =
            [&](vkpt::scene::SceneEntityDefinition& entity,
                vkpt::scene::ComponentKind kind,
                const vkpt::scene::ComponentVariant& component) {
          std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, vkpt::scene::IdentityComponent>) {
              (void)kind;
              entity.name = value.name;
            } else if constexpr (std::is_same_v<T, vkpt::scene::TransformComponent>) {
              entity.has_transform = true;
              entity.transform = value;
              entity.transform.dirty = true;
              qtRemoveLegacyTransformEntry(entity.id);
            } else if constexpr (std::is_same_v<T, vkpt::scene::HierarchyComponent>) {
              entity.has_hierarchy = true;
              entity.hierarchy = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::CameraComponent>) {
              entity.has_camera = true;
              entity.camera = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::LightComponent>) {
              entity.has_light = true;
              entity.light = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::MeshRendererComponent>) {
              entity.has_mesh = true;
              entity.mesh = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::SdfPrimitiveComponent>) {
              entity.has_sdf_primitive = true;
              entity.sdf_primitive = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::MaterialOverrideComponent>) {
              entity.material = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::PhysicsBodyComponent>) {
              entity.has_physics_body = true;
              entity.physics_body = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::ScriptComponent>) {
              entity.script = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::AnimationComponent>) {
              entity.animation = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::BenchmarkTagComponent>) {
              entity.has_benchmark_tag = true;
              entity.benchmark_tag = value;
            }
          }, component);
        };
        auto removeComponent = [&](vkpt::scene::SceneEntityDefinition& entity,
                                   vkpt::scene::ComponentKind kind) {
          switch (kind) {
            case vkpt::scene::ComponentKind::Transform:
              entity.has_transform = false;
              break;
            case vkpt::scene::ComponentKind::Hierarchy:
              entity.has_hierarchy = false;
              entity.hierarchy = {};
              break;
            case vkpt::scene::ComponentKind::Camera:
              entity.has_camera = false;
              entity.camera = {};
              break;
            case vkpt::scene::ComponentKind::Light:
              entity.has_light = false;
              entity.light = {};
              break;
            case vkpt::scene::ComponentKind::MeshRenderer:
              entity.has_mesh = false;
              entity.mesh = {};
              break;
            case vkpt::scene::ComponentKind::SdfPrimitive:
              entity.has_sdf_primitive = false;
              entity.sdf_primitive = {};
              break;
            case vkpt::scene::ComponentKind::MaterialOverride:
              entity.material = {};
              break;
            case vkpt::scene::ComponentKind::PhysicsBody:
              entity.has_physics_body = false;
              entity.physics_body = {};
              break;
            case vkpt::scene::ComponentKind::Script:
              entity.script = {};
              break;
            case vkpt::scene::ComponentKind::Animation:
              entity.animation = {};
              break;
            case vkpt::scene::ComponentKind::BenchmarkTag:
              entity.has_benchmark_tag = false;
              entity.benchmark_tag = {};
              break;
            case vkpt::scene::ComponentKind::Identity:
            case vkpt::scene::ComponentKind::Count:
              break;
          }
        };

        bool changed = false;
        for (const auto& command : commands.commands()) {
          std::visit([&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::CreateEntityCommand>) {
              const auto id = payload.requested_id != 0u ? payload.requested_id : qtNextSceneObjectId();
              auto& entity = qtEnsureSceneObjectEntity(id, payload.name);
              entity.name = payload.name.empty() ? entity.name : payload.name;
              if (payload.requested_parent != 0u) {
                entity.has_hierarchy = true;
                entity.hierarchy.parent = payload.requested_parent;
              }
              changed = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::DestroyEntityCommand>) {
              if (payload.destroy_children) {
                removeEntitySet(collectSubtree(payload.id));
              } else {
                removeEntitySet(std::unordered_set<vkpt::core::StableId>{payload.id});
              }
              changed = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetComponentCommand> ||
                                 std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AddComponentCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Entity");
              applyComponent(entity, payload.kind, payload.component);
              changed = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::RemoveComponentCommand>) {
              if (auto* entity = qtFindEntity(payload.id)) {
                removeComponent(*entity, payload.kind);
                changed = true;
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetTransformCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Entity");
              entity.has_transform = true;
              entity.transform = payload.transform;
              entity.transform.dirty = true;
              qtRemoveLegacyTransformEntry(payload.id);
              changed = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReparentEntityCommand>) {
              if (auto* entity = qtFindEntity(payload.child)) {
                entity->has_hierarchy = true;
                entity->hierarchy.parent = payload.parent;
                changed = true;
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReorderSiblingCommand>) {
              (void)payload;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignMaterialCommand>) {
              if (auto* entity = qtFindEntity(payload.id)) {
                if (entity->has_mesh) {
                  entity->mesh.material_id = payload.material_id;
                } else {
                  entity->material.material_id = payload.material_id;
                }
                changed = true;
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignLightCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Light");
              entity.has_light = true;
              entity.light = payload.light;
              changed = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignCameraCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Camera");
              entity.has_camera = true;
              entity.camera = payload.camera;
              changed = true;
            }
          }, command.payload);
        }

        if (!changed) {
          logger.log(vkpt::log::Severity::Warning,
                     "scripts",
                     "Lua script commands did not mutate scene document",
                     {{"commands", std::to_string(commands.commands().size())}},
                     qtFrameCount);
          return false;
        }

        if (!fullSceneReloadNeeded) {
          bool incrementalOk = true;
          bool postedTransforms = false;
          bool postedCamera = false;
          std::size_t transformUpdateCount = 0u;

          if (!transformTouchedIds.empty()) {
            auto transformBuild = qtBuildInstanceTransformUpdatesForTouchedEntities(
                transformTouchedIds,
                0u,
                qtScriptTransformRevision);
            transformUpdateCount = transformBuild.updates.size();
            incrementalOk = incrementalOk && !transformBuild.unsupported_instances;

            if (incrementalOk && !transformBuild.updates.empty()) {
              incrementalOk = qtPublishInstanceTransformUpdates &&
                              qtPublishInstanceTransformUpdates(transformBuild.updates);
              postedTransforms = incrementalOk;
            }
          }

          if (incrementalOk && (cameraStateTouched || cameraTransformTouched)) {
            auto* activeCamera = qtFindActiveCameraEntity();
            incrementalOk = activeCamera != nullptr && activeCamera->has_camera;
            if (incrementalOk) {
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

              if (cameraTransformTouched && activeCamera->has_transform) {
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
                qtRenderCoordinator->post_camera_state(cameraState);
                postedCamera = true;
              } else if (!qtUseBg && qtTracerReady) {
                bool cameraOk = qtTracer->update_camera_state(cameraState);
                if (!cameraOk) {
                  cameraOk = qtTracer->load_scene_snapshot(qtScene) &&
                             qtTracer->build_or_update_acceleration();
                }
                qtTracerReady = cameraOk && qtTracer->reset_accumulation();
                qtSampleIndex = 0u;
                postedCamera = qtTracerReady;
              } else {
                incrementalOk = false;
              }
            }
          }

          if (incrementalOk) {
            if (scriptBindingsDirty) {
              qtReloadScriptsFromDocument("script command");
            }
            if (physicsTransformTouched) {
              qtSyncPhysicsFromSceneDocument();
            }
            qtDockPanelsDirty = true;
            qtPreviewStatus = "lua playable";
            logger.log(vkpt::log::Severity::Info,
                       "scripts",
                       "Lua script commands applied incrementally",
                       {{"commands", std::to_string(commands.commands().size())},
                        {"transform_updates", postedTransforms ? "true" : "false"},
                        {"transform_update_count", std::to_string(transformUpdateCount)},
                        {"camera_update", postedCamera ? "true" : "false"},
                        {"script_bindings_reloaded", scriptBindingsDirty ? "true" : "false"}},
                       qtFrameCount);
            return true;
          }
        }

        if (qtReloadEditedScene) {
          qtReloadEditedScene("script command");
        } else if (scriptBindingsDirty) {
          qtReloadScriptsFromDocument("script command");
        }
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "Lua script commands applied and scene reloaded",
                   {{"commands", std::to_string(commands.commands().size())}},
                   qtFrameCount);
        return true;
      };
      auto qtDispatchScriptHook =
          [&](vkpt::scripting::ScriptLifecycleHook hook,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds,
              std::string_view source) {
        auto worldResult = qtSceneDocument.to_world();
        if (!worldResult) {
          qtScriptState.status = "dispatch failed: world build";
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          return false;
        }
        auto world = std::move(worldResult.value());
        world.recompute_world_transforms();
        vkpt::scene::WorldCommandBuffer commands;
        vkpt::scripting::ScriptExecutionContext context;
        context.frame = frameIndex;
        context.delta_seconds = dtSeconds;
        context.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - qtScriptRuntimeStartTime).count();
        context.scripts_enabled = qtScriptState.scripts_enabled;
        context.benchmark_mode = qtSceneDocument.benchmark.enabled;
        context.allow_benchmark_scripts = qtScriptState.benchmark_scripts_allowed;
        context.input.active_keys.assign(qtKeysDown.begin(), qtKeysDown.end());
        context.input.mouse_delta_x = qtPendingFpsLookDx;
        context.input.mouse_delta_y = qtPendingFpsLookDy;
        logger.log(vkpt::log::Severity::Debug,
                   "scripts",
                   "dispatching Lua script hook",
                   {{"hook", std::string(vkpt::scripting::to_string(hook))},
                    {"source", std::string(source)},
                    {"frame", std::to_string(frameIndex)},
                    {"dt", std::to_string(dtSeconds)},
                    {"scripts_enabled", context.scripts_enabled ? "true" : "false"},
                    {"lua_compiled_in", qtScriptRuntime->lua_compiled_in() ? "true" : "false"},
                    {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"},
                    {"active_keys", std::to_string(context.input.active_keys.size())},
                    {"mouse_delta_x", std::to_string(context.input.mouse_delta_x)},
                    {"mouse_delta_y", std::to_string(context.input.mouse_delta_y)},
                    {"b_down", qtKeyActive('B') ? "true" : "false"},
                    {"w_down", qtKeyActive('W') ? "true" : "false"},
                    {"a_down", qtKeyActive('A') ? "true" : "false"},
                    {"s_down", qtKeyActive('S') ? "true" : "false"},
                    {"d_down", qtKeyActive('D') ? "true" : "false"}},
                   frameIndex);
        qtScriptState.last_dispatch =
            qtScriptRuntime->dispatch_hook(world, hook, context, commands);
        qtScriptState.binding_summary = vkpt::scripting::SummarizeScriptBindings(
            qtScriptRuntime->bindings(),
            qtScriptRuntime->lua_compiled_in(),
            qtScriptRuntime->execution_available());
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.last_hook = std::string(vkpt::scripting::to_string(hook));
        qtScriptState.last_frame = frameIndex;
        ++qtScriptState.dispatch_count;
        qtScriptState.status =
            std::string(source) + " " + qtScriptState.last_hook +
            " runnable=" + std::to_string(qtScriptState.last_dispatch.runnable_count) +
            " skipped=" + std::to_string(qtScriptState.last_dispatch.skipped_count);
        if (!commands.commands().empty()) {
          qtScriptState.status += " commands=" + std::to_string(commands.commands().size());
          qtApplyScriptCommandsToDocument(commands);
        }
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "Lua script hook dispatch completed",
                   {{"hook", qtScriptState.last_hook},
                    {"source", std::string(source)},
                    {"runnable", std::to_string(qtScriptState.last_dispatch.runnable_count)},
                    {"skipped", std::to_string(qtScriptState.last_dispatch.skipped_count)},
                    {"hook_calls", std::to_string(qtScriptState.last_dispatch.hook_call_count)},
                    {"commands", std::to_string(commands.commands().size())},
                    {"diagnostics", std::to_string(qtScriptState.last_dispatch.diagnostics.size())},
                    {"lua_compiled_in", qtScriptState.last_dispatch.lua_compiled_in ? "true" : "false"},
                    {"execution_available", qtScriptState.last_dispatch.execution_available ? "true" : "false"}},
                   frameIndex);
        ui_runtime_state.status_message = qtScriptState.status;
        qtDockPanelsDirty = true;
        return true;
      };
      qtReloadScriptsFromDocument("startup");
      auto qtLastScriptAutoDispatch = std::chrono::steady_clock::time_point{};
      auto qtApplyScriptPlayback =
          [&](std::chrono::steady_clock::time_point now,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds) {
        if (!qtScriptState.playing) {
          return false;
        }
        logger.log(vkpt::log::Severity::Debug,
                   "scripts",
                   "Lua playback tick considered",
                   {{"frame", std::to_string(frameIndex)},
                    {"dt", std::to_string(dtSeconds)},
                    {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"},
                    {"active_keys", std::to_string(qtKeysDown.size())},
                    {"mouse_delta_x", std::to_string(qtPendingFpsLookDx)},
                    {"mouse_delta_y", std::to_string(qtPendingFpsLookDy)},
                    {"w_down", qtKeyActive('W') ? "true" : "false"},
                    {"a_down", qtKeyActive('A') ? "true" : "false"},
                    {"s_down", qtKeyActive('S') ? "true" : "false"},
                    {"d_down", qtKeyActive('D') ? "true" : "false"}},
                   frameIndex);
        const auto interval = qtScriptRuntime->execution_available()
            ? std::chrono::milliseconds(33)
            : std::chrono::milliseconds(500);
        if (qtLastScriptAutoDispatch != std::chrono::steady_clock::time_point{} &&
            now - qtLastScriptAutoDispatch < interval) {
          return false;
        }
        qtLastScriptAutoDispatch = now;
        return qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate,
                                    frameIndex,
                                    dtSeconds,
                                    "script playback");
      };
      qtReloadEditedScene = [&](std::string_view reason) {
        if (qtFpsMode) {
          qtMaybeSyncFpsCameraObject(true);
        }
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
        if (!sceneResult) {
          ui_runtime_state.status_message = "scene edit failed: rebuild scene data";
          qtPreviewStatus = "scene edit failed";
          return false;
        }
        auto nextScene = std::move(sceneResult.value());
        qtApplySceneLightingFallbackTo(nextScene);
        const auto sceneDelta = vkpt::pathtracer::BuildSceneDeltaUpdate(qtScene, nextScene);
        const bool hasSceneDeltaWork =
            sceneDelta.has_value() &&
            (!sceneDelta->materials.empty() ||
             !sceneDelta->lights.empty() ||
             sceneDelta->environment_color_changed);
        qtScene = std::move(nextScene);
        syncQtCameraPoseFromScene();
        syncQtFpsAnglesFromPose();
        qtCameraFocusDistance = std::max(
            0.25f,
            qtScene.camera_focus_distance > 0.0f
                ? qtScene.camera_focus_distance
                : PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
        qtCameraFocusPoint =
            PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
        qtViewportPickablesRefreshPending = false;
        qtLastViewportPickablesRefresh = std::chrono::steady_clock::now();
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        bool rendererUpdatedIncrementally = false;
        bool accumulationReset = false;
        if (hasSceneDeltaWork) {
          if (qtUseBg && qtRenderCoordinator) {
            qtRenderCoordinator->post_scene_delta(*sceneDelta);
            rendererUpdatedIncrementally = true;
            accumulationReset = true;
          } else if (!qtUseBg && qtTracerReady) {
            rendererUpdatedIncrementally = qtTracer->update_scene_delta(*sceneDelta);
            if (rendererUpdatedIncrementally) {
              qtTracerReady = qtTracer->reset_accumulation();
              rendererUpdatedIncrementally = qtTracerReady;
              qtSampleIndex = 0u;
              accumulationReset = qtTracerReady;
            }
          }
        } else if (sceneDelta.has_value()) {
          rendererUpdatedIncrementally = true;
        }

        if (!rendererUpdatedIncrementally) {
          if (qtUseBg && qtRenderCoordinator) {
            qtRenderCoordinator->post_scene(qtScene);
            accumulationReset = true;
          } else if (!qtUseBg && qtTracerReady) {
            qtTracerReady = qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation();
            qtSampleIndex = 0u;
            accumulationReset = qtTracerReady;
          }
        }
        qtDockPanelsDirty = true;
        qtSyncPhysicsFromSceneDocument();
        qtReloadScriptsFromDocument("scene edited");
        qtPreviewStatus = qtTracerReady
            ? (rendererUpdatedIncrementally && hasSceneDeltaWork
                   ? "scene delta update"
                   : "scene edited")
            : "scene edit renderer reload failed";
        if (reason == std::string_view{"animation playback"}) {
          ui_runtime_state.status_message = "animation playback";
        } else if (reason == std::string_view{"scene loaded"} ||
                   reason == std::string_view{"asset browser scene load"} ||
                   reason == std::string_view{"asset browser model spawn"} ||
                   reason == std::string_view{"scene graph delete"} ||
                   reason == std::string_view{"scene graph reparent"}) {
          ui_runtime_state.status_message = std::string(reason);
        } else {
          ui_runtime_state.status_message = "gizmo " + std::string(reason);
        }
        if (accumulationReset) {
          qtPublishedSample.store(0u, std::memory_order_relaxed);
          qtPublishedRays.store(0u, std::memory_order_relaxed);
        }
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      std::unordered_map<vkpt::core::StableId, vkpt::scene::TransformComponent>
          qtAnimationBaseTransforms;
      auto qtRefreshAnimationBaseTransforms = [&](bool replaceExisting) {
        for (const auto& entity : qtSceneDocument.entities) {
          if (entity.animation.clip.empty()) {
            continue;
          }
          if (!replaceExisting && qtAnimationBaseTransforms.contains(entity.id)) {
            continue;
          }
          qtAnimationBaseTransforms[entity.id] =
              entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
        }
      };
      auto qtEntityHasAnimatedTransformPath =
          [&](const vkpt::scene::SceneEntityDefinition& entity) {
        if (!entity.animation.clip.empty()) {
          return true;
        }
        std::unordered_set<vkpt::core::StableId> visited;
        vkpt::core::StableId parent = entity.has_hierarchy ? entity.hierarchy.parent : 0u;
        while (parent != 0u && visited.insert(parent).second) {
          const auto* parentEntity = static_cast<const vkpt::scene::SceneEntityDefinition*>(nullptr);
          for (const auto& candidate : qtSceneDocument.entities) {
            if (candidate.id == parent) {
              parentEntity = &candidate;
              break;
            }
          }
          if (parentEntity == nullptr) {
            break;
          }
          if (!parentEntity->animation.clip.empty()) {
            return true;
          }
          parent = parentEntity->has_hierarchy ? parentEntity->hierarchy.parent : 0u;
        }
        return false;
      };
      uint32_t qtAnimationTransformRevision = 1u;
      const auto qtAnimationStartTime = std::chrono::steady_clock::now();
      qtPublishInstanceTransformUpdates =
          [&](const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
        if (updates.empty() || !qtTracerReady) {
          return false;
        }
        if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(qtScene, updates)) {
          return false;
        }

        bool posted = false;
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_instance_transforms(updates);
          posted = true;
        } else if (!qtUseBg && qtTracerReady) {
          posted = qtTracer->update_instance_transforms(updates);
          if (posted) {
            qtTracerReady = qtTracer->reset_accumulation();
            posted = qtTracerReady;
            qtSampleIndex = 0u;
          }
        }

        if (!posted) {
          return false;
        }
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtScheduleViewportPickablesRefresh(false);
        qtMarkTransformDockPanelsDirty(false);
        return true;
      };
      auto qtApplySceneAnimation = [&](std::chrono::steady_clock::time_point now) {
        if (qtGizmoDrag.active) {
          return false;
        }
        qtRefreshAnimationBaseTransforms(false);
        bool animated = false;
        const float elapsedSeconds =
            std::chrono::duration<float>(now - qtAnimationStartTime).count();
        for (auto& entity : qtSceneDocument.entities) {
          if (entity.animation.clip.empty() ||
              !AnimationHasAuthoredMotion(entity.animation)) {
            continue;
          }
          const auto baseIt = qtAnimationBaseTransforms.find(entity.id);
          if (baseIt == qtAnimationBaseTransforms.end()) {
            continue;
          }
          entity.has_transform = true;
          entity.transform = SampleAnimationTransform(baseIt->second,
                                                      entity.animation,
                                                      elapsedSeconds);
          animated = true;
        }
        if (!animated) {
          return false;
        }

        const auto worldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
        const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_mesh || !qtEntityHasAnimatedTransformPath(entity)) {
            continue;
          }
          const uint32_t instanceIndex = qtFindRtInstanceIndex(entity.id);
          if (instanceIndex == vkpt::pathtracer::kInvalidRTInstanceIndex) {
            continue;
          }
          const auto worldTransform = ResolveEntityWorldTransform(entity, world);
          vkpt::pathtracer::RTInstanceTransformUpdate update;
          update.entity_id = entity.id;
          update.instance_index = instanceIndex;
          update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                         vkpt::pathtracer::kRTInstanceFlagTransformDirty;
          update.transform_revision = qtAnimationTransformRevision++;
          update.translation = ToPtVec3(worldTransform.translation);
          update.rotation = ToPtQuat4(worldTransform.rotation);
          update.scale = ToPtVec3(worldTransform.scale);
          updates.push_back(update);
        }

        if (qtPublishInstanceTransformUpdates(updates)) {
          qtPreviewStatus = "animation playback";
          return qtTracerReady;
        }
        return qtReloadEditedScene("animation playback");
      };
      qtRefreshAnimationBaseTransforms(true);
      auto qtDeleteSceneGraphTargets =
          [&](std::vector<vkpt::core::StableId> requestedIds,
              vkpt::core::FrameIndex frameIndex,
              std::string_view source,
              std::string_view rowId) {
        requestedIds = SortedUniqueEntityIds(requestedIds);
        if (requestedIds.empty()) {
          ui_runtime_state.status_message = "delete failed: no scene graph item selected";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          return false;
        }

        std::unordered_set<vkpt::core::StableId> requestedSet(requestedIds.begin(), requestedIds.end());
        std::unordered_set<vkpt::core::StableId> removeEntities;
        std::unordered_set<vkpt::core::StableId> removeSdfPrimitives;
        for (const auto& entity : qtSceneDocument.entities) {
          if (requestedSet.contains(entity.id)) {
            removeEntities.insert(entity.id);
          }
        }
        for (const auto& primitive : qtSceneDocument.sdf_primitives) {
          if (requestedSet.contains(primitive.id)) {
            removeSdfPrimitives.insert(primitive.id);
          }
        }

        auto entityParent = [&](const vkpt::scene::SceneEntityDefinition& entity) {
          if (entity.hierarchy.parent != 0u) {
            return entity.hierarchy.parent;
          }
          if (const auto* transform = qtFindTransformEntry(entity.id)) {
            return transform->parent;
          }
          return vkpt::core::StableId{0u};
        };

        bool expanded = true;
        while (expanded) {
          expanded = false;
          for (const auto& entity : qtSceneDocument.entities) {
            if (removeEntities.contains(entity.id)) {
              continue;
            }
            const auto parent = entityParent(entity);
            if (parent != 0u && removeEntities.contains(parent)) {
              removeEntities.insert(entity.id);
              expanded = true;
            }
          }
        }

        if (removeEntities.empty() && removeSdfPrimitives.empty()) {
          ui_runtime_state.status_message = "delete failed: scene graph item is no longer in the scene";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          return false;
        }

        std::unordered_set<vkpt::core::StableId> geometryCandidates;
        for (const auto& entity : qtSceneDocument.entities) {
          if (removeEntities.contains(entity.id) && entity.has_mesh && entity.mesh.mesh_id != 0u) {
            geometryCandidates.insert(entity.mesh.mesh_id);
          }
        }

        qtSceneDocument.entities.erase(
            std::remove_if(qtSceneDocument.entities.begin(),
                           qtSceneDocument.entities.end(),
                           [&](const vkpt::scene::SceneEntityDefinition& entity) {
                             return removeEntities.contains(entity.id);
                           }),
            qtSceneDocument.entities.end());
        qtSceneDocument.transforms.erase(
            std::remove_if(qtSceneDocument.transforms.begin(),
                           qtSceneDocument.transforms.end(),
                           [&](const vkpt::scene::SceneTransformEntry& entry) {
                             return removeEntities.contains(entry.id);
                           }),
            qtSceneDocument.transforms.end());
        qtSceneDocument.sdf_primitives.erase(
            std::remove_if(qtSceneDocument.sdf_primitives.begin(),
                           qtSceneDocument.sdf_primitives.end(),
                           [&](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                             return removeSdfPrimitives.contains(primitive.id);
                           }),
            qtSceneDocument.sdf_primitives.end());

        std::unordered_set<vkpt::core::StableId> remainingGeometryRefs;
        for (const auto& entity : qtSceneDocument.entities) {
          if (entity.has_mesh && entity.mesh.mesh_id != 0u) {
            remainingGeometryRefs.insert(entity.mesh.mesh_id);
          }
        }
        const auto geometryBefore = qtSceneDocument.geometry.size();
        qtSceneDocument.geometry.erase(
            std::remove_if(qtSceneDocument.geometry.begin(),
                           qtSceneDocument.geometry.end(),
                           [&](const vkpt::scene::SceneGeometryDefinition& geometry) {
                             return geometryCandidates.contains(geometry.id) &&
                                    !remainingGeometryRefs.contains(geometry.id);
                           }),
            qtSceneDocument.geometry.end());
        const auto removedGeometry = geometryBefore - qtSceneDocument.geometry.size();

        auto wasRemoved = [&](vkpt::core::StableId id) {
          return removeEntities.contains(id) || removeSdfPrimitives.contains(id);
        };
        auto removeFromSelection = [&](std::vector<vkpt::core::StableId>& ids) {
          ids.erase(std::remove_if(ids.begin(),
                                   ids.end(),
                                   [&](vkpt::core::StableId id) {
                                     return wasRemoved(id);
                                   }),
                    ids.end());
        };
        removeFromSelection(ui_selection_state.selected_entity_ids);
        removeFromSelection(ui_selection_state.hovered_entity_ids);
        if (wasRemoved(ui_selection_state.active_primary_entity)) {
          ui_selection_state.active_primary_entity =
              ui_selection_state.selected_entity_ids.empty()
                  ? 0u
                  : ui_selection_state.selected_entity_ids.front();
        }
        if (wasRemoved(ui_selection_state.hovered_entity)) {
          ui_selection_state.hovered_entity = 0u;
        }
        if (wasRemoved(ui_selection_state.selected_group)) {
          ui_selection_state.selected_group = 0u;
          ui_selection_state.selected_group_name.clear();
        }
        if (wasRemoved(ui_runtime_state.last_clicked_entity)) {
          ui_runtime_state.last_clicked_entity = 0u;
        }
        if (ui_selection_state.selected_entity_ids.empty()) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        }
        qtGizmoDrag = {};
        qtHoveredGizmoHit = std::nullopt;
        qtAnimationBaseTransforms.clear();

        std::vector<vkpt::core::StableId> removedEntityIds(removeEntities.begin(), removeEntities.end());
        removedEntityIds = SortedUniqueEntityIds(std::move(removedEntityIds));
        const bool loaded = qtReloadEditedScene("scene graph delete");
        qtRefreshAnimationBaseTransforms(true);

        vkpt::editor::EditorCommand command;
        command.command_id = "scene_tree.delete";
        command.kind = vkpt::editor::EditorCommandKind::kDeleteEntity;
        command.source_widget = std::string(source);
        command.frame_index = frameIndex;
        command.undoable = false;
        command.redoable = false;
        command.validated = loaded;
        command.payload = vkpt::editor::DeleteEntityCommand{removedEntityIds};
        ui_command_history.push(command);

        const auto removedCount = removeEntities.size() + removeSdfPrimitives.size();
        ui_runtime_state.focused_panel = "scene_tree";
        ui_runtime_state.last_scene_tree_operation =
            "deleted " + std::to_string(removedCount) + " item" + (removedCount == 1u ? "" : "s");
        ui_runtime_state.status_message = loaded
            ? ui_runtime_state.last_scene_tree_operation
            : ui_runtime_state.last_scene_tree_operation + "; renderer reload failed";
        qtPreviewStatus = loaded ? "scene graph delete" : "scene graph delete reload failed";
        qtDockPanelsDirty = true;
        PushUiEvent(ui_event_log,
                    loaded ? "scene_tree_delete" : "scene_tree_delete_reload_failed",
                    "scene_tree",
                    rowId.empty() ? std::string(source) : std::string(rowId),
                    frameIndex,
                    std::to_string(removedGeometry),
                    std::to_string(removedCount),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        logger.log(loaded ? vkpt::log::Severity::Info : vkpt::log::Severity::Error,
                   "app",
                   loaded ? "Qt scene graph item deleted"
                          : "Qt scene graph item deleted but renderer reload failed",
                   {{"items", std::to_string(removedCount)},
                    {"entities", std::to_string(removeEntities.size())},
                    {"sdf_primitives", std::to_string(removeSdfPrimitives.size())},
                    {"geometry", std::to_string(removedGeometry)}});
        return loaded;
      };
      auto qtReparentSceneGraphTargets =
          [&](std::vector<vkpt::core::StableId> requestedIds,
              vkpt::core::StableId targetParent,
              vkpt::core::FrameIndex frameIndex) {
        requestedIds = SortedUniqueEntityIds(std::move(requestedIds));
        if (requestedIds.empty() || targetParent == 0u || qtFindEntity(targetParent) == nullptr) {
          ui_runtime_state.status_message = "reparent failed: invalid scene graph target";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          return false;
        }

        const auto preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
        const auto* preWriteWorld = preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
        std::vector<vkpt::core::StableId> moved;
        for (const auto entityId : requestedIds) {
          auto* entity = qtFindEntity(entityId);
          if (entity == nullptr || entityId == targetParent ||
              qtWouldCreateParentCycle(entityId, targetParent)) {
            continue;
          }
          const auto oldParent = qtEntityParentId(*entity);
          if (!qtSetEntityParentPreserveWorld(entityId, targetParent, preWriteWorld, true)) {
            continue;
          }
          moved.push_back(entityId);

          vkpt::editor::EditorCommand command;
          command.command_id = "scene_tree.reparent";
          command.kind = vkpt::editor::EditorCommandKind::kReparentEntity;
          command.source_widget = "scene_tree";
          command.frame_index = frameIndex;
          command.undoable = false;
          command.redoable = false;
          command.payload = vkpt::editor::ReparentEntityCommand{entityId, oldParent, targetParent, true};
          ui_command_history.push(command);
        }

        if (moved.empty()) {
          ui_runtime_state.status_message = "reparent failed: item cannot be moved there";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          return false;
        }

        ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
        for (const auto entityId : moved) {
          vkpt::editor::EditorCommand selectCommand;
          selectCommand.command_id = "scene_tree.select_reparented";
          selectCommand.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
          selectCommand.source_widget = "scene_tree";
          selectCommand.frame_index = frameIndex;
          selectCommand.payload = vkpt::editor::SelectEntityCommand{
              entityId,
              !ui_selection_state.selected_entity_ids.empty(),
              false};
          ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, selectCommand);
        }
        qtAnimationBaseTransforms.clear();
        const bool loaded = qtReloadEditedScene("scene graph reparent");
        qtRefreshAnimationBaseTransforms(true);
        ui_runtime_state.focused_panel = "scene_tree";
        ui_runtime_state.last_clicked_entity = moved.front();
        ui_runtime_state.last_scene_tree_operation =
            "moved " + std::to_string(moved.size()) + " item" + (moved.size() == 1u ? "" : "s") +
            " under " + std::to_string(targetParent);
        ui_runtime_state.status_message = loaded
            ? ui_runtime_state.last_scene_tree_operation
            : ui_runtime_state.last_scene_tree_operation + "; renderer reload failed";
        qtDockPanelsDirty = true;
        PushUiEvent(ui_event_log,
                    loaded ? "scene_tree_reparent" : "scene_tree_reparent_reload_failed",
                    "scene_tree",
                    "drag_drop",
                    frameIndex,
                    std::to_string(moved.size()),
                    std::to_string(targetParent),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        return loaded;
      };
