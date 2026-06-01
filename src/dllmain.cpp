// AutoPacer v23 - Display Hardware Telemetry
//
// ZERO pacing. ZERO flag modification. 
// This tracks both the CPU Present time AND the actual Hardware VBlank time 
// (when the frame physically hits the monitor). 
// By comparing the two, we can prove if the Nvidia->Intel cross-adapter copy 
// is destroying the FSR3 pacing logic and causing VRR thrashing.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>

static char g_csvPath[MAX_PATH] = "AutoPacer_HardwareStats.csv";
static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v23] %s\n", msg);
        fclose(fp);
    }
}

// ── Telemetry State ───────────────────────────────────────────────────────────
struct FrameRecord {
    int frameNum;
    double cpuPresentGapMs;
    double displayVBlankGapMs;
    UINT syncRefreshCount;
};

const int MAX_FRAMES = 600;
static FrameRecord g_Records[MAX_FRAMES];
static int g_FrameCount = 0;
static bool g_TelemetryDone = false;

static bool g_FirstFrame = true;
static LARGE_INTEGER g_qpcFreq;
static double g_LastCpuTime = 0.0;
static double g_LastDisplayTime = 0.0;

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

// ── Hooked Present (Hardware Wiretap) ─────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        QueryPerformanceFrequency(&g_qpcFreq);
        g_LastCpuTime = GetTimeMs();
        g_FirstFrame = false;
        Log("Telemetry Started. Tracking CPU vs Display timings.");
        Beep(1000, 100);
    }

    if (!g_TelemetryDone) {
        double now = GetTimeMs();
        double cpuGap = now - g_LastCpuTime;
        g_LastCpuTime = now;

        // Ask the display driver for the stats of the PREVIOUS frames that actually hit the screen
        DXGI_FRAME_STATISTICS stats = {};
        double displayGap = 0.0;
        UINT refreshCount = 0;

        if (SUCCEEDED(pSC->GetFrameStatistics(&stats))) {
            double displayTimeMs = (double)(stats.SyncQPCTime.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
            if (g_LastDisplayTime > 0.0 && stats.SyncQPCTime.QuadPart > 0) {
                displayGap = displayTimeMs - g_LastDisplayTime;
            }
            if (stats.SyncQPCTime.QuadPart > 0) {
                g_LastDisplayTime = displayTimeMs;
            }
            refreshCount = stats.SyncRefreshCount;
        }

        // Call original Present perfectly untouched
        HRESULT hr = oPresent(pSC, SyncInterval, Flags);

        // Save to memory
        if (g_FrameCount < MAX_FRAMES) {
            g_Records[g_FrameCount] = { g_FrameCount, cpuGap, displayGap, refreshCount };
            g_FrameCount++;
        } 
        else {
            g_TelemetryDone = true;
            FILE* fp;
            if (fopen_s(&fp, g_csvPath, "w") == 0) {
                fprintf(fp, "Frame,CpuPresentGapMs,DisplayVBlankGapMs,SyncRefreshCount\n");
                for (int i = 0; i < MAX_FRAMES; i++) {
                    fprintf(fp, "%d,%.3f,%.3f,%u\n", 
                        g_Records[i].frameNum, g_Records[i].cpuPresentGapMs, 
                        g_Records[i].displayVBlankGapMs, g_Records[i].syncRefreshCount);
                }
                fclose(fp);
            }
            Log("Telemetry complete. CSV written.");
            Beep(1500, 200);
        }
        return hr;
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
            snprintf(g_csvPath, sizeof(g_csvPath), "%sAutoPacer_HardwareStats.csv", dllPath);
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}