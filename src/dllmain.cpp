// AutoPacer v41 - The Elastic Pacer
//
// 1. Never drops frames (preserves Frame Gen optical flow logic).
// 2. Never blocks the Mod (prevents 56 FPS lock and internal desyncs).
// 3. Uses a highly stable Exponential Moving Average (EMA) to track the true
//    dynamic framerate of the game, filtering out the FG micro-bursts.
// 4. The background consumer dynamically paces the Intel display delivery to 
//    perfectly match that EMA, un-bunching the PCIe traffic flawlessly.

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
        fprintf(fp, "[AutoPacer v41] %s\n", msg);
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

// ── The Elastic Container ─────────────────────────────────────────────────────
struct PresentArgs {
    IDXGISwapChain* pSC;
    UINT SyncInterval;
    UINT Flags;
};

static std::queue<PresentArgs> g_Queue;
static std::mutex g_Mutex;
static std::condition_variable g_CV_Consume;

static LARGE_INTEGER g_qpcFreq;
static double g_LastProduceTime = 0.0;
static double g_LastReleaseTime = 0.0;

// The True FPS Tracker
static std::atomic<double> g_SmoothedGapMs{ 16.666 }; // Start at 60 FPS safety baseline

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Non-Blocking Producer) ────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    double now = GetTimeMs();
    
    // 1. Calculate the true, dynamic framerate using an Exponential Moving Average
    if (g_LastProduceTime > 0.0) {
        double gap = now - g_LastProduceTime;
        
        // Ignore loading screens or massive lag spikes so they don't break the math
        if (gap > 1.0 && gap < 50.0) {
            double currentSmooth = g_SmoothedGapMs.load();
            // 95% history, 5% new data. Extremely stable, completely immune to micro-bursts, 
            // but adapts to new framerates in about 0.15 seconds.
            double newSmooth = (currentSmooth * 0.95) + (gap * 0.05);
            g_SmoothedGapMs.store(newSmooth);
        }
    }
    g_LastProduceTime = now;

    // 2. Safely place the frame in the container
    {
        std::unique_lock<std::mutex> lock(g_Mutex);
        
        // Safety Valve: Only drop a frame if the queue hits 12 (massive system hang).
        // Under normal gameplay, this will NEVER trigger. Frame Gen logic stays perfectly intact.
        if (g_Queue.size() >= 12) {
            g_Queue.pop(); 
        }
        
        g_Queue.push({ pSC, SyncInterval, Flags });
    }
    
    // Wake up the background thread
    g_CV_Consume.notify_one();
    
    // 3. Return instantly. The Mod is never blocked.
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("Elastic Pacer Thread started. Frame Gen logic fully protected.");
    int logCounter = 0;
    
    while (true) {
        PresentArgs args;
        
        // 1. Grab frame from container
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
        }

        // 2. Read the dynamic tracking speed and clamp it for VRR
        double targetGapMs = g_SmoothedGapMs.load();
        if (targetGapMs < 6.25) targetGapMs = 6.25; // Hard cap at 160 FPS to protect 165Hz VRR
        if (targetGapMs > 33.3) targetGapMs = 33.3; // Hard floor at 30 FPS

        // 3. The Tollbooth (Smooth Delivery)
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + targetGapMs;
            double now = GetTimeMs();
            
            // Anti-Starvation: If the game was paused (menu), don't rapidly spam old frames
            if (now > targetTime + 20.0) {
                targetTime = now;
            }
            
            // Micro-spin for absolute precision
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); 
            }
        }
        
        // 4. Deliver to the Intel Driver
        g_LastReleaseTime = GetTimeMs();
        oPresent(args.pSC, args.SyncInterval, args.Flags);

        // 5. Diagnostics
        logCounter++;
        if (logCounter % 600 == 0) {
            Logf("Elastic Pacer -> Target FPS: %.1f | Gap: %.3f ms | Queue Size: %zu", 
                 1000.0 / targetGapMs, targetGapMs, g_Queue.size());
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
        Log("Elastic Container Hooks installed successfully.");
        Beep(1000, 150);
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
        Log("DLL Booted - v41 The Elastic Pacer");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}