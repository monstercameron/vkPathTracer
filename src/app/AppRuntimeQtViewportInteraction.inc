      auto qtSceneScale = [&]() {
        if (qtPickables.empty()) {
          return 2.5f;
        }
        vkpt::editor::Bounds bounds{};
        for (const auto& pickable : qtPickables) {
          if (!pickable.bounds.valid) {
            continue;
          }
          ExpandBounds(bounds, ToPtVec3(pickable.bounds.min));
          ExpandBounds(bounds, ToPtVec3(pickable.bounds.max));
        }
        if (!bounds.valid) {
          return 2.5f;
        }
        const auto extent = PtSub(ToPtVec3(bounds.max), ToPtVec3(bounds.min));
        return std::max(1.0f, PtLength(extent) * 0.25f);
      };
      const float qtCameraMoveUnitsPerSecond = qtSceneScale();
      const float qtFpsUnitScale = ClampFloat(qtCameraMoveUnitsPerSecond * 0.10f, 0.65f, 2.25f);
      const float qtFpsStandEyeHeight = 1.62f * qtFpsUnitScale;
      const float qtFpsCrouchEyeHeight = 1.05f * qtFpsUnitScale;
      const float qtFpsRadius = 0.32f * qtFpsUnitScale;
      const float qtFpsStepHeight = 0.38f * qtFpsUnitScale;
      const float qtFpsSkin = 0.03f * qtFpsUnitScale;
      const float qtFpsWalkSpeed = ClampFloat(1.65f * qtFpsUnitScale,
                                              1.1f,
                                              4.0f);
      const float qtFpsRunSpeed = ClampFloat(qtFpsWalkSpeed * 2.35f,
                                             qtFpsWalkSpeed,
                                             8.0f);
      const float qtFpsCrouchSpeed = qtFpsWalkSpeed * 0.45f;
      const float qtFpsAirControlScale = 0.45f;
      const float qtFpsGravity = 18.0f * qtFpsUnitScale;
      const float qtFpsJumpSpeed = 5.2f * std::sqrt(qtFpsUnitScale);
      auto qtFpsEyePosition = [&]() {
        return PtAdd(qtFpsPlayer.feet_position, {0.0f, qtFpsPlayer.eye_height, 0.0f});
      };
      auto qtFpsFlatForward = [&]() {
        return PtNormalize(vkpt::pathtracer::Vec3{
            std::sin(qtFpsYaw),
            0.0f,
            std::cos(qtFpsYaw)}, {0.0f, 0.0f, -1.0f});
      };
      auto qtFpsFlatRight = [&]() {
        return PtNormalize(PtCross(qtFpsFlatForward(), {0.0f, 1.0f, 0.0f}), {1.0f, 0.0f, 0.0f});
      };
      auto qtRefreshCameraFocusFromPose = [&]() {
        const auto forward = qtCameraForward();
        const float targetDistance = PtLength(PtSub(qtCameraPose.target, qtCameraPose.position));
        const float sceneScale = qtSceneScale();
        const float minimumSceneFocusDistance = std::max(1.0f, sceneScale * 0.25f);
        const float projectedSceneDistance = PtDot(PtSub(qtOrbitCenter, qtCameraPose.position), forward);

        float focusDistance = std::max(0.25f, targetDistance);
        if (targetDistance < minimumSceneFocusDistance &&
            projectedSceneDistance > minimumSceneFocusDistance) {
          focusDistance = projectedSceneDistance;
        }

        qtCameraFocusDistance = std::max(0.25f, focusDistance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(forward, qtCameraFocusDistance));
      };
      auto qtMoveCameraFocusBy = [&](const vkpt::pathtracer::Vec3& delta) {
        qtCameraFocusPoint = PtAdd(qtCameraFocusPoint, delta);
      };
      qtRefreshCameraFocusFromPose();
      auto qtSyncCameraFromFpsPlayer = [&](std::string_view reason) {
        const auto eye = qtFpsEyePosition();
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.position = eye;
        qtCameraPose.target = PtAdd(eye, PtMul(forward, std::max(1.0f, qtCameraFocusDistance)));
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        qtCameraFocusPoint = qtCameraPose.target;
        qtCameraFocusDistance = PtLength(PtSub(qtCameraFocusPoint, qtCameraPose.position));
        qtMaybeSyncFpsCameraObject(false);
        applyQtCameraPose(reason, false);
      };
      auto qtInitializeFpsPlayerFromPose = [&]() {
        qtFpsPlayer = {};
        qtFpsPlayer.initialized = true;
        qtFpsPlayer.eye_height = qtFpsStandEyeHeight;
        qtFpsPlayer.feet_position = {
            qtCameraPose.position.x,
            qtCameraPose.position.y - qtFpsStandEyeHeight,
            qtCameraPose.position.z};
        const auto groundProbeOrigin = PtAdd(qtCameraPose.position, {0.0f, qtFpsStepHeight, 0.0f});
        const auto ground = TraceFpsGround(qtPickables,
                                           groundProbeOrigin,
                                           qtFpsStandEyeHeight + qtFpsStepHeight + qtFpsSkin,
                                           0.62f);
        if (ground.hit) {
          qtFpsPlayer.feet_position.y = ground.position.y;
          qtFpsPlayer.grounded = true;
        }
        qtFpsPlayer.current_speed = 0.0f;
      };
      auto qtResolveFpsHorizontalDelta = [&](const vkpt::pathtracer::Vec3& feetPosition,
                                             const vkpt::pathtracer::Vec3& desiredDelta) {
        return ResolveFpsHorizontalDeltaForPlayer(qtPickables,
                                                  feetPosition,
                                                  desiredDelta,
                                                  qtFpsRadius,
                                                  qtFpsSkin,
                                                  qtFpsPlayer.eye_height);
      };
      auto qtApplyFpsLookDelta = [&](float dx, float dy) {
        constexpr float kLookSensitivity = 0.0045f;
        qtFpsYaw -= dx * kLookSensitivity;
        qtFpsPitch = ClampFloat(qtFpsPitch - dy * kLookSensitivity, -1.45f, 1.45f);
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.target = PtAdd(qtCameraPose.position,
                                    PtMul(forward, std::max(1.0f, qtCameraFocusDistance)));
        qtCameraFocusPoint = qtCameraPose.target;
        qtCameraFocusDistance = PtLength(PtSub(qtCameraFocusPoint, qtCameraPose.position));
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        qtMaybeSyncFpsCameraObject(false);
        applyQtCameraPose("fps look", false);
      };
      auto qtApplyOrbitDrag = [&](float dx, float dy) {
        constexpr float kOrbitSensitivity = 0.006f;
        const auto offset = PtSub(qtCameraPose.position, qtCameraFocusPoint);
        const float radius = std::max(0.05f, PtLength(offset));
        float yaw = std::atan2(offset.x, offset.z) - dx * kOrbitSensitivity;
        float pitch = std::asin(ClampFloat(offset.y / radius, -0.98f, 0.98f)) + dy * kOrbitSensitivity;
        pitch = ClampFloat(pitch, -1.45f, 1.45f);
        const float cosPitch = std::cos(pitch);
        qtCameraPose.position = PtAdd(qtCameraFocusPoint,
                                      PtMul(vkpt::pathtracer::Vec3{
                                          std::sin(yaw) * cosPitch,
                                          std::sin(pitch),
                                          std::cos(yaw) * cosPitch}, radius));
        qtCameraPose.target = qtCameraFocusPoint;
        qtCameraFocusDistance = radius;
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("orbit drag");
      };
      auto qtApplyPanDrag = [&](float dx, float dy) {
        const auto frameRect = qtViewportImageRect();
        const float viewportHeight = std::max(1.0f, frameRect.height);
        const auto forward = qtCameraForward();
        const auto right = PtNormalize(PtCross(forward, qtCameraPose.up), {1.0f, 0.0f, 0.0f});
        const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
        const float focusDistance = std::max(0.05f, qtCameraFocusDistance);
        const float worldPerPixel =
            (2.0f * focusDistance * std::tan(0.5f * DegToRad(std::max(1.0f, qtCameraPose.fov_deg)))) /
            viewportHeight;
        const auto delta = PtAdd(PtMul(right, -dx * worldPerPixel),
                                 PtMul(up, dy * worldPerPixel));
        qtCameraPose.position = PtAdd(qtCameraPose.position, delta);
        qtCameraPose.target = PtAdd(qtCameraPose.target, delta);
        qtMoveCameraFocusBy(delta);
        applyQtCameraPose("pan drag");
      };
      auto qtApplyDolly = [&](float wheelDelta) {
        if (std::fabs(wheelDelta) <= 1.0e-4f) {
          return;
        }
        if (qtFpsMode) {
          if (!qtFpsPlayer.initialized) {
            qtInitializeFpsPlayerFromPose();
          }
          const auto delta = qtResolveFpsHorizontalDelta(
              qtFpsPlayer.feet_position,
              PtMul(qtFpsFlatForward(), wheelDelta * qtFpsWalkSpeed * 0.35f));
          qtFpsPlayer.feet_position = PtAdd(qtFpsPlayer.feet_position, delta);
          qtFpsPlayer.current_speed = 0.0f;
          qtDiscardFpsMovementResults();
          qtSyncCameraFromFpsPlayer("fps dolly");
          qtMarkFpsDockPanelsDirty(true);
          return;
        } else {
          const auto offset = PtSub(qtCameraPose.position, qtCameraFocusPoint);
          const float scale = std::pow(0.88f, wheelDelta);
          const float currentDistance = std::max(0.05f, PtLength(offset));
          const float newDistance = ClampFloat(currentDistance * ClampFloat(scale, 0.25f, 4.0f),
                                               0.05f,
                                               std::max(currentDistance * 32.0f, qtSceneScale() * 64.0f));
          qtCameraPose.position = PtAdd(qtCameraFocusPoint,
                                        PtMul(PtNormalize(offset), newDistance));
          qtCameraPose.target = qtCameraFocusPoint;
          qtCameraFocusDistance = newDistance;
          syncQtFpsAnglesFromPose();
        }
        applyQtCameraPose(qtFpsMode ? "fps dolly" : "orbit dolly");
      };
      auto qtCameraFlatForward = [&]() {
        const auto forward = qtCameraForward();
        const auto flat = vkpt::pathtracer::Vec3{forward.x, 0.0f, forward.z};
        return PtNormalize(flat, forward);
      };
      auto qtCameraFlatRight = [&]() {
        return PtNormalize(PtCross(qtCameraFlatForward(), {0.0f, 1.0f, 0.0f}),
                           {1.0f, 0.0f, 0.0f});
      };
      auto qtApplyContinuousOrbitCameraMovement = [&](float dtSeconds) {
        if (qtFpsMode || qtIsPlayMode() || qtLiveEditViewportInputArmed() ||
            dtSeconds <= 0.0f) {
          return;
        }
        if (qtKeyActive(kQtKeyControl) || qtKeyActive(17) ||
            qtKeyActive(kQtKeyAlt) || IsCtrlDown() || IsAltDown()) {
          return;
        }

        vkpt::pathtracer::Vec3 wishMove{};
        if (qtKeyActive('W') || qtKeyActive(kQtKeyUp)) {
          wishMove = PtAdd(wishMove, qtCameraFlatForward());
        }
        if (qtKeyActive('S') || qtKeyActive(kQtKeyDown)) {
          wishMove = PtSub(wishMove, qtCameraFlatForward());
        }
        if (qtKeyActive('D') || qtKeyActive(kQtKeyRight)) {
          wishMove = PtAdd(wishMove, qtCameraFlatRight());
        }
        if (qtKeyActive('A') || qtKeyActive(kQtKeyLeft)) {
          wishMove = PtSub(wishMove, qtCameraFlatRight());
        }
        if (PtLength(wishMove) <= 1.0e-5f) {
          return;
        }

        float speed = qtCameraMoveUnitsPerSecond;
        if (qtKeyActive(kQtKeyShift) || qtKeyActive(16)) {
          speed *= 2.5f;
        }

        const auto delta = PtMul(PtNormalize(wishMove), speed * dtSeconds);
        qtCameraPose.position = PtAdd(qtCameraPose.position, delta);
        qtCameraPose.target = PtAdd(qtCameraPose.target, delta);
        qtMoveCameraFocusBy(delta);
        syncQtFpsAnglesFromPose();
        qtUserCameraActive = true;
        applyQtCameraPose("wasd move");
      };
      auto qtApplyContinuousFpsMovement = [&](float dtSeconds) {
        if (!qtFpsMode || qtIsPlayMode() || dtSeconds <= 0.0f) {
          return;
        }
        if (!qtFpsPlayer.initialized) {
          qtInitializeFpsPlayerFromPose();
        }

        vkpt::pathtracer::Vec3 wishMove{};
        if (qtKeyActive('W') || qtKeyActive(kQtKeyUp)) {
          wishMove = PtAdd(wishMove, qtFpsFlatForward());
        }
        if (qtKeyActive('S') || qtKeyActive(kQtKeyDown)) {
          wishMove = PtSub(wishMove, qtFpsFlatForward());
        }
        if (qtKeyActive('D') || qtKeyActive(kQtKeyRight)) {
          wishMove = PtAdd(wishMove, qtFpsFlatRight());
        }
        if (qtKeyActive('A') || qtKeyActive(kQtKeyLeft)) {
          wishMove = PtSub(wishMove, qtFpsFlatRight());
        }

        if (auto result = qtFpsCollisionWorker.take_latest_result()) {
          const auto now = std::chrono::steady_clock::now();
          if (result->solve_ms > 4.0 &&
              (qtLastFpsSlowLog == std::chrono::steady_clock::time_point{} ||
               now - qtLastFpsSlowLog > std::chrono::seconds(1))) {
            logger.log(vkpt::log::Severity::Warning,
                       "viewport",
                       "slow fps movement solve",
                       {
                           {"stack", "qtApplyContinuousFpsMovement -> FpsCollisionWorker::run -> SolveFpsMovement -> TraceFpsBodyWallWithProbeHeights/TraceFpsGround"},
                           {"solve_ms", std::to_string(result->solve_ms)},
                           {"pickables", std::to_string(qtPickables.size())},
                           {"sequence", std::to_string(result->sequence)}
                       });
            qtLastFpsSlowLog = now;
          }
          const bool freshCollision = result->collision_revision ==
              qtFpsCollisionWorker.collision_revision();
          const bool freshSequence = result->sequence > qtFpsAppliedMoveSequence;
          if (freshCollision && freshSequence) {
            const bool pendingJump = qtFpsPlayer.jump_queued;
            qtFpsAppliedMoveSequence = result->sequence;
            qtFpsPlayer = result->player;
            qtFpsPlayer.jump_queued = qtFpsPlayer.jump_queued || pendingJump;
            if (result->pose_changed || result->state_changed) {
              qtUserCameraActive = true;
              qtSyncCameraFromFpsPlayer("fps physics");
              qtMarkFpsDockPanelsDirty(result->state_changed);
            }
          }
        }

        const bool wantsCrouch =
            qtKeyActive(kQtKeyControl) || qtKeyActive(17) || qtKeyActive('C');
        const bool wantsRun =
            !wantsCrouch && (qtKeyActive(kQtKeyShift) || qtKeyActive(16));
        const float targetEyeHeight = wantsCrouch ? qtFpsCrouchEyeHeight : qtFpsStandEyeHeight;
        const bool hasWishMove = PtLength(wishMove) > 1.0e-5f;
        const bool needsMovementStep =
            hasWishMove ||
            qtFpsPlayer.jump_queued ||
            !qtFpsPlayer.grounded ||
            qtFpsPlayer.crouching != wantsCrouch ||
            qtFpsPlayer.running != wantsRun ||
            std::fabs(qtFpsPlayer.eye_height - targetEyeHeight) > 1.0e-4f;
        if (!needsMovementStep) {
          qtPendingFpsMoveDt = 0.0f;
          if (qtFpsPlayer.current_speed != 0.0f) {
            qtFpsPlayer.current_speed = 0.0f;
            qtMarkFpsDockPanelsDirty(false);
          }
          return;
        }

        qtPendingFpsMoveDt = ClampFloat(qtPendingFpsMoveDt + dtSeconds, 0.0f, 0.12f);
        if (qtFpsCollisionWorker.has_work() || qtPendingFpsMoveDt <= 1.0e-5f) {
          return;
        }

        FpsMovementRequest request{};
        request.sequence = ++qtFpsMoveSequence;
        request.player = qtFpsPlayer;
        request.wish_move = wishMove;
        request.tuning.stand_eye_height = qtFpsStandEyeHeight;
        request.tuning.crouch_eye_height = qtFpsCrouchEyeHeight;
        request.tuning.radius = qtFpsRadius;
        request.tuning.step_height = qtFpsStepHeight;
        request.tuning.skin = qtFpsSkin;
        request.tuning.walk_speed = qtFpsWalkSpeed;
        request.tuning.run_speed = qtFpsRunSpeed;
        request.tuning.crouch_speed = qtFpsCrouchSpeed;
        request.tuning.air_control_scale = qtFpsAirControlScale;
        request.tuning.gravity = qtFpsGravity;
        request.tuning.jump_speed = qtFpsJumpSpeed;
        request.dt_seconds = ClampFloat(qtPendingFpsMoveDt, 0.0f, 0.08f);
        request.crouching = wantsCrouch;
        request.running = wantsRun;
        qtPendingFpsMoveDt = 0.0f;
        qtFpsCollisionWorker.submit(request);
        qtFpsPlayer.jump_queued = false;
      };
      (void)qtApplyContinuousOrbitCameraMovement;
      (void)qtApplyContinuousFpsMovement;
      qtSetFpsMode = [&](bool enabled,
                         vkpt::core::FrameIndex frameIndex,
                         std::string_view source) {
        if (enabled && qtScriptState.playing) {
          qtSetGameMode(false, frameIndex, source);
        }
        if (qtFpsMode == enabled && (!enabled || qtFpsPlayer.initialized)) {
          return;
        }
        qtFpsMode = enabled;
        qtSyncViewportAssetDropMode();
        qtDiscardFpsMovementResults();
        qtUserCameraActive = true;
        ui_runtime_state.active_viewport_tool = qtFpsMode
            ? vkpt::editor::ViewportTool::Fps
            : vkpt::editor::ViewportTool::Select;
        if (qtFpsMode) {
          syncQtFpsAnglesFromPose();
          qtInitializeFpsPlayerFromPose();
          qtLeftMouseDown = false;
          qtRightMouseDown = false;
          qtMiddleMouseDown = false;
          qtPotentialClick = false;
          ResetViewportMouseClick(qtMouseClickState);
          qtClickDragPixels = 0.0f;
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
          qtGizmoDrag = {};
          qtHoveredGizmoHit = std::nullopt;
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
          qtSetViewportMouseLocked(true);
          qtSyncCameraFromFpsPlayer("fps enter");
        } else {
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
          qtMaybeSyncFpsCameraObject(true);
          qtFpsPlayer.jump_queued = false;
          syncQtFpsAnglesFromPose();
          qtSetViewportMouseLocked(false);
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        }
        ui_runtime_state.status_message = qtFpsMode ? "fps camera mode" : "select camera mode";
        PushUiEvent(ui_event_log,
                    "viewport_camera_mode",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    qtFpsMode ? "fps" : "select",
                    ui_runtime_state.status_message);
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();
      };
      auto qtPublishGizmoTransformUpdates =
          [&](const std::unordered_set<vkpt::core::StableId>& touchedIds,
              vkpt::core::FrameIndex sourceFrame) {
        if (touchedIds.empty()) {
          return true;
        }

        auto transformBuild = qtBuildInstanceTransformUpdatesForTouchedEntities(
            touchedIds,
            0u,
            qtScriptTransformRevision);
        if (transformBuild.unsupported_instances || transformBuild.updates.empty()) {
          if (qtGizmoDrag.active) {
            qtGizmoDrag.scene_reload_pending = true;
          }
          return false;
        }
        if (!(qtPublishInstanceTransformUpdates &&
              qtPublishInstanceTransformUpdates(
                  transformBuild.updates,
                  vkpt::pathtracer::MakeStandardTransformUpdateOptions(
                      vkpt::pathtracer::RenderUpdateReason::EditorGizmoMotion,
                      sourceFrame,
                      "qt-viewport")))) {
          if (qtGizmoDrag.active) {
            qtGizmoDrag.scene_reload_pending = true;
          }
          return false;
        }
        return true;
      };
      auto qtCaptureGizmoDrag = [&](const ViewportGizmoHit& hit, float x, float y) {
        qtGizmoDrag = {};
        qtGizmoDrag.active = true;
        qtGizmoDrag.hit = hit;
        qtGizmoDrag.start_x = x;
        qtGizmoDrag.start_y = y;
        qtSetViewportCursor(CursorForGizmoHit(hit));
        for (const auto selectedId : ui_selection_state.selected_entity_ids) {
          if (auto* entity = qtFindEntity(selectedId)) {
            if (!entity->has_transform) {
              entity->has_transform = true;
              entity->transform = {};
            }
            qtGizmoDrag.entities.push_back({
                selectedId,
                false,
                entity->transform,
                InverseSceneTransformPoint(hit.pivot, entity->transform)});
            if (entity->has_light || entity->has_camera || entity->has_sdf_primitive) {
              qtGizmoDrag.scene_reload_pending = true;
            }
            qtGizmoDrag.publishable_entity_ids.insert(selectedId);
            continue;
          }
          if (auto* primitive = qtFindSdfPrimitive(selectedId)) {
            qtGizmoDrag.entities.push_back({
                selectedId,
                true,
                primitive->transform,
                InverseSceneTransformPoint(hit.pivot, primitive->transform)});
            qtGizmoDrag.scene_reload_pending = true;
          }
        }
        if (qtGizmoDrag.entities.empty()) {
          qtGizmoDrag = {};
        }
      };
      auto qtWriteGizmoTransform = [&](const QtGizmoDragEntityStart& start,
                                       const vkpt::scene::TransformComponent& transform) {
        if (start.sdf_primitive) {
          if (auto* primitive = qtFindSdfPrimitive(start.entity_id)) {
            primitive->transform = transform;
            primitive->transform.dirty = true;
          }
          return;
        }
        if (auto* entity = qtFindEntity(start.entity_id)) {
          entity->has_transform = true;
          entity->transform = transform;
          entity->transform.dirty = true;
        }
      };
      auto qtApplyGizmoDrag = [&](float x, float y, vkpt::core::FrameIndex frameIndex) {
        if (!qtGizmoDrag.active) {
          return;
        }
        ++qtGizmoDragInputSamples;
        const float dx = x - qtGizmoDrag.start_x;
        const float dy = y - qtGizmoDrag.start_y;
        const float dragAlongAxis = dx * qtGizmoDrag.hit.screen_axis_x +
                                    dy * qtGizmoDrag.hit.screen_axis_y;
        for (const auto& start : qtGizmoDrag.entities) {
          auto transform = start.transform;
          if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::Translate) {
            const float units = dragAlongAxis / std::max(1.0f, qtGizmoDrag.hit.pixels_per_unit);
            const auto delta = PtMul(qtGizmoDrag.hit.axis, units);
            transform.translation.x = start.transform.translation.x + delta.x;
            transform.translation.y = start.transform.translation.y + delta.y;
            transform.translation.z = start.transform.translation.z + delta.z;
          } else if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::ScaleAxis) {
            const float units = dragAlongAxis / std::max(1.0f, qtGizmoDrag.hit.pixels_per_unit);
            const float factor = ClampFloat(
                (std::max(1.0e-4f, qtGizmoDrag.hit.axis_world_length) + units) /
                    std::max(1.0e-4f, qtGizmoDrag.hit.axis_world_length),
                0.05f,
                20.0f);
            switch (qtGizmoDrag.hit.axis_index) {
              case 0:
                transform.scale.x = start.transform.scale.x * factor;
                break;
              case 1:
                transform.scale.y = start.transform.scale.y * factor;
                break;
              case 2:
                transform.scale.z = start.transform.scale.z * factor;
                break;
              default:
                transform.scale.x = start.transform.scale.x * factor;
                transform.scale.y = start.transform.scale.y * factor;
                transform.scale.z = start.transform.scale.z * factor;
                break;
            }
            const auto scaledPivot = vkpt::pathtracer::Vec3{
                start.local_pivot.x * transform.scale.x,
                start.local_pivot.y * transform.scale.y,
                start.local_pivot.z * transform.scale.z};
            const auto rotatedPivot = RotatePointByQuat(scaledPivot, transform.rotation);
            const auto newTranslation = PtSub(qtGizmoDrag.hit.pivot, rotatedPivot);
            transform.translation = {newTranslation.x, newTranslation.y, newTranslation.z};
          } else if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::Rotate) {
            const float angle = dragAlongAxis * 0.012f;
            transform.rotation = QuatMultiply(QuatFromAxisAngle(qtGizmoDrag.hit.axis, angle),
                                              start.transform.rotation);
            const auto scaledPivot = vkpt::pathtracer::Vec3{
                start.local_pivot.x * transform.scale.x,
                start.local_pivot.y * transform.scale.y,
                start.local_pivot.z * transform.scale.z};
            const auto rotatedPivot = RotatePointByQuat(scaledPivot, transform.rotation);
            const auto newTranslation = PtSub(qtGizmoDrag.hit.pivot, rotatedPivot);
            transform.translation = {newTranslation.x, newTranslation.y, newTranslation.z};
          }
          qtWriteGizmoTransform(start, transform);
        }
        // Local entity transforms are now up to date. The render-thread publish
        // is coalesced to a single end-of-frame flush in the render loop so we
        // do at most one BuildSceneWorldSnapshot + transform-update post per
        // frame regardless of how many mouse events fire.
        for (const auto& start : qtGizmoDrag.entities) {
          if (start.sdf_primitive) {
            qtGizmoDrag.scene_reload_pending = true;
          }
        }
        if (!qtGizmoDrag.publishable_entity_ids.empty()) {
          qtGizmoDrag.pending_publish = true;
        }
        (void)frameIndex;
      };
      auto qtCommitGizmoDrag = [&](vkpt::core::FrameIndex frameIndex) {
        if (!qtGizmoDrag.active) {
          return;
        }
        if (qtGizmoDrag.scene_reload_pending) {
          qtReloadEditedScene("gizmo commit");
          return;
        }
        if (qtGizmoDrag.pending_publish &&
            !qtGizmoDrag.publishable_entity_ids.empty()) {
          qtGizmoDrag.pending_publish = false;
          ++qtGizmoTransformImmediatePublishes;
          qtGizmoTransformPublishedEntityCount +=
              qtGizmoDrag.publishable_entity_ids.size();
          if (!qtPublishGizmoTransformUpdates(qtGizmoDrag.publishable_entity_ids,
                                              frameIndex)) {
            ++qtGizmoTransformPublishFailures;
          }
        }
        qtRefreshViewportPickablesNow();
        qtMarkTransformDockPanelsDirty(true);
      };
      auto qtSetGizmoMode = [&](vkpt::editor::GizmoMode mode,
                                std::string_view source,
                                vkpt::core::FrameIndex frameIndex) {
        if (ui_runtime_state.active_gizmo_mode == mode) {
          updateQtSelectionOverlay();
          return;
        }
        ui_runtime_state.active_gizmo_mode = mode;
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_runtime_state.status_message =
            std::string("gizmo ") + vkpt::editor::ToString(mode);
        PushUiEvent(ui_event_log,
                    "viewport_gizmo_mode",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    vkpt::editor::ToString(mode),
                    ui_runtime_state.status_message);
        updateQtSelectionOverlay();
        if (mode == vkpt::editor::GizmoMode::None) {
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        }
      };
      auto qtApplyViewportPick = [&](float x, float y, vkpt::core::FrameIndex frameIndex) {
        if (qtFpsMode || qtIsPlayMode() || qtLiveEditViewportInputArmed()) {
          qtPotentialClick = false;
          ResetViewportMouseClick(qtMouseClickState);
          qtClickDragPixels = 0.0f;
          qtGizmoDrag = {};
          qtHoveredGizmoHit = std::nullopt;
          updateQtSelectionOverlay();
          return;
        }
        qtRefreshViewportPickablesIfSnapshotChanged();
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        const auto pickSnapshot = qtCurrentUiSnapshot();
        const auto picked = localPoint
            ? (pickSnapshot
                   ? PickViewportObject(qtPickables,
                                        *pickSnapshot,
                                        qtCameraPose,
                                        localPoint->first,
                                        localPoint->second,
                                        frameRect.width,
                                        frameRect.height,
                                        qtRenderAspect())
                   : PickViewportObject(qtPickables,
                                        qtCameraPose,
                                        localPoint->first,
                                        localPoint->second,
                                        frameRect.width,
                                        frameRect.height,
                                        qtRenderAspect()))
            : std::optional<ViewportPickResult>{};
        vkpt::editor::EditorCommand command;
        command.command_id = picked ? "viewport.pick" : "viewport.clear_selection";
        command.kind = picked ? vkpt::editor::EditorCommandKind::kSelectEntity
                              : vkpt::editor::EditorCommandKind::kClearSelection;
        command.source_widget = "viewport";
        command.frame_index = frameIndex;
        if (picked) {
          command.payload = vkpt::editor::SelectEntityCommand{picked->entity_id, qtAppendSelection(), false};
        } else {
          command.payload = vkpt::editor::ClearSelectionCommand{};
        }
        ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, command);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_command_history.push(command);
        ui_runtime_state.last_clicked_entity = picked ? picked->entity_id : 0u;
        if (picked && ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::Universal;
        } else if (!picked && !qtAppendSelection()) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        }
        ui_runtime_state.status_message = picked
            ? "viewport selected " + (picked->label.empty() ? std::to_string(picked->entity_id) : picked->label)
            : "viewport selection cleared";
        PushUiEvent(ui_event_log,
                    picked ? "viewport_pick" : "viewport_pick_miss",
                    "viewport",
                    "viewport",
                    frameIndex,
                    {},
                    picked ? std::to_string(picked->entity_id) : std::string("none"),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        qtUpdateGizmoHoverCursor(x, y);
        logger.log(vkpt::log::Severity::Info, "app", "Qt viewport pick", {
          {"entity_id", std::to_string(ui_runtime_state.last_clicked_entity)},
          {"x", std::to_string(x)},
          {"y", std::to_string(y)},
          {"selection_count", std::to_string(ui_selection_state.selected_entity_ids.size())}
        });
      };
      auto qtApplySceneGraphActivation =
          [&](const vkpt::platform::QtDockRowActivation& activation,
              vkpt::core::FrameIndex frameIndex) {
        if (activation.entity_id == 0u ||
            (activation.panel_id != "scene_graph" && activation.panel_id != "scene_tree")) {
          return;
        }

        if (activation.action == "delete") {
          qtDeleteSceneGraphTargets({activation.entity_id},
                                    frameIndex,
                                    "scene_tree",
                                    activation.row_id);
          return;
        }
        if (activation.action == "reparent") {
          qtReparentSceneGraphTargets(activation.entity_ids,
                                      activation.target_entity_id,
                                      frameIndex);
          return;
        }
        if (activation.action == "toggle_visibility") {
          qtToggleSceneGraphVisibility(activation.entity_id, frameIndex);
          return;
        }

        qtRefreshViewportPickablesIfSnapshotChanged();
        vkpt::editor::EditorCommand command;
        command.command_id = "scene_tree.select";
        command.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
        command.source_widget = "scene_tree";
        command.frame_index = frameIndex;
        command.payload = vkpt::editor::SelectEntityCommand{
            activation.entity_id,
            activation.append,
            activation.range_mode};

        ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, command);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_command_history.push(command);
        ui_runtime_state.last_clicked_entity = activation.entity_id;
        ui_runtime_state.focused_panel = "scene_tree";
        ui_runtime_state.last_scene_tree_operation =
            "selected " + std::to_string(activation.entity_id);
        if (ui_selection_state.selected_entity_ids.empty()) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        } else if (ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::Universal;
        }
        ui_runtime_state.status_message =
            "scene graph selected " + std::to_string(activation.entity_id);
        PushUiEvent(ui_event_log,
                    "scene_tree_select",
                    "scene_tree",
                    activation.row_id.empty() ? std::string("row") : activation.row_id,
                    frameIndex,
                    {},
                    std::to_string(activation.entity_id),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        qtUpdateGizmoHoverCursor(qtLastMouseX, qtLastMouseY);
        logger.log(vkpt::log::Severity::Info, "app", "Qt scene graph selection", {
          {"entity_id", std::to_string(activation.entity_id)},
          {"row_id", activation.row_id},
          {"append", activation.append ? "true" : "false"},
          {"range_mode", activation.range_mode ? "true" : "false"},
          {"selection_count", std::to_string(ui_selection_state.selected_entity_ids.size())}
        });
      };
      auto qtApplyAssetBrowserActivation =
          [&](const vkpt::platform::QtDockRowActivation& activation,
              vkpt::core::FrameIndex frameIndex) {
        if (activation.panel_id != "asset_browser") {
          return false;
        }
        constexpr std::string_view kScenePrefix = "asset.scene.";
        constexpr std::string_view kModelPrefix = "asset.model.";
        if (activation.viewport_drop &&
            (qtFpsMode || qtIsPlayMode() || qtLiveEditViewportInputArmed())) {
          ui_runtime_state.focused_panel = "asset_browser";
          ui_runtime_state.status_message =
              "viewport model drop ignored: play mode owns the canvas";
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "asset_browser_viewport_drop_ignored",
                      "asset_browser",
                      qtIsPlayMode()
                          ? "play_mode"
                          : (qtLiveEditViewportInputArmed() ? "live_edit_input" : "fps"),
                      frameIndex,
                      {},
                      activation.row_id,
                      ui_runtime_state.status_message);
          logger.log(vkpt::log::Severity::Info,
                     "app",
                     "Qt viewport asset drop ignored outside editor mode",
                     {{"row_id", activation.row_id},
                      {"runtime_mode", std::string(qtRuntimeModeName())},
                      {"play_mode", qtIsPlayMode() ? "true" : "false"},
                      {"fps_mode", qtFpsMode ? "true" : "false"}});
          return true;
        }
        if (activation.row_id.starts_with(kScenePrefix)) {
          const std::string scenePath = activation.row_id.substr(kScenePrefix.size());
          return qtLoadSceneFromAssetBrowser(scenePath, frameIndex);
        }
        if (activation.row_id.starts_with(kModelPrefix)) {
          std::vector<std::string> modelPaths;
          for (const auto& rowId : activation.row_ids) {
            if (rowId.starts_with(kModelPrefix)) {
              modelPaths.push_back(rowId.substr(kModelPrefix.size()));
            }
          }
          if (modelPaths.empty()) {
            modelPaths.push_back(activation.row_id.substr(kModelPrefix.size()));
          }
          return qtSpawnModelGroupFromAssetBrowser(std::move(modelPaths), activation, frameIndex);
        }
        return false;
      };
      auto qtFocusSelected = [&](vkpt::core::FrameIndex frameIndex, std::string_view source) {
        qtRefreshViewportPickablesIfSnapshotChanged();
        const auto bounds = qtActiveSelectionBounds();
        if (!bounds) {
          ui_runtime_state.status_message = "focus selected: no active selection";
          qtDockPanelsDirty = true;
          return false;
        }
        const auto center = PtMul(PtAdd(ToPtVec3(bounds->min), ToPtVec3(bounds->max)), 0.5f);
        const auto extent = PtSub(ToPtVec3(bounds->max), ToPtVec3(bounds->min));
        const float radius = std::max(0.05f, PtLength(extent) * 0.5f);
        const auto forward = qtCameraForward();
        const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, qtCameraPose.fov_deg)));
        const float distance = std::max(0.25f, (radius / std::max(0.05f, tanHalfFov)) * 1.35f);
        qtCameraFocusPoint = center;
        qtCameraFocusDistance = distance;
        qtCameraPose.target = center;
        qtCameraPose.position = PtSub(center, PtMul(forward, distance));
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("focus selected");
        qtSetAuthoredCameraFocusDistance(distance, "focus selected");
        PushUiEvent(ui_event_log,
                    "viewport_focus_selected",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    std::to_string(ui_selection_state.active_primary_entity),
                    "focused selected entity");
        return true;
      };
      auto qtPickFocusAt = [&](float x,
                               float y,
                               vkpt::core::FrameIndex frameIndex,
                               std::string_view source,
                               bool reportMiss) {
        qtRefreshViewportPickablesIfSnapshotChanged();
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        const auto pickSnapshot = qtCurrentUiSnapshot();
        const auto picked = localPoint
            ? (pickSnapshot
                   ? PickViewportObject(qtPickables,
                                        *pickSnapshot,
                                        qtCameraPose,
                                        localPoint->first,
                                        localPoint->second,
                                        frameRect.width,
                                        frameRect.height,
                                        qtRenderAspect())
                   : PickViewportObject(qtPickables,
                                        qtCameraPose,
                                        localPoint->first,
                                        localPoint->second,
                                        frameRect.width,
                                        frameRect.height,
                                        qtRenderAspect()))
            : std::optional<ViewportPickResult>{};
        if (!picked || picked->distance <= 0.0f) {
          if (reportMiss) {
            ui_runtime_state.status_message = "pick focus missed";
            qtDockPanelsDirty = true;
          }
          return false;
        }
        const auto ray = BuildViewportRay(qtCameraPose,
                                          localPoint->first,
                                          localPoint->second,
                                          frameRect.width,
                                          frameRect.height,
                                          qtRenderAspect());
        qtCameraFocusDistance = std::max(0.25f, picked->distance);
        qtCameraFocusPoint = PtAdd(ray.origin, PtMul(ray.direction, qtCameraFocusDistance));
        qtSetAuthoredCameraFocusDistance(qtCameraFocusDistance, "pick focus");
        PushUiEvent(ui_event_log,
                    "viewport_pick_focus",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    std::to_string(picked->entity_id),
                    "picked camera focus");
        return true;
      };
      qtDockFocusSelected = [&](vkpt::core::FrameIndex frameIndex) {
        return qtFocusSelected(frameIndex, "dock");
      };
      qtDockAutoFocus = [&](vkpt::core::FrameIndex frameIndex) {
        if (qtViewportLocalPoint(qtLastMouseX, qtLastMouseY) &&
            qtPickFocusAt(qtLastMouseX, qtLastMouseY, frameIndex, "dock", false)) {
          return true;
        }
        const auto frameRect = qtViewportImageRect();
        const float centerX = frameRect.x + frameRect.width * 0.5f;
        const float centerY = frameRect.y + frameRect.height * 0.5f;
        if (qtPickFocusAt(centerX, centerY, frameIndex, "dock", false)) {
          return true;
        }
        if (qtFocusSelected(frameIndex, "dock")) {
          return true;
        }
        ui_runtime_state.status_message = "auto focus missed";
        qtDockPanelsDirty = true;
        return false;
      };
      auto qtLogMenuCommand = [&](const std::string& actionId,
                                  const vkpt::editor::EditorCommand& menuCommand,
                                  const std::string& statusText,
                                  vkpt::core::FrameIndex frameIndex) {
        auto logged = menuCommand;
        logged.command_id = actionId;
        logged.frame_index = frameIndex;
        if (logged.source_widget.empty()) {
          logged.source_widget = "menu";
        }
        ui_command_history.push(logged);
        ui_runtime_state.status_message = statusText;
        ui_runtime_state.last_menu_action = actionId;
        ui_runtime_state.focused_panel = logged.source_widget;
        PushUiEvent(ui_event_log,
                    "app_action",
                    logged.source_widget,
                    "menu",
                    frameIndex,
                    {},
                    actionId,
                    statusText);
        qtDockPanelsDirty = true;
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
      };
      auto qtInputPrevTime = std::chrono::steady_clock::now();
      double qtLastUiFrameMs = 0.0;
      QtRayMetricAccumulator qtComputerRayMetrics;
      std::unordered_map<std::string, QtRayMetricAccumulator> qtRayMetricsByDevice;
      std::uint64_t qtActiveRenderLastRays = 0u;
      std::uint64_t qtActiveRenderAccumulatedRays = 0u;
      double qtActiveRenderSeconds = 0.0;
      double qtActiveRenderInstantRps = 0.0;
      double qtActiveRenderRollingRps = 0.0;
      double qtActiveRenderAverageRps = 0.0;
      auto qtRecordActiveRenderBatch = [&](std::uint64_t beforeRays,
                                           std::uint64_t afterRays,
                                           double batchMs) {
        if (afterRays < beforeRays || afterRays < qtActiveRenderLastRays) {
          qtActiveRenderLastRays = beforeRays;
          qtActiveRenderAccumulatedRays = 0u;
          qtActiveRenderSeconds = 0.0;
          qtActiveRenderInstantRps = 0.0;
          qtActiveRenderRollingRps = 0.0;
          qtActiveRenderAverageRps = 0.0;
        }
        if (afterRays <= beforeRays || batchMs <= 0.0) {
          qtActiveRenderLastRays = afterRays;
          return;
        }
        const std::uint64_t deltaRays = afterRays - beforeRays;
        const double dt = batchMs / 1000.0;
        qtActiveRenderAccumulatedRays += deltaRays;
        qtActiveRenderSeconds += dt;
        qtActiveRenderInstantRps = static_cast<double>(deltaRays) / dt;
        const double alpha = std::clamp(dt / 2.0, 0.05, 0.35);
        qtActiveRenderRollingRps = qtActiveRenderRollingRps <= 0.0
            ? qtActiveRenderInstantRps
            : (qtActiveRenderRollingRps * (1.0 - alpha) +
               qtActiveRenderInstantRps * alpha);
        qtActiveRenderAverageRps = qtActiveRenderSeconds > 0.0
            ? static_cast<double>(qtActiveRenderAccumulatedRays) / qtActiveRenderSeconds
            : 0.0;
        qtActiveRenderLastRays = afterRays;
      };
      auto qtLastDockPanelSync = std::chrono::steady_clock::time_point{};
      std::vector<QtDockPanelContent> qtLastDockPanels;
      std::string qtLastWindowTitle;
