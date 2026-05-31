#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <atomic>
#include <stdio.h>
#include <MinHook.h>

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

// 5.5ms threshold (Perfect for 165Hz which is 6.06ms). 
// Any frame arriving faster than this is a "Burst" and will be VBlank-synced.
static const long long MIN_FRAME_NS = 5500000LL; 

HANDLE g_hVBlankEvent = nullptr;
std::atomic<IDXGIOutput*> g_pActiveOutput(nullptr);
std::atomic<bool> g_running{ true };
std::atomic<long long> g_lastPresentNs{ 0 };

bool firstFrame = true;

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

// ---------------------------------------------------------
// VBLANK RELAY THREAD (Runs independently, zero FPS impact)
// ---------------------------------------------------------
DWORD WINAPI VBlankThread(LPVOID) {
    WriteLog("VBlank Relay Thread active. Waiting for output pointer...");
    
    while (g_running.load()) {
        IDXGIOutput* pOutput = g_pActiveOutput.load();
        if (pOutput) {
            // Wait for the exact hardware pulse of the monitor
            if (SUCCEEDED(pOutput->WaitForVBlank())) {
                SetEvent(g_hVBlankEvent); // Signal that a VBlank just happened
            } else {
                Sleep(1); // Failsafe if monitor disconnects
            }
        } else {
            Sleep(10); // Sleep until the Present hook finds the monitor
        }
    }
    return 0;
}

// ---------------------------------------------------------
// PRESENT HOOK (The Bouncer)
// ---------------------------------------------------------
HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    using namespace std::chrono;

    if (firstFrame) {
        WriteLog("SUCCESS: Present intercepted. Guarding pipeline.");
        Beep(750, 200);
        firstFrame = false;
    }

    // 1. DYNAMIC MONITOR DETECTION
    // If we haven't found the monitor yet, ask the swapchain directly.
    // This perfectly bypasses the Intel/NVIDIA hybrid hiding issue.
    if (!g_pActiveOutput.load() && pSwapChain) {
        IDXGIOutput* pOut = nullptr;
        if (SUCCEEDED(pSwapChain->GetContainingOutput(&pOut))) {
            g_pActiveOutput.store(pOut);
            WriteLog("SUCCESS: Active monitor identified. VBlank sync armed.");
            Beep(1000, 200);
        }
    }

    // 2. THE BURST CATCHER
    auto nowNs = duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count();
    long long lastNs = g_lastPresentNs.load();

    if (lastNs > 0) {
        long long elapsedNs = nowNs - lastNs;

        // If the frame arrives faster than 5.5ms, it's a Frame Gen Micro-Burst.
        if (elapsedNs < MIN_FRAME_NS) {
            // Clear any stale VBlank signals
            ResetEvent(g_hVBlankEvent);
            // Force the thread to sleep until the relay thread detects the VERY NEXT hardware VBlank
            WaitForSingleObject(g_hVBlankEvent, 8); // 8ms max timeout so it never freezes
        }
    }

    g_lastPresentNs.store(duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count());

    // 3. RELEASE FRAME
    // Force SyncInterval = 0 to allow VRR, and append ALLOW_TEARING flag just in case
    return oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

// ---------------------------------------------------------
// INITIALIZATION THREAD (Crash-Free Dummy Device Method)
// ---------------------------------------------------------
DWORD WINAPI InitThread(LPVOID) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(3000); // Let proxy mods finish loading completely
    
    WriteLog("AutoPacer v3 initialized. Hooking DXGI...");
    
    g_hVBlankEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // Auto-reset event

    CreateThread(nullptr, 0, VBlankThread, nullptr, 0, nullptr);

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0L, 0L, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "DummyClass", NULL };
    RegisterClassExA(&wc);
    HWND hWnd = CreateWindowA("DummyClass", "", WS_OVERLAPPEDWINDOW, 100, 100, 100, 100, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* pDevice = nullptr;
    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &featureLevel, 1, D3D11_SDK_VERSION, &sd, &pSwapChain, &pDevice, NULL, &pContext))) {
        void** pVTable = *reinterpret_cast<void***>(pSwapChain);
        
        MH_Initialize();
        if (MH_CreateHook(pVTable[8], reinterpret_cast<LPVOID>(&hkPresent), reinterpret_cast<LPVOID*>(&oPresent)) == MH_OK) {
            MH_EnableHook(MH_ALL_HOOKS);
            WriteLog("DXGI Present hooked successfully via dummy device.");
        }

        pSwapChain->Release();
        pDevice->Release();
        pContext->Release();
    }
    DestroyWindow(hWnd);
    UnregisterClassA("DummyClass", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}