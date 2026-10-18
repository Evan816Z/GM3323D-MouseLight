// GM3323D (VID 1D57 / PID FA60) read-only Feature report dumper.
// GUI, no console. Double-click to run. Writes a timestamped .txt next to itself.
// ZERO writes to the device: only HidD_GetFeature (read) is used.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <ctime>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static FILE* g_f = NULL;
static bool g_batch = false;

static int msgbox(const wchar_t* text, const wchar_t* cap, UINT type) {
    if (g_batch) return (type & MB_YESNO) ? IDNO : IDOK;
    return MessageBoxW(NULL, text, cap, type);
}

static void logline(const std::string& s) {
    if (!g_f) return;
    fwrite(s.data(), 1, s.size(), g_f);
    fwrite("\n", 1, 1, g_f);
    fflush(g_f);
}

static std::string wtoa(const wchar_t* w) {
    std::string r;
    for (int i = 0; w[i]; i++) r += (char)(w[i] & 0xFF);
    return r;
}

static std::map<int, std::vector<BYTE>> readAllFeatures(HANDLE h, USHORT flen) {
    std::map<int, std::vector<BYTE>> m;
    if (flen == 0 || flen > 256) return m;
    for (int rid = 0; rid < 256; rid++) {
        std::vector<BYTE> b(flen, 0);
        b[0] = (BYTE)rid;
        if (HidD_GetFeature(h, b.data(), flen))
            m[rid] = std::move(b);
    }
    return m;
}

static std::string hexReport(const std::vector<BYTE>& b) {
    char hx[8];
    std::string s;
    for (size_t i = 0; i < b.size(); i++) {
        snprintf(hx, sizeof(hx), " %02x", b[i]);
        s += hx;
    }
    return s;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    if (lpCmdLine && strstr(lpCmdLine, "batch")) g_batch = true;
    wchar_t mod[MAX_PATH];
    GetModuleFileNameW(NULL, mod, MAX_PATH);
    std::wstring wmod(mod);
    size_t pos = wmod.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? L"." : wmod.substr(0, pos);

    time_t t = time(NULL);
    struct tm* ptm = localtime(&t);
    wchar_t ts[64];
    wcsftime(ts, 64, L"%Y%m%d_%H%M%S", ptm);

    std::wstring wfname = dir + L"\\dump_" + ts + L".txt";
    g_f = _wfopen(wfname.c_str(), L"wb");
    if (!g_f) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wfname = std::wstring(tmp) + L"dump_" + ts + L".txt";
        g_f = _wfopen(wfname.c_str(), L"wb");
    }
    if (!g_f) {
        msgbox(L"Cannot write output file.", L"GM3323D dump", MB_ICONERROR);
        return 1;
    }

    logline("=== GM3323D (VID 1D57 PID FA60) Feature report read-only dump ===");
    logline("This tool ONLY reads; it sends no commands to the mouse.");
    logline("");

    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO hdev = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) {
        logline("ERROR: SetupDiGetClassDevs failed.");
        fclose(g_f);
        msgbox(L"Enumeration failed.", L"GM3323D dump", MB_ICONERROR);
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    DWORD idx = 0;
    bool foundAny = false;
    HANDLE fh = INVALID_HANDLE_VALUE;
    std::vector<BYTE> firstSnap; // not used; per-device below
    USHORT featLen = 0;
    std::wstring foundPath;

    while (SetupDiEnumDeviceInterfaces(hdev, NULL, &guid, idx, &did)) {
        idx++;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &needed, NULL);
        std::vector<wchar_t> buf(needed / sizeof(wchar_t) + 2);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, needed, NULL, NULL))
            continue;
        std::wstring path = detail->DevicePath;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(h, &attr)) { CloseHandle(h); continue; }
        if (attr.VendorID != 0x1D57 || attr.ProductID != 0xFA60) { CloseHandle(h); continue; }

        // Only care about a collection that has a Feature report
        PHIDP_PREPARSED_DATA ppd = NULL;
        if (!HidD_GetPreparsedData(h, &ppd)) { CloseHandle(h); continue; }
        HIDP_CAPS caps;
        bool hasFeat = false;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.FeatureReportByteLength > 0 && caps.FeatureReportByteLength <= 256) {
                hasFeat = true;
                foundAny = true;
                foundPath = path;
                featLen = caps.FeatureReportByteLength;
                fh = h;
                logline("PATH: " + wtoa(path.c_str()));
                char ab[128];
                snprintf(ab, sizeof(ab), "  VID=0x%04X PID=0x%04X VER=0x%04X", attr.VendorID, attr.ProductID, attr.VersionNumber);
                logline(ab);
                wchar_t nm[128];
                if (HidD_GetManufacturerString(h, nm, sizeof(nm))) logline("  Mfr: " + wtoa(nm));
                if (HidD_GetProductString(h, nm, sizeof(nm)))   logline("  Product: " + wtoa(nm));
                if (HidD_GetSerialNumberString(h, nm, sizeof(nm))) logline("  Serial: " + wtoa(nm));
                snprintf(ab, sizeof(ab), "  CAPS: UsagePage=0x%04X Usage=0x%04X In=%d Out=%d Feature=%d",
                         caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                         caps.OutputReportByteLength, caps.FeatureReportByteLength);
                logline(ab);
                break; // first matching feature collection is enough
            }
        }
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(hdev);

    if (!foundAny || fh == INVALID_HANDLE_VALUE) {
        fclose(g_f);
        msgbox(
            L"GM3323D mouse (VID 1D57 / PID FA60) not found, or it has no readable Feature report.\n\nMake sure the mouse is connected. If it is, try running this tool as Administrator.",
            L"GM3323D dump", MB_ICONWARNING);
        return 0;
    }

    int choice = msgbox(
        L"Recommended: WATCH mode.\n\nClick YES -> the tool runs for 2 minutes. During that time, open the original GM3323D software and change the lighting (color / mode / brightness) a few times. Every change is recorded automatically.\n\nClick NO -> take a single snapshot of the current state (then run the tool again after you change a setting).",
        L"GM3323D dump", MB_YESNO | MB_ICONQUESTION);

    if (choice == IDYES) {
        logline("");
        logline("=== WATCH MODE: recording changes for 120s ===");
        auto snap = readAllFeatures(fh, featLen);
        for (auto& kv : snap) {
            char hdr[64]; snprintf(hdr, sizeof(hdr), "  [initial] rid=0x%02X:", kv.first);
            logline(std::string(hdr) + hexReport(kv.second));
        }
        ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 120000) {
            Sleep(400);
            auto cur = readAllFeatures(fh, featLen);
            double el = (GetTickCount64() - t0) / 1000.0;
            for (auto& kv : cur) {
                int rid = kv.first;
                auto it = snap.find(rid);
                if (it == snap.end() || it->second != kv.second) {
                    char hdr[64];
                    snprintf(hdr, sizeof(hdr), "  [T+%.1fs] rid=0x%02X CHANGED:", el, rid);
                    std::string line = hdr;
                    if (it != snap.end()) line += "\n      old:" + hexReport(it->second);
                    line += "\n      new:" + hexReport(kv.second);
                    logline(line);
                    snap[rid] = kv.second;
                }
            }
        }
        logline("=== WATCH END ===");
    } else {
        logline("");
        logline("=== SINGLE SNAPSHOT ===");
        auto snap = readAllFeatures(fh, featLen);
        for (auto& kv : snap) {
            char hdr[64]; snprintf(hdr, sizeof(hdr), "  rid=0x%02X:", kv.first);
            logline(std::string(hdr) + hexReport(kv.second));
        }
    }

    CloseHandle(fh);
    fclose(g_f);

    std::wstring msg = L"Dump saved to:\n" + wfname + L"\n\nPlease send me this file.";
    msgbox(msg.c_str(), L"GM3323D dump", MB_ICONINFORMATION);
    return 0;
}
