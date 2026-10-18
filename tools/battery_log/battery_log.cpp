// ============================================================================
//  电量采集工具 (只读) — 用于定位电池电量字节
//  目标: VID 0x1D57 / PID 0xFA60 的 mi_02&col03 集合 (UsagePage 0x000A)
//        这是原厂 Open_DevMonitor 使用的监听通道, 与灯光的 Feature 通道不同
//  用法: 双击运行 -> 按提示操作 -> 把生成的 .txt 发我
//  全程只读: 只 ReadFile, 不写任何数据到设备
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <ctime>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static const USHORT kVID = 0x1D57;
static const USHORT kPID = 0xFA60;

static FILE* g_f = NULL;
static void logline(const std::string& s) {
    if (!g_f) return;
    fwrite(s.data(), 1, s.size(), g_f);
    fwrite("\n", 1, 1, g_f);
    fflush(g_f);
}

class OVERLAPPED_EX {
public:
    OVERLAPPED ov;
    HANDLE ev;
    OVERLAPPED_EX() {
        ZeroMemory(&ov, sizeof(ov));
        ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        ov.hEvent = ev;
    }
    ~OVERLAPPED_EX() { if (ev) CloseHandle(ev); }
    void Reset() { ResetEvent(ev); }
};

// 打开 UP 0x000A 的监听集合
static HANDLE OpenMonitor() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO hdev = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hdev, NULL, &guid, idx, &did); idx++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
        std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, NULL, NULL)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        bool ok = false;
        std::wstring path = detail->DevicePath;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == kVID && attr.ProductID == kPID) {
            PHIDP_PREPARSED_DATA ppd = NULL;
            if (HidD_GetPreparsedData(h, &ppd)) {
                HIDP_CAPS caps;
                if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                    if (caps.UsagePage == 0x000A) ok = true;
                }
                HidD_FreePreparsedData(ppd);
            }
        }
        CloseHandle(h);
        if (!ok) continue;
        // 以 OVERLAPPED 方式打开, 便于设置读取超时
        result = CreateFileW(detail->DevicePath, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (result != INVALID_HANDLE_VALUE) {
            logline("PATH: " + std::string(path.begin(), path.end()));
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(hdev);
    return result;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t mod[MAX_PATH];
    GetModuleFileNameW(NULL, mod, MAX_PATH);
    std::wstring wmod(mod);
    size_t pos = wmod.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? L"." : wmod.substr(0, pos);

    time_t t = time(NULL);
    struct tm* ptm = localtime(&t);
    wchar_t ts[64];
    wcsftime(ts, 64, L"%Y%m%d_%H%M%S", ptm);
    std::wstring wfname = dir + L"\\battery_" + ts + L".txt";

    int r = MessageBoxW(NULL,
        L"电量采集（只读，不会改动鼠标）\n\n"
        L"开始前请做两件事：\n"
        L"1. 打开原厂 GM3323D 软件，看清它显示的电量百分比（例如 87%）。\n"
        L"2. 记住这个数字，采集结束后连同本文件一起发我。\n\n"
        L"点击「是」开始采集 60 秒。期间不要关闭原厂软件。",
        L"电量采集", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return 0;

    g_f = _wfopen(wfname.c_str(), L"wb");
    if (!g_f) {
        MessageBoxW(NULL, L"无法写入输出文件。", L"电量采集", MB_ICONERROR);
        return 1;
    }

    logline("=== 电量采集 (只读) ===");
    logline("VID=0x1D57 PID=0xFA60  集合=mi_02&col03 (UsagePage 0x000A)");

    HANDLE h = OpenMonitor();
    if (h == INVALID_HANDLE_VALUE) {
        logline("ERROR: 找不到电量监听集合(UP 0x000A)。鼠标是否连接？");
        fclose(g_f);
        MessageBoxW(NULL, L"找不到鼠标的电量通道。\n请确认鼠标已连接，或以管理员身份重试。",
                    L"电量采集", MB_ICONWARNING);
        return 0;
    }

    HIDP_CAPS caps;
    int inLen = 5;
    PHIDP_PREPARSED_DATA ppd = NULL;
    if (HidD_GetPreparsedData(h, &ppd)) {
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) inLen = caps.InputReportByteLength;
        HidD_FreePreparsedData(ppd);
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "InputReportByteLength=%d  (日志时长 60 秒)", inLen);
    logline(hdr);
    logline("");

    // 同时打开 Feature 集合, 一并记录配置态(便于后续定位 DPI)
    HANDLE hf = INVALID_HANDLE_VALUE;
    {
        GUID guid; HidD_GetHidGuid(&guid);
        HDEVINFO hdev = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hdev != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA did; did.cbSize = sizeof(did);
            for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hdev, NULL, &guid, idx, &did); idx++) {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
                std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2);
                auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, NULL, NULL)) continue;
                HANDLE t2 = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        NULL, OPEN_EXISTING, 0, NULL);
                if (t2 == INVALID_HANDLE_VALUE) continue;
                HIDD_ATTRIBUTES a; a.Size = sizeof(a);
                bool ok = false;
                if (HidD_GetAttributes(t2, &a) && a.VendorID == kVID && a.ProductID == kPID) {
                    PHIDP_PREPARSED_DATA p2 = NULL;
                    if (HidD_GetPreparsedData(t2, &p2)) {
                        HIDP_CAPS c2;
                        if (HidP_GetCaps(p2, &c2) == HIDP_STATUS_SUCCESS && c2.FeatureReportByteLength > 0)
                            ok = true;
                        HidD_FreePreparsedData(p2);
                    }
                }
                CloseHandle(t2);
                if (ok) {
                    hf = CreateFileW(detail->DevicePath, GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if (hf != INVALID_HANDLE_VALUE) break;
                }
            }
            SetupDiDestroyDeviceInfoList(hdev);
        }
    }

    // 采集循环: 每 500ms 记录一次
    ULONGLONG t0 = GetTickCount64();
    int samples = 0, got = 0;
    while (GetTickCount64() - t0 < 60000) {
        // 1) 输入报告(电量通道)
        std::vector<BYTE> buf(inLen > 0 ? inLen : 64, 0);
        {
            OVERLAPPED_EX ox;
            DWORD rd = 0;
            BOOL pending = ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, &ox.ov);
            if (!pending && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ox.ev, 400) == WAIT_OBJECT_0) {
                    GetOverlappedResult(h, &ox.ov, &rd, FALSE);
                } else {
                    CancelIo(h);
                    rd = 0;
                }
            }
            if (rd > 0) {
                std::string line = "IN  ";
                char hx[8];
                for (DWORD i = 0; i < rd; i++) {
                    snprintf(hx, sizeof(hx), "%02x ", buf[i]);
                    line += hx;
                }
                logline(line);
                got++;
            }
        }
        // 2) 配置态(用于定位 DPI): 只在拿到 05 0d 签名时记录
        if (hf != INVALID_HANDLE_VALUE) {
            std::vector<BYTE> fb(132, 0);
            fb[0] = 0x04;
            if (HidD_GetFeature(hf, fb.data(), 132)) {
                if (fb[0] == 0x05 && fb[1] == 0x0d) {
                    std::string line = "CFG ";
                    char hx[8];
                    for (int i = 0; i < 132; i++) {
                        snprintf(hx, sizeof(hx), "%02x ", fb[i]);
                        line += hx;
                    }
                    logline(line);
                }
            }
        }
        samples++;
        Sleep(500);
    }

    char tail[128];
    snprintf(tail, sizeof(tail), "\n=== 采集结束: %d 次采样, %d 条输入报告 ===", samples, got);
    logline(tail);
    logline("请把本文件、以及你在原厂软件里看到的电量百分比一起发我。");

    CloseHandle(h);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    fclose(g_f);

    std::wstring msg = L"采集完成。\n\n文件: " + wfname +
        L"\n\n请把这个文件发我，并告诉我当时原厂软件显示的电量百分比（例如 87%）。";
    MessageBoxW(NULL, msg.c_str(), L"电量采集", MB_ICONINFORMATION);
    return 0;
}
