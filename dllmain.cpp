#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <deque>
#include <numeric>
#include <stdio.h>
#include <MinHook.h>

#pragma comment(lib, "winmm.lib") // Required for accurate sleep timers

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

LARGE_INTEGER g_Freq = { 0 };
LARGE_INTEGER g_LastArrival = { 0 };
LARGE_INTEGER g_LastPresent = { 0 };
std::deque<double> g_ArrivalDeltas;
bool g_FirstFrame = true;

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_FirstFrame) {
        WriteLog("SUCCESS: Dynamic VSync Pacer Active.");
        Beep(750, 150);
        Beep(1000, 150);
        g_FirstFrame = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // 1. MEASURE TRUE ENGINE FPS (Arrival Times)
    if (g_LastArrival.QuadPart != 0) {
        double deltaMs = (double)(now.QuadPart - g_LastArrival.QuadPart) * 1000.0 / g_Freq.QuadPart;
        // Ignore loading screens or massive stutters > 100ms
        if (deltaMs > 0.0 && deltaMs < 100.0) {
            g_ArrivalDeltas.push_back(deltaMs);
            if (g_ArrivalDeltas.size() > 60) { // Keep rolling average of last 60 frames (~0.5 seconds)
                g_ArrivalDeltas.pop_front();
            }
        }
    }
    g_LastArrival = now;

    // 2. CALCULATE DYNAMIC TARGET (Your "Current FPS" VSync)
    double targetIntervalMs = 6.33; // Default 158 FPS safety ceiling
    if (g_ArrivalDeltas.size() >= 10) {
        double sum = std::accumulate(g_ArrivalDeltas.begin(), g_ArrivalDeltas.end(), 0.0);
        targetIntervalMs = sum / g_ArrivalDeltas.size();
    }

    // Clamp to Safe VRR Range (Max 158 FPS to prevent ceiling tears, Min 40 FPS)
    if (targetIntervalMs < 6.33) targetIntervalMs = 6.33; 
    if (targetIntervalMs > 25.0) targetIntervalMs = 25.0;

    // 3. THE DYNAMIC PACER (Space frames evenly across the PCIe bus)
    if (g_LastPresent.QuadPart != 0) {
        long long targetTicks = g_LastPresent.QuadPart + (long long)(targetIntervalMs * g_Freq.QuadPart / 1000.0);

        QueryPerformanceCounter(&now);
        while (now.QuadPart < targetTicks) {
            double msRemaining = (double)(targetTicks - now.QuadPart) * 1000.0 / g_Freq.QuadPart;
            if (msRemaining > 2.0) {
                Sleep(1); // Yield CPU to game engine (Fixes Latency/FPS Drops)
            } else {
                YieldProcessor(); // Spin for the final 1ms for flawless accuracy
            }
            QueryPerformanceCounter(&now);
        }
        g_LastPresent.QuadPart = targetTicks; // Stay strictly on the rhythmic grid
    } else {
        g_LastPresent = now;
    }

    // Anti-Lag Catchup: If a huge stutter naturally delays the frame, reset the grid
    QueryPerformanceCounter(&now);
    if (now.QuadPart > g_LastPresent.QuadPart + (long long)(2.0 * g_Freq.QuadPart / 1000.0)) {
        g_LastPresent = now;
    }

    // 4. PRESENT (With VRR explicitly forced ON)
    return oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); 
    
    WriteLog("Translation Layer woke up. Initializing Timers...");

    // Setup High-Res Timers for Hybrid Sleep
    timeBeginPeriod(1); 
    QueryPerformanceFrequency(&g_Freq);
    
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
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}