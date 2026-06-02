// AutoPacer v45 - The True Dynamic Container
//
// 1. Solves the "Death Spiral" by measuring the (ArrivalTime - LastReturnTime). 
//    This mathematically subtracts our Container's wait time from the FPS calculation, 
//    revealing the Mod's true, unhindered rendering speed.
// 2. Queue limited to 4 to physically prevent DXGI buffer-overwrites (flashing UI/duplicates).
// 3. Background thread uses a Micro-Nudge to maintain a perfect 1-frame buffer,
//    ensuring perfect input latency and zero Intel PCIe dropped frames.

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
        fprintf(fp, "[AutoPacer v45] %s\n", msg);
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
static double g_LastReturnTime = 0.0;
static double g_LastReleaseTime = 0.0;

// True FPS Tracker
const int HISTORY_SIZE = 16;
static double g_History[HISTORY_SIZE];
static int g_HistoryIdx = 0;
static std::atomic<double> g_BaseGapMs{ 16.666 }; // Default 60 FPS

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Producer) ─────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    double arrivalTime = GetTimeMs();
    
    // 1. Calculate True Framerate (Unblocked Time)
    // By measuring from when we LAST RETURNED to NOW, we completely remove 
    // the Container's wait time from the equation. The Death Spiral is dead.
    if (g_LastReturnTime > 0.0) {
        double unblockedGap = arrivalTime - g_LastReturnTime;
        
        // Ignore load screens so they don't break the math
        if (unblockedGap > 1.0 && unblockedGap < 50.0) {
            g_History[g_HistoryIdx] = unblockedGap;
            g_HistoryIdx = (g_HistoryIdx + 1) % HISTORY_SIZE;
            
            double sum = 0.0;
            for(int i = 0; i < HISTORY_SIZE; i++) sum += g_History[i];
            
            g_BaseGapMs.store(sum / (double)HISTORY_SIZE);
        }
    }

    // 2. Container Lock
    {
        std::unique_lock<std::mutex> lock(g_Mutex);
        
        // Limit to 4. Prevents the Mod from wrapping the 6-buffer ring.
        // No flashing UI, no duplicate frames.
        g_CV_Produce.wait(lock, [] { return g_Queue.size() < 4; });
        
        g_Queue.push({ pSC, SyncInterval, Flags });
    }
    
    g_CV_Consume.notify_one();
    
    // 3. Record Return Time & Exit
    g_LastReturnTime = GetTimeMs(); 
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("True Dynamic Container Thread started. Death Spiral immunity active.");
    int logCounter = 0;
    
    while (true) {
        PresentArgs args;
        size_t qSize = 0;
        
        // 1. Get Frame
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
            qSize = g_Queue.size();
        }
        
        g_CV_Produce.notify_one();

        // 2. Calculate Delivery Speed
        double finalGap = g_BaseGapMs.load();
        
        // The Micro-Nudge: Maintain a perfect 1-frame queue depth for flawless latency
        if (qSize >= 2) {
            finalGap -= 0.15; // Queue is building up, deliver slightly faster
        } else if (qSize == 0) {
            finalGap += 0.15; // Queue is starving, deliver slightly slower
        }

        // 3. VRR Clamps (Keeps monitor inside 165Hz VRR range)
        if (finalGap < 6.25) finalGap = 6.25; // MAX 160 FPS
        if (finalGap > 33.3) finalGap = 33.3; // MIN 30 FPS

        // 4. Smooth Delivery
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + finalGap;
            double now = GetTimeMs();
            
            // Anti-Starvation
            if (now > targetTime + 20.0) {
                targetTime = now;
            }
            
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); // Absolute precision
            }
        }
        
        // 5. Deliver to Intel Driver
        g_LastReleaseTime = GetTimeMs();
        oPresent(args.pSC, args.SyncInterval, args.Flags);

        // 6. Diagnostics
        logCounter++;
        if (logCounter % 600 == 0) {
            Logf("Dynamic Pacer -> True Game FPS: %.1f | Delivering at: %.1f FPS | Queue: %zu", 
                 1000.0 / g_BaseGapMs.load(), 1000.0 / finalGap, qSize);
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
        Log("Container Hooks installed successfully.");
        Beep(1000, 150);
    }
    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq);
        
        for (int i = 0; i < HISTORY_SIZE; i++) g_History[i] = 16.666;

        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v45 The True Dynamic Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}