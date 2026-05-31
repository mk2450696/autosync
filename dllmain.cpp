// ============================================================
//  AutoPacer v2 - Cross-Adapter VBlank Synchronizer
//  Fixes FG tearing on iGPU display + dGPU render setups
//
//  How it works:
//  - A dedicated relay thread calls WaitForVBlank on the Intel
//    output and releases one semaphore slot per VBlank (~6ms at 165Hz)
//  - The Present hook checks inter-frame spacing
//  - Normal frames (>5.5ms apart) pass through with zero added latency
//  - Burst frames (<5.5ms apart, caused by FG) wait for a VBlank slot
//  - Result: FG frames are spread across VBlank intervals, no tearing
//  - Also hooks CreateSwapChain/CreateSwapChainForHwnd to catch
//    proxy swapchains created by DLSS Enabler / OptiScaler
// ============================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <chrono>
#include <atomic>
#include <stdio.h>
#include <MinHook.h>

// ============================================================
//  Configuration
// ============================================================

// Minimum nanoseconds between frame presentations.
// At 165Hz: one VBlank = 6,060,606 ns (~6ms).
// 5,500,000 ns = 5.5ms threshold catches bursts without capping
// normal 160fps output (6.25ms per frame, well above threshold).
static const long long MIN_FRAME_NS = 5500000LL;

// How long to wait for a VBlank slot before giving up (ms).
// Set slightly above one VBlank period so we never get stuck.
static const DWORD VBLANK_TIMEOUT_MS = 10;

// ============================================================
//  Typedefs
// ============================================================
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* CreateSwapChain_t)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(__stdcall* CreateSwapChainForHwnd_t)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

// ============================================================
//  Globals
// ============================================================
static Present_t              oPresent              = nullptr;
static CreateSwapChain_t      oCreateSwapChain      = nullptr;
static CreateSwapChainForHwnd_t oCreateSwapChainForHwnd = nullptr;

static IDXGIOutput*  g_pIntelOutput      = nullptr;
static HANDLE        g_hVBlankSemaphore  = nullptr;  // Max 1 slot
static std::atomic<bool> g_running       { false };

// Timestamp of last Present call (nanoseconds since epoch, atomic)
static std::atomic<long long> g_lastPresentNs { 0 };

// ============================================================
//  Logging
// ============================================================
static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
    }
}

static void LogFmt(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

// ============================================================
//  VBlank Relay Thread
//  Runs independently on Intel output.
//  Releases one semaphore slot per hardware VBlank (~6ms at 165Hz).
//  Never called from the render thread - zero impact on frame timing.
// ============================================================
static DWORD WINAPI VBlankRelayThread(LPVOID) {
    Log("VBlank relay thread started.");

    while (g_running.load()) {
        if (g_pIntelOutput) {
            HRESULT hr = g_pIntelOutput->WaitForVBlank();
            if (SUCCEEDED(hr)) {
                // Release exactly one slot. Max=1 prevents accumulation:
                // if no frame is waiting, the slot is lost (correct - VRR
                // adjusts to the actual frame delivery rate).
                LONG prev = 0;
                ReleaseSemaphore(g_hVBlankSemaphore, 1, &prev);
                // If prev was already 1 (slot unconsumed), don't over-fill.
                // Windows semaphore handles this - release fails silently if at max.
            }
        } else {
            Sleep(1);
        }
    }
    return 0;
}

// ============================================================
//  Present Hook - The core sync logic
//
//  Called for every frame presentation, including FG-generated frames.
//  The key insight: we only stall frames that arrive too quickly
//  (burst frames). Normal frames pass through with zero latency.
// ============================================================
static HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    using namespace std::chrono;

    // Log first successful interception
    static std::atomic<bool> firstFrame { true };
    if (firstFrame.exchange(false)) {
        Log("SUCCESS: Present intercepted. VBlank sync active.");
        Beep(880, 150);
    }

    // Get current time in nanoseconds
    auto nowNs = duration_cast<nanoseconds>(
        high_resolution_clock::now().time_since_epoch()
    ).count();

    long long lastNs = g_lastPresentNs.load(std::memory_order_relaxed);

    if (lastNs > 0) {
        long long elapsedNs = nowNs - lastNs;

        if (elapsedNs < MIN_FRAME_NS) {
            // BURST FRAME DETECTED
            // This frame arrived too quickly after the previous one.
            // Wait for a VBlank slot from the relay thread.
            // This spreads burst frames across VBlank intervals.
            WaitForSingleObject(g_hVBlankSemaphore, VBLANK_TIMEOUT_MS);
        }
        // NORMAL FRAME: elapsed > 5.5ms, pass through immediately.
        // Zero added latency for the majority of frames.
    }

    // Update timestamp
    g_lastPresentNs.store(
        duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed
    );

    // Present with:
    // - SyncInterval = 0: let VRR handle sync, not the driver
    // - Flags: preserve original flags, strip DO_NOT_WAIT (we've already waited)
    UINT flags = Flags & ~DXGI_PRESENT_DO_NOT_WAIT;
    return oPresent(pSwapChain, 0, flags);
}

// ============================================================
//  Hook Present on a specific swapchain's vtable
//  Called whenever any swapchain is created (catches proxy swapchains)
// ============================================================
static void TryHookSwapChainPresent(IDXGISwapChain* pSC) {
    if (!pSC || oPresent) return; // Already hooked

    void** vtable = *reinterpret_cast<void***>(pSC);
    MH_STATUS s = MH_CreateHook(vtable[8], &hkPresent, reinterpret_cast<void**>(&oPresent));
    if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(vtable[8]);
        Log("Present hook installed via swapchain vtable.");
    }
}

// ============================================================
//  CreateSwapChain Hook
//  Catches swapchain creation by proxy DLLs (DLSS Enabler, OptiScaler)
// ============================================================
static HRESULT __stdcall hkCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    HRESULT hr = oCreateSwapChain(pFactory, pDevice, pDesc, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        Log("CreateSwapChain intercepted. Hooking Present...");
        TryHookSwapChainPresent(*ppSC);
    }
    return hr;
}

// ============================================================
//  CreateSwapChainForHwnd Hook (DX12 path)
// ============================================================
static HRESULT __stdcall hkCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSDesc,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    HRESULT hr = oCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFSDesc, pOutput, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        Log("CreateSwapChainForHwnd intercepted. Hooking Present...");
        TryHookSwapChainPresent(*ppSC);
    }
    return hr;
}

// ============================================================
//  Find Intel Output
//  Searches all adapters for VendorId 0x8086 (Intel).
//  Called once at startup, result stored in g_pIntelOutput.
// ============================================================
static IDXGIOutput* FindIntelOutput() {
    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory)))
        return nullptr;

    IDXGIOutput* result = nullptr;
    IDXGIAdapter* pAdapter = nullptr;

    for (UINT a = 0; pFactory->EnumAdapters(a, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC desc = {};
        pAdapter->GetDesc(&desc);

        if (desc.VendorId == 0x8086) { // Intel VendorId
            IDXGIOutput* pOut = nullptr;
            if (SUCCEEDED(pAdapter->EnumOutputs(0, &pOut))) {
                result = pOut;
                LogFmt("Intel adapter found at index %u. VBlank relay will use this output.", a);
            }
        }
        pAdapter->Release();
        if (result) break;
    }

    pFactory->Release();

    if (!result) {
        Log("WARNING: Intel output not found. VBlank sync will fall back to timeout mode.");
    }

    return result;
}

// ============================================================
//  Main Initialization Thread
// ============================================================
static DWORD WINAPI InitThread(LPVOID) {
    // Wait for dxgi.dll to be loaded by the game or proxy mod
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);

    // Wait for game + mods (DLSS Enabler, OptiScaler, etc.) to fully load.
    // They typically load in the first 2-3 seconds. We wait 3s to be safe.
    Sleep(3000);

    Log("AutoPacer v2 initializing...");

    // Create VBlank semaphore (max 1 slot - no accumulation)
    g_hVBlankSemaphore = CreateSemaphore(nullptr, 0, 1, nullptr);
    if (!g_hVBlankSemaphore) {
        Log("ERROR: Failed to create semaphore.");
        return 0;
    }

    // Find Intel output for VBlank relay
    g_pIntelOutput = FindIntelOutput();

    // Start VBlank relay thread
    g_running = true;
    CreateThread(nullptr, 0, VBlankRelayThread, nullptr, 0, nullptr);

    // Initialize MinHook
    MH_Initialize();

    // ---- Method 1: Hook IDXGIFactory::CreateSwapChain ----
    // This catches swapchains created by proxy DLLs (DLSS Enabler, OptiScaler)
    // which may not use the standard dxgi.dll vtable for their output swapchain.
    {
        IDXGIFactory* pFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
            void** fvt = *reinterpret_cast<void***>(pFactory);

            // CreateSwapChain is at vtable index 10 in IDXGIFactory
            if (MH_CreateHook(fvt[10], &hkCreateSwapChain,
                reinterpret_cast<void**>(&oCreateSwapChain)) == MH_OK) {
                MH_EnableHook(fvt[10]);
                Log("CreateSwapChain hook installed.");
            }

            // Also hook CreateSwapChainForHwnd (DX12 path, vtable 15 of IDXGIFactory2)
            IDXGIFactory2* pFactory2 = nullptr;
            if (SUCCEEDED(pFactory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&pFactory2))) {
                void** f2vt = *reinterpret_cast<void***>(pFactory2);
                if (MH_CreateHook(f2vt[15], &hkCreateSwapChainForHwnd,
                    reinterpret_cast<void**>(&oCreateSwapChainForHwnd)) == MH_OK) {
                    MH_EnableHook(f2vt[15]);
                    Log("CreateSwapChainForHwnd hook installed.");
                }
                pFactory2->Release();
            }
            pFactory->Release();
        }
    }

    // ---- Method 2: Hook Present directly via dummy DX11 device ----
    // This catches the underlying real DXGI swapchain that proxy mods
    // use internally for their output. Belt-and-suspenders approach.
    if (!oPresent) {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "APv2Dummy";
        RegisterClassExA(&wc);
        HWND hWnd = CreateWindowA("APv2Dummy", "", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width = 1;
        sd.BufferDesc.Height = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* pDev = nullptr;
        IDXGISwapChain* pSC = nullptr;
        ID3D11DeviceContext* pCtx = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &fl, 1, D3D11_SDK_VERSION, &sd,
            &pSC, &pDev, nullptr, &pCtx
        );

        if (SUCCEEDED(hr)) {
            void** vtable = *reinterpret_cast<void***>(pSC);
            MH_STATUS s = MH_CreateHook(vtable[8], &hkPresent,
                reinterpret_cast<void**>(&oPresent));
            if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
                MH_EnableHook(vtable[8]);
                Log("Direct Present hook installed via dummy device.");
            } else {
                LogFmt("Direct Present hook failed: MH status %d", (int)s);
            }
            pSC->Release(); pDev->Release(); pCtx->Release();
        } else {
            LogFmt("Dummy device creation failed: 0x%X", (unsigned)hr);
        }

        DestroyWindow(hWnd);
        UnregisterClassA("APv2Dummy", wc.hInstance);
    }

    // Initialize last present time to now
    g_lastPresentNs.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count()
    );

    Beep(1000, 200);
    Log("AutoPacer v2 ready. Waiting for first frame...");
    return 0;
}

// ============================================================
//  DLL Entry Point
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}
