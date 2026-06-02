// AutoPacer v44 - The Dynamic Ring-Safe Container
//
// Explains the v38 144FPS success: At matching speeds, the Mod never lapped the Pacer.
// When speeds drifted, the Mod wrapped around the 6-buffer DXGI ring and overwrote 
// frames before they were presented, destroying the FG sequence and flashing the UI.
//
// FIX: 
// 1. A strict Queue Size limit of 4 safely backpressures the Mod, mathematically
//    preventing DXGI wrap-around overwrites and preserving the FG sequence.
// 2. A 16-frame Rolling Average completely absorbs the FG micro-bursts, finding the
//    true FPS and dynamically pacing the Intel display to un-bunch the PCIe traffic.

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
        fprintf(fp, "[AutoPacer v44] %s\n", msg);
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

// ── The Container State ───────────────────────────────────────────────────────
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
const int HISTORY_SIZE = 16; // 16 frames is enough to average out the FG bursts
static double g_History[HISTORY_SIZE];
static int g_HistoryIdx = 0;
static std::atomic<double> g_DynamicGapMs{ 16.666 }; // Default 60fps start

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Producer) ─────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    double now = GetTimeMs();
    
    // 1. Calculate True Framerate
    if (g_LastProduceTime > 0.0) {
        double gap = now - g_LastProduceTime;
        // Ignore load screens and heavy lag spikes in the math
        if (gap > 1.0 && gap < 50.0) {
            g_History[g_HistoryIdx] = gap;
            g_HistoryIdx = (g_HistoryIdx + 1) % HISTORY_SIZE;
            
            double sum = 0.0;
            for(int i = 0; i < HISTORY_SIZE; i++) sum += g_History[i];
            
            g_DynamicGapMs.store(sum / (double)HISTORY_SIZE);
        }
    }
    g_LastProduceTime = now;

    // 2. Put Frame in Container (The Ring-Buffer Lock)
    {
        std::unique_lock<std::mutex> lock(g_Mutex);
        
        // Mathematical Protection: The Mod uses 6 buffers.
        // We stop accepting frames at 4. This guarantees the Mod is paused BEFORE 
        // it can wrap around the ring and overwrite the frame we are currently holding.
        g_CV_Produce.wait(lock, [] { return g_Queue.size() < 4; });
        
        g_Queue.push({ pSC, SyncInterval, Flags });
    }
    
    g_CV_Consume.notify_one();
    
    // Return instantly so the Mod's Frame Gen sequence continues flawlessly.
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("Dynamic Ring-Safe Container Thread started.");
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
        
        // Notify the Mod that there is space in the queue
        g_CV_Produce.notify_one();

        // 2. Get the Dynamic Target and Clamp it for VRR
        double targetGapMs = g_DynamicGapMs.load();
        if (targetGapMs < 6.25) targetGapMs = 6.25; // MAX = 160 FPS (Safe limit for 165Hz VRR)
        if (targetGapMs > 33.3) targetGapMs = 33.3; // MIN = 30 FPS

        // 3. Perfect Hardware Delivery Timing
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + targetGapMs;
            double now = GetTimeMs();
            
            // Anti-Starvation: Don't rapidly speed up if the game paused
            if (now > targetTime + targetGapMs) {
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
            Logf("Dynamic Pacer -> Game Average FPS: %.1f | Delivering at: %.1f FPS | Gap: %.3f ms | Queue: %zu", 
                 1000.0 / g_DynamicGapMs.load(), 1000.0 / targetGapMs, targetGapMs, g_Queue.size());
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
        
        // Pre-fill rolling average to a safe 60 FPS
        for (int i = 0; i < HISTORY_SIZE; i++) g_History[i] = 16.666;

        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v44 The Dynamic Ring-Safe Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}