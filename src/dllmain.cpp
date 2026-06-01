// AutoPacer v43 - The Smart Water-Valve Container
//
// 1. Prevents "Buffer Overwrites" (UI flashing/duplicates) by strictly limiting 
//    the container size to (BufferCount - 2). If full, it safely blocks the Mod.
// 2. Uses a dynamic "Water Valve" algorithm. Instead of calculating CPU math, 
//    the delivery thread speeds up or slows down based entirely on how full 
//    the container is, effortlessly locking onto the Mod's true framerate.
// 3. Clamps delivery speed to 160 FPS max, keeping the Intel display comfortably 
//    inside the 165Hz VRR window.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <queue>
#include <mutex>
#include <condition_variable>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v43] %s\n", msg);
        fclose(fp);
    }
}
static void Logf(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── VTable helpers ────────────────────────────────────────────────────────────
static bool WritePtr(void** addr, void* newVal, void** oldVal) {
    DWORD old;
    if (!VirtualProtect(addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (oldVal) *oldVal = *addr;
    *addr = newVal;
    VirtualProtect(addr, sizeof(void*), old, &old);
    return true;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent = nullptr;
static const int SLOT_Present = 8;

// ── The Smart Container ───────────────────────────────────────────────────────
struct PresentArgs {
    IDXGISwapChain* pSC;
    UINT SyncInterval;
    UINT Flags;
};

static std::queue<PresentArgs> g_Queue;
static std::mutex g_Mutex;
static std::condition_variable g_CV_Produce;
static std::condition_variable g_CV_Consume;

static LARGE_INTEGER g_qpcFreq;
static double g_LastReleaseTime = 0.0;
static double g_TargetGapMs = 6.94; // Start at ~144 FPS pacing

static UINT g_SwapchainBuffers = 6; // Default safe assumption
static bool g_FirstFrame = true;

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Mod's Thread) ─────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        g_FirstFrame = false;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pSC->GetDesc(&desc))) {
            g_SwapchainBuffers = desc.BufferCount;
            Logf("First Frame: Detected %u Swapchain Buffers.", g_SwapchainBuffers);
        }
        Beep(1000, 150);
    }

    // Maximum safe container capacity to prevent Buffer Overwrites
    size_t maxSafeQueueSize = (g_SwapchainBuffers > 2) ? (g_SwapchainBuffers - 2) : 1;

    {
        std::unique_lock<std::mutex> lock(g_Mutex);
        
        // If the container reaches the max safe size, pause the Mod briefly.
        // This provides perfect backpressure without breaking Frame Gen.
        g_CV_Produce.wait(lock, [&] { return g_Queue.size() < maxSafeQueueSize; });
        
        g_Queue.push({ pSC, SyncInterval, Flags });
    }
    
    // Notify the background delivery thread
    g_CV_Consume.notify_one();
    
    return S_OK; 
}

// ── Background Pacer Thread (The Intel Delivery) ──────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("Smart Water-Valve Container Thread started.");
    int logCounter = 0;
    
    while (true) {
        PresentArgs args;
        size_t currentQueueSize = 0;
        
        // 1. Grab a frame from the container
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
            currentQueueSize = g_Queue.size();
        }
        
        // Free up space for the Mod
        g_CV_Produce.notify_one();

        // 2. The "Water Valve" Pacing Algorithm
        // Target Queue Size = 1.
        if (currentQueueSize > 1) {
            g_TargetGapMs -= 0.1; // Queue is filling up -> Speed up delivery
        } else if (currentQueueSize == 0) {
            g_TargetGapMs += 0.1; // Queue is empty -> Slow down delivery
        }

        // 3. Strict VRR Clamps
        if (g_TargetGapMs < 6.25) g_TargetGapMs = 6.25; // MAX 160 FPS (Keeps it safely under 165Hz limit)
        if (g_TargetGapMs > 33.3) g_TargetGapMs = 33.3; // MIN 30 FPS

        // 4. Smooth Delivery (The Tollbooth)
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + g_TargetGapMs;
            double now = GetTimeMs();
            
            // Anti-Starvation check for loading screens/menus
            if (now > targetTime + 20.0) targetTime = now;
            
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); // Absolute microsecond precision
            }
        }
        
        // 5. Present to the Intel Hardware
        g_LastReleaseTime = GetTimeMs();
        oPresent(args.pSC, args.SyncInterval, args.Flags);

        // 6. Diagnostics
        logCounter++;
        if (logCounter % 600 == 0) {
            Logf("Valve Status -> Speed: %.1f FPS (Gap: %.3f ms) | Queue Level: %zu", 
                 1000.0 / g_TargetGapMs, g_TargetGapMs, currentQueueSize);
        }
    }
    return 0;
}

// ── Init Thread ───────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) { if (GetModuleHandleA("dxgi.dll")) break; Sleep(50); }
    HWND gameWnd = nullptr;
    for (int i = 0; i < 600; ++i) {
        Sleep(50);
        HWND fg = GetForegroundWindow();
        if (fg) {
            char title[256] = {}; GetWindowTextA(fg, title, sizeof(title));
            RECT r = {}; GetClientRect(fg, &r);
            if ((r.right - r.left) > 400 && title[0] != '\0') { gameWnd = fg; break; }
        }
    }
    Sleep(500);

    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyWindow", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("DummyWindow", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pDummySC = nullptr; ID3D11Device* pDummyDev = nullptr; D3D_FEATURE_LEVEL featureLevel;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySC, &pDummyDev, &featureLevel, nullptr))) {
        void** vtable = *(void***)pDummySC;
        WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent);
        pDummySC->Release(); pDummyDev->Release();
        Log("Smart Container Hooks installed successfully.");
    }
    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq);
        
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v43 The Smart Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}