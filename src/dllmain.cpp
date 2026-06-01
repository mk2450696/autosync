// AutoPacer v26 - The Answer Reader (Queue Saturation Telemetry)
//
// Injects the DXGI_PRESENT_TEST (0x1) and records the exact HRESULT (the answer) 
// the Intel driver gives us, along with the VBlank monitor timings. 
// This will prove if the Intel queue is returning DXGI_ERROR_WAS_STILL_DRAWING 
// (queue full) right before the mod forces the frame and causes a stutter.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>

static char g_csvPath[MAX_PATH] = "AutoPacer_AnswerStats.csv";
static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v26] %s\n", msg);
        fclose(fp);
    }
}

// ── Telemetry State ───────────────────────────────────────────────────────────
struct FrameRecord {
    int frameNum;
    void* swapchainPtr;
    double cpuGapMs;
    HRESULT testHr;
    double testDurationMs;
    HRESULT renderHr;
    double renderDurationMs;
    double dispGapMs;
};

const int MAX_FRAMES = 600;
static FrameRecord g_Records[MAX_FRAMES];
static int g_FrameCount = 0;
static bool g_TelemetryDone = false;

static bool g_FirstFrame = true;
static LARGE_INTEGER g_qpcFreq;
static double g_LastCpuTime = 0.0;
static double g_LastDispTime = 0.0;

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
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

// ── Hooked Present (The Answer Reader) ────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        QueryPerformanceFrequency(&g_qpcFreq);
        g_LastCpuTime = GetTimeMs();
        g_FirstFrame = false;
        Log("Telemetry Started. Tracking Test Results and Hardware Queue.");
        Beep(1000, 100);
    }

    if (!g_TelemetryDone) {
        double now = GetTimeMs();
        double cpuGap = now - g_LastCpuTime;
        g_LastCpuTime = now;

        // 1. Inject the Hardware Probe (0x1) and record the driver's answer
        double testStart = GetTimeMs();
        HRESULT testHr = oPresent(pSC, 0, DXGI_PRESENT_TEST);
        double testDuration = GetTimeMs() - testStart;

        // 2. Read the Monitor's actual VBlank hardware timings
        DXGI_FRAME_STATISTICS stats = {};
        double dispGap = 0.0;
        if (SUCCEEDED(pSC->GetFrameStatistics(&stats))) {
            double dispTimeMs = (double)(stats.SyncQPCTime.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
            if (g_LastDispTime > 0.0 && stats.SyncQPCTime.QuadPart > 0) dispGap = dispTimeMs - g_LastDispTime;
            if (stats.SyncQPCTime.QuadPart > 0) g_LastDispTime = dispTimeMs;
        }

        // 3. Do the actual render request that the Mod asked for
        double renderStart = GetTimeMs();
        HRESULT renderHr = oPresent(pSC, SyncInterval, Flags);
        double renderDuration = GetTimeMs() - renderStart;

        // Save to memory
        if (g_FrameCount < MAX_FRAMES) {
            g_Records[g_FrameCount] = { g_FrameCount, pSC, cpuGap, testHr, testDuration, renderHr, renderDuration, dispGap };
            g_FrameCount++;
        } 
        else {
            g_TelemetryDone = true;
            FILE* fp;
            if (fopen_s(&fp, g_csvPath, "w") == 0) {
                fprintf(fp, "Frame,Swapchain,CpuGapMs,Test_HRESULT,Test_DurationMs,Render_HRESULT,Render_DurationMs,DispVBlankGapMs\n");
                for (int i = 0; i < MAX_FRAMES; i++) {
                    fprintf(fp, "%d,%p,%.3f,0x%08X,%.3f,0x%08X,%.3f,%.3f\n", 
                        g_Records[i].frameNum, g_Records[i].swapchainPtr, g_Records[i].cpuGapMs,
                        g_Records[i].testHr, g_Records[i].testDurationMs,
                        g_Records[i].renderHr, g_Records[i].renderDurationMs,
                        g_Records[i].dispGapMs);
                }
                fclose(fp);
            }
            Log("Telemetry complete. CSV written.");
            Beep(1500, 200); // The second beep is back!
        }
        return renderHr;
    }

    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init Thread (Dummy Swapchain) ─────────────────────────────────────────────
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
    }

    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_csvPath, sizeof(g_csvPath), "%sAutoPacer_AnswerStats.csv", dllPath);
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}