// AutoPacer v39 - The Dynamic Async Container (The Holy Grail)
//
// Perfectly decouples the Mod's bursty PCIe submission from the Intel hardware delivery.
// 1. Mod drops frames into a thread-safe queue and gets an instant S_OK (No blocking, no flashing).
// 2. The producer tracks the true, natural FPS of the game using a 32-frame rolling average.
// 3. The consumer background thread dynamically updates its delivery pace to exactly match 
//    the rolling average, ensuring the Intel driver receives perfectly un-bunched frames 
//    safely within the 165Hz VRR window.

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
#include <atomic>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v39] %s\n", msg);
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

// ── The Dynamic Container State ───────────────────────────────────────────────
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
static double g_LastProduceTime = 0.0;
static double g_LastReleaseTime = 0.0;

// Dynamic Pacing Variables
const int HISTORY_SIZE = 32;
static double g_DeltaHistory[HISTORY_SIZE];
static int g_HistoryIdx = 0;
static std::atomic<double> g_DynamicTargetMs{ 16.666 }; // Default 60fps start

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Producer) ─────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    double now = GetTimeMs();
    
    // 1. Calculate Dynamic Framerate
    if (g_LastProduceTime > 0.0) {
        double gap = now - g_LastProduceTime;
        // Ignore load screens and massive stutters in our average
        if (gap > 2.0 && gap < 100.0) {
            g_DeltaHistory[g_HistoryIdx] = gap;
            g_HistoryIdx = (g_HistoryIdx + 1) % HISTORY_SIZE;
            
            double sum = 0.0;
            for(int i = 0; i < HISTORY_SIZE; i++) sum += g_DeltaHistory[i];
            g_DynamicTargetMs.store(sum / (double)HISTORY_SIZE);
        }
    }
    g_LastProduceTime = now;

    // 2. Put Frame in the Container
    std::unique_lock<std::mutex> lock(g_Mutex);
    
    // Allow up to 4 frames in the queue. 
    // This gives the Mod massive breathing room to prevent the static-flashing crashes,
    // while ensuring we don't exceed the 6-buffer DXGI limit.
    g_CV_Produce.wait(lock, [] { return g_Queue.size() < 4; });
    
    g_Queue.push({ pSC, SyncInterval, Flags });
    g_CV_Consume.notify_one();
    
    // 3. Return instantly so the Mod never blocks
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("Dynamic Asynchronous Container Thread started.");
    int logCounter = 0;
    
    while (true) {
        PresentArgs args;
        
        // 1. Get Frame from Container
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
        }
        g_CV_Produce.notify_one();

        // 2. Get the Dynamic Target and Clamp it safely for VRR
        double target = g_DynamicTargetMs.load();
        if (target < 6.25) target = 6.25; // MAX = 160 FPS (Keeps it safely under 165Hz limit)
        if (target > 33.3) target = 33.3; // MIN = 30 FPS

        // 3. The Tollbooth (Pace the hardware delivery)
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + target;
            double now = GetTimeMs();
            
            // Anti-Starvation check: If the game paused (e.g. menus), reset the clock
            // so we don't try to rapidly "catch up" and spam the Intel driver.
            if (now > targetTime + target) {
                targetTime = now;
            }
            
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); // Ultra-precise micro-spin
            }
        }
        
        // 4. Deliver to Intel Driver
        g_LastReleaseTime = GetTimeMs();
        oPresent(args.pSC, args.SyncInterval, args.Flags);

        // 5. Periodic Logging (Every 600 frames = ~5 seconds)
        logCounter++;
        if (logCounter % 600 == 0) {
            Logf("Dynamic Pacer Status -> Target Gap: %.3f ms (%.1f FPS) | Queue Size: %zu", 
                 target, 1000.0 / target, g_Queue.size());
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
        Log("Dynamic Container Hooks installed successfully.");
        Beep(1000, 150);
    }
    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq);
        
        // Pre-fill history to 60fps to prevent math errors on boot
        for (int i = 0; i < HISTORY_SIZE; i++) g_DeltaHistory[i] = 16.666;

        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v39 The Dynamic Async Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}