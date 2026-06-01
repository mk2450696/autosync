// AutoPacer v29 - The DWM Serializer (CASO PCIe Fix)
//
// Telemetry proved CPU pacing is a flawless 7.7ms (130FPS), but PCIe cross-adapter 
// transfers are bunching frames up, causing the Intel iGPU to drop them (0.000ms gap)
// and stutter (14.1ms gap).
// This injects DwmFlush() to act as a hardware tollbooth, forcing the bunched frames 
// to serialize perfectly to the Intel compositor's hardware clock.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <stdio.h>

static char g_csvPath[MAX_PATH] = "AutoPacer_AnswerStats.csv";
static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v29] %s\n", msg);
        fclose(fp);
    }
}

// ── Telemetry State ───────────────────────────────────────────────────────────
static volatile bool g_StartRecording = false;
static volatile bool g_TelemetryDone = false;

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

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (!g_StartRecording || g_TelemetryDone) {
        // Even when not recording, apply the fix so you can feel if it works!
        DwmFlush(); 
        return oPresent(pSC, SyncInterval, Flags);
    }

    double now = GetTimeMs();
    double cpuGap = now - g_LastCpuTime;
    g_LastCpuTime = now;

    // 1. Hardware Probe
    double testStart = GetTimeMs();
    HRESULT testHr = oPresent(pSC, 0, DXGI_PRESENT_TEST);
    double testDuration = GetTimeMs() - testStart;

    // 2. Monitor Hardware Timings
    DXGI_FRAME_STATISTICS stats = {};
    double dispGap = 0.0;
    if (SUCCEEDED(pSC->GetFrameStatistics(&stats))) {
        double dispTimeMs = (double)(stats.SyncQPCTime.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
        if (g_LastDispTime > 0.0 && stats.SyncQPCTime.QuadPart > 0) dispGap = dispTimeMs - g_LastDispTime;
        if (stats.SyncQPCTime.QuadPart > 0) g_LastDispTime = dispTimeMs;
    }

    // 3. THE FIX: The DWM Hardware Tollbooth
    // This stops the PCIe burst. It aligns the game to Intel's hardware clock.
    DwmFlush();

    // 4. Render Request
    double renderStart = GetTimeMs();
    HRESULT renderHr = oPresent(pSC, SyncInterval, Flags);
    double renderDuration = GetTimeMs() - renderStart;

    // Save to memory
    if (g_FrameCount < MAX_FRAMES) {
        g_Records[g_FrameCount] = { g_FrameCount, pSC, cpuGap, testHr, testDuration, renderHr, renderDuration, dispGap };
        g_FrameCount++;
    } 
    
    if (g_FrameCount >= MAX_FRAMES) {
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
        Beep(1500, 200); 
    }
    
    return renderHr;
}

// ── Background Hotkey Thread ──────────────────────────────────────────────────
static DWORD WINAPI HotkeyThread(LPVOID) {
    while (!g_StartRecording && !g_TelemetryDone) {
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            g_LastCpuTime = GetTimeMs();
            g_StartRecording = true;
            Log("F8 Pressed. Telemetry Started.");
            Beep(1000, 150);
            break;
        }
        Sleep(100);
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

    Log("Game window found. Spawning dummy swapchain to hook Present.");
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
        Log("Hook installed successfully. Waiting for F8.");
    }

    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq); 
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_csvPath, sizeof(g_csvPath), "%sAutoPacer_AnswerStats.csv", dllPath);
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }
        Log("DLL Booted.");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}