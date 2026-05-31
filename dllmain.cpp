#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <vector>
#include <numeric>
#include <stdio.h>
#include <MinHook.h>

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

std::vector<double> frameTimes;
auto lastPresentTime = std::chrono::high_resolution_clock::now();
bool hookSuccessful = false;

// The Intel Monitor Pointer
IDXGIOutput* pActiveOutput = nullptr;

void WriteLog(const char* message) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!hookSuccessful) {
        WriteLog("SUCCESS: First Frame Intercepted! AutoPacer is actively pacing frames.");
        Beep(750, 300); 
        hookSuccessful = true;
    }

    // 1. DYNAMIC PACER (The Bouncer)
    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = now - lastPresentTime;

    if (elapsed.count() > 0 && elapsed.count() < 100.0) {
        if (frameTimes.size() >= 10) {
            frameTimes.erase(frameTimes.begin());
        }
        frameTimes.push_back(elapsed.count());
    }

    if (!frameTimes.empty()) {
        double avgFrameTime = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) / frameTimes.size();
        double targetTime = avgFrameTime - 0.2; 

        if (elapsed.count() < targetTime) {
            while (true) {
                auto spinNow = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> spinElapsed = spinNow - lastPresentTime;
                if (spinElapsed.count() >= targetTime) break;
            }
        }
    }

    // 2. CROSS-ADAPTER VBLANK SYNC (The Tear Killer)
    // If we don't have the Intel monitor yet, find it.
    if (!pActiveOutput) {
        IDXGIFactory* pFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
            IDXGIAdapter* pAdapter = nullptr;
            // Scan all GPUs for an active monitor output (This will find the Intel iGPU)
            for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                if (pAdapter->EnumOutputs(0, &pActiveOutput) != DXGI_ERROR_NOT_FOUND) {
                    WriteLog("SUCCESS: Located Active Monitor. VBlank Sync Engaged.");
                    pAdapter->Release();
                    break;
                }
                pAdapter->Release();
            }
            pFactory->Release();
        }
    }

    // If we found the Intel monitor, force NVIDIA to wait for its invisible refresh cycle
    if (pActiveOutput) {
        pActiveOutput->WaitForVBlank();
    }

    lastPresentTime = std::chrono::high_resolution_clock::now();
    
    // Release the frame instantly (SyncInterval=0) because we manually handled the sync
    return oPresent(pSwapChain, 0, Flags);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    while (GetModuleHandleA("dxgi.dll") == NULL) {
        Sleep(100);
    }
    Sleep(2000); 
    
    WriteLog("AutoPacer woke up. DXGI found. Attempting to hook...");
    
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
            Beep(1000, 300); 
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
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}