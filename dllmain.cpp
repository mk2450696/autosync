#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <vector>
#include <numeric>
#include <MinHook.h>

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

std::vector<double> frameTimes;
auto lastPresentTime = std::chrono::high_resolution_clock::now();

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
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
        double targetTime = avgFrameTime - 0.2; // 0.2ms breathing buffer

        // THE BOUNCER: Stop Micro-Bursts
        if (elapsed.count() < targetTime) {
            while (true) {
                auto spinNow = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> spinElapsed = spinNow - lastPresentTime;
                if (spinElapsed.count() >= targetTime) break;
            }
        }
    }

    lastPresentTime = std::chrono::high_resolution_clock::now();
    
    // Force SyncInterval = 0 to keep Intel VRR active
    return oPresent(pSwapChain, 0, Flags);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
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
        MH_CreateHook(pVTable[8], reinterpret_cast<LPVOID>(&hkPresent), reinterpret_cast<LPVOID*>(&oPresent));
        MH_EnableHook(MH_ALL_HOOKS);

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