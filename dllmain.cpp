#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <deque>
#include <numeric>
#include <stdio.h>
#include <MinHook.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#endif

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

LARGE_INTEGER g_qpcFreq;
LARGE_INTEGER g_lastPresentTime = { 0 };
std::deque<double> g_frameDeltas;
HANDLE g_hTimer = nullptr;

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
        WriteLog("SUCCESS: Adaptive Smoother (v7) Active. 0% CPU Burn.");
        Beep(750, 150);
        Beep(1000, 150);
        g_FirstFrame = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_lastPresentTime.QuadPart != 0) {
        double deltaMs = (double)(now.QuadPart - g_lastPresentTime.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
        
        // Ignore massive spikes (loading screens, alt-tabs) so they don't corrupt the math
        if (deltaMs > 0.0 && deltaMs < 100.0) {
            g_frameDeltas.push_back(deltaMs);
            if (g_frameDeltas.size() > 60) {
                g_frameDeltas.pop_front();
            }
        }

        if (g_frameDeltas.size() >= 10) {
            double sum = std::accumulate(g_frameDeltas.begin(), g_frameDeltas.end(), 0.0);
            double avgDelta = sum / g_frameDeltas.size();

            // THE 25% HEADROOM RULE
            // Allows FPS to climb freely, but utterly crushes 1000fps Frame Gen micro-bursts.
            double targetGapMs = avgDelta * 0.75;

            // Safe VRR Clamps (Max 158 FPS ceiling, Min 30 FPS floor)
            if (targetGapMs < 6.33) targetGapMs = 6.33; 
            if (targetGapMs > 33.3) targetGapMs = 33.3;

            double timeSinceLastPresent = (double)(now.QuadPart - g_lastPresentTime.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;

            // THE 0% CPU SPACER
            if (timeSinceLastPresent < targetGapMs) {
                double waitMs = targetGapMs - timeSinceLastPresent;
                
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = - (long long)(waitMs * 10000.0); // 100-ns intervals
                
                SetWaitableTimer(g_hTimer, &dueTime, 0, NULL, NULL, 0);
                WaitForSingleObject(g_hTimer, INFINITE);
                
                // Update 'now' after waking up from the hardware sleep
                QueryPerformanceCounter(&now);
            }
        }
    }

    g_lastPresentTime = now;
    
    // Pass the frame with VRR Tearing flag explicitly enforced
    return oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); 
    
    WriteLog("Translation Layer v7 woke up. Initializing High-Res Timers...");

    QueryPerformanceFrequency(&g_qpcFreq);
    
    // Create the High-Resolution hardware timer (Windows 10/11)
    g_hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_hTimer) g_hTimer = CreateWaitableTimer(NULL, FALSE, NULL); // Failsafe
    
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