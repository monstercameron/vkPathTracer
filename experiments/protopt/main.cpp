#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

#include "ProtoptShader.h"

namespace
{
constexpr UINT kFrameCount = 2;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

std::ofstream gLog;

void Log(const std::string& message)
{
    if (!gLog.is_open())
    {
        gLog.open("protopt_log.txt", std::ios::out | std::ios::app);
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);

    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " | " << message << "\n";
    const auto text = line.str();

    if (gLog.is_open())
    {
        gLog << text;
        gLog.flush();
    }
    OutputDebugStringA(text.c_str());
}

std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        std::string msg = std::string(what) + " failed with HRESULT " + HrToString(hr);
        Log(msg);
        throw std::runtime_error(msg);
    }
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return L"";
    const int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (count <= 0)
    {
        Log("Utf8ToWide failed with GetLastError=" + std::to_string(GetLastError()));
        return L"";
    }
    std::wstring out(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), count) <= 0)
    {
        Log("Utf8ToWide conversion failed with GetLastError=" + std::to_string(GetLastError()));
        return L"";
    }
    return out;
}

std::string WideToUtf8(const wchar_t* ws)
{
    if (!ws || !*ws)
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        Log("WideToUtf8 failed with GetLastError=" + std::to_string(GetLastError()));
        return {};
    }
    std::string out(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), count, nullptr, nullptr) <= 0)
    {
        Log("WideToUtf8 conversion failed with GetLastError=" + std::to_string(GetLastError()));
        return {};
    }
    if (!out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}


struct FrameConstants
{
    float resolutionX;
    float resolutionY;
    float timeSec;
    float frameIndex;
};

class ProtoptApp
{
public:
    ~ProtoptApp()
    {
        try
        {
            WaitForGpu();
        }
        catch (...)
        {
            Log("protopt cleanup: WaitForGpu failed");
        }
        if (m_fenceEvent)
        {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        if (m_hwnd && IsWindow(m_hwnd))
        {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
        m_hwnd = nullptr;
    }

    int Run(HINSTANCE instance, int showCommand)
    {
        Log("protopt startup");
        CreateWindowAndDevice(instance, showCommand);

        auto last = std::chrono::steady_clock::now();
        m_startTime = last;
        m_titleTimer = last;

        MSG msg{};
        while (msg.message != WM_QUIT)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (m_minimized)
            {
                Sleep(16);
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            m_lastCpuFrameMs = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;

            Render();
            UpdateTitleIfNeeded(now);
        }

        WaitForGpu();
        Log("protopt shutdown");
        return static_cast<int>(msg.wParam);
    }

private:
    HWND m_hwnd = nullptr;
    UINT m_width = 1280;
    UINT m_height = 720;
    bool m_minimized = false;
    bool m_allowTearing = false;
    std::string m_adapterName = "unknown adapter";

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> m_renderTargets;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> m_commandAllocators;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12Fence> m_fence;
    std::array<UINT64, kFrameCount> m_fenceValues{};
    UINT64 m_nextFenceValue = 1;
    HANDLE m_fenceEvent = nullptr;
    UINT m_frameIndex = 0;

    std::chrono::steady_clock::time_point m_startTime{};
    std::chrono::steady_clock::time_point m_titleTimer{};
    uint64_t m_frameCounter = 0;
    uint64_t m_titleFrameCounter = 0;
    std::array<uint64_t, kFrameCount> m_accumFrameCounters{};
    double m_lastCpuFrameMs = 0.0;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ProtoptApp* app = nullptr;
        if (msg == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<ProtoptApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        else
        {
            app = reinterpret_cast<ProtoptApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (app)
            return app->HandleMessage(hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
        {
            const UINT newWidth = LOWORD(lParam);
            const UINT newHeight = HIWORD(lParam);
            m_minimized = (wParam == SIZE_MINIMIZED);
            if (!m_minimized && newWidth > 0 && newHeight > 0 && m_swapChain)
            {
                Resize(newWidth, newHeight);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void CreateWindowAndDevice(HINSTANCE instance, int showCommand)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ProtoptApp::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"protopt_window_class";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);

        RECT rect{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        m_hwnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"protopt starting...",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            instance,
            this);

        if (!m_hwnd)
            throw std::runtime_error("CreateWindowExW failed");

        InitD3D12();
        ShowWindow(m_hwnd, showCommand);
        UpdateWindow(m_hwnd);
    }

    void InitD3D12()
    {
        Log("D3D12 init begin");

#if defined(_DEBUG)
        if (ComPtr<ID3D12Debug> debug; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            Log("D3D12 debug layer enabled");
        }
#endif

        UINT factoryFlags = 0;
#if defined(_DEBUG)
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

        m_allowTearing = CheckTearingSupport();
        SelectAdapterAndCreateDevice();
        CreateCommandQueue();
        CreateSwapChain();
        CreateDescriptorHeaps();
        CreateRenderTargets();
        CreateCommandObjects();
        CreatePipeline();
        CreateSyncObjects();

        Log("D3D12 init success; adapter=" + m_adapterName + (m_allowTearing ? "; tearing=yes" : "; tearing=no"));
    }

    bool CheckTearingSupport()
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
        {
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &allowTearing,
                    sizeof(allowTearing))))
            {
                return allowTearing == TRUE;
            }
        }
        return false;
    }

    void SelectAdapterAndCreateDevice()
    {
        ComPtr<IDXGIAdapter1> chosen;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (m_factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                chosen = adapter;
                m_adapterName = WideToUtf8(desc.Description);
                break;
            }
        }

        if (!chosen)
        {
            Log("No hardware D3D12 adapter found; using WARP");
            ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)), "EnumWarpAdapter");
            DXGI_ADAPTER_DESC1 desc{};
            chosen->GetDesc1(&desc);
            m_adapterName = WideToUtf8(desc.Description);
        }

        ThrowIfFailed(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");
    }

    void CreateCommandQueue()
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 0;
        ThrowIfFailed(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue");
    }

    void CreateSwapChain()
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = kBackBufferFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kFrameCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> swap1;
        ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
                          m_commandQueue.Get(),
                          m_hwnd,
                          &desc,
                          nullptr,
                          nullptr,
                          &swap1),
                      "CreateSwapChainForHwnd");
        ThrowIfFailed(m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        ThrowIfFailed(swap1.As(&m_swapChain), "Query IDXGISwapChain3");
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    void CreateDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = kFrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap");
        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(UINT index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(m_rtvDescriptorSize);
        return handle;
    }

    void CreateRenderTargets()
    {
        for (UINT i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])), "Get swapchain buffer");
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, RtvHandle(i));
        }
    }

    void CreateCommandObjects()
    {
        for (UINT i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])), "CreateCommandAllocator");
        }

        ThrowIfFailed(m_device->CreateCommandList(
                          0,
                          D3D12_COMMAND_LIST_TYPE_DIRECT,
                          m_commandAllocators[m_frameIndex].Get(),
                          nullptr,
                          IID_PPV_ARGS(&m_commandList)),
                      "CreateCommandList");
        ThrowIfFailed(m_commandList->Close(), "Close initial command list");
    }

    void CreatePipeline()
    {
        ComPtr<ID3DBlob> rootBlob;
        ComPtr<ID3DBlob> errorBlob;

        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.Constants.ShaderRegister = 0;
        rootParam.Constants.RegisterSpace = 0;
        rootParam.Constants.Num32BitValues = 4;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &rootParam;
        rootDesc.NumStaticSamplers = 0;
        rootDesc.pStaticSamplers = nullptr;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        HRESULT hr = D3D12SerializeRootSignature(
            &rootDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &rootBlob,
            &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
                Log(std::string("Root signature error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
            ThrowIfFailed(hr, "D3D12SerializeRootSignature");
        }

        ThrowIfFailed(m_device->CreateRootSignature(
                          0,
                          rootBlob->GetBufferPointer(),
                          rootBlob->GetBufferSize(),
                          IID_PPV_ARGS(&m_rootSignature)),
                      "CreateRootSignature");

        ComPtr<ID3DBlob> vs;
        ComPtr<ID3DBlob> ps;
        CompileShader("VSMain", "vs_5_1", &vs);
        CompileShader("PSMain", "ps_5_1", &ps);

        D3D12_RASTERIZER_DESC raster{};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.FrontCounterClockwise = FALSE;
        raster.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        raster.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        raster.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        raster.DepthClipEnable = TRUE;
        raster.MultisampleEnable = FALSE;
        raster.AntialiasedLineEnable = FALSE;
        raster.ForcedSampleCount = 0;
        raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        // Accumulate path samples directly in the swapchain target; the blend factor
        // below supplies the running average weight without a separate history texture.
        rtBlend.BlendEnable = TRUE;
        rtBlend.LogicOpEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
        rtBlend.DestBlend = D3D12_BLEND_INV_BLEND_FACTOR;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blend{};
        blend.AlphaToCoverageEnable = FALSE;
        blend.IndependentBlendEnable = FALSE;
        for (auto& target : blend.RenderTarget)
            target = rtBlend;

        D3D12_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        depth.StencilEnable = FALSE;
        depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout = {nullptr, 0};
        pso.pRootSignature = m_rootSignature.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.RasterizerState = raster;
        pso.BlendState = blend;
        pso.DepthStencilState = depth;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
        pso.SampleDesc.Count = 1;
        pso.SampleDesc.Quality = 0;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipelineState)), "CreateGraphicsPipelineState");
    }

    void CompileShader(const char* entry, const char* target, ID3DBlob** blob)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(
            kShaderSource,
            strlen(kShaderSource),
            "protopt_embedded.hlsl",
            nullptr,
            nullptr,
            entry,
            target,
            flags,
            0,
            blob,
            &errors);
        if (errors)
        {
            Log(std::string("Shader compiler output for ") + entry + ": " + static_cast<const char*>(errors->GetBufferPointer()));
        }
        ThrowIfFailed(hr, "D3DCompile");
    }

    void CreateSyncObjects()
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
        m_fenceValues.fill(0);
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
            throw std::runtime_error("CreateEventW for fence failed");
    }

    void Resize(UINT width, UINT height)
    {
        if (width == m_width && height == m_height)
            return;

        Log("Resize requested: " + std::to_string(width) + "x" + std::to_string(height));
        WaitForGpu();

        for (auto& rt : m_renderTargets)
            rt.Reset();

        const UINT flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ThrowIfFailed(m_swapChain->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat, flags), "ResizeBuffers");
        m_width = width;
        m_height = height;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        CreateRenderTargets();
        m_frameCounter = 0;
        m_titleFrameCounter = 0;
        m_accumFrameCounters.fill(0);
    }

    void Render()
    {
        const auto now = std::chrono::steady_clock::now();
        const float timeSec = static_cast<float>(std::chrono::duration<double>(now - m_startTime).count());
        FrameConstants constants{
            static_cast<float>(m_width),
            static_cast<float>(m_height),
            timeSec,
            static_cast<float>(m_frameCounter)};

        ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator::Reset");
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get()), "CommandList::Reset");

        auto toRenderTarget = TransitionBarrier(
            m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &toRenderTarget);

        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);

        const auto currentFrameIndex = m_frameIndex;
        const auto rtv = RtvHandle(currentFrameIndex);
        const FLOAT clearColor[4] = {0.02f, 0.025f, 0.035f, 1.0f};

        uint64_t& frameAccumCount = m_accumFrameCounters[currentFrameIndex];
        if (frameAccumCount == 0)
        {
            m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        }
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const double blendStep = static_cast<double>(std::min<uint64_t>(frameAccumCount + 1, 32ull));
        const FLOAT blendFactor[4] = {
            static_cast<FLOAT>(1.0 / blendStep),
            static_cast<FLOAT>(1.0 / blendStep),
            static_cast<FLOAT>(1.0 / blendStep),
            1.0f};
        m_commandList->OMSetBlendFactor(blendFactor);

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetGraphicsRoot32BitConstants(0, 4, &constants, 0);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(3, 1, 0, 0);

        auto toPresent = TransitionBarrier(
            m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &toPresent);

        ThrowIfFailed(m_commandList->Close(), "CommandList::Close");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_commandQueue->ExecuteCommandLists(1, lists);

        const UINT presentFlags = m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        ThrowIfFailed(m_swapChain->Present(0, presentFlags), "Present");
        MoveToNextFrame();

        ++m_frameCounter;
        ++frameAccumCount;
        ++m_titleFrameCounter;
    }

    void MoveToNextFrame()
    {
        // Track one fence value per back buffer so allocator reuse waits only
        // for the GPU work that last touched the buffer we are about to record.
        const UINT64 currentFence = m_nextFenceValue;
        ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFence), "CommandQueue::Signal");
        m_fenceValues[m_frameIndex] = currentFence;
        ++m_nextFenceValue;

        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

        if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
        {
            ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), "Fence::SetEventOnCompletion");
            WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
        }
    }

    void WaitForGpu()
    {
        if (!m_commandQueue || !m_fence || !m_fenceEvent)
            return;

        const UINT64 fence = m_nextFenceValue++;
        ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence), "WaitForGpu Signal");
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent), "WaitForGpu SetEventOnCompletion");
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
        m_fenceValues.fill(fence);
    }

    void UpdateTitleIfNeeded(std::chrono::steady_clock::time_point now)
    {
        const double elapsed = std::chrono::duration<double>(now - m_titleTimer).count();
        if (elapsed < 0.25)
            return;

        const double fps = static_cast<double>(m_titleFrameCounter) / elapsed;
        const double frameMs = fps > 0.0 ? 1000.0 / fps : 0.0;
        const double raysPerSec = static_cast<double>(m_width) * static_cast<double>(m_height) * fps;
        const double gigaRays = raysPerSec / 1.0e9;

        std::ostringstream title;
        title << "protopt D3D12 | "
              << m_width << "x" << m_height << " | "
              << std::fixed << std::setprecision(1) << fps << " FPS | "
              << std::setprecision(3) << frameMs << " ms | "
              << std::setprecision(3) << gigaRays << " primary Grays/s | "
              << "CPU frame " << std::setprecision(3) << m_lastCpuFrameMs << " ms | "
              << "frame " << m_frameCounter << " | "
              << m_adapterName;

        SetWindowTextW(m_hwnd, Utf8ToWide(title.str()).c_str());
        m_titleTimer = now;
        m_titleFrameCounter = 0;
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        ProtoptApp app;
        return app.Run(instance, showCommand);
    }
    catch (const std::bad_alloc& e)
    {
        const std::string message = std::string("out of memory: ") + e.what();
        Log(std::string("FATAL: ") + message);
        MessageBoxA(nullptr, message.c_str(), "protopt fatal error", MB_ICONERROR | MB_OK);
        return 1;
    }
    catch (const std::exception& e)
    {
        Log(std::string("FATAL: ") + e.what());
        MessageBoxA(nullptr, e.what(), "protopt fatal error", MB_ICONERROR | MB_OK);
        return 1;
    }
    catch (...)
    {
        Log("FATAL: non-standard exception");
        MessageBoxA(nullptr, "non-standard exception", "protopt fatal error", MB_ICONERROR | MB_OK);
        return 1;
    }
}
