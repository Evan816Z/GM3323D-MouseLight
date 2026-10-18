// hid_probe.cpp - READ-ONLY diagnostic probe for the GM3323D-family mouse
// Build: g++ -O2 -std=c++17 hid_probe.cpp -o hid_probe.exe -lsetupapi -lhid -ladvapi32
// Usage: hid_probe.exe [--vid 00FF] [--pid BACC]
// This tool NEVER writes to the device. Read-only analysis only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidusage.h>
#include <hidclass.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "advapi32.lib")

typedef BOOLEAN (WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
typedef BOOLEAN (WINAPI *PFN_HidD_GetInputReport)(HANDLE, PVOID, ULONG);

static USHORT g_vid = 0x00FF;
static USHORT g_pid = 0xBACC;

struct DevInfo {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    std::wstring product, mfr, serial;
    bool opened = false;
};

static void PrintHex(const BYTE* p, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        printf("    %04zX: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02X ", p[i + j]); else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            BYTE c = p[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        printf("|\n");
    }
}

static std::wstring GetHidString(HANDLE h, BOOLEAN (WINAPI*fn)(HANDLE, PVOID, ULONG)) {
    wchar_t buf[256] = {0};
    if (fn(h, buf, sizeof(buf))) return buf;
    return L"";
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev = {0};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &sz);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated;
}

// Enumerate all present HID device interfaces
static std::vector<DevInfo> EnumerateHid() {
    std::vector<DevInfo> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hdev = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return out;
    }
    for (DWORD idx = 0; ; idx++) {
        SP_DEVICE_INTERFACE_DATA did;
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(hdev, nullptr, &hidGuid, idx, &did))
            break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need + 2, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, &need, nullptr))
            continue;
        DevInfo di;
        di.path = detail->DevicePath;
        // Robustness: some builds place the path at +4 instead of +8
        if (di.path.find(L"hid#") == std::wstring::npos) {
            auto alt = reinterpret_cast<const wchar_t*>(buf.data() + 4);
            if (wcsstr(alt, L"hid#")) di.path = alt;
        }
        out.push_back(di);
    }
    SetupDiDestroyDeviceInfoList(hdev);
    return out;
}

static bool TryOpenRead(const std::wstring& path, HANDLE& h) {
    h = CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    return h != INVALID_HANDLE_VALUE;
}

static void DescribeCollection(const DevInfo& di, bool doReads) {
    printf("=====================================================================\n");
    printf("PATH: %ls\n", di.path.c_str());

    HANDLE h = INVALID_HANDLE_VALUE;
    if (!TryOpenRead(di.path, h)) {
        DWORD e = GetLastError();
        printf("  Open(GENERIC_READ) FAILED err=%lu %s\n", e,
               e == 5 ? "(ACCESS_DENIED - system-exclusive collection; needs elevation)" : "");
        printf("\n");
        return;
    }

    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (HidD_GetAttributes(h, &attr)) {
        printf("  VID=0x%04X PID=0x%04X VER=0x%04X\n", attr.VendorID, attr.ProductID, attr.VersionNumber);
    } else {
        printf("  HidD_GetAttributes failed: %lu\n", GetLastError());
    }
    printf("  Mfr=%ls\n", GetHidString(h, HidD_GetManufacturerString).c_str());
    printf("  Product=%ls\n", GetHidString(h, HidD_GetProductString).c_str());
    printf("  Serial=%ls\n", GetHidString(h, HidD_GetSerialNumberString).c_str());

    PHIDP_PREPARSED_DATA ppd = nullptr;
    if (!HidD_GetPreparsedData(h, &ppd)) {
        printf("  HidD_GetPreparsedData failed: %lu\n", GetLastError());
        CloseHandle(h);
        printf("\n");
        return;
    }
    HIDP_CAPS caps;
    NTSTATUS st = HidP_GetCaps(ppd, &caps);
    if (st == HIDP_STATUS_SUCCESS) {
        printf("  CAPS: UsagePage=0x%04X Usage=0x%04X  In=%u Out=%u Feature=%u  LinkCollections=%u\n",
               caps.UsagePage, caps.Usage, caps.InputReportByteLength,
               caps.OutputReportByteLength, caps.FeatureReportByteLength,
               caps.NumberLinkCollectionNodes);
        printf("        InVal=%u InBtn=%u OutVal=%u OutBtn=%u FeatVal=%u FeatBtn=%u\n",
               caps.NumberInputValueCaps, caps.NumberInputButtonCaps,
               caps.NumberOutputValueCaps, caps.NumberOutputButtonCaps,
               caps.NumberFeatureValueCaps, caps.NumberFeatureButtonCaps);

        // Enumerate value caps for each report type (read-only, documented API)
        struct { const char* name; HIDP_REPORT_TYPE type; USHORT count; } groups[] = {
            {"INPUT",  HidP_Input,   caps.NumberInputValueCaps},
            {"OUTPUT", HidP_Output,  caps.NumberOutputValueCaps},
            {"FEATURE",HidP_Feature, caps.NumberFeatureValueCaps},
        };
        for (auto& g : groups) {
            if (!g.count) continue;
            USHORT n = g.count;
            std::vector<HIDP_VALUE_CAPS> vc(n);
            NTSTATUS s2 = HidP_GetValueCaps(g.type, vc.data(), &n, ppd);
            printf("  --- %s value caps (%u) ---\n", g.name, g.count);
            if (s2 != HIDP_STATUS_SUCCESS) {
                printf("    HidP_GetValueCaps failed: 0x%08X\n", s2);
                continue;
            }
            for (USHORT i = 0; i < n; i++) {
                auto& c = vc[i];
                printf("    [%u] ReportID=0x%02X UsagePage=0x%04X Usage=0x%04X "
                       "BitSize=%u ReportCount=%u Logical=[%ld,%ld] IsRange=%d HasNull=%d\n",
                       i, c.ReportID, c.UsagePage,
                       c.IsRange ? c.Range.UsageMin : c.NotRange.Usage,
                       c.BitSize, c.ReportCount, c.LogicalMin, c.LogicalMax,
                       c.IsRange, c.HasNull);
                if (c.IsRange)
                    printf("        UsageRange=[0x%04X,0x%04X] DataIndex=[%u,%u]\n",
                           c.Range.UsageMin, c.Range.UsageMax,
                           c.Range.DataIndexMin, c.Range.DataIndexMax);
                else
                    printf("        DataIndex=%u\n", c.NotRange.DataIndex);
            }
        }

        if (caps.FeatureReportByteLength > 0 && caps.FeatureReportByteLength <= 256) {
            printf("  --- read-only GetFeature sweep (report ids 0..15) ---\n");
            for (int rid = 0; rid < 16; rid++) {
                std::vector<BYTE> b(caps.FeatureReportByteLength, 0);
                b[0] = (BYTE)rid;
                if (HidD_GetFeature(h, b.data(), (ULONG)b.size()))
                    printf("    id=0x%02X -> ", rid), PrintHex(b.data(), b.size());
            }
        }
    } else {
        printf("  HidP_GetCaps failed: 0x%08X\n", st);
    }
    HidD_FreePreparsedData(ppd);

    // Raw report descriptor via the DOCUMENTED IOCTL (read-only)
    printf("  --- collection descriptor (IOCTL_HID_GET_COLLECTION_DESCRIPTOR) ---\n");
    {
        std::vector<BYTE> dbuf(4096, 0);
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_HID_GET_COLLECTION_DESCRIPTOR, nullptr, 0,
                            dbuf.data(), (DWORD)dbuf.size(), &ret, nullptr) && ret > 0) {
            printf("  descriptor: %lu bytes\n", ret);
            PrintHex(dbuf.data(), ret);
        } else {
            printf("  descriptor IOCTL failed: %lu\n", GetLastError());
        }
    }
    // Collection information (descriptor size, VID/PID/version, read-only)
    {
        HID_COLLECTION_INFORMATION ci;
        ZeroMemory(&ci, sizeof(ci));
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_HID_GET_COLLECTION_INFORMATION, nullptr, 0,
                            &ci, sizeof(ci), &ret, nullptr))
            printf("  collection info: DescriptorSize=%lu Polled=%d VID=0x%04X PID=0x%04X VER=0x%04X\n",
                   ci.DescriptorSize, ci.Polled, ci.VendorID, ci.ProductID, ci.VersionNumber);
    }

    // Read-only: try GetInputReport (Win8+) and overlapped input reads
    auto pGetInputReport = reinterpret_cast<PFN_HidD_GetInputReport>(
        GetProcAddress(GetModuleHandleW(L"hid.dll"), "HidD_GetInputReport"));
    if (pGetInputReport) {
        printf("  --- read-only HidD_GetInputReport sweep ---\n");
        bool any = false;
        for (int rid = 0; rid < 16; rid++) {
            BYTE b[256] = {0};
            b[0] = (BYTE)rid;
            if (pGetInputReport(h, b, sizeof(b))) {
                printf("    id=0x%02X -> ", rid);
                PrintHex(b, 32);
                any = true;
            }
        }
        if (!any) printf("    (no input reports readable)\n");
    }

    if (doReads) {
        printf("  --- overlapped input reads (1.5s timeout x4) ---\n");
        HANDLE hr = CreateFileW(di.path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (hr == INVALID_HANDLE_VALUE) {
            printf("    overlapped open failed: %lu\n", GetLastError());
        } else {
            for (int i = 0; i < 4; i++) {
                BYTE b[256] = {0};
                OVERLAPPED ov = {0};
                ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                DWORD got = 0;
                BOOL ok = ReadFile(hr, b, 65, nullptr, &ov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(ov.hEvent, 1500) == WAIT_OBJECT_0 &&
                        GetOverlappedResult(hr, &ov, &got, FALSE)) {
                        printf("    read#%d (%lu bytes):\n", i, got);
                        PrintHex(b, got);
                    } else {
                        CancelIo(hr);
                        printf("    read#%d: timeout\n", i);
                    }
                } else if (ok) {
                    printf("    read#%d sync:\n", i);
                    PrintHex(b, 65);
                } else {
                    printf("    read#%d failed: %lu\n", i, GetLastError());
                }
                CloseHandle(ov.hEvent);
            }
            CloseHandle(hr);
        }
    }
    CloseHandle(h);
    printf("\n");
}

// ---------------- watch mode: read-only change detection ----------------
struct WatchColl {
    std::wstring path;
    std::wstring tag;
    HANDLE h = INVALID_HANDLE_VALUE;      // query handle (feature reads)
    HANDLE hRead = INVALID_HANDLE_VALUE;  // overlapped handle (input reads)
    std::vector<std::pair<BYTE, std::vector<BYTE>>> featState;  // reportID -> last bytes
    std::vector<BYTE> inBuf;
    OVERLAPPED ov = {0};
    bool readPending = false;
    bool hasFeature = false;
};

static void PrintStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

static int WatchMode(int seconds) {
    printf("=== WATCH MODE (read-only) : %d s, target VID=0x%04X PID=0x%04X, elevated=%s ===\n",
           seconds, g_vid, g_pid, IsElevated() ? "YES" : "NO");
    printf("Now change light settings in the ORIGINAL GM3323D software.\n");
    printf("This tool only READS. Press Ctrl+C to stop early.\n\n");

    auto devs = EnumerateHid();
    std::vector<WatchColl> colls;
    for (auto& d : devs) {
        WatchColl wc;
        wc.path = d.path;
        wc.h = CreateFileW(d.path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (wc.h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        USHORT vid = 0, pid = 0;
        if (HidD_GetAttributes(wc.h, &attr)) { vid = attr.VendorID; pid = attr.ProductID; }
        if (vid != g_vid || pid != g_pid) { CloseHandle(wc.h); continue; }

        wc.tag = d.path;
        auto pos = wc.tag.find(L"#{");
        if (pos != std::wstring::npos) wc.tag = wc.tag.substr(0, pos);

        PHIDP_PREPARSED_DATA ppd = nullptr;
        if (HidD_GetPreparsedData(wc.h, &ppd)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                printf("watching: %ls\n  In=%u Out=%u Feature=%u\n", wc.tag.c_str(),
                       caps.InputReportByteLength, caps.OutputReportByteLength,
                       caps.FeatureReportByteLength);
                // collect feature report IDs from value+button caps
                std::vector<BYTE> ids;
                if (caps.NumberFeatureValueCaps) {
                    USHORT n = caps.NumberFeatureValueCaps;
                    std::vector<HIDP_VALUE_CAPS> vc(n);
                    USHORT got = n;
                    if (HidP_GetValueCaps(HidP_Feature, vc.data(), &got, ppd) == HIDP_STATUS_SUCCESS)
                        for (USHORT i = 0; i < got; i++) ids.push_back(vc[i].ReportID);
                }
                if (caps.NumberFeatureButtonCaps) {
                    USHORT n = caps.NumberFeatureButtonCaps;
                    std::vector<HIDP_BUTTON_CAPS> bc(n);
                    USHORT got = n;
                    if (HidP_GetButtonCaps(HidP_Feature, bc.data(), &got, ppd) == HIDP_STATUS_SUCCESS)
                        for (USHORT i = 0; i < got; i++) ids.push_back(bc[i].ReportID);
                }
                if (ids.empty() && caps.FeatureReportByteLength > 0)
                    for (int r = 0; r < 16; r++) ids.push_back((BYTE)r);
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                for (BYTE id : ids) {
                    USHORT len = caps.FeatureReportByteLength;
                    if (len < 1 || len > 256) continue;
                    std::vector<BYTE> b(len, 0);
                    b[0] = id;
                    BOOL ok = HidD_GetFeature(wc.h, b.data(), len);
                    wc.featState.push_back({id, ok ? b : std::vector<BYTE>()});
                    printf("  feature id=0x%02X initial: %s\n", id,
                           ok ? "" : "(unreadable)");
                    if (ok) PrintHex(b.data(), b.size());
                }
                wc.hasFeature = !wc.featState.empty();
            }
            HidD_FreePreparsedData(ppd);
        }
        // overlapped read handle for input reports
        wc.hRead = CreateFileW(d.path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (wc.hRead != INVALID_HANDLE_VALUE) {
            wc.inBuf.assign(256, 0);
            ZeroMemory(&wc.ov, sizeof(wc.ov));
            wc.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            wc.readPending = ReadFile(wc.hRead, wc.inBuf.data(), 256, nullptr, &wc.ov) ||
                             GetLastError() == ERROR_IO_PENDING;
        }
        colls.push_back(std::move(wc));
    }
    if (colls.empty()) {
        printf("no openable collections for VID=0x%04X PID=0x%04X\n", g_vid, g_pid);
        printf("(run as Administrator to also watch system mouse/kbd collections)\n");
        return 1;
    }

    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) / 1000 < seconds) {
        for (auto& wc : colls) {
            // feature polls
            if (wc.hasFeature) {
                for (auto& fs : wc.featState) {
                    if (fs.second.empty()) continue;
                    std::vector<BYTE> b(fs.second.size(), 0);
                    b[0] = fs.first;
                    if (HidD_GetFeature(wc.h, b.data(), (ULONG)b.size()) && b != fs.second) {
                        PrintStamp();
                        printf("FEATURE CHANGE id=0x%02X on %ls\n", fs.first, wc.tag.c_str());
                        printf("    old: "); PrintHex(fs.second.data(), fs.second.size());
                        printf("    new: "); PrintHex(b.data(), b.size());
                        fs.second = b;
                    }
                }
            }
            // input reads
            if (wc.readPending) {
                DWORD got = 0;
                if (GetOverlappedResult(wc.hRead, &wc.ov, &got, FALSE) && got > 0) {
                    static std::vector<BYTE> lastIn;
                    bool changed = (got != lastIn.size()) ||
                                   memcmp(wc.inBuf.data(), lastIn.data(), got) != 0;
                    if (changed) {
                        PrintStamp();
                        printf("INPUT on %ls (%lu bytes):\n", wc.tag.c_str(), got);
                        PrintHex(wc.inBuf.data(), got);
                        lastIn.assign(wc.inBuf.begin(), wc.inBuf.begin() + got);
                    }
                    ZeroMemory(&wc.ov, sizeof(wc.ov));
                    wc.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    std::fill(wc.inBuf.begin(), wc.inBuf.end(), 0);
                    wc.readPending = ReadFile(wc.hRead, wc.inBuf.data(), 256, nullptr, &wc.ov) ||
                                     GetLastError() == ERROR_IO_PENDING;
                }
            }
        }
        Sleep(200);
    }
    printf("\nwatch finished.\n");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int watchSeconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--vid") && i + 1 < argc) g_vid = (USHORT)wcstoul(argv[++i], nullptr, 16);
        else if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) g_pid = (USHORT)wcstoul(argv[++i], nullptr, 16);
        else if (!wcsncmp(argv[i], L"--watch", 7)) {
            const wchar_t* p = argv[i] + 7;
            if (*p == L'=') p++;
            watchSeconds = *p ? _wtoi(p) : 120;
        }
    }
    if (watchSeconds > 0) return WatchMode(watchSeconds);

    printf("=== GM3323D mouse read-only probe ===\n");
    printf("target VID=0x%04X PID=0x%04X  elevated=%s\n\n", g_vid, g_pid, IsElevated() ? "YES" : "NO");

    auto devs = EnumerateHid();
    printf("total HID interfaces: %zu\n", devs.size());

    // Pass 1: identify by attributes (open + GetAttributes)
    std::vector<DevInfo> matched, denied;
    for (auto& d : devs) {
        HANDLE h;
        if (TryOpenRead(d.path, h)) {
            HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
            if (HidD_GetAttributes(h, &attr)) {
                d.vid = attr.VendorID; d.pid = attr.ProductID;
                d.product = GetHidString(h, HidD_GetProductString);
                d.mfr = GetHidString(h, HidD_GetManufacturerString);
            }
            CloseHandle(h);
            if (d.vid == g_vid && d.pid == g_pid) matched.push_back(d);
        } else {
            denied.push_back(d);
        }
    }
    printf("opened+matched: %zu, open-denied: %zu\n\n", matched.size(), denied.size());

    if (matched.empty()) {
        printf("!! No openable device with VID=0x%04X PID=0x%04X\n", g_vid, g_pid);
        printf("Dumping all openable HID devices for manual identification:\n");
        for (auto& d : devs) {
            HANDLE h;
            if (TryOpenRead(d.path, h)) {
                HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
                USHORT v = 0, p = 0;
                if (HidD_GetAttributes(h, &attr)) { v = attr.VendorID; p = attr.ProductID; }
                printf("  VID=0x%04X PID=0x%04X  %ls\n", v, p, d.path.c_str());
                CloseHandle(h);
            }
        }
        printf("\nDenied collections (re-run as Administrator to probe these):\n");
        for (auto& d : denied) printf("  %ls\n", d.path.c_str());
        return 1;
    }

    printf("### TARGET COLLECTIONS ###\n");
    for (auto& d : matched) DescribeCollection(d, true);

    if (!IsElevated() && !denied.empty()) {
        printf("### NOTE ###\n");
        printf("Some collections could not be opened (system mouse/kbd collections are\n");
        printf("opened exclusively by Windows). Re-run this probe as Administrator to\n");
        printf("inspect them:\n");
        for (auto& d : denied) printf("  %ls\n", d.path.c_str());
        printf("\n");
    }
    return 0;
}
