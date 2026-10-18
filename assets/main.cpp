// ============================================================================
//  GM3323D Mouse Light Control  —  轻量级用户态灯光控制工具
//  目标设备: VID 0x1D57 / PID 0xFA60  (设备名 "GM3323D")
//  协议文档: docs/protocol.md
//
//  技术栈: C++17 + Win32 + 原生 HID API (SetupAPI / HidD_* )
//  无第三方库, 无 .NET/MFC/Qt, 无控制台窗口, 静态链接
//  全部写入策略: 读回当前 132 字节 -> 只改已知字段 -> 其余原样回写
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dbt.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ============================ 协议常量 ============================
// 全部来自实测 (见 docs/protocol.md)
static const USHORT kVID = 0x1D57;
static const USHORT kPID = 0xFA60;
static const BYTE   kReportID = 0x04;      // 目标报告 ID
static const int    kFeatLen  = 132;       // Feature 报告长度 (含首字节)
static const BYTE   kSig0 = 0x05;          // 配置态签名
static const BYTE   kSig1 = 0x0D;

// 配置态内偏移
enum {
    OFF_TYPE   = 0,   // 0x05
    OFF_LEN    = 1,   // 0x0D
    OFF_MODE   = 3,   // 灯效模式 0x00-0x0A
    OFF_BRIGHT = 5,   // 亮度 1-8
    OFF_R      = 6,   // R
    OFF_G      = 7,   // G
    OFF_B      = 8,   // B
};

// 关灯是独立菜单项, 不出现在灯效模式列表里, 避免"关灯模式 + 开灯标志"自相矛盾
static const BYTE kModeOff = 0x00;

// 灯效模式枚举 (已确认: 值 -> 原厂 UI 名称, 来自 res/Language/English.xml)
struct ModeInfo { BYTE value; const wchar_t* name; };
static const ModeInfo kModes[] = {
    { 0x01, L"固定色 (Static)" },
    { 0x02, L"呼吸 (Breathing)" },
    { 0x03, L"霓虹 (Neon)" },
    { 0x04, L"循环呼吸 (Cycle Breathing)" },
    { 0x05, L"固定色-随DPI (Static DPI)" },
    { 0x06, L"呼吸-随DPI (Breathing DPI)" },
    { 0x07, L"流光 (Wave)" },
    { 0x08, L"跑马灯 (Marquee)" },
};
static const int kModeCount = (int)(sizeof(kModes) / sizeof(kModes[0]));
// 0x09 / 0x0A 是确实存在的模式值, 但原厂语言文件中无名称 -> 按"不编造"原则不列入 UI

// 默认模板: 从本机真实设备读取到的 132 字节配置 (2026-10-18 抓包)
// 其中 [3]=模式 [5]=亮度 [6..8]=RGB; 写入时只改这 5 个字节, 其余原样保留
static const BYTE kDefaultTemplate[kFeatLen] = {
    0x05,0x0d,0x01,0x01,0x04,0x08,0x00,0x00,0xff,0x06,0x01,0x00,
    0x00,0x00,0x00,0x06,0x00,0x00,0x11,0x00,0x29,0x0e,0x00,0x00,
    0x0f,0x00,0x00,0x01,0x00,0x00,0x01,0x00,0x00,0x09,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// 写入格式。
// 设备对我们 GetFeature(0x04) 的回应首字节是 0x05(数据) 而非 0x04(报告ID),
// 因此写入时首字节该填"报告ID"还是"数据"存在二义性, 二者都实现, 由探测结果决定。
enum WriteFormat { kWriteRaw = 0, kWriteIDFirst = 1 };

// ============================ 全局状态 ============================
struct Settings {
    BYTE mode;
    BYTE bright;
    BYTE r, g, b;
    WriteFormat fmt;
    bool autostart;
};
static Settings g_cfg = { 0x01, 0x08, 0x00, 0x00, 0xFF, kWriteRaw, false };

static HANDLE   g_dev = INVALID_HANDLE_VALUE;
static HANDLE   g_mon = INVALID_HANDLE_VALUE;   // 电量/状态监听集合 (UP 0x000A)
static int      g_battery = -1;                 // 电量百分比, -1 = 未知
static HWND     g_hwnd = NULL;
static HWND     g_settings = NULL;
static HMENU    g_menu = NULL;
static UINT     g_taskbarCreated = 0;
static UINT     g_msgOpen = 0;      // 跨实例唤醒: 让已有实例打开设置窗口
static bool     g_lightOn = true;

static const wchar_t* kAppName   = L"GM3323D 鼠标灯控";
static const wchar_t* kRegPath   = L"Software\\GM3323DLight";
static const wchar_t* kRunKey    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName   = L"GM3323DLight";
static const wchar_t* kMutexName = L"Local\\GM3323DLight_SingleInstance_2026";
#define IDI_APP 101                 // 应用图标(见 resources/app.rc)

#define WM_APP_TRAY     (WM_APP + 1)
#define WM_APP_BATT     (WM_APP + 2)   // 电量线程 -> UI: 电量变化
#define IDM_TURN_ON      1001
#define IDM_TURN_OFF     1002
#define IDM_PICK_COLOR   1003
#define IDM_AUTOSTART    1004
#define IDM_RESTORE      1005
#define IDM_SETTINGS     1006
#define IDM_EXIT         1007
#define IDM_FMT          1008
#define IDM_MODE_BASE    2000
#define IDM_BRIGHT_BASE  2100
static const UINT kIconId = 1;

// ============================ 注册表 ============================
static void RegSetDWORD(const wchar_t* name, DWORD v) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(hk);
    }
}
static DWORD RegGetDWORD(const wchar_t* name, DWORD def) {
    HKEY hk; DWORD v = def, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, name, 0, NULL, (BYTE*)&v, &sz);
        RegCloseKey(hk);
    }
    return v;
}
static void SaveSettings() {
    RegSetDWORD(L"mode",   g_cfg.mode);
    RegSetDWORD(L"bright", g_cfg.bright);
    RegSetDWORD(L"r", g_cfg.r);
    RegSetDWORD(L"g", g_cfg.g);
    RegSetDWORD(L"b", g_cfg.b);
    RegSetDWORD(L"fmt", (DWORD)g_cfg.fmt);
    RegSetDWORD(L"on", g_lightOn ? 1 : 0);
}
static void LoadSettings() {
    g_cfg.mode   = (BYTE)RegGetDWORD(L"mode",   0x01);
    g_cfg.bright = (BYTE)RegGetDWORD(L"bright", 0x08);
    g_cfg.r = (BYTE)RegGetDWORD(L"r", 0x00);
    g_cfg.g = (BYTE)RegGetDWORD(L"g", 0x00);
    g_cfg.b = (BYTE)RegGetDWORD(L"b", 0xFF);
    g_cfg.fmt = (WriteFormat)RegGetDWORD(L"fmt", (DWORD)kWriteRaw);
    g_lightOn = RegGetDWORD(L"on", 1) != 0;
    // 修正矛盾状态: 标记为"开"却存了关灯模式
    if (g_lightOn && g_cfg.mode == kModeOff) { g_cfg.mode = 0x01; SaveSettings(); }
}

static bool IsAutostartEnabled() {
    HKEY hk; bool en = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = { 0 }; DWORD sz = sizeof(buf);
        if (RegQueryValueExW(hk, kRunName, 0, NULL, (BYTE*)buf, &sz) == ERROR_SUCCESS && buf[0])
            en = true;
        RegCloseKey(hk);
    }
    return en;
}
static void SetAutostart(bool enable) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &hk) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(hk, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)(cmd.size() + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(hk, kRunName);
    }
    RegCloseKey(hk);
}

// ============================ HID 层 ============================
// 枚举并打开目标设备的 Feature 集合
static HANDLE OpenDevice() {
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
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, NULL, NULL))
            continue;

        // 先以只读打开做匹配, 避免独占冲突
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        bool match = false;
        if (HidD_GetAttributes(h, &attr)) {
            if (attr.VendorID == kVID && attr.ProductID == kPID) {
                PHIDP_PREPARSED_DATA ppd = NULL;
                if (HidD_GetPreparsedData(h, &ppd)) {
                    HIDP_CAPS caps;
                    if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                        if (caps.FeatureReportByteLength >= kFeatLen) match = true;
                    }
                    HidD_FreePreparsedData(ppd);
                }
            }
        }
        CloseHandle(h);
        if (!match) continue;

        // 匹配成功 -> 优先以读写权限打开(用于 SetFeature)
        HANDLE rw = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (rw == INVALID_HANDLE_VALUE)
            rw = CreateFileW(detail->DevicePath, GENERIC_READ,   // 降级: 仅可读, 写入会失败
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (rw != INVALID_HANDLE_VALUE) { result = rw; break; }
    }
    SetupDiDestroyDeviceInfoList(hdev);
    return result;
}

static void CloseDevice() {
    if (g_dev != INVALID_HANDLE_VALUE) { CloseHandle(g_dev); g_dev = INVALID_HANDLE_VALUE; }
}

// 读回当前配置。设备会交替返回多种"人格", 只认 05 0d 签名的配置态。
// 返回是否成功读到有效配置。
static bool ReadConfig(BYTE* out) {
    if (g_dev == INVALID_HANDLE_VALUE) return false;
    for (int attempt = 0; attempt < 24; attempt++) {
        std::vector<BYTE> b(kFeatLen, 0);
        b[0] = kReportID;
        if (HidD_GetFeature(g_dev, b.data(), kFeatLen)) {
            if (b[0] == kSig0 && b[1] == kSig1) {
                memcpy(out, b.data(), kFeatLen);
                return true;
            }
        }
        if (attempt < 23) Sleep(60);   // 设备只在变化瞬间吐配置, 需要重试
    }
    return false;
}

// 应用配置: 读回 -> 只改已知字段 -> 回写。读不到则用保存的模板。
static bool ApplyConfig() {
    if (g_dev == INVALID_HANDLE_VALUE) g_dev = OpenDevice();
    if (g_dev == INVALID_HANDLE_VALUE) return false;

    BYTE buf[kFeatLen];
    if (!ReadConfig(buf)) memcpy(buf, kDefaultTemplate, kFeatLen);

    const BYTE mode = g_lightOn ? g_cfg.mode : 0x00;
    buf[OFF_MODE]   = mode;
    buf[OFF_BRIGHT] = g_cfg.bright;
    buf[OFF_R] = g_cfg.r;
    buf[OFF_G] = g_cfg.g;
    buf[OFF_B] = g_cfg.b;

    BOOL ok;
    if (g_cfg.fmt == kWriteRaw) {
        // 原样下发: 首字节就是数据(0x05)
        ok = HidD_SetFeature(g_dev, buf, kFeatLen);
    } else {
        // 首字节放报告 ID, 数据整体后移 1 位
        BYTE buf2[kFeatLen];
        buf2[0] = kReportID;
        memcpy(buf2 + 1, buf + 1, kFeatLen - 1);
        ok = HidD_SetFeature(g_dev, buf2, kFeatLen);
    }
    return ok != FALSE;
}

// ============================ 电量监听 (Battery monitoring) ============================
// 电量协议 (2026-10-18 09:05 实测, 电量采集工具 + 原厂显示 70%):
//   col03 5 字节 Input 报告 = 03 00 40 01 07  (21 次采样全部一致)
//   映射: [4] = 0..10 档电量  ->  *10 = 百分比   (0x07 -> 70% 吻合)
//         [0]=03 报告类型, [2]=0x40 疑似电压/原始ADC, [3]=01 疑似充电标志
//   ⚠ 仅单点校准; 若后续不同电量下的采样不吻合, 只需调整 ParseBattery()。

// 电量监听集合
static HANDLE OpenMonitorCollection() {
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
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == kVID && attr.ProductID == kPID) {
            PHIDP_PREPARSED_DATA ppd = NULL;
            if (HidD_GetPreparsedData(h, &ppd)) {
                HIDP_CAPS caps;
                if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                    // UP 0x000A = 原厂 Open_DevMonitor 的目标集合
                    if (caps.UsagePage == 0x000A) ok = true;
                }
                HidD_FreePreparsedData(ppd);
            }
        }
        CloseHandle(h);
        if (!ok) continue;
        result = CreateFileW(detail->DevicePath, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (result != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(hdev);
    return result;
}

// 返回电量百分比; -1 = 尚未确认协议 / 读取失败
// 电量协议 (2026-10-18 09:05 实测, 电量采集工具 + 原厂显示 70%):
//   col03 5 字节 Input 报告 = 03 00 40 01 07  (21 次采样全部一致)
//   映射: [4] = 0..10 档电量  ->  *10 = 百分比   (0x07 -> 70% 吻合)
//         [0]=03 报告类型, [2]=0x40 疑似电压/原始ADC, [3]=01 疑似充电标志
//   ⚠ 仅单点校准; 若后续不同电量下的采样不吻合, 只需调整此函数。
static bool ParseBattery(const BYTE* buf, DWORD rd, int* pctOut) {
    if (rd < 5 || buf[0] != 0x03) return false;
    if (buf[4] > 10) return false;
    *pctOut = (int)buf[4] * 10;
    return true;
}

// 电量监听线程: 阻塞式 ReadFile (报告约 2.9s 一条), 空闲 CPU 为 0。
// 设备拔出时 ReadFile 失败 -> 关闭句柄休眠重试; 重新插入后自动恢复。
static DWORD WINAPI BatteryThread(LPVOID) {
    int lastPosted = -1;
    for (;;) {
        if (g_hwnd == NULL) { Sleep(500); continue; }
        if (g_mon == INVALID_HANDLE_VALUE) {
            g_mon = OpenMonitorCollection();
            if (g_mon == INVALID_HANDLE_VALUE) { Sleep(3000); continue; }
            lastPosted = -1;   // 重连后强制刷新一次
        }
        BYTE buf[64] = { 0 };
        DWORD rd = 0;
        if (ReadFile(g_mon, buf, sizeof(buf), &rd, NULL) && rd >= 5) {
            int pct = -1;
            if (ParseBattery(buf, rd, &pct) && pct != lastPosted) {
                lastPosted = pct;
                g_battery = pct;
                PostMessageW(g_hwnd, WM_APP_BATT, 0, 0);
            }
        } else {
            // 拔出 / 出错: 重置并稍后重试
            CloseHandle(g_mon);
            g_mon = INVALID_HANDLE_VALUE;
            if (g_battery != -1) { g_battery = -1; PostMessageW(g_hwnd, WM_APP_BATT, 0, 0); }
            Sleep(2000);
        }
    }
}

// ============================ 托盘图标 ============================
// 3x5 像素字体 (0-9), 每位 5 行, 低 3 位有效
static const unsigned char kFont3x5[10][5] = {
    {0x7,0x5,0x5,0x5,0x7}, // 0
    {0x2,0x6,0x2,0x2,0x7}, // 1
    {0x7,0x1,0x7,0x4,0x7}, // 2
    {0x7,0x1,0x7,0x1,0x7}, // 3
    {0x5,0x5,0x7,0x1,0x1}, // 4
    {0x7,0x4,0x7,0x1,0x7}, // 5
    {0x7,0x4,0x7,0x5,0x7}, // 6
    {0x7,0x1,0x1,0x1,0x1}, // 7
    {0x7,0x5,0x7,0x5,0x7}, // 8
    {0x7,0x5,0x7,0x1,0x7}, // 9
};

// 绘制电池形状托盘图标, 内部显示电量百分比
// pct < 0 表示电量未知 -> 只画电池轮廓 + "?"
static HICON MakeBatteryIcon(int pct, BYTE r, BYTE g, BYTE b) {
    const int W = 16, H = 16;
    BITMAPV5HEADER bi = { 0 };
    bi.bV5Size = sizeof(bi); bi.bV5Width = W; bi.bV5Height = H; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_RGB; bi.bV5AlphaMask = 0xFF000000;
    void* bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP mask = CreateBitmap(W, H, 1, 1, NULL);
    ReleaseDC(NULL, dc);
    if (!color || !bits) { if (color) DeleteObject(color); if (mask) DeleteObject(mask); return NULL; }

    auto* px = (DWORD*)bits;
    // 透明底
    for (int i = 0; i < W * H; i++) px[i] = 0x00000000;

    auto put = [&](int x, int y, BYTE rr, BYTE gg, BYTE bb) {
        if (x >= 0 && x < W && y >= 0 && y < H)
            px[y * W + x] = 0xFF000000 | ((DWORD)bb << 16) | ((DWORD)gg << 8) | rr;
    };

    // 电池主体: x 2..12, y 3..12 ; 右侧接头 x 13..14, y 5..10
    const int bx0 = 2, bx1 = 13, by0 = 3, by1 = 13;   // [bx0,bx1) 主体
    // 空槽底色
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) put(x, y, 0x30, 0x34, 0x3B);
    // 电量填充(从左)
    if (pct > 0) {
        int fw = (bx1 - bx0) * pct / 100;
        BYTE fr, fg, fb;
        if (pct <= 20)      { fr = 0xE8; fg = 0x4C; fb = 0x4C; }  // 低电量红
        else if (pct <= 50) { fr = 0xE8; fg = 0xA0; fb = 0x3C; }  // 中 橙
        else                { fr = 0x3C; fg = 0xC8; fb = 0x6A; }  // 充足 绿
        for (int y = by0; y < by1; y++)
            for (int x = bx0; x < bx0 + fw; x++) put(x, y, fr, fg, fb);
    }
    // 接头
    for (int y = 5; y <= 10; y++)
        for (int x = 13; x <= 14; x++) put(x, y, 0xC8, 0xCD, 0xD2);
    // 外框 (浅灰)
    for (int x = bx0; x < bx1; x++) { put(x, by0, 0xC8,0xCD,0xD2); put(x, by1 - 1, 0xC8,0xCD,0xD2); }
    for (int y = by0; y < by1; y++) { put(bx0, y, 0xC8,0xCD,0xD2); put(bx1 - 1, y, 0xC8,0xCD,0xD2); }

    // 内部文字
    if (pct >= 0) {
        char txt[5];
        int n = snprintf(txt, sizeof(txt), "%d", pct);
        int total = n * 4 - 1;                       // 每字 3px + 1px 间隔
        int sx = bx0 + ((bx1 - bx0) - total) / 2;
        int sy = by0 + ((by1 - by0) - 5) / 2;
        for (int i = 0; i < n; i++) {
            int d = txt[i] - '0';
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 3; col++)
                    if (kFont3x5[d][row] & (1 << (2 - col)))
                        put(sx + i * 4 + col, sy + row, 0xFF, 0xFF, 0xFF);
        }
    } else {
        // 未知: 画一个 "?" (3x5)
        static const unsigned char q[5] = { 0x7,0x1,0x3,0x0,0x2 };
        int sx = bx0 + 3, sy = by0 + 2;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (q[row] & (1 << (2 - col)))
                    put(sx + col, sy + row, 0xC8, 0xCD, 0xD2);
    }
    (void)r; (void)g; (void)b;

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

static void UpdateTrayIcon() {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    HICON ic = MakeBatteryIcon(g_battery, g_cfg.r, g_cfg.g, g_cfg.b);
    if (ic) nid.hIcon = ic; else nid.uFlags &= ~NIF_ICON;
    std::wstring tip = kAppName;
    tip += g_lightOn ? L"\n灯光: 开" : L"\n灯光: 关";
    if (g_battery >= 0) {
        wchar_t bt[32];
        swprintf(bt, 32, L"\n电量: %d%%", g_battery);
        tip += bt;
    } else {
        tip += L"\n电量: 未接入";
    }
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (ic) DestroyIcon(ic);
}

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    HICON ic = MakeBatteryIcon(g_battery, g_cfg.r, g_cfg.g, g_cfg.b);
    if (ic) nid.hIcon = ic;
    std::wstring tip = kAppName;
    tip += g_lightOn ? L"\n灯光: 开" : L"\n灯光: 关";
    if (g_battery >= 0) {
        wchar_t bt[32];
        swprintf(bt, 32, L"\n电量: %d%%", g_battery);
        tip += bt;
    } else {
        tip += L"\n电量: 未接入";
    }
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (ic) DestroyIcon(ic);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ============================ 菜单 ============================
static void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();

    AppendMenuW(m, MF_STRING, IDM_TURN_ON,  L"开灯");
    AppendMenuW(m, MF_STRING, IDM_TURN_OFF, L"关灯");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    // 颜色
    HMENU cm = CreatePopupMenu();
    AppendMenuW(cm, MF_STRING, IDM_PICK_COLOR, L"自定义颜色...");
    AppendMenuW(m, MF_STRING | MF_POPUP, (UINT_PTR)cm, L"选择颜色");

    // 灯效模式
    HMENU mm = CreatePopupMenu();
    for (int i = 0; i < kModeCount; i++)
        AppendMenuW(mm, MF_STRING, IDM_MODE_BASE + i, kModes[i].name);
    AppendMenuW(m, MF_STRING | MF_POPUP, (UINT_PTR)mm, L"灯效模式");

    // 亮度
    HMENU bm = CreatePopupMenu();
    for (int i = 1; i <= 8; i++) {
        wchar_t t[32];
        swprintf(t, 32, L"%d 档", i);
        AppendMenuW(bm, MF_STRING, IDM_BRIGHT_BASE + i, t);
    }
    AppendMenuW(m, MF_STRING | MF_POPUP, (UINT_PTR)bm, L"亮度");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置...");
    AppendMenuW(m, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_STRING, IDM_RESTORE, L"恢复默认 / 关闭灯光");

    // 写入格式排错开关: 若灯光无反应, 用它切换另一种组包方式, 无需重新编译
    const wchar_t* fmtName = (g_cfg.fmt == kWriteRaw) ? L"写入格式: 原样(默认)"
                                                      : L"写入格式: 报告ID前缀";
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FMT, fmtName);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(cm); DestroyMenu(mm); DestroyMenu(bm); DestroyMenu(m);
}

// ============================ 设置窗口 ============================
enum {
    IDC_COLORBOX = 3001, IDC_PICK = 3002,
    IDC_MODE = 3003, IDC_BRIGHT = 3004,
    IDC_SAVE = 3005, IDC_APPLY = 3006,
    IDC_BATT = 3008
};
// 下拉框高度: 让 8 个选项一次全部可见, 无需滚动
static const int kComboDropH = 190;

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_PICK) {
            static COLORREF cust[16] = { 0 };
            CHOOSECOLORW cc = { 0 };
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = h;
            cc.lpCustColors = cust;
            cc.rgbResult = RGB(g_cfg.r, g_cfg.g, g_cfg.b);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                g_cfg.r = GetRValue(cc.rgbResult);
                g_cfg.g = GetGValue(cc.rgbResult);
                g_cfg.b = GetBValue(cc.rgbResult);
                InvalidateRect(GetDlgItem(h, IDC_COLORBOX), NULL, TRUE);
            }
        } else if (id == IDC_APPLY) {
            int mi = (int)SendMessageW(GetDlgItem(h, IDC_MODE), CB_GETCURSEL, 0, 0);
            if (mi >= 0) g_cfg.mode = kModes[mi].value;
            int bi = (int)SendMessageW(GetDlgItem(h, IDC_BRIGHT), CB_GETCURSEL, 0, 0);
            if (bi >= 0) g_cfg.bright = (BYTE)(bi + 1);
            g_lightOn = true;
            ApplyConfig();          // 鼠标本身的变化即反馈, 不弹窗
            SaveSettings();
            UpdateTrayIcon();
        } else if (id == IDC_SAVE) {
            int mi = (int)SendMessageW(GetDlgItem(h, IDC_MODE), CB_GETCURSEL, 0, 0);
            if (mi >= 0) g_cfg.mode = kModes[mi].value;
            int bi = (int)SendMessageW(GetDlgItem(h, IDC_BRIGHT), CB_GETCURSEL, 0, 0);
            if (bi >= 0) g_cfg.bright = (BYTE)(bi + 1);
            g_lightOn = true;
            SaveSettings();
            ApplyConfig();
            UpdateTrayIcon();
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        // 鼠标滚轮直接切换下拉框选项(下拉列表本身已足够高, 8 项全部可见)
        POINT pt;
        GetCursorPos(&pt);
        HWND w = WindowFromPoint(pt);
        if (w != GetDlgItem(h, IDC_MODE) && w != GetDlgItem(h, IDC_BRIGHT)) break;
        if (!IsWindowEnabled(w)) break;
        int delta = (short)HIWORD(w);
        int cur  = (int)SendMessageW(w, CB_GETCURSEL, 0, 0);
        int cnt  = (int)SendMessageW(w, CB_GETCOUNT, 0, 0);
        if (cnt <= 0 || cur < 0) break;
        int next = cur + (delta < 0 ? 1 : -1);
        if (next < 0) next = 0;
        if (next >= cnt) next = cnt - 1;
        if (next != cur) SendMessageW(w, CB_SETCURSEL, next, 0);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == GetDlgItem(h, IDC_COLORBOX)) {
            static HBRUSH s_brush = NULL;
            if (s_brush) DeleteObject(s_brush);
            s_brush = CreateSolidBrush(RGB(g_cfg.r, g_cfg.g, g_cfg.b));
            return (LRESULT)s_brush;
        }
        break;
    case WM_CLOSE:
        g_settings = NULL;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void OpenSettings() {
    if (g_settings) { SetForegroundWindow(g_settings); return; }
    HINSTANCE hi = GetModuleHandleW(NULL);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"GM3323DLightSettings";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(0, L"GM3323DLightSettings", L"鼠标灯控 - 设置",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 360, 400,
                             NULL, NULL, hi, NULL);
    if (wc.hIcon) {
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIcon);
    }

    const int LX = 22, CX = 92;          // 标签列 / 控件列
    int y = 18;
    const HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    auto label = [&](const wchar_t* t, int ly, int bold = 0) {
        HWND s = CreateWindowW(L"STATIC", t, WS_VISIBLE | WS_CHILD, LX, ly, 260, 20, h, NULL, hi, NULL);
        SendMessageW(s, WM_SETFONT, (WPARAM)hf, TRUE);
        return s;
    };

    // ---------- 灯光 ----------
    label(L"灯光", y); y += 26;
    label(L"颜色", y + 3);
    HWND sw = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_SUNKEN,
                            CX, y, 70, 24, h, (HMENU)IDC_COLORBOX, hi, NULL);
    (void)sw;
    CreateWindowW(L"BUTTON", L"选择颜色...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  CX + 84, y, 150, 24, h, (HMENU)IDC_PICK, hi, NULL);
    y += 34;

    label(L"灯效", y + 3);
    HWND cb = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST |
                            CBS_HASSTRINGS | WS_VSCROLL,
                            CX, y, 234, kComboDropH, h, (HMENU)IDC_MODE, hi, NULL);
    SendMessageW(cb, WM_SETFONT, (WPARAM)hf, TRUE);
    for (int i = 0; i < kModeCount; i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kModes[i].name);
        if (kModes[i].value == g_cfg.mode) SendMessageW(cb, CB_SETCURSEL, i, 0);
    }
    if (SendMessageW(cb, CB_GETCURSEL, 0, 0) < 0) SendMessageW(cb, CB_SETCURSEL, 0, 0);
    y += 34;

    label(L"亮度", y + 3);
    HWND bb = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST |
                            CBS_HASSTRINGS | WS_VSCROLL,
                            CX, y, 234, kComboDropH, h, (HMENU)IDC_BRIGHT, hi, NULL);
    SendMessageW(bb, WM_SETFONT, (WPARAM)hf, TRUE);
    for (int i = 1; i <= 8; i++) {
        wchar_t t[32]; swprintf(t, 32, L"%d 档", i);
        SendMessageW(bb, CB_ADDSTRING, 0, (LPARAM)t);
    }
    SendMessageW(bb, CB_SETCURSEL, (int)g_cfg.bright - 1, 0);
    y += 44;

    // ---------- 电量 ----------
    label(L"电量", y); y += 26;
    wchar_t bt[128];
    if (g_battery >= 0) swprintf(bt, 128, L"当前电量：%d%%", g_battery);
    else                swprintf(bt, 128, L"当前电量：未接入（等待鼠标上报）");
    HWND bs = CreateWindowW(L"STATIC", bt, WS_VISIBLE | WS_CHILD, LX, y, 300, 20, h,
                            (HMENU)IDC_BATT, hi, NULL);
    SendMessageW(bs, WM_SETFONT, (WPARAM)hf, TRUE);
    y += 34;

    // ---------- 按钮 ----------
    CreateWindowW(L"BUTTON", L"应用", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  LX, y, 110, 30, h, (HMENU)IDC_APPLY, hi, NULL);
    CreateWindowW(L"BUTTON", L"保存并应用", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  LX + 124, y, 110, 30, h, (HMENU)IDC_SAVE, hi, NULL);

    g_settings = h;
    SetForegroundWindow(h);
}

// ============================ 主窗口 ============================
static bool ApplyAndReport(HWND hwnd) {
    bool ok = ApplyConfig();
    if (!ok && hwnd)
        MessageBoxW(hwnd, L"找不到鼠标（VID 1D57 / PID FA60）。\n请确认鼠标已连接。",
                    kAppName, MB_ICONWARNING);
    return ok;
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;

    case WM_APP_TRAY:
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) ShowContextMenu(h);
        else if (l == WM_LBUTTONDBLCLK) OpenSettings();
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDM_TURN_ON) {
            g_lightOn = true;
            if (g_cfg.mode == kModeOff) g_cfg.mode = 0x01;   // 开灯时确保不是关灯模式
            ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
        } else if (id == IDM_TURN_OFF) {
            g_lightOn = false;
            ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
        } else if (id == IDM_PICK_COLOR) {
            static COLORREF cust[16] = { 0 };
            CHOOSECOLORW cc = { 0 };
            cc.lStructSize = sizeof(cc); cc.hwndOwner = h; cc.lpCustColors = cust;
            cc.rgbResult = RGB(g_cfg.r, g_cfg.g, g_cfg.b);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                g_cfg.r = GetRValue(cc.rgbResult);
                g_cfg.g = GetGValue(cc.rgbResult);
                g_cfg.b = GetBValue(cc.rgbResult);
                g_lightOn = true;
                ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
            }
        } else if (id >= IDM_MODE_BASE && id < IDM_MODE_BASE + kModeCount) {
            g_cfg.mode = kModes[id - IDM_MODE_BASE].value;
            g_lightOn = true;
            ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
        } else if (id > IDM_BRIGHT_BASE && id <= IDM_BRIGHT_BASE + 8) {
            g_cfg.bright = (BYTE)(id - IDM_BRIGHT_BASE);
            ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
        } else if (id == IDM_AUTOSTART) {
            SetAutostart(!IsAutostartEnabled());
        } else if (id == IDM_FMT) {
            // 切换写入组包格式后再立刻应用一次, 便于验证
            g_cfg.fmt = (g_cfg.fmt == kWriteRaw) ? kWriteIDFirst : kWriteRaw;
            SaveSettings();
            ApplyConfig();          // 看鼠标灯是否变化即可, 不弹窗
        } else if (id == IDM_RESTORE) {
            // 安全恢复: 关灯 + 复位到默认固定色
            g_lightOn = false;
            g_cfg.mode = 0x01; g_cfg.bright = 0x08;
            ApplyAndReport(h); SaveSettings(); UpdateTrayIcon();
        } else if (id == IDM_SETTINGS) {
            OpenSettings();
        } else if (id == IDM_EXIT) {
            DestroyWindow(h);
        }
        return 0;
    }

    case WM_APP_BATT:
        UpdateTrayIcon();
        if (g_settings) {
            HWND bs = GetDlgItem(g_settings, IDC_BATT);
            if (bs) {
                wchar_t bt[128];
                if (g_battery >= 0) swprintf(bt, 128, L"当前电量：%d%%", g_battery);
                else                swprintf(bt, 128, L"当前电量：未接入");
                SetWindowTextW(bs, bt);
            }
        }
        return 0;

    case WM_DEVICECHANGE:
        // 插拔事件驱动, 无任何轮询
        if (w == DBT_DEVICEARRIVAL) {
            CloseDevice();
            g_dev = OpenDevice();
            if (g_dev != INVALID_HANDLE_VALUE) {
                ApplyConfig();          // 重连后自动恢复配置
                UpdateTrayIcon();
            }
        } else if (w == DBT_DEVICEREMOVECOMPLETE) {
            CloseDevice();
        }
        return TRUE;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated) { AddTrayIcon(); return 0; }
    if (msg == g_msgOpen) { OpenSettings(); return 0; }
    return DefWindowProcW(h, msg, w, l);
}

// ============================ 入口 ============================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_msgOpen = RegisterWindowMessageW(L"GM3323DLight_OpenSettings_2026");

    // ---- 单实例: 已运行则唤醒它打开设置, 本实例静默退出(不弹窗) ----
    HANDLE mx = CreateMutexW(NULL, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mx) CloseHandle(mx);
        PostMessageW(HWND_BROADCAST, g_msgOpen, 0, 0);
        return 0;
    }

    LoadSettings();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GM3323DLightMain";
    RegisterClassW(&wc);

    // 隐藏的顶层窗口: WS_EX_TOOLWINDOW 使其不出现在 Alt-Tab,
    // 同时能可靠接收跨实例广播消息与设备插拔通知
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"GM3323DLightMain", kAppName,
                             WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { if (mx) CloseHandle(mx); return 1; }
    ShowWindow(g_hwnd, SW_HIDE);

    AddTrayIcon();

    // 电量监听线程(阻塞读, 空闲零 CPU)
    CreateThread(NULL, 0, BatteryThread, NULL, 0, NULL);

    // 打开设备并应用已保存配置
    g_dev = OpenDevice();
    if (g_dev != INVALID_HANDLE_VALUE) ApplyConfig();

    // 注册设备插拔通知(仅本顶层隐藏窗口)
    DEV_BROADCAST_DEVICEINTERFACE_W f = { 0 };
    f.dbcc_size = sizeof(f);
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&f.dbcc_classguid);
    RegisterDeviceNotificationW(g_hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);

    // 纯事件驱动: 无定时器, 空闲时不消耗 CPU
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseDevice();
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return 0;
}
