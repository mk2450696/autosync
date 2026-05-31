#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <MinHook.h>

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

LARGE_INTEGER g_qpcFreq;
LARGE_INTEGER g_LastReturnTime = { 0 };
LARGE_INTEGER g_LastBatchStartTime = { 0 };
LARGE_INTEGER g_LastPresentTime = { 0 };

double g_TargetInterval = 6.33; // Default 158 FPS
int g_FramesInBatch = 0;
bool g_FirstFrame = true;
bool g_IsInitialized = false;

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_FirstFrame) {
        WriteLog("SUCCESS: Dynamic Batch Pacer Active. VRR is fully tracking.");
        Beep(750, 150);
        Beep(1000, 150);
        g_FirstFrame = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // 1. BATCH DETECTION (Isolate Real Frames from Fake Frames)
    // If the gap since we last returned is > 3.0ms, the game engine actually had to render.
    double gapMs = 0.0;
    if (g_LastReturnTime.QuadPart != 0) {
        gapMs = (now.QuadPart - g_LastReturnTime.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
    }

    if (gapMs > 3.0 || g_LastBatchStartTime.QuadPart == 0) {
        if (g_FramesInBatch > 0) {
            // Calculate exact True FPS of the last batch
            double batchDurationMs = (now.QuadPart - g_LastBatchStartTime.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
            double measuredInterval = batchDurationMs / g_FramesInBatch;

            // Safe VRR Clamps (158 FPS ceiling, 40 FPS floor)
            if (measuredInterval < 6.33) measuredInterval = 6.33;
            if (measuredInterval > 25.0) measuredInterval = 25.0;

            // Exponential Moving Average to make VRR buttery smooth
            g_TargetInterval = (g_TargetInterval * 0.85) + (measuredInterval * 0.15);
        }
        g_LastBatchStartTime = now;
        g_FramesInBatch = 0;
    }

    g_FramesInBatch++;

    // 2. THE DYNAMIC METRONOME (Zero Latency Spinlock)
    long long targetTicks = g_LastPresentTime.QuadPart + (long long)(g_TargetInterval * g_qpcFreq.QuadPart / 1000.0);

    if (now.QuadPart >= targetTicks) {
        // If we dropped a frame naturally, catch up instantly
        g_LastPresentTime = now;
    } else {
        // Pure hardware spinlock for nanosecond precision (NO Windows Sleep latency!)
        while (now.QuadPart < targetTicks) {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        }
        g_LastPresentTime.QuadPart = targetTicks; // Keep rhythm strict
    }

    // 3. PRESENT FRAME
    HRESULT res = oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);

    // Mark the exact microsecond we return to the mod
    QueryPerformanceCounter(&now);
    g_LastReturnTime = now;

    return res;
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); 
    
    WriteLog("AutoPacer v6 woke up. Initializing Timers...");
    QueryPerformanceFrequency(&g_qpcFreq);
    
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
            WriteLog("DXGI Hook planted successfully.");
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
        if (!g_IsInitialized) {
            g_IsInitialized = true;
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        }
    }
    return TRUE;
}