      std::function<bool(std::string_view)> qtReloadEditedScene;
      std::function<bool(const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>&,
                         vkpt::pathtracer::InstanceTransformUpdateOptions)>
          qtPublishInstanceTransformUpdates;
      uint32_t qtScriptTransformRevision = 1u;
      auto qtNearlyEqual = [](float lhs, float rhs) {
        return std::fabs(lhs - rhs) <= 1.0e-4f;
      };
      auto qtVecNearlyEqual =
          [&](const vkpt::scene::Vec3& lhs, const vkpt::scene::Vec3& rhs) {
        return qtNearlyEqual(lhs.x, rhs.x) &&
               qtNearlyEqual(lhs.y, rhs.y) &&
               qtNearlyEqual(lhs.z, rhs.z);
      };
      auto qtQuatNearlyEqual =
          [&](const vkpt::scene::Quat& lhs, const vkpt::scene::Quat& rhs) {
        const bool direct =
            qtNearlyEqual(lhs.x, rhs.x) &&
            qtNearlyEqual(lhs.y, rhs.y) &&
            qtNearlyEqual(lhs.z, rhs.z) &&
            qtNearlyEqual(lhs.w, rhs.w);
        const bool negated =
            qtNearlyEqual(lhs.x, -rhs.x) &&
            qtNearlyEqual(lhs.y, -rhs.y) &&
            qtNearlyEqual(lhs.z, -rhs.z) &&
            qtNearlyEqual(lhs.w, -rhs.w);
        return direct || negated;
      };
      auto qtTransformNearlyEqual =
          [&](const vkpt::scene::TransformComponent& lhs,
              const vkpt::scene::TransformComponent& rhs) {
        return qtVecNearlyEqual(lhs.translation, rhs.translation) &&
               qtQuatNearlyEqual(lhs.rotation, rhs.rotation) &&
               qtVecNearlyEqual(lhs.scale, rhs.scale);
      };
      auto qtLightNearlyEqual =
          [&](const vkpt::scene::LightComponent& lhs,
              const vkpt::scene::LightComponent& rhs) {
        return lhs.type == rhs.type &&
               qtVecNearlyEqual(lhs.color, rhs.color) &&
               qtNearlyEqual(lhs.intensity, rhs.intensity) &&
               qtNearlyEqual(lhs.radius, rhs.radius) &&
               qtVecNearlyEqual(lhs.direction, rhs.direction) &&
               qtNearlyEqual(lhs.beam_angle_degrees, rhs.beam_angle_degrees) &&
               qtNearlyEqual(lhs.blend, rhs.blend);
      };
      auto qtCameraNearlyEqual =
          [&](const vkpt::scene::CameraComponent& lhs,
              const vkpt::scene::CameraComponent& rhs) {
        return qtNearlyEqual(lhs.fov, rhs.fov) &&
               qtNearlyEqual(lhs.focal_length_mm, rhs.focal_length_mm) &&
               qtNearlyEqual(lhs.sensor_width_mm, rhs.sensor_width_mm) &&
               qtNearlyEqual(lhs.sensor_height_mm, rhs.sensor_height_mm) &&
               qtNearlyEqual(lhs.aperture_radius, rhs.aperture_radius) &&
               qtNearlyEqual(lhs.focus_distance, rhs.focus_distance) &&
               qtNearlyEqual(lhs.f_stop, rhs.f_stop) &&
               qtNearlyEqual(lhs.shutter_seconds, rhs.shutter_seconds) &&
               qtNearlyEqual(lhs.iso, rhs.iso) &&
               qtNearlyEqual(lhs.exposure_compensation, rhs.exposure_compensation) &&
               qtNearlyEqual(lhs.white_balance_kelvin, rhs.white_balance_kelvin) &&
               lhs.iris_blade_count == rhs.iris_blade_count &&
               qtNearlyEqual(lhs.iris_rotation_degrees, rhs.iris_rotation_degrees) &&
               qtNearlyEqual(lhs.iris_roundness, rhs.iris_roundness) &&
               qtNearlyEqual(lhs.anamorphic_squeeze, rhs.anamorphic_squeeze);
      };
      auto qtBuildRtMaterialIndexById = [&]() {
        std::unordered_map<vkpt::core::StableId, uint32_t> materialIndexById;
        materialIndexById.reserve(qtSceneDocument.materials.size());
        for (std::size_t index = 0; index < qtSceneDocument.materials.size(); ++index) {
          const auto id = qtSceneDocument.materials[index].id;
          if (id != 0u) {
            materialIndexById.emplace(id, static_cast<uint32_t>(index));
          }
        }
        return materialIndexById;
      };
      struct QtInstanceTransformBuildResult {
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        bool unsupported_instances = false;
      };
      auto qtBuildTransformAffectedEntityIds =
          [&](const std::unordered_set<vkpt::core::StableId>& transformTouchedIds,
              const std::unordered_map<vkpt::core::StableId,
                                       const vkpt::scene::SceneEntityDefinition*>&
                  entitiesById) {
        std::unordered_set<vkpt::core::StableId> affectedIds = transformTouchedIds;
        if (affectedIds.empty()) {
          return affectedIds;
        }

        std::unordered_map<vkpt::core::StableId,
                           std::vector<vkpt::core::StableId>>
            childrenByParent;
        childrenByParent.reserve(entitiesById.size());
        for (const auto& [id, entity] : entitiesById) {
          (void)id;
          if (entity != nullptr &&
              entity->has_hierarchy &&
              entity->hierarchy.parent != 0u) {
            childrenByParent[entity->hierarchy.parent].push_back(entity->id);
          }
        }

        std::vector<vkpt::core::StableId> stack(transformTouchedIds.begin(),
                                                transformTouchedIds.end());
        while (!stack.empty()) {
          const auto parent = stack.back();
          stack.pop_back();
          const auto childrenIt = childrenByParent.find(parent);
          if (childrenIt == childrenByParent.end()) {
            continue;
          }
          for (const auto child : childrenIt->second) {
            if (affectedIds.insert(child).second) {
              stack.push_back(child);
            }
          }
        }
        return affectedIds;
      };
      auto qtBuildInstanceTransformUpdatesForTouchedEntities =
          [&](const std::unordered_set<vkpt::core::StableId>& transformTouchedIds,
              uint32_t baseFlags,
              uint32_t& transformRevision,
              const vkpt::scene::SceneWorld* world = nullptr,
              const std::unordered_map<vkpt::core::StableId,
                                       const vkpt::scene::SceneEntityDefinition*>*
                  entityIndex = nullptr,
              const std::unordered_set<vkpt::core::StableId>* affectedIds = nullptr) {
        QtInstanceTransformBuildResult result;
        if (transformTouchedIds.empty()) {
          return result;
        }

        std::unordered_map<vkpt::core::StableId,
                           const vkpt::scene::SceneEntityDefinition*>
            localEntitiesById;
        const auto* entitiesById = entityIndex;
        if (entitiesById == nullptr) {
          localEntitiesById.reserve(qtSceneDocument.entities.size());
          for (const auto& entity : qtSceneDocument.entities) {
            localEntitiesById.emplace(entity.id, &entity);
          }
          entitiesById = &localEntitiesById;
        }
        std::optional<std::unordered_set<vkpt::core::StableId>> localAffectedIds;
        const auto* affectedTransformIds = affectedIds;
        if (affectedTransformIds == nullptr) {
          localAffectedIds =
              qtBuildTransformAffectedEntityIds(transformTouchedIds, *entitiesById);
          affectedTransformIds = &*localAffectedIds;
        }

        result.updates.reserve(qtScene.instances.size());
        std::optional<vkpt::scene::SceneWorld> localWorldSnapshot;
        const auto* resolvedWorld = world;
        if (resolvedWorld == nullptr) {
          localWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
          resolvedWorld = localWorldSnapshot ? &localWorldSnapshot.value() : nullptr;
        }
        for (std::size_t index = 0; index < qtScene.instances.size(); ++index) {
          const auto& instance = qtScene.instances[index];
          const auto entityIt = entitiesById->find(instance.entity_id);
          if (entityIt == entitiesById->end() || entityIt->second == nullptr) {
            continue;
          }
          if (!affectedTransformIds->contains(entityIt->second->id)) {
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
              ResolveEntityWorldTransform(*entityIt->second, resolvedWorld);
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
      struct QtRtInstanceLocalBoundsCacheEntry {
        uint32_t first_vertex = 0u;
        uint32_t vertex_count = 0u;
        const vkpt::pathtracer::Vec3* local_vertices_data = nullptr;
        std::size_t local_vertices_size = 0u;
        vkpt::editor::Bounds bounds{};
        bool valid = false;
      };
      std::vector<QtRtInstanceLocalBoundsCacheEntry> qtRtInstanceLocalBoundsCache;
      auto qtRefreshSelectionBoundsFromSnapshot =
          [&](const vkpt::scene::RenderSceneSnapshot& snapshot) {
        if (qtRtInstanceLocalBoundsCache.size() != snapshot.instances.size()) {
          qtRtInstanceLocalBoundsCache.assign(snapshot.instances.size(), {});
        }
        auto localBoundsForInstance =
            [&](std::size_t instanceIndex,
                const vkpt::pathtracer::RTInstance& instance)
                -> std::optional<vkpt::editor::Bounds> {
          if (instance.local_vertex_count == 0u ||
              instance.local_first_vertex >= snapshot.local_vertices.size() ||
              instanceIndex >= qtRtInstanceLocalBoundsCache.size()) {
            return std::nullopt;
          }
          auto& cache = qtRtInstanceLocalBoundsCache[instanceIndex];
          if (cache.valid &&
              cache.first_vertex == instance.local_first_vertex &&
              cache.vertex_count == instance.local_vertex_count &&
              cache.local_vertices_data == snapshot.local_vertices.data() &&
              cache.local_vertices_size == snapshot.local_vertices.size()) {
            return cache.bounds;
          }

          vkpt::editor::Bounds localBounds{};
          const uint64_t localEnd = std::min<uint64_t>(
              static_cast<uint64_t>(snapshot.local_vertices.size()),
              static_cast<uint64_t>(instance.local_first_vertex) +
                  static_cast<uint64_t>(instance.local_vertex_count));
          for (uint64_t index = instance.local_first_vertex; index < localEnd; ++index) {
            ExpandBounds(localBounds,
                         snapshot.local_vertices[static_cast<std::size_t>(index)]);
          }
          cache.first_vertex = instance.local_first_vertex;
          cache.vertex_count = instance.local_vertex_count;
          cache.local_vertices_data = snapshot.local_vertices.data();
          cache.local_vertices_size = snapshot.local_vertices.size();
          cache.bounds = localBounds;
          cache.valid = localBounds.valid;
          if (!cache.valid) {
            return std::nullopt;
          }
          return cache.bounds;
        };
        ui_selection_state.per_item_bounds.clear();
        ui_selection_state.per_item_bounds.reserve(
            ui_selection_state.selected_entity_ids.size());
        ui_selection_state.aggregate_bounds = {};
        auto expandPoint = [&](vkpt::editor::Bounds& bounds,
                               const vkpt::pathtracer::Vec3& point) {
          ExpandBounds(bounds, point);
          ExpandBounds(ui_selection_state.aggregate_bounds, point);
        };
        auto expandInstanceBounds = [&](vkpt::editor::Bounds& bounds,
                                        std::size_t instanceIndex,
                                        const vkpt::pathtracer::RTInstance& instance) {
          bool found = false;
          if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
            const auto localBounds = localBoundsForInstance(instanceIndex, instance);
            if (!localBounds) {
              return false;
            }
            const auto transform = TransformFromRtInstance(instance);
            const auto localMin = ToPtVec3(localBounds->min);
            const auto localMax = ToPtVec3(localBounds->max);
            const vkpt::pathtracer::Vec3 corners[8] = {
                {localMin.x, localMin.y, localMin.z},
                {localMax.x, localMin.y, localMin.z},
                {localMin.x, localMax.y, localMin.z},
                {localMax.x, localMax.y, localMin.z},
                {localMin.x, localMin.y, localMax.z},
                {localMax.x, localMin.y, localMax.z},
                {localMin.x, localMax.y, localMax.z},
                {localMax.x, localMax.y, localMax.z}};
            for (const auto& corner : corners) {
              expandPoint(bounds, ApplySceneTransformToPoint(corner, transform));
              found = true;
            }
            return found;
          }

          const uint64_t firstIndex =
              static_cast<uint64_t>(instance.first_triangle) * 3ull;
          const uint64_t indexCount =
              static_cast<uint64_t>(instance.triangle_count) * 3ull;
          if (firstIndex > snapshot.indices.size() ||
              indexCount > snapshot.indices.size() - firstIndex) {
            return false;
          }
          for (uint64_t offset = 0u; offset < indexCount; ++offset) {
            const uint32_t vertexIndex =
                snapshot.indices[static_cast<std::size_t>(firstIndex + offset)];
            if (vertexIndex >= snapshot.vertices.size()) {
              continue;
            }
            expandPoint(bounds, snapshot.vertices[vertexIndex]);
            found = true;
          }
          return found;
        };
        for (const auto selectedId : ui_selection_state.selected_entity_ids) {
          auto bounds = vkpt::editor::Bounds{};
          bool found = false;
          for (std::size_t instanceIndex = 0; instanceIndex < snapshot.instances.size();
               ++instanceIndex) {
            const auto& instance = snapshot.instances[instanceIndex];
            if (instance.entity_id != selectedId) {
              continue;
            }
            found = expandInstanceBounds(bounds, instanceIndex, instance) || found;
          }
          if (found && bounds.valid) {
            ui_selection_state.per_item_bounds.push_back({selectedId, bounds});
          }
        }
      };
      auto qtRefreshSelectionBoundsFromCurrentSnapshot = [&]() {
        if (const auto snapshot = qtCurrentUiSnapshot()) {
          qtRefreshSelectionBoundsFromSnapshot(*snapshot);
        } else {
          RebuildSelectionBounds(ui_selection_state, qtPickables);
        }
      };
      auto qtLastViewportPickablesRefresh = std::chrono::steady_clock::time_point{};
      bool qtViewportPickablesRefreshPending = false;
      constexpr auto kQtTransformPickablesRefreshInterval =
          std::chrono::milliseconds(100);
      constexpr std::uint64_t kQtPickablesMutationGraceNs =
          250u * 1000000ull;  // 250ms
      auto qtDeferTransformPickablesRefresh = [&]() {
        if (qtGizmoDrag.active ||
            qtFpsMode ||
            qtIsPlayMode() ||
            qtLiveEditViewportInputArmed()) {
          return true;
        }
        const auto nowNs = vkpt::core::metrics::UiInputLatencyNowNs();
        return qtLastMutationPublishNs != 0u &&
               nowNs > qtLastMutationPublishNs &&
               (nowNs - qtLastMutationPublishNs) < kQtPickablesMutationGraceNs;
      };
      auto qtSetViewportPickablesFromSnapshot =
          [&](vkpt::scene::RenderSceneSnapshot::Ptr snapshot) {
        if (snapshot) {
          qtViewportPickablesSnapshot = std::move(snapshot);
          qtViewportPickablesSnapshotGeneration =
              qtViewportPickablesSnapshot->generation;
          qtViewportPickablesTopologyRevision =
              qtViewportPickablesSnapshot->topology_revision;
          qtViewportPickablesTransformRevision =
              qtViewportPickablesSnapshot->transform_revision;
          qtPickables =
              BuildViewportPickables(qtSceneDocument, *qtViewportPickablesSnapshot);
        } else {
          qtViewportPickablesSnapshot.reset();
          qtViewportPickablesSnapshotGeneration = 0u;
          qtViewportPickablesTopologyRevision = 0u;
          qtViewportPickablesTransformRevision = 0u;
          qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
        }
      };
      auto qtRefreshViewportPickablesNow = [&]() {
        qtSetViewportPickablesFromSnapshot(qtCurrentUiSnapshot());
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtViewportPickablesRefreshPending = false;
        qtLastViewportPickablesRefresh = std::chrono::steady_clock::now();
        updateQtSelectionOverlay();
      };
      auto qtRefreshViewportPickablesIfSnapshotChanged = [&]() {
        const auto snapshot = qtCurrentUiSnapshot();
        if (!snapshot) {
          return;
        }
        if (snapshot->topology_revision == qtViewportPickablesTopologyRevision &&
            snapshot->transform_revision == qtViewportPickablesTransformRevision) {
          qtViewportPickablesSnapshot = snapshot;
          qtViewportPickablesSnapshotGeneration = snapshot->generation;
          return;
        }
        // Topology changes (geometry add/remove, mesh swap) must rebuild the
        // pickable triangles immediately — the indices encoded in the existing
        // list are no longer valid. Transform-only changes (gizmo drag,
        // physics step, Lua mutation) only shift world bounds; rebuilding
        // BuildViewportPickables for 1000+ instances costs ~14ms/frame at
        // p50 and pegs the UI thread at single-digit FPS during drag/physics.
        // Throttle the transform-only rebuild to kQtTransformPickablesRefreshInterval
        // (100ms) and let qtMaybeRefreshPendingViewportPickables catch up
        // when the cadence allows. The pickables stay world-stale for at
        // most one throttle interval, which is acceptable: gizmo dragging
        // skips this path entirely (qtGizmoDrag.active gate at the call
        // site), and post-drag picking always sees the freshest snapshot.
        const bool topologyChanged =
            snapshot->topology_revision != qtViewportPickablesTopologyRevision;
        if (!topologyChanged) {
          if (qtDeferTransformPickablesRefresh()) {
            qtViewportPickablesSnapshot = snapshot;
            qtViewportPickablesSnapshotGeneration = snapshot->generation;
            qtViewportPickablesTransformRevision = snapshot->transform_revision;
            qtViewportPickablesRefreshPending = true;
            return;
          }
          const auto now = std::chrono::steady_clock::now();
          if (qtLastViewportPickablesRefresh !=
                  std::chrono::steady_clock::time_point{} &&
              (now - qtLastViewportPickablesRefresh) <
                  kQtTransformPickablesRefreshInterval) {
            qtViewportPickablesSnapshot = snapshot;
            qtViewportPickablesSnapshotGeneration = snapshot->generation;
            qtViewportPickablesTransformRevision = snapshot->transform_revision;
            qtViewportPickablesRefreshPending = true;
            return;
          }
        }
        qtSetViewportPickablesFromSnapshot(snapshot);
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
        qtRefreshSelectionBoundsFromCurrentSnapshot();
        updateQtSelectionOverlay();
        if (qtGizmoDrag.active) {
          return;
        }
      };
      auto qtMaybeRefreshPendingViewportPickables =
          [&](std::chrono::steady_clock::time_point now) {
        if (!qtViewportPickablesRefreshPending || qtGizmoDrag.active) {
          return;
        }
        if (qtDeferTransformPickablesRefresh()) {
          return;
        }
        if (qtLastViewportPickablesRefresh == std::chrono::steady_clock::time_point{} ||
            now - qtLastViewportPickablesRefresh >=
                kQtTransformPickablesRefreshInterval) {
          qtRefreshViewportPickablesNow();
        }
      };
      auto qtMakeScriptBootstrapOptions = [&](bool enableFallback) {
        vkpt::scene::SceneScriptBootstrapOptions options;
        options.enable_default_fps_fallback = enableFallback;
        return options;
      };
      auto qtRecordScriptBootstrapState =
          [&](const vkpt::scene::SceneScriptRuntimeWorld& runtimeWorld) {
        const auto& decision = runtimeWorld.decision;
        qtScriptState.bootstrap_policy = std::string(vkpt::scene::to_string(decision.policy));
        qtScriptState.bootstrap_operation =
            std::string(vkpt::scene::to_string(decision.fallback.operation));
        qtScriptState.bootstrap_scene_init_injected = runtimeWorld.scene_init_injected;
        qtScriptState.bootstrap_fallback_injected = runtimeWorld.fallback_injected;
        qtScriptState.bootstrap_target.clear();
        if (decision.fallback.target_entity_id != 0u) {
          qtScriptState.bootstrap_target =
              std::to_string(decision.fallback.target_entity_id);
          if (!decision.fallback.target_entity_name.empty()) {
            qtScriptState.bootstrap_target += " " + decision.fallback.target_entity_name;
          }
        } else if (decision.main_camera.has_value()) {
          qtScriptState.bootstrap_target =
              std::to_string(decision.main_camera->entity_id);
          if (!decision.main_camera->entity_name.empty()) {
            qtScriptState.bootstrap_target += " " + decision.main_camera->entity_name;
          }
        }
        qtScriptState.bootstrap_diagnostics.clear();
        for (const auto& diagnostic : decision.diagnostics) {
          qtScriptState.bootstrap_diagnostics.push_back(
              diagnostic.code + ": " + diagnostic.message);
        }
        qtScriptState.bootstrap_status =
            runtimeWorld.scene_init_injected || runtimeWorld.fallback_injected
                ? "runtime overlay active"
                : "authored runtime world";
        if (!decision.diagnostics.empty()) {
          qtScriptState.bootstrap_status += "; " + decision.diagnostics.back().code;
        }
      };
      auto qtBuildScriptRuntimeWorld =
          [&](bool enableFallback)
              -> vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld> {
        auto runtimeWorld = vkpt::scene::BuildSceneScriptRuntimeWorld(
            qtSceneDocument, qtMakeScriptBootstrapOptions(enableFallback));
        if (runtimeWorld) {
          qtRecordScriptBootstrapState(runtimeWorld.value());
        }
        return runtimeWorld;
      };
      std::optional<vkpt::scene::SceneScriptRuntimeWorld> qtScriptRuntimeWorldCache;
      bool qtScriptRuntimeWorldCacheFallback = false;
      auto qtStoreScriptRuntimeWorldCache =
          [&](vkpt::scene::SceneScriptRuntimeWorld runtimeWorld, bool enableFallback)
              -> vkpt::scene::SceneScriptRuntimeWorld& {
        qtScriptRuntimeWorldCacheFallback = enableFallback;
        qtScriptRuntimeWorldCache = std::move(runtimeWorld);
        return *qtScriptRuntimeWorldCache;
      };
      auto qtRebuildCachedScriptRuntimeWorld =
          [&](bool enableFallback)
              -> vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld*> {
        auto worldResult = qtBuildScriptRuntimeWorld(enableFallback);
        if (!worldResult) {
          qtScriptRuntimeWorldCache.reset();
          return vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld*>::error(
              worldResult.error());
        }
        auto& cached =
            qtStoreScriptRuntimeWorldCache(std::move(worldResult.value()), enableFallback);
        return vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld*>::ok(&cached);
      };
      auto qtGetCachedScriptRuntimeWorld =
          [&](bool enableFallback)
              -> vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld*> {
        if (!qtScriptRuntimeWorldCache ||
            qtScriptRuntimeWorldCacheFallback != enableFallback) {
          return qtRebuildCachedScriptRuntimeWorld(enableFallback);
        }
        return vkpt::core::Result<vkpt::scene::SceneScriptRuntimeWorld*>::ok(
            &*qtScriptRuntimeWorldCache);
      };
      auto qtReplayScriptCommandsIntoRuntimeWorldCache =
          [&](const vkpt::scene::WorldCommandBuffer& commands) {
        if (!qtScriptRuntimeWorldCache || commands.commands().empty()) {
          return;
        }
        auto replayResult = commands.replay(qtScriptRuntimeWorldCache->world);
        if (!replayResult) {
          qtScriptRuntimeWorldCache.reset();
          logger.log(vkpt::log::Severity::Warning,
                     "scripts",
                     "script runtime world cache invalidated after command replay failed",
                     {{"commands", std::to_string(commands.commands().size())}},
                     qtFrameCount);
        }
      };
      auto qtNormalizeScriptLightType = [](std::string_view type) {
        std::string out;
        out.reserve(type.size());
        bool lastUnderscore = false;
        for (const char ch : type) {
          const auto c = static_cast<unsigned char>(ch);
          if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            lastUnderscore = false;
          } else if (!lastUnderscore && !out.empty()) {
            out.push_back('_');
            lastUnderscore = true;
          }
        }
        while (!out.empty() && out.back() == '_') {
          out.pop_back();
        }
        return out;
      };
      auto qtScriptLightIsEnvironment = [&](std::string_view type) {
        const auto normalized = qtNormalizeScriptLightType(type);
        return normalized == "environment" ||
               normalized == "environment_sky" ||
               normalized == "environment_hdri" ||
               normalized == "environmenthdri" ||
               normalized == "hdri" ||
               normalized == "hdri_sky" ||
               normalized == "open_sky" ||
               normalized == "sky";
      };
      auto qtScriptLightIsSpot = [&](std::string_view type) {
        const auto normalized = qtNormalizeScriptLightType(type);
        return normalized == "spot" ||
               normalized == "spot_light" ||
               normalized == "spotlight";
      };
      auto qtEntityVisiblePath =
          [&](const vkpt::scene::SceneEntityDefinition& entity,
              const std::unordered_map<vkpt::core::StableId,
                                       const vkpt::scene::SceneEntityDefinition*>&
                  entitiesById) {
        const auto* current = &entity;
        std::unordered_set<vkpt::core::StableId> visited;
        for (std::size_t depth = 0u;
             current != nullptr && depth <= entitiesById.size() &&
             visited.insert(current->id).second;
             ++depth) {
          if (!current->visible) {
            return false;
          }
          if (!current->has_hierarchy || current->hierarchy.parent == 0u) {
            return true;
          }
          const auto parentIt = entitiesById.find(current->hierarchy.parent);
          current = parentIt == entitiesById.end() ? nullptr : parentIt->second;
        }
        return true;
      };
      auto qtBuildRtLightIndexByEntity =
          [&](const std::unordered_map<vkpt::core::StableId,
                                       const vkpt::scene::SceneEntityDefinition*>&
                  entitiesById) {
        std::unordered_map<vkpt::core::StableId, std::uint32_t> lightIndexByEntity;
        lightIndexByEntity.reserve(qtSceneDocument.entities.size());
        std::unordered_set<vkpt::core::StableId> ecsLightIds;
        ecsLightIds.reserve(qtSceneDocument.entities.size());
        std::uint32_t lightIndex = 0u;
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_light) {
            continue;
          }
          ecsLightIds.insert(entity.id);
          if (!qtScriptLightIsEnvironment(entity.light.type)) {
            lightIndexByEntity.emplace(entity.id, lightIndex++);
          }
        }
        for (const auto& light : qtSceneDocument.lights) {
          if (ecsLightIds.contains(light.id) ||
              qtScriptLightIsEnvironment(light.light.type)) {
            continue;
          }
          lightIndexByEntity.emplace(light.id, lightIndex++);
        }
        (void)entitiesById;
        return lightIndexByEntity;
      };
      auto qtBuildScriptLightDelta =
          [&](const std::unordered_set<vkpt::core::StableId>& lightTouchedIds,
              const vkpt::scene::SceneWorld* world,
              const std::unordered_map<vkpt::core::StableId,
                                       const vkpt::scene::SceneEntityDefinition*>&
                  entitiesById,
              const std::unordered_map<vkpt::core::StableId, std::uint32_t>&
                  lightIndexByEntity)
              -> std::optional<vkpt::pathtracer::RTSceneDeltaUpdate> {
        if (lightTouchedIds.empty()) {
          return vkpt::pathtracer::RTSceneDeltaUpdate{};
        }
        vkpt::pathtracer::RTSceneDeltaUpdate delta;
        delta.lights.reserve(lightTouchedIds.size());
        for (const auto lightId : lightTouchedIds) {
          const auto entityIt = entitiesById.find(lightId);
          if (entityIt == entitiesById.end() ||
              entityIt->second == nullptr ||
              !entityIt->second->has_light) {
            return std::nullopt;
          }
          const auto& entity = *entityIt->second;
          const auto indexIt = lightIndexByEntity.find(lightId);
          if (indexIt == lightIndexByEntity.end() ||
              indexIt->second >= qtScene.lights.size() ||
              qtScriptLightIsEnvironment(entity.light.type)) {
            return std::nullopt;
          }

          auto rtLight = qtScene.lights[indexIt->second];
          bool hasTransform = entity.has_transform;
          vkpt::scene::TransformComponent transform =
              entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
          if (entity.has_transform && entity.has_hierarchy &&
              entity.hierarchy.parent != 0u) {
            transform = ResolveEntityWorldTransform(entity, world);
          }
          rtLight.position =
              hasTransform ? ToPtVec3(transform.translation)
                           : vkpt::pathtracer::Vec3{0.0f, 1.8f, 0.0f};
          rtLight.color = ToPtVec3(entity.light.color);
          rtLight.intensity =
              qtEntityVisiblePath(entity, entitiesById)
                  ? std::max(0.0f, entity.light.intensity)
                  : 0.0f;
          rtLight.radius = std::max(0.0f, entity.light.radius);
          auto direction = ToPtVec3(entity.light.direction);
          if (PtLength(direction) <= 1.0e-4f) {
            direction = hasTransform
                ? RotatePointByQuat({0.0f, 0.0f, -1.0f}, transform.rotation)
                : vkpt::pathtracer::Vec3{0.0f, -1.0f, 0.0f};
          }
          rtLight.direction = PtNormalize(direction, {0.0f, -1.0f, 0.0f});
          if (qtScriptLightIsSpot(entity.light.type)) {
            const float innerHalfDegrees =
                std::clamp(entity.light.beam_angle_degrees * 0.5f, 1.0f, 89.0f);
            const float outerHalfDegrees = std::clamp(
                innerHalfDegrees * (1.0f + std::max(0.0f, entity.light.blend)),
                innerHalfDegrees + 0.25f,
                89.5f);
            constexpr float kPi = 3.14159265358979323846f;
            rtLight.spot_inner_cos = std::cos(innerHalfDegrees * kPi / 180.0f);
            rtLight.spot_outer_cos = std::cos(outerHalfDegrees * kPi / 180.0f);
          } else {
            rtLight.spot_inner_cos = -1.0f;
            rtLight.spot_outer_cos = -1.0f;
          }
          delta.lights.push_back(
              vkpt::pathtracer::RTLightUpdate{indexIt->second, rtLight});
        }
        return delta;
      };
      auto qtReloadScriptsFromDocument = [&](std::string_view reason) {
        auto worldResult = qtRebuildCachedScriptRuntimeWorld(qtScriptsRunning());
        if (!worldResult) {
          qtScriptState.status = "world build failed";
          ui_runtime_state.last_warning_or_error = "script reload failed: world build";
          qtDockPanelsDirty = true;
          return false;
        }
        auto& world = worldResult.value()->world;
        // reload_bindings reuses the cached LuaStatePool when binding identity
        // is unchanged (toggle re-entry case), so this is no longer a heavy
        // call on play->edit->play. The on_load hook is still dispatched so
        // scripts get a fresh runtime_state regardless.
        qtScriptState.binding_summary = qtScriptRuntime->reload_bindings(world);
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.variables = qtScriptRuntime->variable_snapshots();
        qtScriptState.runtime_states = qtScriptRuntime->runtime_states();
        qtScriptState.status = std::string("bindings reloaded") +
            (reason.empty() ? std::string{} : " (" + std::string(reason) + ")");
        qtScriptState.last_hook = "reload";
        qtScriptState.last_frame = qtFrameCount;
        qtDockPanelsDirty = true;
        return true;
      };
      bool qtSkipScriptReloadForSceneReload = false;
      std::chrono::steady_clock::time_point qtLastDirectScriptSnapshotPublish{};
      constexpr auto kQtPlayModeDirectSnapshotInterval =
          std::chrono::milliseconds(1000);
      auto qtApplyScriptCommandsToDocument =
          [&](const vkpt::scene::WorldCommandBuffer& commands) {
        if (commands.commands().empty()) {
          return false;
        }
        const auto applyStart = std::chrono::steady_clock::now();
        std::size_t createCount = 0u;
        std::size_t destroyCount = 0u;
        std::size_t setTransformCount = 0u;
        std::size_t assignCameraCount = 0u;
        std::size_t assignLightCount = 0u;
        std::size_t assignMaterialCount = 0u;
        std::unordered_set<vkpt::core::StableId> transformTouchedIds;
        std::unordered_set<vkpt::core::StableId> lightTouchedIds;
        std::unordered_set<vkpt::core::StableId> materialTouchedIds;
        bool cameraStateTouched = false;
        bool cameraTransformTouched = false;
        bool fullSceneReloadNeeded = false;
        bool scriptBindingsDirty = false;
        bool physicsTransformTouched = false;
        bool audioSceneDirty = false;
        bool touchesHeroRoot = false;
        bool touchesActionCamera = false;
        bool touchesActionLight = false;
        std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneEntityDefinition*>
            sceneEntitiesById;
        sceneEntitiesById.reserve(qtSceneDocument.entities.size());
        for (const auto& entity : qtSceneDocument.entities) {
          if (entity.id != 0u) {
            sceneEntitiesById.emplace(entity.id, &entity);
          }
        }
        const auto existingLightIndexByEntity =
            qtBuildRtLightIndexByEntity(sceneEntitiesById);
        const auto rtMaterialIndexById = qtBuildRtMaterialIndexById();
        auto canPatchExistingLightEntity = [&](vkpt::core::StableId id) {
          const auto entityIt = sceneEntitiesById.find(id);
          return entityIt != sceneEntitiesById.end() &&
                 entityIt->second != nullptr &&
                 entityIt->second->has_light &&
                 existingLightIndexByEntity.contains(id) &&
                 !qtScriptLightIsEnvironment(entityIt->second->light.type);
        };
        auto markLightTouched = [&](vkpt::core::StableId id) {
          if (canPatchExistingLightEntity(id)) {
            lightTouchedIds.insert(id);
          } else {
            fullSceneReloadNeeded = true;
          }
        };
        auto markTransformTouched = [&](vkpt::core::StableId id) {
          transformTouchedIds.insert(id);
          touchesHeroRoot = touchesHeroRoot || id == 9110u;
          touchesActionCamera = touchesActionCamera || id == 9101u;
          if (auto* entity = qtFindEntity(id)) {
            cameraTransformTouched = cameraTransformTouched || entity->has_camera;
            if (entity->has_light) {
              markLightTouched(id);
            }
            fullSceneReloadNeeded =
                fullSceneReloadNeeded || entity->has_sdf_primitive;
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
            case vkpt::scene::ComponentKind::AudioListener:
            case vkpt::scene::ComponentKind::AudioEmitter:
              audioSceneDirty = true;
              break;
            case vkpt::scene::ComponentKind::UiPanel:
              break;
            case vkpt::scene::ComponentKind::Hierarchy:
            case vkpt::scene::ComponentKind::Light:
            case vkpt::scene::ComponentKind::MeshRenderer:
            case vkpt::scene::ComponentKind::SdfPrimitive:
            case vkpt::scene::ComponentKind::MaterialOverride:
            case vkpt::scene::ComponentKind::PhysicsBody:
              fullSceneReloadNeeded = true;
              break;
            case vkpt::scene::ComponentKind::BenchmarkTag:
            case vkpt::scene::ComponentKind::Skeleton:
            case vkpt::scene::ComponentKind::Count:
              break;
          }
        };
        for (const auto& command : commands.commands()) {
          std::visit([&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::CreateEntityCommand>) {
              ++createCount;
              // Script-spawned UI/helper entities do not touch the render scene.
              // Component commands that add render-visible data still mark the
              // batch for reload below.
              fullSceneReloadNeeded =
                  fullSceneReloadNeeded || payload.requested_id == 0u;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::DestroyEntityCommand>) {
              ++destroyCount;
              fullSceneReloadNeeded = true;
              scriptBindingsDirty = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetComponentCommand> ||
                                 std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AddComponentCommand>) {
              bool componentChanged = true;
              if (payload.kind == vkpt::scene::ComponentKind::Transform) {
                const auto* transform =
                    std::get_if<vkpt::scene::TransformComponent>(&payload.component);
                const auto entityIt = sceneEntitiesById.find(payload.id);
                componentChanged =
                    transform == nullptr ||
                    entityIt == sceneEntitiesById.end() ||
                    entityIt->second == nullptr ||
                    !entityIt->second->has_transform ||
                    !qtTransformNearlyEqual(entityIt->second->transform, *transform);
              }
              if (componentChanged) {
                classifyComponentMutation(payload.id, payload.kind, false);
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::RemoveComponentCommand>) {
              classifyComponentMutation(payload.id, payload.kind, true);
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetTransformCommand>) {
              ++setTransformCount;
              const auto entityIt = sceneEntitiesById.find(payload.id);
              const bool transformChanged =
                  entityIt == sceneEntitiesById.end() ||
                  entityIt->second == nullptr ||
                  !entityIt->second->has_transform ||
                  !qtTransformNearlyEqual(entityIt->second->transform, payload.transform);
              if (transformChanged) {
                markTransformTouched(payload.id);
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReparentEntityCommand>) {
              fullSceneReloadNeeded = true;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignMaterialCommand>) {
              ++assignMaterialCount;
              const auto entityIt = sceneEntitiesById.find(payload.id);
              const auto* entity =
                  entityIt == sceneEntitiesById.end() ? nullptr : entityIt->second;
              const bool materialKnown = rtMaterialIndexById.contains(payload.material_id);
              if (entity == nullptr) {
                fullSceneReloadNeeded = true;
              } else if (entity->has_mesh) {
                const bool materialChanged = entity->mesh.material_id != payload.material_id;
                const auto instanceIt = qtRtInstanceIndexByEntity.find(payload.id);
                const bool canPatchInstanceMaterial =
                    materialChanged &&
                    materialKnown &&
                    instanceIt != qtRtInstanceIndexByEntity.end() &&
                    instanceIt->second < qtScene.instances.size() &&
                    qtScene.instances[instanceIt->second].has_flag(
                        vkpt::pathtracer::kRTInstanceFlagDynamicTransform);
                if (canPatchInstanceMaterial) {
                  materialTouchedIds.insert(payload.id);
                } else if (materialChanged) {
                  fullSceneReloadNeeded = true;
                }
              } else if (entity->has_sdf_primitive) {
                const bool materialChanged =
                    entity->material.material_id != payload.material_id;
                fullSceneReloadNeeded = fullSceneReloadNeeded || materialChanged;
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignCameraCommand>) {
              ++assignCameraCount;
              const auto entityIt = sceneEntitiesById.find(payload.id);
              const bool cameraChanged =
                  entityIt == sceneEntitiesById.end() ||
                  entityIt->second == nullptr ||
                  !entityIt->second->has_camera ||
                  !qtCameraNearlyEqual(entityIt->second->camera, payload.camera);
              cameraStateTouched = cameraStateTouched || cameraChanged;
              touchesActionCamera = touchesActionCamera || payload.id == 9101u;
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignLightCommand>) {
              ++assignLightCount;
              const auto entityIt = sceneEntitiesById.find(payload.id);
              const bool lightChanged =
                  entityIt == sceneEntitiesById.end() ||
                  entityIt->second == nullptr ||
                  !entityIt->second->has_light ||
                  !qtLightNearlyEqual(entityIt->second->light, payload.light);
              const bool canPatchExistingLight =
                  lightChanged &&
                  entityIt != sceneEntitiesById.end() &&
                  entityIt->second != nullptr &&
                  entityIt->second->has_light &&
                  existingLightIndexByEntity.contains(payload.id) &&
                  !qtScriptLightIsEnvironment(payload.light.type);
              if (lightChanged) {
                if (canPatchExistingLight) {
                  lightTouchedIds.insert(payload.id);
                } else {
                  fullSceneReloadNeeded = true;
                }
              }
              touchesActionLight = touchesActionLight || payload.id == 9103u;
            }
          }, command.payload);
        }
        const auto applyClassifyEnd = std::chrono::steady_clock::now();
        const auto transformAffectedIds =
            qtBuildTransformAffectedEntityIds(transformTouchedIds, sceneEntitiesById);
        bool transformRenderInstanceTouched = false;
        if (!transformTouchedIds.empty()) {
          for (const auto& entity : qtSceneDocument.entities) {
            if (transformAffectedIds.contains(entity.id)) {
              if (entity.has_light) {
                markLightTouched(entity.id);
              }
              if (entity.has_sdf_primitive) {
                fullSceneReloadNeeded = true;
              }
            }
            if (entity.has_camera && transformAffectedIds.contains(entity.id)) {
              cameraTransformTouched = true;
            }
            if (entity.has_physics_body && transformAffectedIds.contains(entity.id)) {
              physicsTransformTouched = true;
            }
          }
          for (const auto& instance : qtScene.instances) {
            const auto entityIt = sceneEntitiesById.find(instance.entity_id);
            if (entityIt != sceneEntitiesById.end() && entityIt->second != nullptr &&
                transformAffectedIds.contains(entityIt->second->id)) {
              transformRenderInstanceTouched = true;
              break;
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
                     {"assign_material", std::to_string(assignMaterialCount)},
                     {"transform_entities", std::to_string(transformTouchedIds.size())},
                     {"light_entities", std::to_string(lightTouchedIds.size())},
                     {"material_entities", std::to_string(materialTouchedIds.size())},
                    {"transform_render_instance_touched", transformRenderInstanceTouched ? "true" : "false"},
                    {"camera_state_touched", cameraStateTouched ? "true" : "false"},
                    {"camera_transform_touched", cameraTransformTouched ? "true" : "false"},
                    {"physics_transform_touched", physicsTransformTouched ? "true" : "false"},
                    {"audio_scene_dirty", audioSceneDirty ? "true" : "false"},
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
            } else if constexpr (std::is_same_v<T, vkpt::scene::AudioListenerComponent>) {
              entity.has_audio_listener = true;
              entity.audio_listener = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::AudioEmitterComponent>) {
              entity.has_audio_emitter = true;
              entity.audio_emitter = value;
            } else if constexpr (std::is_same_v<T, vkpt::scene::UiPanelComponent>) {
              entity.has_ui_panel = true;
              entity.ui_panel = value;
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
            case vkpt::scene::ComponentKind::AudioListener:
              entity.has_audio_listener = false;
              entity.audio_listener = {};
              break;
            case vkpt::scene::ComponentKind::AudioEmitter:
              entity.has_audio_emitter = false;
              entity.audio_emitter = {};
              break;
            case vkpt::scene::ComponentKind::UiPanel:
              entity.has_ui_panel = false;
              entity.ui_panel = {};
              break;
            case vkpt::scene::ComponentKind::BenchmarkTag:
              entity.has_benchmark_tag = false;
              entity.benchmark_tag = {};
              break;
            case vkpt::scene::ComponentKind::Skeleton:
              entity.has_skeleton = false;
              entity.skeleton = {};
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
              if (!entity.has_transform ||
                  !qtTransformNearlyEqual(entity.transform, payload.transform)) {
                entity.has_transform = true;
                entity.transform = payload.transform;
                entity.transform.dirty = true;
                qtRemoveLegacyTransformEntry(payload.id);
                changed = true;
              }
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
                  if (entity->mesh.material_id != payload.material_id) {
                    entity->mesh.material_id = payload.material_id;
                    changed = true;
                  }
                } else {
                  if (entity->material.material_id != payload.material_id) {
                    entity->material.material_id = payload.material_id;
                    changed = true;
                  }
                }
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignLightCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Light");
              if (!entity.has_light || !qtLightNearlyEqual(entity.light, payload.light)) {
                entity.has_light = true;
                entity.light = payload.light;
                changed = true;
              }
            } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignCameraCommand>) {
              auto& entity = qtEnsureSceneObjectEntity(payload.id, "Script Camera");
              if (!entity.has_camera || !qtCameraNearlyEqual(entity.camera, payload.camera)) {
                entity.has_camera = true;
                entity.camera = payload.camera;
                changed = true;
              }
            }
          }, command.payload);
        }
        const auto applyMutationEnd = std::chrono::steady_clock::now();

        if (!changed) {
          logger.log(vkpt::log::Severity::Warning,
                     "scripts",
                     "Lua script commands did not mutate scene document",
                     {{"commands", std::to_string(commands.commands().size())}},
                     qtFrameCount);
          VKP_METRIC_OBSERVE(
              "vkp.qt.script_command_apply_us",
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - applyStart).count());
          return false;
        }
        qtReplayScriptCommandsIntoRuntimeWorldCache(commands);
        const auto applyReplayEnd = std::chrono::steady_clock::now();

        if (!fullSceneReloadNeeded) {
          std::optional<vkpt::scene::SceneWorld> scriptCommandWorldSnapshot;
          auto scriptCommandWorld = [&]() -> const vkpt::scene::SceneWorld* {
            if (!scriptCommandWorldSnapshot) {
              const auto worldStart = std::chrono::steady_clock::now();
              scriptCommandWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
              VKP_METRIC_OBSERVE(
                  "vkp.qt.script_world_snapshot_us",
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - worldStart).count());
            }
            return scriptCommandWorldSnapshot ? &scriptCommandWorldSnapshot.value() : nullptr;
          };
          bool incrementalOk = true;
          bool postedTransforms = false;
          bool postedCamera = false;
          bool postedLightDelta = false;
          bool scriptTransformSnapshotDirty = false;
          bool scriptCameraSnapshotDirty = false;
          bool scriptMaterialSnapshotDirty = false;
          bool directTransformSnapshotDirty = false;
          bool directCameraSnapshotDirty = false;
          bool directMaterialSnapshotDirty = false;
          std::vector<vkpt::pathtracer::RTInstanceMaterialUpdate> materialUpdates;
          std::size_t transformUpdateCount = 0u;
          std::size_t lightUpdateCount = 0u;
          std::size_t materialUpdateCount = 0u;

          if (!materialTouchedIds.empty()) {
            materialUpdates.reserve(materialTouchedIds.size());
            for (const auto id : materialTouchedIds) {
              const auto entityIt = sceneEntitiesById.find(id);
              const auto instanceIt = qtRtInstanceIndexByEntity.find(id);
              if (entityIt == sceneEntitiesById.end() ||
                  entityIt->second == nullptr ||
                  !entityIt->second->has_mesh ||
                  instanceIt == qtRtInstanceIndexByEntity.end() ||
                  instanceIt->second >= qtScene.instances.size()) {
                incrementalOk = false;
                break;
              }
              const auto materialIt =
                  rtMaterialIndexById.find(entityIt->second->mesh.material_id);
              if (materialIt == rtMaterialIndexById.end()) {
                incrementalOk = false;
                break;
              }
              if (qtScene.instances[instanceIt->second].material_index ==
                  materialIt->second) {
                continue;
              }
              vkpt::pathtracer::RTInstanceMaterialUpdate update;
              update.entity_id = id;
              update.instance_index = static_cast<uint32_t>(instanceIt->second);
              update.material_index = materialIt->second;
              materialUpdates.push_back(update);
            }
            materialUpdateCount = materialUpdates.size();
          }

          if (!transformTouchedIds.empty() && transformRenderInstanceTouched) {
            auto transformBuild = qtBuildInstanceTransformUpdatesForTouchedEntities(
                transformTouchedIds,
                0u,
                qtScriptTransformRevision,
                scriptCommandWorld(),
                &sceneEntitiesById,
                &transformAffectedIds);
            transformUpdateCount = transformBuild.updates.size();
            incrementalOk = incrementalOk && !transformBuild.unsupported_instances;

            if (incrementalOk && !transformBuild.updates.empty()) {
              if (qtUseBg && qtRenderCoordinator) {
                incrementalOk = vkpt::pathtracer::ApplyInstanceTransformUpdates(
                    qtScene,
                    transformBuild.updates,
                    vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly);
                if (incrementalOk) {
                  scriptTransformSnapshotDirty = true;
                  qtScheduleViewportPickablesRefresh(false);
                  qtMarkTransformDockPanelsDirty(false);
                }
              } else {
                incrementalOk = false;
                if (!qtUseBg && qtTracerReady) {
                  const auto transformOptions =
                      vkpt::pathtracer::MakeStandardTransformUpdateOptions(
                          vkpt::pathtracer::RenderUpdateReason::ScriptTransformMotion,
                          qtFrameCount,
                          "qt-script");
                  const auto plan =
                      qtTracer->plan_instance_transform_update(transformBuild.updates,
                                                               transformOptions);
                  if (vkpt::pathtracer::TransformUpdateStatusAllowedByPolicy(
                          plan.status,
                          transformOptions.fallback_policy) &&
                      plan.can_apply_without_full_fallback()) {
                    const auto result =
                        qtTracer->apply_instance_transform_update(transformBuild.updates,
                                                                  transformOptions);
                    incrementalOk = result.applied() &&
                        vkpt::pathtracer::ApplyInstanceTransformUpdates(
                            qtScene,
                            transformBuild.updates,
                            vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly);
                    if (incrementalOk) {
                      directTransformSnapshotDirty = true;
                      qtScheduleViewportPickablesRefresh(false);
                      qtMarkTransformDockPanelsDirty(false);
                    }
                  }
                } else {
                  incrementalOk = qtPublishInstanceTransformUpdates &&
                                  qtPublishInstanceTransformUpdates(
                                      transformBuild.updates,
                                      vkpt::pathtracer::MakeStandardTransformUpdateOptions(
                                          vkpt::pathtracer::RenderUpdateReason::ScriptTransformMotion,
                                          qtFrameCount,
                                          "qt-script"));
                }
              }
              postedTransforms = incrementalOk;
            }
          }
          const auto applyTransformEnd = std::chrono::steady_clock::now();

          if (incrementalOk && !lightTouchedIds.empty()) {
            const auto lightDelta =
                qtBuildScriptLightDelta(lightTouchedIds,
                                        scriptCommandWorld(),
                                        sceneEntitiesById,
                                        existingLightIndexByEntity);
            incrementalOk = lightDelta.has_value();
            if (incrementalOk) {
              lightUpdateCount = lightDelta->lights.size();
              if (!lightDelta->lights.empty() ||
                  lightDelta->environment_color_changed ||
                  !lightDelta->materials.empty()) {
                incrementalOk =
                    vkpt::pathtracer::ApplySceneDeltaUpdate(qtScene, *lightDelta);
                if (incrementalOk && qtUseBg && qtRenderCoordinator) {
                  scriptMaterialSnapshotDirty = true;
                  postedLightDelta = true;
                } else if (incrementalOk && !qtUseBg && qtTracerReady) {
                  incrementalOk = qtTracer->update_scene_delta(*lightDelta);
                  if (incrementalOk) {
                    directMaterialSnapshotDirty = true;
                    postedLightDelta = true;
                  }
                } else {
                  incrementalOk = false;
                }
                if (postedLightDelta) {
                  qtPublishedSample.store(0u, std::memory_order_relaxed);
                  qtPublishedRays.store(0u, std::memory_order_relaxed);
                }
              } else {
                postedLightDelta = true;
              }
            }
          }
          const auto applyLightEnd = std::chrono::steady_clock::now();

          if (incrementalOk && !materialUpdates.empty()) {
            vkpt::pathtracer::RTSceneDeltaUpdate materialDelta;
            materialDelta.instance_materials = materialUpdates;
            incrementalOk =
                vkpt::pathtracer::ApplySceneDeltaUpdate(qtScene, materialDelta);
            if (incrementalOk && qtUseBg && qtRenderCoordinator) {
              scriptMaterialSnapshotDirty = true;
            } else if (incrementalOk && !qtUseBg && qtTracerReady) {
              incrementalOk = qtTracer->update_scene_delta(materialDelta);
              if (incrementalOk) {
                directMaterialSnapshotDirty = true;
              }
            } else {
              incrementalOk = false;
            }
            if (incrementalOk) {
              qtPublishedSample.store(0u, std::memory_order_relaxed);
              qtPublishedRays.store(0u, std::memory_order_relaxed);
            }
          }
          const auto applyMaterialEnd = std::chrono::steady_clock::now();

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
                vkpt::scene::TransformComponent transform = activeCamera->transform;
                if (activeCamera->has_hierarchy && activeCamera->hierarchy.parent != 0u) {
                  transform = ResolveEntityWorldTransform(*activeCamera, scriptCommandWorld());
                }
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
                scriptCameraSnapshotDirty = true;
                postedCamera = true;
              } else if (!qtUseBg && qtTracerReady) {
                bool cameraOk = qtTracer->update_camera_state(cameraState);
                if (!cameraOk) {
                  cameraOk = qtTracer->load_scene_snapshot(qtScene) &&
                             qtTracer->build_or_update_acceleration();
                }
                incrementalOk = cameraOk;
                postedCamera = incrementalOk;
                if (incrementalOk) {
                  directCameraSnapshotDirty = true;
                }
              } else {
                incrementalOk = false;
              }
            }
          }
          const auto applyCameraEnd = std::chrono::steady_clock::now();

          if (incrementalOk) {
            if (qtUseBg && qtRenderCoordinator &&
                (scriptTransformSnapshotDirty ||
                 scriptCameraSnapshotDirty ||
                 scriptMaterialSnapshotDirty)) {
              qtPublishRenderSnapshotForBackground(false,
                                                   scriptTransformSnapshotDirty,
                                                   scriptCameraSnapshotDirty,
                                                   scriptMaterialSnapshotDirty);
              qtPublishedSample.store(0u, std::memory_order_relaxed);
              qtPublishedRays.store(0u, std::memory_order_relaxed);
            }
            if (!qtUseBg && qtTracerReady &&
                (directTransformSnapshotDirty ||
                 directCameraSnapshotDirty ||
                 directMaterialSnapshotDirty)) {
              qtTracerReady = qtTracer->reset_accumulation();
              incrementalOk = qtTracerReady;
              qtSampleIndex = 0u;
              if (incrementalOk) {
                const auto now = std::chrono::steady_clock::now();
                const bool throttleSnapshot = qtIsPlayMode();
                const bool publishSnapshot =
                    !throttleSnapshot ||
                    qtLastDirectScriptSnapshotPublish ==
                        std::chrono::steady_clock::time_point{} ||
                    now - qtLastDirectScriptSnapshotPublish >=
                        kQtPlayModeDirectSnapshotInterval;
                if (publishSnapshot) {
                  (void)qtPublishRenderSnapshot(false,
                                                directTransformSnapshotDirty,
                                                directCameraSnapshotDirty,
                                                directMaterialSnapshotDirty);
                  qtLastDirectScriptSnapshotPublish = now;
                }
                qtPublishedSample.store(0u, std::memory_order_relaxed);
                qtPublishedRays.store(0u, std::memory_order_relaxed);
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
            if (audioSceneDirty && qtAudioSystem) {
              qtAudioSystem->load_scene_audio(qtSceneDocument, RuntimeSceneDisplayName(config.scene_path.value));
            }
            qtDockPanelsDirty = true;
            qtPreviewStatus = qtIsPlayMode()
                ? "play mode"
                : (qtIsLiveEditMode() ? "live edit" : "editor mode");
            logger.log(vkpt::log::Severity::Info,
                       "scripts",
                       "Lua script commands applied incrementally",
                        {{"commands", std::to_string(commands.commands().size())},
                         {"transform_updates", postedTransforms ? "true" : "false"},
                         {"transform_update_count", std::to_string(transformUpdateCount)},
                         {"light_delta", postedLightDelta ? "true" : "false"},
                        {"light_update_count", std::to_string(lightUpdateCount)},
                        {"material_update_count", std::to_string(materialUpdateCount)},
                        {"camera_update", postedCamera ? "true" : "false"},
                        {"script_bindings_reloaded", scriptBindingsDirty ? "true" : "false"}},
                       qtFrameCount);
            const auto applyEnd = std::chrono::steady_clock::now();
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_classify_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyClassifyEnd - applyStart).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_mutate_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyMutationEnd - applyClassifyEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_replay_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyReplayEnd - applyMutationEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_transform_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyTransformEnd - applyReplayEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_light_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyLightEnd - applyTransformEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_material_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyMaterialEnd - applyLightEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_camera_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyCameraEnd - applyMaterialEnd).count());
            VKP_METRIC_OBSERVE(
                "vkp.qt.script_command_apply_us",
                std::chrono::duration_cast<std::chrono::microseconds>(
                    applyEnd - applyStart).count());
            return true;
          }
        }

        const auto reloadStart = std::chrono::steady_clock::now();
        if (qtReloadEditedScene) {
          const bool previousSkipScriptReload = qtSkipScriptReloadForSceneReload;
          qtSkipScriptReloadForSceneReload = !scriptBindingsDirty;
          qtReloadEditedScene("script command");
          qtSkipScriptReloadForSceneReload = previousSkipScriptReload;
        } else if (scriptBindingsDirty) {
          qtReloadScriptsFromDocument("script command");
        }
        logger.log(vkpt::log::Severity::Info,
                   "scripts",
                   "Lua script commands applied and scene reloaded",
                   {{"commands", std::to_string(commands.commands().size())}},
                   qtFrameCount);
        const auto applyEnd = std::chrono::steady_clock::now();
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_command_classify_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                applyClassifyEnd - applyStart).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_command_mutate_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                applyMutationEnd - applyClassifyEnd).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_command_replay_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                applyReplayEnd - applyMutationEnd).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_command_reload_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                applyEnd - reloadStart).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_command_apply_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                applyEnd - applyStart).count());
        return true;
      };
      auto qtDispatchScriptHook =
          [&](vkpt::scripting::ScriptLifecycleHook hook,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds,
              std::string_view source) {
        if (!qtScriptsRunning()) {
          qtScriptState.status = "script dispatch blocked: press F1 for Play or F2 for Live Edit";
          ui_runtime_state.status_message = qtScriptState.status;
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          logger.log(vkpt::log::Severity::Warning,
                     "scripts",
                     "script dispatch blocked outside script runtime mode",
                     {{"hook", std::string(vkpt::scripting::to_string(hook))},
                      {"source", std::string(source)}},
                     frameIndex);
          return false;
        }
        try {
        const auto dispatchStart = std::chrono::steady_clock::now();
        vkpt::scene::SceneScriptRuntimeWorld* runtimeWorld = nullptr;
        // Both Play (F1) and LiveEdit (F2) reuse the cached runtime world. The
        // cache is invalidated on mode toggle, scene reload, and scripted
        // command replay failure (see qtScriptRuntimeWorldCache.reset() sites),
        // so it stays consistent with the document. Rebuilding it every
        // dispatch was the dominant per-frame cost on scenes carrying skeletal
        // assets — `runtime_document.to_world()` deep-copies every entity's
        // animation_clips vector, which can easily run into hundreds of
        // kilobytes per skeletal entity (see hero_hit_demo.json + low_poly_hero
        // glTF: ~14 clips × tens of joints × tens of keyframes each). That
        // scaled the F2 frame loop down to ~7 FPS in the user's report.
        auto worldResult = qtGetCachedScriptRuntimeWorld(qtScriptsRunning());
        if (!worldResult) {
          qtScriptState.status = "dispatch failed: world build";
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          return false;
        }
        runtimeWorld = worldResult.value();
        const auto dispatchWorldEnd = std::chrono::steady_clock::now();
        auto& world = runtimeWorld->world;
        // Script bindings and Lua entity APIs read local component state.
        // Recomputing the full transform cache here costs O(scene size) every
        // OnUpdate and is only needed by callers that explicitly query world
        // transforms, which the scripting API does not expose.
        vkpt::scene::WorldCommandBuffer commands;
        vkpt::scripting::ScriptExecutionContext context;
        context.frame = frameIndex;
        context.delta_seconds = dtSeconds;
        context.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - qtScriptRuntimeStartTime).count();
        context.scripts_enabled = qtScriptsRunning() && qtScriptState.scripts_enabled;
        context.deterministic = deterministic;
        if (startupDeterminismContext) {
          context.determinism_base_seed = startupDeterminismContext->base_seed;
          context.determinism_scenario_id = startupDeterminismContext->scenario_id;
        }
        context.game_mode = qtIsPlayMode();
        context.runtime.mode = std::string(qtRuntimeModeName());
        context.runtime.scripts_running = qtScriptsRunning();
        context.benchmark_mode = qtSceneDocument.benchmark.enabled;
        context.allow_benchmark_scripts = qtScriptState.benchmark_scripts_allowed;
        const bool luaInputEnabled = qtIsPlayMode() || qtScriptState.viewport_input_forwarding;
        context.input.enabled = luaInputEnabled;
        context.input.viewport_focused = luaInputEnabled;
        if (luaInputEnabled) {
          for (const int key : qtKeysDown) {
            if (key == kQtKeyEscape || key == kQtKeyF1 || key == kQtKeyF2) {
              continue;
            }
            context.input.active_keys.push_back(key);
          }
          context.input.mouse_delta_x = qtPendingFpsLookDx;
          context.input.mouse_delta_y = qtPendingFpsLookDy;
          if (qtLeftMouseDown) {
            context.input.active_mouse_buttons.push_back(0);
          }
          if (qtRightMouseDown) {
            context.input.active_mouse_buttons.push_back(1);
          }
          if (qtMiddleMouseDown) {
            context.input.active_mouse_buttons.push_back(2);
          }
        }
        context.editor.canvas_enabled = !qtIsPlayMode();
        context.editor.is_editing = qtGizmoDrag.active;
        if (qtGizmoDrag.active && !qtGizmoDrag.entities.empty()) {
          context.editor.edited_entity_id = qtGizmoDrag.entities.front().entity_id;
          context.editor.edited_component = "Transform";
        }
        const auto dispatchContextEnd = std::chrono::steady_clock::now();
        logger.log(vkpt::log::Severity::Debug,
                   "scripts",
                   "dispatching Lua script hook",
                   {{"hook", std::string(vkpt::scripting::to_string(hook))},
                    {"source", std::string(source)},
                    {"frame", std::to_string(frameIndex)},
                    {"dt", std::to_string(dtSeconds)},
                    {"runtime_mode", std::string(qtRuntimeModeName())},
                    {"scripts_enabled", context.scripts_enabled ? "true" : "false"},
                    {"play_mode", context.game_mode ? "true" : "false"},
                    {"lua_input_enabled", context.input.enabled ? "true" : "false"},
                    {"lua_compiled_in", qtScriptRuntime->lua_compiled_in() ? "true" : "false"},
                    {"execution_available", qtScriptRuntime->execution_available() ? "true" : "false"},
                    {"active_keys", std::to_string(context.input.active_keys.size())},
                    {"active_mouse_buttons",
                     std::to_string(context.input.active_mouse_buttons.size())},
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
        const auto dispatchLuaEnd = std::chrono::steady_clock::now();
        qtScriptState.binding_summary = vkpt::scripting::SummarizeScriptBindings(
            qtScriptRuntime->bindings(),
            qtScriptRuntime->lua_compiled_in(),
            qtScriptRuntime->execution_available());
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.variables = qtScriptRuntime->variable_snapshots();
        qtScriptState.runtime_states = qtScriptRuntime->runtime_states();
        qtScriptState.last_hook = std::string(vkpt::scripting::to_string(hook));
        qtScriptState.last_frame = frameIndex;
        ++qtScriptState.dispatch_count;
        qtScriptState.status =
            std::string(source) + " " + qtScriptState.last_hook +
            " runnable=" + std::to_string(qtScriptState.last_dispatch.runnable_count) +
            " skipped=" + std::to_string(qtScriptState.last_dispatch.skipped_count);
        const auto dispatchStateEnd = std::chrono::steady_clock::now();
        const auto dispatchApplyStart = std::chrono::steady_clock::now();
        if (!commands.commands().empty()) {
          qtScriptState.status += " commands=" + std::to_string(commands.commands().size());
          qtApplyScriptCommandsToDocument(commands);
        }
        const auto dispatchApplyEnd = std::chrono::steady_clock::now();
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
        const auto dispatchEnd = std::chrono::steady_clock::now();
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_world_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchWorldEnd - dispatchStart).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_context_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchContextEnd - dispatchWorldEnd).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_lua_dispatch_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchLuaEnd - dispatchContextEnd).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_state_sync_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchStateEnd - dispatchLuaEnd).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_apply_commands_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchApplyEnd - dispatchApplyStart).count());
        VKP_METRIC_OBSERVE(
            "vkp.qt.script_total_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                dispatchEnd - dispatchStart).count());
        return true;
        } catch (const std::exception& error) {
          const std::string hookName(vkpt::scripting::to_string(hook));
          const std::string sourceName(source);
          qtScriptState.scripts_enabled = false;
          qtScriptState.playing = false;
          qtScriptState.mode = std::string(kRuntimeModeEditName);
          qtScriptState.last_hook = hookName;
          qtScriptState.last_frame = frameIndex;
          qtScriptState.status =
              std::string("script runtime fault; scripts disabled: ") + error.what();
          ui_runtime_state.status_message = qtScriptState.status;
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          qtSyncViewportAssetDropMode();
          logger.log(vkpt::log::Severity::Error,
                     "scripts",
                     "script dispatch failed; scripts disabled",
                     {{"hook", hookName},
                      {"source", sourceName},
                      {"error", error.what()}},
                     frameIndex);
          return false;
        } catch (...) {
          const std::string hookName(vkpt::scripting::to_string(hook));
          const std::string sourceName(source);
          qtScriptState.scripts_enabled = false;
          qtScriptState.playing = false;
          qtScriptState.mode = std::string(kRuntimeModeEditName);
          qtScriptState.last_hook = hookName;
          qtScriptState.last_frame = frameIndex;
          qtScriptState.status = "script runtime fault; scripts disabled";
          ui_runtime_state.status_message = qtScriptState.status;
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          qtSyncViewportAssetDropMode();
          logger.log(vkpt::log::Severity::Error,
                     "scripts",
                     "script dispatch failed with unknown exception; scripts disabled",
                     {{"hook", hookName},
                      {"source", sourceName}},
                     frameIndex);
          return false;
        }
      };
      (void)qtDispatchScriptHook;
      qtReloadScriptsFromDocument("startup");
      // Lua OnUpdate dispatch still runs on the UI thread. Use a post-dispatch
      // cooldown so a slow hook/application pass does not immediately cause
      // another script pass on the next rendered frame.
      double qtScriptHookAccumulatorSeconds = 0.0;
      constexpr double kQtScriptHookTargetIntervalSeconds = 1.0 / 30.0;
      constexpr double kQtScriptHookMaxDispatchDtSeconds = 0.10;
      std::chrono::steady_clock::time_point qtNextScriptHookTime{};
      auto qtApplyGameModeScripts =
          [&](std::chrono::steady_clock::time_point now,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds) {
        if (!qtScriptsRunning()) {
          qtScriptHookAccumulatorSeconds = 0.0;
          qtNextScriptHookTime = {};
          return false;
        }
        qtScriptHookAccumulatorSeconds = std::min(
            qtScriptHookAccumulatorSeconds + std::max(0.0, dtSeconds),
            kQtScriptHookMaxDispatchDtSeconds);
        if (qtNextScriptHookTime != std::chrono::steady_clock::time_point{} &&
            now < qtNextScriptHookTime) {
          return false;
        }
        if (qtScriptHookAccumulatorSeconds < kQtScriptHookTargetIntervalSeconds) {
          return false;
        }
        const double dispatchedDt =
            std::min(qtScriptHookAccumulatorSeconds, kQtScriptHookMaxDispatchDtSeconds);
        qtScriptHookAccumulatorSeconds = 0.0;
        const bool dispatched =
            qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate,
                                 frameIndex,
                                 dispatchedDt,
                                 qtIsPlayMode() ? "play mode" : "live edit");
        qtNextScriptHookTime = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kQtScriptHookTargetIntervalSeconds));
        return dispatched;
      };
      // Phase 5 RAG05+: tick the animation sampler each frame against the
      // cached scripting world so entities with Skeleton + AnimationComponent
      // get their joint_world_matrices written. This is the live counterpart
      // to the explicit tick_animations() calls in tests/hero_hit_demo_smoke.
      //
      // Ragdoll integration is handled here defensively: when the script flips
      // RagdollComponent.active = true with seed_from_animation/has_impulse
      // hints, we log the activation, capture the animation pose for any
      // future seeding, and CLEAR the one-shot flags so they only fire once.
      // The full Jolt body construction + step is deferred — the ragdoll
      // bodies need to live alongside the existing ThreadedPhysicsWorld's
      // JPH::PhysicsSystem, which is owned by the dedicated physics worker
      // thread and not yet exposed by IPhysicsWorld. Driving the bodies from
      // the qt thread without that exposure would race with the worker. The
      // smoke (tests/hero_hit_demo_smoke) covers the full path with a
      // dedicated JPH::PhysicsSystem; production wiring is tracked under the
      // RAG-MAIN-LOOP TODO below.
      // RAG-LIVE-LOOP fix: when the script-bearing entity carries
      // RagdollComponent / AnimationComponent but the Skeleton is on a
      // different entity (the asset-import root or one of its descendants —
      // typically a sibling under the same parent for `assets[].parent ==
      // entity.parent`), we need to walk the scene to locate the skeleton
      // host. The wiring lambda then attaches the ragdoll keyed by the
      // origin entity id (so script-side enable_ragdoll mapping stays
      // intact) but seeds + reads pose data through the resolved skeleton
      // host. The cache below is populated lazily and cleared whenever the
      // scene is reloaded.
      std::unordered_map<vkpt::core::StableEntityId,
                         vkpt::core::StableEntityId>
          qtRagdollSkeletonHostByOrigin;
      auto qtResolveSkeletonHost =
          [&](vkpt::scene::SceneWorld& world,
              vkpt::core::StableEntityId origin)
              -> vkpt::core::StableEntityId {
        if (origin == 0u) {
          return 0u;
        }
        auto* origin_entity = world.get_entity(origin);
        if (origin_entity != nullptr && origin_entity->skeleton.has_value()) {
          return origin;
        }
        if (auto cached = qtRagdollSkeletonHostByOrigin.find(origin);
            cached != qtRagdollSkeletonHostByOrigin.end()) {
          // Validate cached entry still has a skeleton; entity may have been
          // destroyed by a scene edit.
          if (auto* host = world.get_entity(cached->second);
              host != nullptr && host->skeleton.has_value()) {
            return cached->second;
          }
          qtRagdollSkeletonHostByOrigin.erase(cached);
        }
        // BFS over descendants first.
        std::vector<vkpt::core::StableEntityId> frontier =
            world.children_of(origin);
        for (std::size_t i = 0; i < frontier.size(); ++i) {
          const auto candidate = frontier[i];
          if (auto* rec = world.get_entity(candidate);
              rec != nullptr && rec->skeleton.has_value()) {
            qtRagdollSkeletonHostByOrigin[origin] = candidate;
            return candidate;
          }
          auto next = world.children_of(candidate);
          frontier.insert(frontier.end(), next.begin(), next.end());
          if (frontier.size() > 256u) break;  // safety cap
        }
        // Fallback: walk siblings under the same parent. The hero_hit_demo
        // scene parents both the asset-ref entity (script + ragdoll +
        // animation) and the imported asset root (skeleton + clips) under
        // the same demo root, making them siblings.
        if (origin_entity != nullptr && origin_entity->hierarchy.has_value()) {
          const auto parent = origin_entity->hierarchy->parent;
          if (parent != 0u) {
            for (const auto sibling : world.children_of(parent)) {
              if (sibling == origin) continue;
              if (auto* rec = world.get_entity(sibling);
                  rec != nullptr && rec->skeleton.has_value()) {
                qtRagdollSkeletonHostByOrigin[origin] = sibling;
                return sibling;
              }
              // Walk descendants of the sibling too.
              std::vector<vkpt::core::StableEntityId> sib_frontier =
                  world.children_of(sibling);
              for (std::size_t i = 0; i < sib_frontier.size(); ++i) {
                const auto cand = sib_frontier[i];
                if (auto* rec = world.get_entity(cand);
                    rec != nullptr && rec->skeleton.has_value()) {
                  qtRagdollSkeletonHostByOrigin[origin] = cand;
                  return cand;
                }
                auto next = world.children_of(cand);
                sib_frontier.insert(sib_frontier.end(), next.begin(), next.end());
                if (sib_frontier.size() > 256u) break;
              }
            }
          }
        }
        return 0u;
      };
      bool qtSceneTreeDumpDone = false;
      auto qtTickAnimationsAndRagdolls =
          [&](double dtSeconds, vkpt::core::FrameIndex frameIndex) {
        (void)frameIndex;
        if (!qtScriptRuntimeWorldCache.has_value()) {
          return;
        }
        if (!std::isfinite(dtSeconds) || dtSeconds <= 0.0) {
          return;
        }
        const float dt = static_cast<float>(
            std::min(dtSeconds, 0.10));  // cap for safety
        auto& world = qtScriptRuntimeWorldCache->world;
        const auto tickStart = std::chrono::steady_clock::now();
        // One-shot scene tree dump for ragdoll/animation diagnostics. Logs
        // each entity's id + parent + which of {skeleton, ragdoll, anim,
        // clips} are present. Helps verify the parent->skeleton mapping when
        // skinning/ragdoll appears broken.
        if (!qtSceneTreeDumpDone) {
          qtSceneTreeDumpDone = true;
          for (const auto eid : world.all_entities()) {
            auto* rec = world.get_entity(eid);
            if (rec == nullptr) continue;
            const auto parent = rec->hierarchy.has_value()
                                    ? rec->hierarchy->parent
                                    : vkpt::core::StableEntityId{0};
            logger.log(vkpt::log::Severity::Info, "ragdoll",
                       "scene tree at startup",
                       {{"entity_id", std::to_string(eid)},
                        {"parent_id", std::to_string(parent)},
                        {"name", rec->identity.name},
                        {"has_skeleton",
                         rec->skeleton.has_value() ? "1" : "0"},
                        {"has_ragdoll",
                         rec->ragdoll.has_value() ? "1" : "0"},
                        {"has_animation",
                         rec->animation.has_value() ? "1" : "0"},
                        {"has_clips",
                         rec->clips.empty() ? "0" : "1"}});
          }
        }
        // 1. Animation sampler — early-outs internally on entities with no
        //    skeleton / no AnimationComponent / no clips, so this is cheap on
        //    scenes that don't carry any animated entities.
        //    RAG-LIVE-LOOP: relay AnimationComponents authored on a
        //    non-skeleton entity (e.g. the asset-ref entity) to the resolved
        //    skeleton host so the sampler sees them paired up. We mirror the
        //    component onto the host (replacing only when the host doesn't
        //    already have its own authored component) and mirror the clips
        //    list back to the origin if the host has clips but origin
        //    doesn't (so the origin's lua bindings still resolve clip
        //    names).
        for (const auto eid : world.all_entities()) {
          auto* rec = world.get_entity(eid);
          if (rec == nullptr) continue;
          if (rec->skeleton.has_value()) continue;  // skeleton host already
          if (!rec->animation.has_value()) continue;
          const auto host_id = qtResolveSkeletonHost(world, eid);
          if (host_id == 0u || host_id == eid) continue;
          auto* host = world.get_entity(host_id);
          if (host == nullptr || !host->skeleton.has_value()) continue;
          // Mirror clips both ways. The asset loader fills clips on the
          // host (skeleton owner); the lua bindings on the origin entity
          // need to see them too so set_animation_clip("idle") can resolve
          // the name. Conversely, if the scene authored clips on the
          // origin (rare), forward them to the host.
          if (rec->clips.empty() && !host->clips.empty()) {
            rec->clips = host->clips;
          }
          if (host->clips.empty() && !rec->clips.empty()) {
            host->clips = rec->clips;
          }
          // Forward AnimationComponent fields from origin to host.
          // The script may have called set_animation_clip with a name that
          // didn't resolve at script time (origin's clips were empty until
          // the mirror above). On the next tick origin still has
          // clip_index=-1; in that case keep host's auto-selected clip
          // (default 0) so the asset still animates. If the origin has a
          // valid clip_index, that is the script's authoritative choice.
          if (!host->animation.has_value()) {
            host->animation = rec->animation;
          }
          auto& host_anim = *host->animation;
          const auto& src = *rec->animation;
          if (src.clip_index >= 0) {
            host_anim.clip_index = src.clip_index;
          }
          host_anim.speed = src.speed;
          host_anim.loop = src.loop;
          host_anim.paused = src.paused;
          // time_seconds is advanced by tick_animations on the host; do
          // not overwrite from the origin every frame (would freeze
          // playback at 0). Only honor explicit script writes via
          // set_animation_time would require origin->host one-shot — out
          // of scope; current scenes rely on continuous playback.
        }
        vkpt::animation::tick_animations(world, dt);
        // 2. Ragdoll lifecycle + step. RAG-MAIN-LOOP wiring: when an
        //    entity's RagdollComponent.active flips false->true, attach a
        //    ragdoll to qtRagdollWorld; when it flips true->false, detach.
        //    Per active frame, after the step, write the latest joint
        //    matrices into EntityRecord::joint_world_matrices so the
        //    renderer/skinning sees the ragdoll pose.
        for (const auto entity_id : world.all_entities()) {
          auto* entity = world.get_entity(entity_id);
          if (entity == nullptr) {
            continue;
          }
          const bool wantsActive =
              entity->ragdoll.has_value() && entity->ragdoll->active;
          const auto handleIt = qtRagdollHandles.find(entity_id);
          const bool hasHandle = (handleIt != qtRagdollHandles.end());
          // Resolve the skeleton host. If the origin entity carries the
          // skeleton itself, host_id == entity_id and `host_entity` aliases
          // `entity`. Otherwise the host is a sibling/descendant carrying
          // the skeleton + clips imported from the model asset.
          const auto host_id =
              entity->skeleton.has_value()
                  ? entity_id
                  : qtResolveSkeletonHost(world, entity_id);
          auto* host_entity = (host_id == 0u || host_id == entity_id)
                                  ? entity
                                  : world.get_entity(host_id);

          if (wantsActive && !hasHandle &&
              host_entity != nullptr &&
              host_entity->skeleton.has_value() &&
              qtRagdollWorld.is_available()) {
            const auto& skel = *host_entity->skeleton;
            // Spawn pose: use entity's translation as world origin.
            vkpt::scene::Mat4 spawn{};
            spawn.values[0] = 1.0f;
            spawn.values[5] = 1.0f;
            spawn.values[10] = 1.0f;
            spawn.values[15] = 1.0f;
            if (entity->transform.has_value()) {
              spawn.values[12] = entity->transform->translation.x;
              spawn.values[13] = entity->transform->translation.y;
              spawn.values[14] = entity->transform->translation.z;
            }
            vkpt::physics::RagdollConfig cfg;
            cfg.capsule_radius_scale = entity->ragdoll->capsule_radius_scale;
            cfg.spine_capsule_radius = entity->ragdoll->spine_capsule_radius;
            cfg.head_capsule_radius = entity->ragdoll->head_capsule_radius;
            cfg.density = entity->ragdoll->density;
            cfg.self_collision = entity->ragdoll->self_collision;
            const auto handle = qtRagdollWorld.attach_ragdoll(
                entity_id, skel, spawn, cfg);
            if (handle.valid()) {
              qtRagdollHandles[entity_id] = handle;
              logger.log(vkpt::log::Severity::Info, "ragdoll",
                         "ragdoll_attached_qt",
                         {{"entity_id", std::to_string(entity_id)},
                          {"skeleton_host_id", std::to_string(host_id)},
                          {"handle", std::to_string(handle.id)},
                          {"joints", std::to_string(skel.joints.size())}});
              // Seed from the freshly-sampled animation pose if requested.
              // Read pose from the SKELETON HOST (not the origin entity);
              // the sampler writes joint_world_matrices on the host.
              auto anim_world = host_entity->joint_world_matrices;
              if (entity->transform.has_value()) {
                const auto& t = entity->transform->translation;
                for (auto& m : anim_world) {
                  m.values[12] += t.x;
                  m.values[13] += t.y;
                  m.values[14] += t.z;
                }
              }
              if (entity->ragdoll->seed_from_animation && !anim_world.empty()) {
                auto prev_it = qtRagdollPrevAnim.find(entity_id);
                const std::vector<vkpt::scene::Mat4>* prev_ptr =
                    (prev_it != qtRagdollPrevAnim.end() &&
                     prev_it->second.size() == anim_world.size())
                        ? &prev_it->second
                        : nullptr;
                qtRagdollWorld.seed_ragdoll_pose(handle, anim_world, prev_ptr,
                                                 dt);
              }
              if (entity->ragdoll->has_impulse) {
                std::int32_t target = skel.root_index;
                const auto& target_name = entity->ragdoll->impulse_joint;
                if (!target_name.empty()) {
                  for (std::size_t j = 0; j < skel.joints.size(); ++j) {
                    if (skel.joints[j].name == target_name) {
                      target = static_cast<std::int32_t>(j);
                      break;
                    }
                  }
                }
                if (target < 0) target = skel.root_index;
                qtRagdollWorld.apply_ragdoll_impulse(handle, target,
                                                    entity->ragdoll->impulse);
              }
              entity->ragdoll->seed_from_animation = false;
              entity->ragdoll->has_impulse = false;
            }
          } else if (!wantsActive && hasHandle) {
            qtRagdollWorld.detach_ragdoll(handleIt->second);
            qtRagdollHandles.erase(handleIt);
          }

          // Cache the just-sampled animation pose for any future seed call.
          // Read from the host_entity (skeleton owner), key by origin
          // entity_id so the seed lookup at activation matches.
          if (host_entity != nullptr && host_entity->skeleton.has_value() &&
              !host_entity->joint_world_matrices.empty()) {
            qtRagdollPrevAnim[entity_id] = host_entity->joint_world_matrices;
            if (entity->transform.has_value()) {
              const auto& t = entity->transform->translation;
              for (auto& m : qtRagdollPrevAnim[entity_id]) {
                m.values[12] += t.x;
                m.values[13] += t.y;
                m.values[14] += t.z;
              }
            }
          }
        }

        // 3. Step the ragdoll world.
        qtRagdollWorld.step(dt);

        // 4. After step, for each active ragdoll write joint matrices into
        //    the SKELETON HOST entity record so the renderer/skinning
        //    consumes the physics pose (authority arbiter would otherwise
        //    hand back animation results). The handle is keyed by origin
        //    entity_id (matching the lua-side enable_ragdoll mapping) but
        //    we resolve to the host before writing.
        for (const auto& [entity_id, handle] : qtRagdollHandles) {
          auto* entity = world.get_entity(entity_id);
          if (entity == nullptr) continue;
          const auto host_id =
              entity->skeleton.has_value()
                  ? entity_id
                  : qtResolveSkeletonHost(world, entity_id);
          auto* host = (host_id == 0u || host_id == entity_id)
                           ? entity
                           : world.get_entity(host_id);
          if (host == nullptr) continue;
          auto rag_local = qtRagdollWorld.read_ragdoll_joint_matrices(handle);
          if (rag_local.empty()) continue;
          // RagdollWorld::read_ragdoll_joint_matrices() now returns
          // matrices in MESH-LOCAL skinning frame (matches the convention
          // produced by AnimationSampler::compose_world_matrices()), so
          // the previous translation-subtract here would over-shift.
          // See the comment chain in Ragdoll::read_joint_local_skinning_matrices():
          // each joint folds the owning body's rigid motion since build()
          // into bind_world[j], expressed in mesh-local space — exactly
          // what compute_skinning_matrices() expects.
          host->joint_world_matrices = std::move(rag_local);
        }

        // Phase 4 GPU side: compute skinning matrices and stage them onto a
        // per-entity cache so the next snapshot publish carries them. The
        // CPU skinning fallback (ApplyCpuSkinningToScene) deforms
        // qtScene.local_vertices in place so dynamic-instance intersection
        // (which reads local_vertices via local_first_vertex) sees the
        // posed mesh. The full GPU compute + BLAS refit dispatch remains a
        // documented Phase 4 scope cut (see skin_vertices_cs.hlsl); this
        // path is the hard fallback authorized by that scope.
        qtSkinnedSnapshotEntries.clear();
        bool anySkinned = false;
        std::size_t debug_skel_count = 0;
        std::size_t debug_jw_empty = 0;
        std::size_t debug_no_match_inst = 0;
        for (const auto entity_id : world.all_entities()) {
          auto* entity = world.get_entity(entity_id);
          if (entity == nullptr) continue;
          if (!entity->skeleton.has_value()) continue;
          ++debug_skel_count;
          if (entity->joint_world_matrices.empty()) {
            ++debug_jw_empty;
            continue;
          }
          const auto& skel = *entity->skeleton;
          if (entity->joint_world_matrices.size() != skel.joints.size()) continue;
          auto matrices = vkpt::animation::compute_skinning_matrices(
              skel, entity->joint_world_matrices);
          // The skeleton lives on the imported asset root; the RT instances
          // are typically child mesh entities (one per material bucket).
          // Match the skeleton to every RT instance whose parent chain
          // includes this entity, AND to a direct match if any (for hand-
          // authored single-entity meshes).
          std::vector<std::uint32_t> matched_instances;
          if (auto it = qtRtInstanceIndexByEntity.find(entity_id);
              it != qtRtInstanceIndexByEntity.end()) {
            matched_instances.push_back(it->second);
          }
          for (const auto child_id : world.all_entities()) {
            if (child_id == entity_id) continue;
            const auto* child = world.get_entity(child_id);
            if (child == nullptr) continue;
            // Walk parent chain looking for entity_id.
            vkpt::core::StableEntityId cursor = child_id;
            bool linked = false;
            for (int depth = 0; depth < 16; ++depth) {
              const auto* cur = world.get_entity(cursor);
              if (cur == nullptr) break;
              if (!cur->hierarchy.has_value()) break;
              const auto parent = cur->hierarchy->parent;
              if (parent == 0u) break;
              if (parent == entity_id) { linked = true; break; }
              cursor = parent;
            }
            if (!linked) continue;
            if (auto it2 = qtRtInstanceIndexByEntity.find(child_id);
                it2 != qtRtInstanceIndexByEntity.end()) {
              matched_instances.push_back(it2->second);
            }
          }
          if (matched_instances.empty()) {
            ++debug_no_match_inst;
            continue;
          }
          for (const auto inst_idx : matched_instances) {
            vkpt::scene::SkinnedInstanceEntry entry;
            entry.instance_index = inst_idx;
            entry.skeleton_joint_count =
                static_cast<std::uint32_t>(skel.joints.size());
            entry.skinning_matrices = matrices;
            qtSkinnedSnapshotEntries.push_back(std::move(entry));
          }
          anySkinned = true;
        }
        if (anySkinned) {
          // CPU-side skinning into qtScene.local_vertices. This is the
          // hard-fallback authorized by the P4 GPU-side scope: visually
          // correct, slower than GPU compute, but preserves all
          // frames-in-flight / readback-fence invariants because mutation
          // happens on the qt thread before the renderer reads the scene.
          vkpt::scene::ApplyCpuSkinningToScene(qtScene, qtSkinnedSnapshotEntries);
          if (!qtSkinnedSceneDirty) {
            logger.log(vkpt::log::Severity::Info, "skinning",
                       "cpu skinning active (P4 hard fallback)",
                       {{"entries", std::to_string(qtSkinnedSnapshotEntries.size())},
                        {"first_joints", std::to_string(qtSkinnedSnapshotEntries.front().skeleton_joint_count)}});
          }
          qtSkinnedSceneDirty = true;
        } else {
          static int skinning_diag_logged = 0;
          if (skinning_diag_logged == 0 && debug_skel_count > 0) {
            skinning_diag_logged = 1;
            logger.log(vkpt::log::Severity::Info, "skinning",
                       "skinning gate diagnostic",
                       {{"skeletal_entities", std::to_string(debug_skel_count)},
                        {"with_empty_joint_world", std::to_string(debug_jw_empty)},
                        {"no_matching_rt_instance", std::to_string(debug_no_match_inst)}});
          }
          if (qtSkinnedSceneDirty) {
            // Last frame had skinning; this frame doesn't. Restore bind pose so
            // the mesh doesn't stick at the last skinned position.
            if (qtScene.local_vertices.size() == qtScene.bind_local_vertices.size()) {
              qtScene.local_vertices = qtScene.bind_local_vertices;
            }
            qtSkinnedSceneDirty = false;
          }
        }

        const auto tickEnd = std::chrono::steady_clock::now();
        VKP_METRIC_OBSERVE(
            "vkp.qt.animation_tick_us",
            std::chrono::duration_cast<std::chrono::microseconds>(
                tickEnd - tickStart).count());
      };
      qtReloadEditedScene = [&](std::string_view reason) {
        const auto reloadStart = std::chrono::steady_clock::now();
        if (qtFpsMode) {
          qtMaybeSyncFpsCameraObject(true);
        }
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
        const auto conversionEnd = std::chrono::steady_clock::now();
        if (!sceneResult) {
          ui_runtime_state.status_message = "scene edit failed: rebuild scene data";
          qtPreviewStatus = "scene edit failed";
          return false;
        }
        auto nextScene = std::move(sceneResult.value());
        const auto sceneDelta = vkpt::pathtracer::BuildSceneDeltaUpdate(qtScene, nextScene);
        const auto deltaEnd = std::chrono::steady_clock::now();
        const bool hasSceneDeltaWork =
            sceneDelta.has_value() &&
            (!sceneDelta->materials.empty() ||
             !sceneDelta->lights.empty() ||
             sceneDelta->environment_color_changed);
        qtScene = std::move(nextScene);
        if (qtAudioSystem) {
          qtAudioSystem->load_scene_audio(qtSceneDocument, RuntimeSceneDisplayName(config.scene_path.value));
        }
        qtRebuildRtInstanceIndexCache();
        syncQtCameraPoseFromScene();
        syncQtFpsAnglesFromPose();
        qtCameraFocusDistance = std::max(
            0.25f,
            qtScene.camera_focus_distance > 0.0f
                ? qtScene.camera_focus_distance
                : PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
        qtCameraFocusPoint =
            PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        if (!qtUseBg) {
          (void)qtPublishRenderSnapshot(true, true, true, true);
        }
        qtSetViewportPickablesFromSnapshot(qtCurrentUiSnapshot());
        qtViewportPickablesRefreshPending = false;
        qtLastViewportPickablesRefresh = std::chrono::steady_clock::now();
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        const auto pickablesEnd = std::chrono::steady_clock::now();
        bool rendererUpdatedIncrementally = false;
        bool accumulationReset = false;
        const auto rendererStart = std::chrono::steady_clock::now();
        if (hasSceneDeltaWork) {
          if (qtUseBg && qtRenderCoordinator) {
            qtPublishRenderSnapshotForBackground(false, false, false, true);
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
            qtPublishRenderSnapshotForBackground(true, true, true, true);
            accumulationReset = true;
          } else if (!qtUseBg && qtTracer) {
            const bool configured = qtTracerReady || qtTracer->configure(qtSettings);
            qtTracerReady = configured &&
                            qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation();
            qtSampleIndex = 0u;
            accumulationReset = qtTracerReady;
          }
        }
        const auto rendererEnd = std::chrono::steady_clock::now();
        qtDockPanelsDirty = true;
        qtSyncPhysicsFromSceneDocument();
        if (!qtSkipScriptReloadForSceneReload) {
          qtReloadScriptsFromDocument("scene edited");
        }
        qtPreviewStatus = qtTracerReady
            ? (rendererUpdatedIncrementally && hasSceneDeltaWork
                   ? "scene delta update"
                   : "scene edited")
            : "scene edit renderer reload failed";
        if (reason == std::string_view{"scene loaded"} ||
                   reason == std::string_view{"asset browser scene load"} ||
                   reason == std::string_view{"asset browser model spawn"} ||
                   reason == std::string_view{"scene graph delete"} ||
                   reason == std::string_view{"scene graph reparent"} ||
                   reason == std::string_view{"scene graph visibility"}) {
          ui_runtime_state.status_message = std::string(reason);
        } else {
          ui_runtime_state.status_message = "gizmo " + std::string(reason);
        }
        if (accumulationReset) {
          qtPublishedSample.store(0u, std::memory_order_relaxed);
          qtPublishedRays.store(0u, std::memory_order_relaxed);
        }
        const auto reloadEnd = std::chrono::steady_clock::now();
        auto ms = [](auto begin, auto end) {
          return std::to_string(
              std::chrono::duration<double, std::milli>(end - begin).count());
        };
        logger.log(vkpt::log::Severity::Debug,
                   "app",
                   "Qt scene edit reload timings",
                   {{"reason", std::string(reason)},
                    {"entities", std::to_string(qtSceneDocument.entities.size())},
                    {"rt_instances", std::to_string(qtScene.instances.size())},
                    {"rt_lights", std::to_string(qtScene.lights.size())},
                    {"delta", sceneDelta.has_value() ? "true" : "false"},
                    {"delta_lights", sceneDelta ? std::to_string(sceneDelta->lights.size()) : "0"},
                    {"full_reload", rendererUpdatedIncrementally ? "false" : "true"},
                    {"conversion_ms", ms(reloadStart, conversionEnd)},
                    {"delta_ms", ms(conversionEnd, deltaEnd)},
                    {"pickables_ms", ms(deltaEnd, pickablesEnd)},
                    {"renderer_ms", ms(rendererStart, rendererEnd)},
                    {"total_ms", ms(reloadStart, reloadEnd)}});
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      auto qtToggleSceneGraphVisibility =
          [&](vkpt::core::StableId entityId, vkpt::core::FrameIndex frameIndex) {
        auto entityIt = std::find_if(
            qtSceneDocument.entities.begin(),
            qtSceneDocument.entities.end(),
            [entityId](const vkpt::scene::SceneEntityDefinition& entity) {
              return entity.id == entityId;
            });
        if (entityIt == qtSceneDocument.entities.end()) {
          ui_runtime_state.status_message =
              "scene graph visibility target missing #" + std::to_string(entityId);
          logger.log(vkpt::log::Severity::Warning,
                     "app",
                     "Qt scene graph visibility target missing",
                     {{"entity_id", std::to_string(entityId)}});
          return false;
        }

        std::unordered_map<vkpt::core::StableId, vkpt::scene::SceneEntityDefinition*>
            entitiesById;
        entitiesById.reserve(qtSceneDocument.entities.size());
        for (auto& entity : qtSceneDocument.entities) {
          entitiesById.emplace(entity.id, &entity);
        }
        auto entityEffectiveVisible =
            [&](const vkpt::scene::SceneEntityDefinition& entity) {
          const auto* current = &entity;
          std::unordered_set<vkpt::core::StableId> visited;
          visited.reserve(8u);
          for (std::size_t depth = 0u;
               current != nullptr && depth <= qtSceneDocument.entities.size() &&
               visited.insert(current->id).second;
               ++depth) {
            if (!current->visible) {
              return false;
            }
            if (current->hierarchy.parent == 0u) {
              break;
            }
            const auto parentIt = entitiesById.find(current->hierarchy.parent);
            current = parentIt == entitiesById.end() ? nullptr : parentIt->second;
          }
          return true;
        };

        const bool currentlyVisible = entityEffectiveVisible(*entityIt);
        if (currentlyVisible) {
          entityIt->visible = false;
        } else {
          vkpt::scene::SceneEntityDefinition* current = &*entityIt;
          std::unordered_set<vkpt::core::StableId> visited;
          visited.reserve(8u);
          for (std::size_t depth = 0u;
               current != nullptr && depth <= qtSceneDocument.entities.size() &&
               visited.insert(current->id).second;
               ++depth) {
            current->visible = true;
            if (current->hierarchy.parent == 0u) {
              break;
            }
            const auto parentIt = entitiesById.find(current->hierarchy.parent);
            current = parentIt == entitiesById.end() ? nullptr : parentIt->second;
          }
        }
        const bool visible = !currentlyVisible;
        const std::string displayName = QtEntityDisplayName(*entityIt);
        const std::string state = visible ? "shown" : "hidden";
        const bool reloaded = qtReloadEditedScene("scene graph visibility");
        ui_runtime_state.last_scene_tree_operation =
            state + " " + std::to_string(entityId);
        ui_runtime_state.status_message =
            "scene graph " + state + " " + displayName;
        PushUiEvent(ui_event_log,
                    "scene_tree_visibility",
                    "scene_tree",
                    "entity." + std::to_string(entityId),
                    frameIndex,
                    {},
                    std::to_string(entityId),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        logger.log(vkpt::log::Severity::Info,
                   "app",
                   "Qt scene graph visibility toggled",
                   {{"entity_id", std::to_string(entityId)},
                    {"visible", visible ? "true" : "false"},
                    {"reload", reloaded ? "true" : "false"}});
        return reloaded;
      };
      auto qtLogInstanceTransformUpdate =
          [&](vkpt::log::Severity severity,
              std::string_view message,
              std::size_t updateCount,
              const vkpt::pathtracer::InstanceTransformUpdateOptions& options,
              vkpt::pathtracer::InstanceTransformUpdateStatus status,
              std::uint32_t matchedOrAppliedCount = 0u,
              const char* detail = nullptr) {
        if (!logger.enabled(severity)) {
          return;
        }
        logger.log(severity,
                   "app",
                   message,
                   {{"updates", std::to_string(updateCount)},
                    {"matched_or_applied", std::to_string(matchedOrAppliedCount)},
                    {"status", vkpt::pathtracer::ToString(status)},
                    {"reason", vkpt::pathtracer::ToString(options.reason)},
                    {"policy", vkpt::pathtracer::ToString(options.fallback_policy)},
                    {"source", options.source_system ? options.source_system : ""},
                    {"detail", detail ? detail : ""}},
                   options.source_frame);
      };
      qtPublishInstanceTransformUpdates =
          [&](const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates,
              vkpt::pathtracer::InstanceTransformUpdateOptions options) {
        if (updates.empty() || !qtTracerReady) {
          return false;
        }

        bool posted = false;
        if (qtUseBg && qtRenderCoordinator) {
          const bool mirrored = vkpt::pathtracer::ApplyInstanceTransformUpdates(
              qtScene,
              updates,
              vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly);
          if (mirrored) {
            qtPublishRenderSnapshotForBackground(false, true, false, false);
            posted = true;
          }
          qtLogInstanceTransformUpdate(
              mirrored ? vkpt::log::Severity::Debug : vkpt::log::Severity::Warning,
              mirrored ? "published Qt instance transform snapshot"
                       : "Qt instance transform update failed app metadata mirror",
              updates.size(),
              options,
              mirrored ? vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported
                       : vkpt::pathtracer::InstanceTransformUpdateStatus::Failed);
        } else if (!qtUseBg && qtTracerReady) {
          const auto plan = qtTracer->plan_instance_transform_update(updates, options);
          if (!vkpt::pathtracer::TransformUpdateStatusAllowedByPolicy(
                  plan.status,
                  options.fallback_policy) ||
              !plan.can_apply_without_full_fallback()) {
            posted = false;
            qtLogInstanceTransformUpdate(
                vkpt::log::Severity::Warning,
                "Qt instance transform update rejected before direct apply",
                updates.size(),
                options,
                plan.status,
                plan.matched_count,
                plan.message);
          } else {
            const auto result = qtTracer->apply_instance_transform_update(updates, options);
            posted = result.applied();
            qtLogInstanceTransformUpdate(
                posted ? vkpt::log::Severity::Debug : vkpt::log::Severity::Warning,
                posted ? "applied Qt instance transform update directly"
                       : "Qt direct instance transform update failed",
                updates.size(),
                options,
                result.status,
                result.applied_count,
                result.message);
          }
          if (posted &&
              vkpt::pathtracer::ApplyInstanceTransformUpdates(
                  qtScene,
                  updates,
                  vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly)) {
            qtTracerReady = qtTracer->reset_accumulation();
            posted = qtTracerReady;
            if (posted) {
              (void)qtPublishRenderSnapshot(false, true, false, false);
            }
            qtSampleIndex = 0u;
          } else {
            posted = false;
          }
        }

        if (!posted) {
          qtLogInstanceTransformUpdate(
              vkpt::log::Severity::Warning,
              "Qt instance transform update was not posted",
              updates.size(),
              options,
              vkpt::pathtracer::InstanceTransformUpdateStatus::Failed);
          return false;
        }
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtScheduleViewportPickablesRefresh(false);
        qtMarkTransformDockPanelsDirty(false);
        return true;
      };
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

        std::vector<vkpt::core::StableId> removedEntityIds(removeEntities.begin(), removeEntities.end());
        removedEntityIds = SortedUniqueEntityIds(std::move(removedEntityIds));
        const bool loaded = qtReloadEditedScene("scene graph delete");

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
        const bool loaded = qtReloadEditedScene("scene graph reparent");
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
