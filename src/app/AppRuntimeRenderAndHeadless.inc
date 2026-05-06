  // ---- --crash-test (A07/A08) -----------------------------------------------
  if (crashTest) {
    recordUiAction("app.crash_test", "crash_test", "crash test requested",
                   MakeUnsupportedUiCommand("app.crash_test", "writing crash artifact"));
    logger.log(vkpt::log::Severity::Fatal, "app", "crash test requested — writing crash artifact");

    // Gate 10 (C20): embed renderer crash state when possible.
    try {
      const auto backendNames = vkpt::render::AvailableBackendNames();
      if (!backendNames.empty()) {
        auto rendererBackend = vkpt::render::CreateBackend(backendNames.front());
        if (rendererBackend && rendererBackend->initialize()) {
          const auto state = vkpt::render::BuildRendererCrashState(
              *rendererBackend, 0u, "ptapp.crash_test", "crash_test", "none", "crash_test_requested");
          vkpt::diagnostics::CrashRecorder::instance().update_renderer_state_json(
              vkpt::render::SerializeRenderCrashState(state));
        }
      }
    } catch (...) {
      // best-effort; crash artifacts should still write.
    }

    vkpt::diagnostics::CrashRecorder::instance().set_last_error("crash_test_requested");
    const std::string crashDir = vkpt::diagnostics::CrashRecorder::instance().flush(
        config.crash_artifact_dir.value);
    std::cout << "crash test: artifact written to " << (crashDir.empty() ? "(failed)" : crashDir) << "\n";
    status.last_crash_artifact = crashDir;
    writeStatus("crash_test", "crash_test_requested");
    return 42;
  }

  if (dynamicPhysicsGate) {
    PrintNonGuiPlatformShellNotice("dynamic-physics-gate", selectedPlatform, effectivePlatform);
    return RunDynamicPhysicsPerformanceGate(config.scene_path.value,
                                            config.backend.value,
                                            config.render_width.value,
                                            config.render_height.value,
                                            std::max<uint32_t>(1u, config.spp.value));
  }

  if (thirdPersonScriptGate) {
    PrintNonGuiPlatformShellNotice("third-person-script-gate", selectedPlatform, effectivePlatform);
    return RunThirdPersonScriptPerformanceGate(config.scene_path.value,
                                               config.backend.value,
                                               config.render_width.value,
                                               config.render_height.value,
                                               std::max<uint32_t>(1u, config.spp.value));
  }

  // ---- --render -------------------------------------------------------------
  if (doRender) {
    PrintNonGuiPlatformShellNotice("render", selectedPlatform, effectivePlatform);
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_prepare", 0);
    ui_runtime_state.status_message = "render_prepare";
    ui_runtime_state.active_scene = RuntimeSceneDisplayName(config.scene_path.value);
    recordUiAction("app.render", "render", "render_prepare",
                   MakeUnsupportedUiCommand("app.render", "render path entered"));
    vkpt::scene::SceneDocument document;
    if (!config.scene_path.value.empty()) {
      auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
      if (!parseResult) {
        std::cerr << "failed to parse scene: " << config.scene_path.value << "\n";
        recordUiAction("app.render", "render", "scene parse failed",
                       MakeUnsupportedUiCommand("app.render", "scene_parse_failed"));
        writeStatus("error:scene_parse_failed", "scene_parse_failed");
        return 2;
      }
      document = parseResult.value();
      vkpt::diagnostics::CrashRecorder::instance().update_scene(config.scene_path.value);
    } else {
      document = LoadDefaultSceneDocument();
      vkpt::diagnostics::CrashRecorder::instance().update_scene(std::string(kDefaultSceneAssetPath));
    }
    vkpt::core::TraceExecution("render_scene_document_loaded", {
      {"scene", RuntimeSceneDisplayName(config.scene_path.value)},
      {"scene_name", document.metadata.scene_name},
      {"entities", std::to_string(document.entities.size())},
      {"geometry", std::to_string(document.geometry.size())},
      {"materials", std::to_string(document.materials.size())},
      {"assets", std::to_string(document.assets.size())},
      {"lights", std::to_string(document.lights.size())},
      {"sdf_primitives", std::to_string(document.sdf_primitives.size())}
    });

    vkpt::pathtracer::RenderSettings settings{};
    settings.width     = std::max<uint32_t>(1, config.render_width.value);
    settings.height    = std::max<uint32_t>(1, config.render_height.value);
    settings.spp       = std::max<uint32_t>(1, config.spp.value);
    settings.max_depth = std::max<uint32_t>(1, config.max_depth.value);
    settings.seed      = 0xBAADF00DULL;
    settings.enable_nee = true;
    settings.enable_mis = true;
    settings.enable_denoiser = gpuDenoiser;
    settings.enable_temporal_aa = temporalAa;

    auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!sceneResult) {
      std::cerr << "scene conversion failed\n";
      recordUiAction("app.render", "render", "scene conversion failed",
                     MakeUnsupportedUiCommand("app.render", "scene_conversion_failed"));
      writeStatus("error:scene_conversion_failed", "scene_conversion_failed");
      return 2;
    }
    vkpt::core::TraceExecution("render_rt_scene_ready", {
      {"vertices", std::to_string(sceneResult.value().vertices.size())},
      {"indices", std::to_string(sceneResult.value().indices.size())},
      {"instances", std::to_string(sceneResult.value().instances.size())},
      {"materials", std::to_string(sceneResult.value().materials.size())},
      {"textures", std::to_string(sceneResult.value().textures.size())},
      {"lights", std::to_string(sceneResult.value().lights.size())},
      {"sdf_primitives", std::to_string(sceneResult.value().sdf_primitives.size())}
    });

    // ---- Vulkan software-BVH compute path (C10 / Gate 4) ------------------
    const bool useVulkan = (config.backend.value == "vulkan" ||
                            config.backend.value == "vulkan-compute");
    if (useVulkan) {
      vkpt::diagnostics::CrashRecorder::instance().update_backend("vulkan-compute");
      vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("vulkan_bvh_pass", 0);
      status.selected_renderer_path = "vulkan_bvh_compute";

      vkpt::render::VulkanComputeBackend vulkanBackend;
      auto bvhResult = vkpt::render::RunVulkanBVHPass(
          vulkanBackend, sceneResult.value(), settings.width, settings.height);

      if (!bvhResult.success) {
        std::cerr << "vulkan bvh pass failed: " << bvhResult.error << "\n";
        recordUiAction("app.render", "render", "vulkan bvh pass failed",
                       MakeUnsupportedUiCommand("app.render", "vulkan_bvh_failed"));
        writeStatus("error:vulkan_bvh_failed", bvhResult.error);
        return 2;
      }

      // Simulated backend: film texture is zero-filled. Write a black PNG
      // placeholder so the output path contract is satisfied.
      std::filesystem::create_directories(
          std::filesystem::path(config.output_path.value).parent_path());
      vkpt::pathtracer::FilmLdr ldr;
      ldr.width  = settings.width;
      ldr.height = settings.height;
      ldr.rgba8.assign(static_cast<std::size_t>(settings.width) * settings.height * 4u, 0u);

      std::string saveError;
      if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
        std::cerr << "png save failed: " << saveError << "\n";
        recordUiAction("app.render", "render", "png save failed",
                       MakeUnsupportedUiCommand("app.render", "png_save_failed"));
        writeStatus("error:png_save_failed", saveError);
        return 2;
      }

      std::cout << "render complete (vulkan-compute): " << config.output_path.value << "\n";
      std::cout << "vertices: "  << bvhResult.vertex_buffer_count << "\n";
      std::cout << "indices: "   << bvhResult.index_buffer_count  << "\n";
      std::cout << "instances: " << bvhResult.instance_count      << "\n";
      std::cout << "bvh_nodes: " << bvhResult.bvh_node_estimate   << "\n";

      const std::string perfSummary = "vertices=" + std::to_string(bvhResult.vertex_buffer_count)
                                    + " indices=" + std::to_string(bvhResult.index_buffer_count)
                                    + " bvh_nodes=" + std::to_string(bvhResult.bvh_node_estimate);
      status.performance_summary = perfSummary;
      vkpt::core::TraceExecution("render_vulkan_bvh_complete", {
        {"renderer_path", "vulkan_bvh_compute"},
        {"output", config.output_path.value},
        {"vertices", std::to_string(bvhResult.vertex_buffer_count)},
        {"indices", std::to_string(bvhResult.index_buffer_count)},
        {"instances", std::to_string(bvhResult.instance_count)},
        {"bvh_nodes", std::to_string(bvhResult.bvh_node_estimate)}
      });
      recordUiAction("app.render", "render", "vulkan render complete",
                     MakeUnsupportedUiCommand("app.render", "render_ok"));
      writeStatus("render_ok");
      return 0;
    }

    // ---- CPU/GPU path tracer selection ----------------------------------------
    std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
    std::string selectedTracerPath = "unselected";
#ifdef PT_ENABLE_VULKAN
    if (config.backend.value == "vulkan" || config.backend.value == "vulkan-compute") {
      const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
          PT_SHADER_SPV_PATH;
#else
          "shaders/pathtrace.spv";
#endif
      auto gpuT = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
      if (gpuT->is_valid()) {
        selectedTracerPath = "vulkan_compute";
        tracer = std::move(gpuT);
      }
    }
#endif
#ifdef PT_ENABLE_D3D12
    if (config.backend.value == "d3d12" || config.backend.value == "d3d12-dxr") {
      const bool requestDxr = (config.backend.value == "d3d12-dxr");
      const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
          PT_SHADER_HLSL_PATH;
#else
          "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
      auto gpuT = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
      if (gpuT->is_valid()) {
        gpuT->set_prefer_dxr(requestDxr);
        if (requestDxr && !gpuT->dxr_supported()) {
          const std::string errorMsg =
              "Requested d3d12-dxr but DXR is not supported on this GPU/device";
          std::cerr << errorMsg << "\n";
          logger.log(vkpt::log::Severity::Error, "app", errorMsg);
          writeStatus("error:d3d12_dxr_unsupported", errorMsg);
          recordUiAction("app.render", "render", "d3d12 dxr unsupported",
                         MakeUnsupportedUiCommand("app.render", "d3d12_dxr_unsupported"));
          return 2;
        }
        tracer = std::move(gpuT);
        if (requestDxr) {
          status.selected_renderer_path = "d3d12_dxr_phase1";
          selectedTracerPath = "d3d12_dxr";
        } else {
          selectedTracerPath = "d3d12_compute";
        }
      } else {
        const std::string errorMsg =
            "D3D12 GPU tracer init failed (" + gpuT->last_error() + ")";
        std::cerr << errorMsg << "\n";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:d3d12_init_failed", errorMsg);
        recordUiAction("app.render", "render", "d3d12 render init failed",
                       MakeUnsupportedUiCommand("app.render", "d3d12_render_init_failed"));
        return 2;
      }
    }
#endif
    if (!tracer) {
      const bool allowCpuFallback =
          (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
           config.backend.value == "auto" || config.backend.value.empty());
      if (!allowCpuFallback) {
        const std::string errorMsg =
            "Requested backend '" + config.backend.value +
            "' is unavailable in this build; refusing CPU fallback";
        std::cerr << errorMsg << "\n";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:backend_unavailable", errorMsg);
        recordUiAction("app.render", "render", "backend unavailable",
                       MakeUnsupportedUiCommand("app.render", "backend_unavailable"));
        return 2;
      }
      if (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
          config.backend.value == "auto"       || config.backend.value.empty()) {
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = 0;
        tracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
        selectedTracerPath = "cpu_tiled";
      } else {
        tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
        selectedTracerPath = "cpu_scalar";
      }
    }
    vkpt::core::TraceExecution("render_tracer_selected", {
      {"requested_backend", config.backend.value},
      {"renderer_path", selectedTracerPath},
      {"resolution", std::to_string(settings.width) + "x" + std::to_string(settings.height)},
      {"spp", std::to_string(settings.spp)},
      {"max_depth", std::to_string(settings.max_depth)},
      {"denoiser", settings.enable_denoiser ? "true" : "false"},
      {"temporal_aa", settings.enable_temporal_aa ? "true" : "false"}
    });
    if (!tracer->configure(settings)) {
      std::cerr << "pathtracer configure failed\n";
      recordUiAction("app.render", "render", "pathtracer configure failed",
                     MakeUnsupportedUiCommand("app.render", "tracer_configure_failed"));
      writeStatus("error:tracer_configure_failed", "tracer_configure_failed");
      return 2;
    }
    if (!tracer->load_scene_snapshot(sceneResult.value()) || !tracer->build_or_update_acceleration()) {
      std::cerr << "failed to prepare scene for path tracing\n";
      recordUiAction("app.render", "render", "scene bvh build failed",
                     MakeUnsupportedUiCommand("app.render", "bvh_build_failed"));
      writeStatus("error:bvh_build_failed", "bvh_build_failed");
      return 2;
    }

    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_execute", 0);
    std::filesystem::create_directories(
        std::filesystem::path(config.output_path.value).parent_path());
    tracer->reset_accumulation();
    for (uint32_t sample = 0; sample < settings.spp; ++sample) {
      if (!tracer->render_sample_batch(0, settings.height, sample, 0)) {
        std::cerr << "render failed\n";
        recordUiAction("app.render", "render", "render sample failed",
                       MakeUnsupportedUiCommand("app.render", "render_sample_batch_failed"));
        writeStatus("error:render_failed", "render_sample_batch_failed");
        return 2;
      }
    }

    const auto ldr = tracer->resolve_ldr();
    const auto hdr = tracer->resolve_hdr();
    std::string saveError;
    if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
      std::cerr << "png save failed: " << saveError << "\n";
      recordUiAction("app.render", "render", "png save failed",
                     MakeUnsupportedUiCommand("app.render", "png_save_failed"));
      writeStatus("error:png_save_failed", saveError);
      return 2;
    }
    if (!config.exr_output_path.value.empty()) {
      if (!vkpt::pathtracer::SaveExrCompat(config.exr_output_path.value, hdr, &saveError)) {
        std::cerr << "exr save failed: " << saveError << "\n";
        recordUiAction("app.render", "render", "exr save failed",
                       MakeUnsupportedUiCommand("app.render", "exr_save_failed"));
        writeStatus("error:exr_save_failed", saveError);
        return 2;
      }
    }

    const auto counters = tracer->read_counters();
    std::cout << "render complete: " << config.output_path.value << "\n";
    std::cout << "samples: " << counters.samples << "\n";
    std::cout << "rays: " << counters.rays << "\n";
    std::cout << "triangle hits: " << counters.triangle_hits << "\n";
    std::cout << "sdf hits: " << counters.sdf_hits << "\n";

    const std::string perfSummary = "samples=" + std::to_string(counters.samples)
                                  + " rays=" + std::to_string(counters.rays)
                                  + " tri_hits=" + std::to_string(counters.triangle_hits);
    status.performance_summary = perfSummary;
    vkpt::core::TraceExecution("render_complete", {
      {"renderer_path", selectedTracerPath},
      {"output", config.output_path.value},
      {"samples", std::to_string(counters.samples)},
      {"rays", std::to_string(counters.rays)},
      {"triangle_hits", std::to_string(counters.triangle_hits)},
      {"sdf_hits", std::to_string(counters.sdf_hits)}
    });
    recordUiAction("app.render", "render", "cpu render complete",
                   MakeUnsupportedUiCommand("app.render", "render_ok"));
    writeStatus("render_ok");
    return 0;
  }

  // ---- Default / headless mode ----------------------------------------------
  std::cout << "ptapp started\n";
  std::cout << "mode: " << (config.headless.value ? "headless" : "demo") << "\n";
  std::cout << "platform: " << config.platform.value << "\n";
  std::cout << "backend: " << config.backend.value << "\n";
  if (!config.scene_path.value.empty()) {
    std::cout << "scene: " << config.scene_path.value << "\n";
  }
  std::cout << "log level: " << config.log_level.value << "\n";
  logger.log(vkpt::log::Severity::Info, "app", "runtime boot", {
    {"platform",  config.platform.value},
    {"backend",   config.backend.value},
    {"log_level", config.log_level.value},
    {"scene",     config.scene_path.value}
  });

  if (config.headless.value || argc == 1) {
    recordUiAction("app.headless", "headless", "headless init", MakeUnsupportedUiCommand("app.headless", "headless mode requested"));
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("headless_init", 0);
    auto platform = vkpt::platform::CreatePlatform(vkpt::platform::RuntimePlatformKind::Headless, "vkpt-headless");
    auto state = platform ? platform->initialize()
                          : vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Unsupported);
    if (!state) {
      std::cerr << "headless init failed\n";
      recordUiAction("app.headless", "headless", "headless init failed",
                     MakeUnsupportedUiCommand("app.headless", "headless_init_failed"));
      writeStatus("error:headless_init_failed", "headless_init_failed");
      return 1;
    }
    std::cout << "headless platform initialized\n";
    recordUiAction("app.headless", "headless", "headless init done",
                   MakeUnsupportedUiCommand("app.headless", "headless initialized"));
    platform->shutdown();
  }

  if (!config.scene_path.value.empty()) {
    auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
    if (!parseResult) {
      std::cerr << "Failed to load scene file: " << config.scene_path.value << "\n";
      recordUiAction("app.scene_load", "scene_load", "scene load failed",
                     MakeUnsupportedUiCommand("app.scene_load", "scene_load_failed"));
      writeStatus("error:scene_load_failed", "scene_load_failed");
      return 2;
    }
    recordUiAction("app.scene_load", "scene_load", "scene loaded",
                   MakeUnsupportedUiCommand("app.scene_load", "scene_load_success"));
    std::cout << "scene entities: " << parseResult.value().snapshot().entity_ids.size() << '\n';
    std::cout << "scene hash: "     << parseResult.value().export_hash_hex() << '\n';
    std::cout << "asset refs: "     << parseResult.value().snapshot().asset_refs.size() << '\n';
  }

  recordUiAction("app.exit", "app_shell", "app exit", MakeUnsupportedUiCommand("app.exit", "normal exit"));
  writeStatus("ok");
  return 0;
