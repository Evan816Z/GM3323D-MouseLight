// ============================================================================
//  hid_probe.cpp  --  Generic HID capture/scan probe for off-brand mice
//
//  Usage:
//    hid_probe.exe <VID> <PID> [seconds]
//      VID/PID may be decimal or 0x-prefixed hex, e.g.  1D57 FA60  (== 0x1d57 0xfa60)
//      seconds = capture window length (default 60; ESC stops early).
//
//  What it does:
//    1. Enumerates every HID collection of the target device and prints topology
//       (UsagePage, Feature/Input report lengths) -- this is what tells you which
//       collection accepts GetFeature (calling it on a no-feature collection HANGS).
//    2. For each feature-bearing collection, scans report IDs 1..63 with a
//       200 ms TIMEOUT-BOUNDED GetFeature (a hung rid is skipped, never fatal).
//    3. Runs a blocking ReadFile thread per Input collection (battery, etc.).
//    4. During the window, records ALL distinct "coherent" states per report ID
//       with timestamps, and reports which BYTE OFFSETS varied when you drive a
//       setting -> that is where the setting lives.
//
//  Output: console summary + %TEMP%\hid_probe_<timestamp>.txt
//
//  Build (MinGW-w64):
//    g++ -O2 -std=c++17 -static hid_probe.cpp -o hid_probe.exe -lsetupapi -lhid
//
//  NOTE: keep this file ASCII. Run it with the FACTORY software open and change
//  the relevant setting WHILE the window is counting down.
// ============================================================================
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static const int kScanRidMax = 63;
static const int kTimeoutMs  = 200;     // per-GetFeature timeout (prevents hangs)
static const int kCap        = 2000;    // max distinct states kept per report id
static const int kMaxFeatLen = 256;     // safety cap on feature buffer size

struct Collection {
    int    idx;
    USHORT vid, pid;
    USHORT usagePage;
    USHORT usage;
    DWORD  featLen;
    DWORD  inLen;
    HANDLE h;
    bool   hasFeature;
    bool   hasInput;
    // feature report ids discovered from preparsed caps (may be empty for
    // "raw" config collections -> we then scan 1..kScanRidMax)
    std::set<UCHAR> featRids;
    // calibration: working buffer size per rid (0 = not a live feature rid)
    std::map<UCHAR,int> fSize;
    // captured distinct states per rid
    std::map<UCHAR, std::set<std::vector<BYTE>>> fState;
    std::map<UCHAR, std::vector<double>> fTime;   // timestamp per captured state
    int    fHits[64];
    // input capture
    std::set<std::vector<BYTE>> iState;
    std::vector<double>         iTime;
    int    iHits;
    HANDLE thread;
};

static std::vector<Collection> g_cols;
static volatile bool g_stop = false;
static HANDLE g_hdevInfo = INVALID_HANDLE_VALUE;

// ---------- helpers ----------
static std::string hexline(const BYTE* b, int n) {
    char tmp[8]; std::string s;
    for (int i = 0; i < n; i++) { snprintf(tmp,sizeof(tmp),"%02x ", b[i]); s += tmp; }
    return s;
}
static std::string nowstamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char t[32]; snprintf(t,sizeof(t),"%04d%02d%02d_%02d%02d%02d",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return t;
}

// ---------- timeout-bounded GetFeature ----------
struct FeatArg { HANDLE h; UCHAR rid; BYTE* buf; int n; BOOL* ok; };
static DWORD WINAPI GetFeatThr(LPVOID p) {
    FeatArg* a = (FeatArg*)p;
    a->buf[0] = a->rid;
    *(a->ok) = HidD_GetFeature(a->h, a->buf, a->n);
    return 0;
}
static int GetFeatureBounded(HANDLE h, UCHAR rid, std::vector<BYTE>& out, int n) {
    out.assign(n, 0);
    BOOL ok = FALSE;
    FeatArg a{h, rid, out.data(), n, &ok};
    HANDLE th = CreateThread(NULL, 0, GetFeatThr, &a, 0, NULL);
    if (!th) return -1;
    if (WaitForSingleObject(th, kTimeoutMs) == WAIT_TIMEOUT) { CloseHandle(th); return -2; } // skip
    CloseHandle(th);
    return ok ? n : -1;
}

// ---------- Input capture thread ----------
static DWORD WINAPI InputThread(LPVOID p) {
    Collection* c = (Collection*)p;
    std::vector<BYTE> buf(c->inLen + 64, 0);
    while (!g_stop) {
        DWORD rd = 0;
        if (ReadFile(c->h, buf.data(), (DWORD)buf.size(), &rd, NULL) && rd >= 1) {
            c->iHits++;
            std::vector<BYTE> v(buf.begin(), buf.begin() + rd);
            if ((int)c->iState.size() < kCap) {
                if (c->iState.empty() || *c->iState.rbegin() != v) {
                    c->iState.insert(v);
                    c->iTime.push_back(GetTickCount64()/1000.0);
                }
            }
        } else {
            Sleep(200);
        }
    }
    return 0;
}

// ---------- discover collections ----------
static void EnumerateCollections(USHORT tVid, USHORT tPid) {
    GUID guid; HidD_GetHidGuid(&guid);
    g_hdevInfo = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (g_hdevInfo == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA did; did.cbSize = sizeof(did);
    int colIdx = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(g_hdevInfo, NULL, &guid, i, &did); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(g_hdevInfo, &did, NULL, 0, &need, NULL);
        std::vector<wchar_t> dbuf(need/(DWORD)sizeof(wchar_t)+2);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)dbuf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(g_hdevInfo, &did, detail, need, NULL, NULL)) continue;

        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ|FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        bool match = false;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == tVid && attr.ProductID == tPid)
            match = true;
        PHIDP_PREPARSED_DATA ppd = NULL;
        HIDP_CAPS caps; bool gotCaps = false;
        if (HidD_GetPreparsedData(h, &ppd)) {
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) gotCaps = true;
        }
        if (match && gotCaps) {
            Collection c; memset(&c,0,sizeof(c));
            c.idx = colIdx++; c.vid=tVid; c.pid=tPid;
            c.usagePage = caps.UsagePage; c.usage = caps.Usage;
            c.featLen = caps.FeatureReportByteLength;
            c.inLen   = caps.InputReportByteLength;
            c.hasFeature = c.featLen > 0;
            c.hasInput   = c.inLen   > 0;
            c.h = h;
            c.thread = NULL;
            // discover feature report ids from preparsed value/button caps
            if (c.hasFeature) {
                USHORT nVal=0; HidP_GetValueCaps(HidP_Feature, NULL, &nVal, ppd);
                if (nVal) {
                    std::vector<HIDP_VALUE_CAPS> vc(nVal);
                    if (HidP_GetValueCaps(HidP_Feature, vc.data(), &nVal, ppd)==HIDP_STATUS_SUCCESS)
                        for (auto& v: vc) c.featRids.insert(v.ReportID);
                }
                USHORT nBtn=0; HidP_GetButtonCaps(HidP_Feature, NULL, &nBtn, ppd);
                if (nBtn) {
                    std::vector<HIDP_BUTTON_CAPS> bc(nBtn);
                    if (HidP_GetButtonCaps(HidP_Feature, bc.data(), &nBtn, ppd)==HIDP_STATUS_SUCCESS)
                        for (auto& v: bc) c.featRids.insert(v.ReportID);
                }
                // "raw" config collections often omit caps; always also scan 1..max
            }
            g_cols.push_back(c);
            if (!c.hasFeature && !c.hasInput) CloseHandle(h); // nothing to do, drop handle
            HidD_FreePreparsedData(ppd);
            continue; // keep handle if feature/input present
        }
        CloseHandle(h);
        if (gotCaps) HidD_FreePreparsedData(ppd);
    }
}

// ---------- calibration: find working buffer size per rid ----------
static void Calibrate(Collection& c) {
    if (!c.hasFeature) return;
    // candidate rids: caps-discovered, else 1..kScanRidMax
    std::vector<UCHAR> rids(c.featRids.begin(), c.featRids.end());
    if (rids.empty()) for (UCHAR r=1; r<=kScanRidMax; r++) rids.push_back(r);
    for (UCHAR rid : rids) {
        int size = 0;
        // try a few plausible sizes; the device usually dictates one
        for (int n : { (int)c.featLen, 16, 32, 64, 128, 132, 256 }) {
            if (n > kMaxFeatLen || n < 2) continue;
            std::vector<BYTE> b;
            int r = GetFeatureBounded(c.h, rid, b, n);
            if (r > 0) { size = n; break; }
            if (r == -2) break; // timed out -> skip this rid entirely
        }
        if (size > 0) c.fSize[rid] = size;
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: hid_probe <VID> <PID> [seconds]\n");
        printf("  VID/PID decimal or 0x-hex, e.g. 1D57 FA60\n");
        return 1;
    }
    auto parse = [](const char* s)->USHORT {
        if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) return (USHORT)strtoul(s+2,0,16);
        return (USHORT)atoi(s);
    };
    USHORT vid = parse(argv[1]), pid = parse(argv[2]);
    int windowSec = (argc>=4) ? atoi(argv[3]) : 60;
    if (windowSec <= 0) windowSec = 60;

    printf("=== HID capture/scan probe ===\n");
    printf("Target VID=0x%04X PID=0x%04X  window=%ds  (ESC to stop early)\n\n", vid, pid, windowSec);

    EnumerateCollections(vid, pid);
    if (g_cols.empty()) {
        printf("No matching device found. Check VID/PID and that the mouse is plugged in.\n");
        if (g_hdevInfo != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(g_hdevInfo);
        return 1;
    }

    printf("Topology:\n");
    for (auto& c : g_cols) {
        printf("  col#%d  UP=0x%04X usage=0x%04X  feat=%u  in=%u  %s%s\n",
            c.idx, c.usagePage, c.usage, c.featLen, c.inLen,
            c.hasFeature?"[FEATURE]":"", c.hasInput?"[INPUT]":"");
    }
    printf("\n");

    printf("Calibrating report sizes (this may take a few seconds)...\n");
    for (auto& c : g_cols) Calibrate(c);
    for (auto& c : g_cols) {
        if (!c.hasFeature) continue;
        printf("  col#%d feature rids live:", c.idx);
        for (auto& kv : c.fSize) printf(" 0x%02X(%d)", kv.first, kv.second);
        if (c.fSize.empty()) printf(" none (will scan 1..%d anyway)", kScanRidMax);
        printf("\n");
    }
    printf("\n");

    // start input threads
    for (auto& c : g_cols) {
        if (c.hasInput) {
            c.thread = CreateThread(NULL, 0, InputThread, &c, 0, NULL);
            if (c.thread) printf("  started input capture for col#%d\n", c.idx);
        }
    }

    printf("\n>>> NOW change the setting in the factory software (drag slider / cycle colors).\n");
    printf(">>> The probe records every distinct coherent state it sees.\n\n");

    ULONGLONG t0 = GetTickCount64();
    ULONGLONG lastPrint = 0;
    while ((GetTickCount64() - t0) < (ULONGLONG)windowSec*1000) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { printf("\n--- ESC ---\n"); break; }
        for (auto& c : g_cols) {
            if (!c.hasFeature) continue;
            std::vector<UCHAR> rids;
            for (auto& kv : c.fSize) rids.push_back(kv.first);
            if (rids.empty()) for (UCHAR r=1; r<=kScanRidMax; r++) rids.push_back(r);
            for (UCHAR rid : rids) {
                int n = c.fSize.count(rid) ? c.fSize[rid] : (int)c.featLen;
                if (n <= 0) n = 132;
                std::vector<BYTE> b;
                int r = GetFeatureBounded(c.h, rid, b, n);
                if (r <= 0) continue;
                c.fHits[rid]++;
                // keep only coherent-looking states: not all-zero, not a single repeated byte
                bool allSame = true;
                for (size_t i=1;i<b.size();i++) if (b[i]!=b[0]) { allSame=false; break; }
                if (allSame && b[0]==0x00) continue;
                if ((int)c.fState[rid].size() < kCap) {
                    auto it = c.fState[rid].find(b);
                    if (it == c.fState[rid].end()) {
                        c.fState[rid].insert(b);
                        c.fTime[rid].push_back((GetTickCount64()-t0)/1000.0);
                    }
                }
            }
        }
        ULONGLONG el = GetTickCount64() - t0;
        if (el - lastPrint >= 1000) {
            lastPrint = el;
            printf("\r  T+%.0fs (ESC to stop)   ", el/1000.0); fflush(stdout);
        }
        Sleep(15);
    }
    printf("\n\n=== Captured summary ===\n");

    // candidate feature states: low distinct count (<40) means structured config
    bool anyFeature = false;
    for (auto& c : g_cols) {
        if (!c.hasFeature) continue;
        for (auto& kv : c.fState) {
            UCHAR rid = kv.first;
            int cnt = (int)kv.second.size();
            if (cnt == 0) continue;
            anyFeature = true;
            printf("\ncol#%d rid=0x%02X  distinct=%d\n", c.idx, rid, cnt);
            if (cnt <= 40) {
                int idx = 0;
                for (auto& st : kv.second) {
                    double t = (idx < (int)c.fTime[rid].size()) ? c.fTime[rid][idx] : 0;
                    printf("   T+%.1fs  %s\n", t, hexline(st.data(), (int)st.size()).c_str());
                    idx++;
                }
            } else {
                // too many states: just show first + variation map
                auto it = kv.second.begin();
                printf("   (too many; showing first) %s\n", hexline(it->data(),(int)it->size()).c_str());
            }
        }
    }
    if (!anyFeature) printf("(no feature states captured)\n");

    // input states
    bool anyInput = false;
    for (auto& c : g_cols) {
        if (!c.hasInput) continue;
        if (c.iState.empty()) continue;
        anyInput = true;
        printf("\ncol#%d INPUT distinct=%d  (battery/monitor traffic)\n", c.idx, (int)c.iState.size());
        int idx = 0;
        for (auto& st : c.iState) {
            double t = (idx < (int)c.iTime.size()) ? c.iTime[idx] : 0;
            printf("   T+%.1fs  %s\n", t, hexline(st.data(),(int)st.size()).c_str());
            idx++;
            if (idx >= 20) { printf("   ... (%d more)\n", (int)c.iState.size()-idx); break; }
        }
    }
    if (!anyInput) printf("(no input states captured)\n");

    // write log file
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string path = "hid_probe_" + nowstamp() + ".txt";
    std::wstring wpath = std::wstring(tmp) + std::wstring(path.begin(), path.end());
    FILE* f = _wfopen(wpath.c_str(), L"w");
    if (f) {
        fprintf(f, "HID capture log  VID=0x%04X PID=0x%04X\n", vid, pid);
        for (auto& c : g_cols) {
            fprintf(f, "\n[topology] col#%d UP=0x%04X feat=%u in=%u\n", c.idx, c.usagePage, c.featLen, c.inLen);
            if (c.hasFeature) for (auto& kv : c.fState) {
                fprintf(f, "\n[col#%d rid=0x%02X distinct=%d]\n", c.idx, kv.first, (int)kv.second.size());
                int idx=0;
                for (auto& st : kv.second) {
                    double t = (idx<(int)c.fTime[kv.first].size())?c.fTime[kv.first][idx]:0;
                    fprintf(f, "  T+%.1fs %s\n", t, hexline(st.data(),(int)st.size()).c_str());
                    idx++;
                }
            }
            if (c.hasInput) for (auto& st : c.iState)
                fprintf(f, "[col#%d INPUT] %s\n", c.idx, hexline(st.data(),(int)st.size()).c_str());
        }
        fclose(f);
        printf("\nLog written: %s\n", wpath.c_str());
    }

    g_stop = true;
    Sleep(300);
    for (auto& c : g_cols) if (c.h != INVALID_HANDLE_VALUE) CloseHandle(c.h);
    if (g_hdevInfo != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(g_hdevInfo);
    printf("Done. Send the .txt to the agent for analysis.\n");
    return 0;
}
