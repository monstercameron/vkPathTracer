    const std::string rawTitleBase = BuildWindowTitleBase("raw");
    vkpt::platform::DesktopPlatform platform(rawTitleBase);
    auto platformState = platform.initialize();
    if (!platformState) {
      std::cerr << "desktop init failed\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop init failed");
      recordUiAction("app.window", "window", "desktop init failed",
                     MakeUnsupportedUiCommand("app.window", "desktop_init_failed"));
      writeStatus("error:desktop_init_failed", "desktop_init_failed");
      return 1;
    }
    logger.log(vkpt::log::Severity::Info, "app", "desktop platform initialized");

    auto* window = platform.window();
    auto* desktopWindow = static_cast<vkpt::platform::DesktopWindow*>(window);
    if (!window || !desktopWindow) {
      std::cerr << "desktop platform returned invalid window handle\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop platform window missing");
      return 1;
    }
    LogDesktopWindowState("after window object creation", *desktopWindow);
    logger.log(vkpt::log::Severity::Info,
               "app",
               std::string("desktop window object created: ") +
                   std::to_string(desktopWindow->metrics().width) + "x" +
                   std::to_string(desktopWindow->metrics().height));
    std::cout << "[ui] is_open(initial): " << (desktopWindow->is_open() ? "true" : "false") << "\n";
    if (windowWidth != 1280u || windowHeight != 720u) {
      logger.log(vkpt::log::Severity::Info,
                 "app",
                 std::string("applying requested window resize to ") +
                   std::to_string(windowWidth) + "x" + std::to_string(windowHeight));
      desktopWindow->resize(windowWidth, windowHeight);
    }
    LogDesktopWindowState("after explicit window resize attempt", *desktopWindow);
    std::unordered_map<NativeMenuId, std::string> menuCommandLookup;
    ui_runtime_state.active_layout_name = ui_layout_state.active_layout_name;
    ui_runtime_state.status_message = "window initialized";
    std::cout << "[ui] layout name: " << ui_layout_state.active_layout_name << "\n";
    std::cout << "[ui] panel count: " << ui_layout_state.panels.size() << "\n";
#ifdef _WIN32
    LogDesktopWindowState("before menu attach", *desktopWindow);
    {
      const auto uiMenu = vkpt::editor::BuildDefaultMenuBar();
      if (const auto nativeMenu = BuildNativeMenuBarFromModel(uiMenu, menuCommandLookup)) {
        SetMenu(static_cast<HWND>(desktopWindow->native_handle()), nativeMenu);
        DrawMenuBar(static_cast<HWND>(desktopWindow->native_handle()));
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string("native menu bar attached with ") +
                       std::to_string(menuCommandLookup.size()) + " command mappings");
        if (!menuCommandLookup.empty()) {
          auto first = menuCommandLookup.begin();
          auto last = menuCommandLookup.begin();
          for (auto it = std::next(menuCommandLookup.begin());
               it != menuCommandLookup.end(); ++it) {
            last = it;
          }
          logger.log(vkpt::log::Severity::Info, "app",
                     std::string("native menu ids: first=") +
                     std::to_string(first->first) + " action=" + first->second +
                     " last=" + std::to_string(last->first) + " action=" + last->second);
        }
      } else {
        logger.log(vkpt::log::Severity::Warning, "app", "native menu bar creation failed");
      }
    }
    desktopWindow->resize(windowWidth, windowHeight);
    LogDesktopWindowState("after menu attach resize", *desktopWindow);
#endif
    if (!desktopWindow->native_handle()) {
      std::cerr << "desktop window native_handle is null after initialization\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop native handle missing");
    }

#ifdef _WIN32
    if (const auto hwnd = static_cast<HWND>(desktopWindow->native_handle())) {
      ShowWindow(hwnd, SW_SHOWNORMAL);
      SetForegroundWindow(hwnd);
      UpdateWindow(hwnd);
    }

    if (const auto hwnd = static_cast<HWND>(desktopWindow->native_handle())) {
      RECT clientRect{};
      RECT windowRect{};
      GetWindowRect(hwnd, &windowRect);
      GetClientRect(hwnd, &clientRect);
      logger.log(vkpt::log::Severity::Info, "app",
                 std::string("native rect ") +
                 std::to_string(windowRect.left) + "," + std::to_string(windowRect.top) + " " +
                 std::to_string(windowRect.right - windowRect.left) + "x" +
                 std::to_string(windowRect.bottom - windowRect.top) +
                 " client " + std::to_string(clientRect.right - clientRect.left) + "x" +
                 std::to_string(clientRect.bottom - clientRect.top));
      if ((clientRect.right - clientRect.left) <= 0 ||
          (clientRect.bottom - clientRect.top) <= 0) {
        std::cout << "window client area is non-positive; will continue and trust resize event updates\n";
        logger.log(vkpt::log::Severity::Warning, "app",
                   "native client area is zero at startup; forcing fallback size");
        desktopWindow->resize(std::max<std::uint32_t>(windowWidth, 1280u),
                             std::max<std::uint32_t>(windowHeight, 720u));
      }
    }
#endif
    LogDesktopWindowState("after startup visibility/bootstrap", *desktopWindow);

    std::cout << "vkpt desktop window open (" << desktopWindow->metrics().width << "x"
              << desktopWindow->metrics().height << ")\n";
    std::cout << "Close the window to exit.\n";
    if (windowFrameLimit != 0u) {
      std::cout << "[ui] gui smoke frame limit: " << windowFrameLimit << "\n";
    }
    LogRuntimeMetadata(logger, "raw_platform_initialized", requestedPlatform,
                       selectedPlatform, effectivePlatform, openWindow, doRender,
                       autoExitWindow, windowFrameLimit, config.ui_present_hz.value);

    std::unique_ptr<vkpt::pathtracer::IPathTracer> previewTracer;
    std::string previewGpuName;   // GPU name shown in overlay (empty = CPU)
    std::string previewGpuInfo;   // one-line device info for diagnostics
#ifdef PT_ENABLE_VULKAN
    if (config.backend.value == "vulkan" || config.backend.value == "vulkan-compute") {
      const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
          PT_SHADER_SPV_PATH;
#else
          "shaders/pathtrace.spv";
#endif
      auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
      if (gpuTracer->is_valid()) {
        previewGpuName = gpuTracer->gpu_name();
        std::ostringstream ginfo;
        ginfo << gpuTracer->gpu_name()
              << "  [" << gpuTracer->gpu_type() << "]"
              << "  " << gpuTracer->vram_mb() << " MB VRAM"
              << "  Vulkan " << VK_VERSION_MAJOR(gpuTracer->vulkan_api())
              << "." << VK_VERSION_MINOR(gpuTracer->vulkan_api());
        previewGpuInfo = ginfo.str();
        std::cout << "[gpu] " << previewGpuInfo << "\n";
        logger.log(vkpt::log::Severity::Info, "app",
                   "Using Vulkan GPU path tracer: " + previewGpuInfo);
        previewTracer = std::move(gpuTracer);
      } else {
        logger.log(vkpt::log::Severity::Warning, "app",
                   "Vulkan GPU tracer init failed (" + gpuTracer->last_error() +
                   "), falling back to CPU tiled");
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
      auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
      if (gpuTracer->is_valid()) {
        gpuTracer->set_prefer_dxr(requestDxr);
        if (requestDxr && !gpuTracer->dxr_supported()) {
          const std::string errorMsg =
              "Requested d3d12-dxr but DXR is not supported on this GPU/device";
          logger.log(vkpt::log::Severity::Error, "app", errorMsg);
          writeStatus("error:d3d12_dxr_unsupported", errorMsg);
          return 1;
        }
        previewGpuName = gpuTracer->gpu_name();
        std::ostringstream ginfo;
        ginfo << gpuTracer->gpu_name()
              << "  D3D12"
            << "  " << gpuTracer->vram_mb() << " MB VRAM"
            << "  DXR=" << (gpuTracer->dxr_supported() ? "yes" : "no")
            << "(tier " << gpuTracer->dxr_tier_string() << ")";
        if (requestDxr) {
          ginfo << "  mode=DXR-phase1";
        }
        previewGpuInfo = ginfo.str();
        std::cout << "[gpu] " << previewGpuInfo << "\n";
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string(requestDxr ? "Using D3D12 DXR path tracer (phase1 compute fallback): "
                                          : "Using D3D12 GPU path tracer: ") + previewGpuInfo);
        previewTracer = std::move(gpuTracer);
      } else {
        const std::string errorMsg =
            "D3D12 GPU tracer init failed (" + gpuTracer->last_error() + ")";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:d3d12_init_failed", errorMsg);
        return 1;
      }
    }
#endif
    if (!previewTracer) {
      const bool allowCpuFallback =
          (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
           config.backend.value == "auto" || config.backend.value.empty());
      if (!allowCpuFallback) {
        const std::string errorMsg =
            "Requested backend '" + config.backend.value +
            "' is unavailable in this build; refusing CPU fallback";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:backend_unavailable", errorMsg);
        return 1;
      }
      if (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
          config.backend.value == "auto" || config.backend.value.empty()) {
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = InteractiveCpuWorkerCount();
        previewTracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
      } else {
        previewTracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
      }
    }
    vkpt::pathtracer::RenderSettings previewSettings{};
    const uint32_t windowPreviewWidth = std::max<uint32_t>(1u, static_cast<uint32_t>(desktopWindow->metrics().width));
    const uint32_t windowPreviewHeight = std::max<uint32_t>(1u, static_cast<uint32_t>(desktopWindow->metrics().height));
    const float previewScale = std::min(1.0f, 960.0f / static_cast<float>(windowPreviewWidth));
    previewSettings.width = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(windowPreviewWidth) * previewScale));
    previewSettings.height = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(windowPreviewHeight) * previewScale));
    // Smoke runs use a bounded sample cap so coordinator shutdown is exercised.
    previewSettings.spp = (windowFrameLimit != 0u)
        ? 1u
        : std::numeric_limits<uint32_t>::max();
    previewSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
    previewSettings.seed = 0xC001D00Dull;
    previewSettings.enable_nee = true;
    previewSettings.enable_mis = true;
    previewSettings.enable_denoiser = true;
    previewSettings.enable_temporal_aa = true;

    std::string previewError;
    bool previewTracerReady = false;
    bool previewNonBlack = false;
    bool previewRendered = false;
    uint32_t previewSampleIndex = 0u;
    uint32_t previewD3D12RetryCount = 0u;
    const uint32_t previewPixelCount = previewSettings.width * previewSettings.height;
    std::vector<uint32_t> previewPixelOrder(previewPixelCount);
    uint32_t previewPixelCursor = 0u;
    const uint32_t previewPixelsPerBatch = 1024u;
    const auto previewTraceBudgetPerFrame = std::chrono::milliseconds(16);
    uint32_t previewChunkCounter = 0u;

    // Background tiled CPU preview is owned by RenderCoordinator.  The main
    // thread posts commands and consumes immutable frames; it never mutates the
    // active tiled tracer after the coordinator starts.
    std::unique_ptr<vkpt::render::RenderCoordinator> bgRenderCoordinator;
    bool useBgRender = false;

    auto resetPreviewPixelOrder = [&](uint32_t sampleIndex) {
      std::iota(previewPixelOrder.begin(), previewPixelOrder.end(), 0u);
      if (previewPixelOrder.size() <= 1u) {
        return;
      }

      // Deterministic per-sample Fisher-Yates shuffle for spatially uniform progressive updates.
      uint32_t state = 0x9e3779b9u ^ (sampleIndex * 0x85ebca6bu + 0xc2b2ae35u);
      auto nextRand = [&]() -> uint32_t {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return state;
      };

      for (uint32_t i = previewPixelCount - 1u; i > 0u; --i) {
        const uint32_t j = nextRand() % (i + 1u);
        std::swap(previewPixelOrder[i], previewPixelOrder[j]);
      }
    };
    resetPreviewPixelOrder(previewSampleIndex);

    auto buildWindowSceneData = [&](vkpt::pathtracer::RTSceneData& outScene, std::string* outErr) -> bool {
      outScene = {};
      if (!config.scene_path.value.empty()) {
        auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
        if (!parseResult) {
          if (outErr) {
            *outErr = "scene parse failed";
          }
          return false;
        }
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(parseResult.value());
        if (!sceneResult) {
          if (outErr) {
            *outErr = "scene conversion failed";
          }
          return false;
        }
        outScene = std::move(sceneResult.value());
      } else {
        vkpt::scene::SceneDocument document = LoadDefaultSceneDocument();
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
        if (!sceneResult) {
          if (outErr) {
            *outErr = "builtin scene conversion failed";
          }
          return false;
        }
        outScene = std::move(sceneResult.value());
      }

      return true;
      };

    vkpt::pathtracer::RTSceneData previewScene;
    if (!buildWindowSceneData(previewScene, &previewError)) {
      // Fall back to a guaranteed visible diagnostic texture if scene load/setup fails.
      const uint32_t fbw = std::max<uint32_t>(64u, static_cast<uint32_t>(desktopWindow->metrics().width));
      const uint32_t fbh = std::max<uint32_t>(64u, static_cast<uint32_t>(desktopWindow->metrics().height));
      std::vector<std::uint8_t> diagnostic;
      diagnostic.resize(static_cast<std::size_t>(fbw) * static_cast<std::size_t>(fbh) * 4u, 255u);
      for (uint32_t y = 0; y < fbh; ++y) {
        for (uint32_t x = 0; x < fbw; ++x) {
          const std::size_t idx = (static_cast<std::size_t>(y) * fbw + x) * 4u;
          const bool checker = ((x / 32u) + (y / 32u)) % 2u == 0u;
          diagnostic[idx + 0u] = checker ? static_cast<std::uint8_t>((255u * x) / std::max(1u, fbw - 1u)) : 16u;
          diagnostic[idx + 1u] = checker ? static_cast<std::uint8_t>((255u * y) / std::max(1u, fbh - 1u)) : 220u;
          diagnostic[idx + 2u] = checker ? 32u : 64u;
          diagnostic[idx + 3u] = 255u;
        }
      }
      desktopWindow->set_framebuffer_rgba(diagnostic, fbw, fbh);
      previewNonBlack = true;
      logger.log(vkpt::log::Severity::Warning, "traceprobe",
                 "window preview scene setup failed; using diagnostic texture",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"error", previewError}
                 },
                 0);
    } else if (!previewTracer->configure(previewSettings) ||
               !previewTracer->load_scene_snapshot(previewScene) ||
               !previewTracer->build_or_update_acceleration() ||
               !previewTracer->reset_accumulation()) {
      previewError = "window preview tracer init failed";
      std::string previewInitFailureReason;
#ifdef PT_ENABLE_D3D12
      if (const auto* d3d12Tracer =
              dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(previewTracer.get())) {
        previewInitFailureReason = d3d12Tracer->last_error();
      }
#endif
      if (previewInitFailureReason.empty()) previewInitFailureReason = "unavailable";
      previewError = previewError + " (" + previewInitFailureReason + ")";
      logger.log(vkpt::log::Severity::Error, "traceprobe",
                 "window preview tracer initialization failed",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"width", std::to_string(previewSettings.width)},
                   {"height", std::to_string(previewSettings.height)},
                   {"spp", std::to_string(previewSettings.spp)},
                   {"max_depth", std::to_string(previewSettings.max_depth)},
                   {"error", previewInitFailureReason}
                  },
                  0);
    } else {
      previewTracerReady = true;
      previewError.clear();
      logger.log(vkpt::log::Severity::Info, "traceprobe",
                 "window preview tracer initialized",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"width", std::to_string(previewSettings.width)},
                   {"height", std::to_string(previewSettings.height)},
                   {"spp", std::to_string(previewSettings.spp)},
                   {"max_depth", std::to_string(previewSettings.max_depth)},
                   {"lights", std::to_string(previewScene.lights.size())},
                   {"materials", std::to_string(previewScene.materials.size())},
                   {"instances", std::to_string(previewScene.instances.size())},
                   {"sdf_primitives", std::to_string(previewScene.sdf_primitives.size())}
                 },
                 0);

      // Launch a coordinator for TiledCpuPathTracer so the main thread's Win32
      // message pump is never blocked and never mutates the live tracer.
      if (dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(previewTracer.get()) != nullptr) {
        useBgRender = true;
        vkpt::render::RenderCoordinatorConfig coordinatorConfig{};
        coordinatorConfig.publish_hz = 60u;
        coordinatorConfig.immediate_publish_count = 4u;
        bgRenderCoordinator = std::make_unique<vkpt::render::RenderCoordinator>(
            std::move(previewTracer),
            previewSettings,
            previewScene,
            coordinatorConfig);
        if (!bgRenderCoordinator->start()) {
          useBgRender = false;
          previewTracerReady = false;
          previewError = "window preview background coordinator failed";
        }
        logger.log(vkpt::log::Severity::Info, "traceprobe",
                   "background render coordinator launched for TiledCpuPathTracer");
      }
    }

    std::ostringstream overlay;
    overlay << "UI shell ready\n"
            << "backend: '" << config.backend.value << "'\n"
            << "scene: " << (config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value) << "\n"
            << "layout: " << ui_layout_state.active_layout_name << "\n"
            << "path tracing: " << (previewTracerReady ? "on" : "failed")
            << (previewNonBlack ? " (visible)" : " (diagnostic fallback)") << "\n";
    if (!previewError.empty()) {
      overlay << "status: " << previewError << "\n";
    }
    desktopWindow->set_overlay_text(overlay.str());

    SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
    UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                   ui_event_log, ui_command_history);
