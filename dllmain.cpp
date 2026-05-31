#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <stdio.h>
#include <MinHook.h>

#pragma comment(lib, "winmm.lib") // Required for precise timers

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

auto lastPresentTime = std::chrono::high_resolution_clock::now();
bool hookSuccessful = false;
bool isInitialized = false; // Prevents double-injection lag

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!hookSuccessful) {
        WriteLog("SUCCESS: Lite Translation Layer Active. Pacing at max 158 FPS.");
        Beep(750, 200); 
        hookSuccessful = true;
    }

    // THE TRAFFIC LIGHT: Enforce a strict minimum gap to prevent 165Hz PCIe bursts.
    // 1000ms / 158 FPS = 6.33 milliseconds minimum gap.
    const double minGapMs = 6.33; 

    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = now - lastPresentTime;

    if (elapsed.count() < minGapMs) {
        // Hybrid Sleep: Saves CPU performance (Fixes the 145->105 FPS drop)
        while (elapsed.count() < minGapMs) {
            double remaining = minGapMs - elapsed.count();
            if (remaining > 2.0) {
                Sleep(1); // Yield thread efficiently to game engine
            } else {
                YieldProcessor(); // Ultra-light spin for the final 1ms precision
            }
            now = std::chrono::high_resolution_clock::now();
            elapsed = now - lastPresentTime;
        }
    }

    lastPresentTime = std::chrono::high_resolution_clock::now();
    
    // Force VRR compatibility: SyncInterval 0, and ensure AllowTearing flag is present
    return oPresent(pSwapChain, 0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); 
    
    WriteLog("Translation Layer woke up. Attempting to hook...");
    
    // Increase Windows timer resolution for precision hybrid sleeping
    timeBeginPeriod(1); 
    
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
            Beep(1000, 200); 
        }

        pSwapChain->Release();
        pDevice->Release();
        pContext->Release();
    }
    DestroyWindow(hWnd);
    UnregisterClassA("DummyClass", wc.hInstance);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        if (!isInitialized) {
            isInitialized = true;
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        }
    }
    return TRUE;
}