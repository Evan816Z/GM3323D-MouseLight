// config_watch.cpp — GM3323D 配置态高频监听探针（只读，零写入）。
// 关键事实: 设备只在「配置被改动后的瞬间」把 132 字节配置态(签名 05 0d)吐给 GetFeature(0x04)。
//   -> 单纯轮询抓不到, 必须在原厂软件里"改一个 DPI 值/切一次回报率"的瞬间本工具才能截获。
// 用法见 config_watch.bat / 运行后屏上的提示。
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <conio.h>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static const USHORT kVID = 0x1D57, kPID = 0xFA60;
static const ULONG kFeatLen = 132;
static const BYTE  kReportID = 0x04;

static FILE* g_f = NULL;

static void cprint(const char* s) { printf("%s", s); fflush(stdout); }
static void logline(const std::string& s) {
    printf("%s\n", s.c_str()); fflush(stdout);
    if (g_f) { fwrite(s.data(), 1, s.size(), g_f); fwrite("\n", 1, 1, g_f); fflush(g_f); }
}

static std::string hex(const std::vector<BYTE>& b, int n) {
    char hx[8]; std::string s;
    for (int i = 0; i < n && i < (int)b.size(); i++) { snprintf(hx, 8, " %02x", b[i]); s += hx; }
    return s;
}

int main(void) {
    cprint("GM3323D config-watch probe starting...\n");
    wchar_t mod[MAX_PATH]; GetModuleFileNameW(NULL, mod, MAX_PATH);
    std::wstring dir(mod);
    size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos) dir = dir.substr(0, p); else dir = L".";
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t fn[MAX_PATH];
    // 1) 先试 exe 同目录; 失败则回退到 %TEMP%
    swprintf(fn, MAX_PATH, L"%s\\config_watch_%04d%02d%02d_%02d%02d%02d.txt",
             dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_f = _wfopen(fn, L"wb");
    if (!g_f) {
        wchar_t tp[MAX_PATH];
        DWORD tl = GetTempPathW(MAX_PATH, tp);
        if (tl) {
            swprintf(fn, MAX_PATH, L"%sconfig_watch_%04d%02d%02d_%02d%02d%02d.txt",
                     tp, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            g_f = _wfopen(fn, L"wb");
        }
    }
    if (!g_f) {
        cprint("ERROR: cannot write log file (even in %TEMP%). Check permissions.\n");
        return 1;
    }
    char pathbuf[512];
    snprintf(pathbuf, 512, "log file: %ls\n", fn);
    cprint(pathbuf);

    logline("=== GM3323D config-watch (read-only, 10ms polling) ===");
    logline("IMPORTANT: the config snapshot only appears at the MOMENT you change DPI / polling-rate.");
    logline("  Just leaving the original software open (without changing anything) captures nothing.");
    logline("Steps: run this -> in original software drag DPI slot#1 to MIN(200) then press 1 here,");
    logline("       drag it to MAX(10000) then press 2. Change polling-rate 1-2 times and press 3.");
    logline("       Stops after 4 captures, or press ESC to stop early.");
    logline("");

    // ---- 枚举打开 col04 (Feature 132) ----
    GUID guid; HidD_GetHidGuid(&guid);
    HDEVINFO hdev = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) { logline("SetupDiGetClassDevs 失败"); return 1; }
    SP_DEVICE_INTERFACE_DATA did; did.cbSize = sizeof(did);
    DWORD idx = 0; HANDLE fh = INVALID_HANDLE_VALUE;
    while (SetupDiEnumDeviceInterfaces(hdev, NULL, &guid, idx, &did)) {
        idx++;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
        std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2);
        auto* det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, det, need, NULL, NULL)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at; at.Size = sizeof(at);
        if (!HidD_GetAttributes(h, &at) || at.VendorID != kVID || at.ProductID != kPID) { CloseHandle(h); continue; }
        PHIDP_PREPARSED_DATA ppd = NULL;
        if (!HidD_GetPreparsedData(h, &ppd)) { CloseHandle(h); continue; }
        HIDP_CAPS caps;
        bool ok = false;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
            ok = (caps.FeatureReportByteLength >= kFeatLen);
        HidD_FreePreparsedData(ppd);
        if (!ok) { CloseHandle(h); continue; }
        fh = h; break;
    }
    SetupDiDestroyDeviceInfoList(hdev);
    if (fh == INVALID_HANDLE_VALUE) {
        logline("!!! Mouse (VID 1D57/PID FA60) Feature collection NOT found.");
        logline("    1) make sure the mouse is connected; 2) try closing MouseLight.exe first (avoid handle lock); 3) run as admin if needed.");
        cprint("\nPress any key to exit...\n"); _getch();
        if (g_f) fclose(g_f);
        return 1;
    }
    logline("Device opened. Now go change a DPI value in the original software...");

    ULONGLONG t0 = GetTickCount64();
    std::vector<BYTE> last;
    long sigHits = 0, polls = 0, distinct = 0;
    const long kStopAfter = 4;
    ULONGLONG tPrompt = 0;
    while (GetTickCount64() - t0 < 150000) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { logline("--- ESC: 提前结束 ---"); break; }
        while (_kbhit()) {
            int c = _getch();
            if (c >= '1' && c <= '9') {
                char mk[64];
                snprintf(mk, 64, "--- MARKER %c  (T+%.1fs) ---", c, (GetTickCount64() - t0) / 1000.0);
                logline(mk);
            }
        }
        std::vector<BYTE> b(kFeatLen, 0);
        b[0] = kReportID;
        polls++;
        if (HidD_GetFeature(fh, b.data(), kFeatLen)) {
            if (b[0] == 0x05 && b[1] == 0x0d) {
                sigHits++;
                if (b != last) {
                    char hd[80];
                    snprintf(hd, 80, "[T+%7.2fs] CONFIG:", (GetTickCount64() - t0) / 1000.0);
                    logline(std::string(hd) + hex(b, kFeatLen));
                    last = b;
                    distinct++;
                    printf("  >> captured config #%ld!\n", distinct); fflush(stdout);
                }
            }
        }
        if ((polls % 500) == 0) {
            printf("\r  T+%.0fs  已抓配置 %ld 条 / 命中 %ld  (ESC 结束; 没抓到就去改个 DPI)   ",
                   (GetTickCount64() - t0) / 1000.0, distinct, sigHits);
            fflush(stdout);
        }
        if (distinct >= kStopAfter) { logline("--- captured enough, stopping ---"); break; }
        if (distinct == 0 && GetTickCount64() - t0 > 30000 && GetTickCount64() - tPrompt > 15000) {
            tPrompt = GetTickCount64();
            cprint("\n[tip] 30s passed with no capture: please drag a DPI slider (or change polling-rate) in the original software now.\n");
        }
        Sleep(10);
    }
    printf("\n");
    char sum[128];
    snprintf(sum, 128, "=== END: %ld polls, %ld config hits, %ld distinct ===", polls, sigHits, distinct);
    logline(sum);
    CloseHandle(fh);
    if (g_f) fclose(g_f);
    cprint("Done. Log saved. Press any key to exit...\n");
    for (int i = 0; i < 60 && !_kbhit(); i++) Sleep(100);
    return 0;
}
