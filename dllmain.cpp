#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <MinHook.h>

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

LARGE_INTEGER g_qpcFreq;
LARGE_INTEGER g_lastTicks;
long long g_targetTicks = 0;
bool g_firstFrame = true;

// TARGET FPS CEILING (Safe zone for 165Hz VRR)
const double TARGET_FPS = 158.0;

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_firstFrame) {
        WriteLog("SUCCESS: Pure QPC Translation Layer Active. Guarding at 158 FPS.");
        Beep(750, 150);
        Beep(1000, 150);
        g_firstFrame = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    long long elapsedTicks = now.QuadPart - g_lastTicks.QuadPart;

    // THE MICROSECOND SPACER
    // If the frame arrived faster than 6.33ms (Frame Gen Burst), hold it precisely.
    if (elapsedTicks < g_targetTicks) {
        long long spinUntil = g_lastTicks.QuadPart + g_targetTicks;
        while (now.QuadPart < spinUntil) {
            YieldProcessor(); // Hardware-level CPU pause (Zero OS scheduling lag)
            QueryPerformanceCounter(&now);
        }
    }

    g_lastTicks = now;

    // Release the frame with forced VRR compatibility
    return oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); // Give proxy mods time to unpack
    
    WriteLog("Translation Layer woke up. Initializing High-Res Timers...");

    // Initialize High-Precision Hardware Timer
    QueryPerformanceFrequency(&g_qpcFreq);
    g_targetTicks = (g_qpcFreq.QuadPart * 10000000LL) / (long long)(TARGET_FPS * 10000000.0);
    QueryPerformanceCounter(&g_lastTicks);
    
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
            WriteLog("DXGI Hook planted successfully via dummy device.");
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