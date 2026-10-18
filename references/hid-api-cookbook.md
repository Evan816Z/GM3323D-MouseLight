# HID API Cookbook (Win32, C++17)

Copy-paste snippets for the operations an off-brand-mouse driver needs. All are adapted from the
working GM3323D reference app (`assets/main.cpp`). Link libs:
`-lsetupapi -lhid -lcomdlg32 -lshell32 -ladvapi32 -lgdi32 -luser32`.

## 1. Enumerate + open the Feature collection (VID/PID match)

```cpp
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>

static HANDLE OpenFeatureDevice(USHORT vid, USHORT pid, int minFeatLen) {
    GUID guid; HidD_GetHidGuid(&guid);
    HDEVINFO hdev = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA did; did.cbSize = sizeof(did);
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hdev, NULL, &guid, idx, &did); idx++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0, &need, NULL);
        std::vector<wchar_t> buf(need / (DWORD)sizeof(wchar_t) + 2);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, NULL, NULL)) continue;

        // match with read-only open first (avoid exclusive conflicts)
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool match = false;
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid && attr.ProductID == pid) {
            PHIDP_PREPARSED_DATA ppd = NULL;
            if (HidD_GetPreparsedData(h, &ppd)) {
                HIDP_CAPS caps;
                if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
                    if (caps.FeatureReportByteLength >= (ULONG)minFeatLen) match = true;
                HidD_FreePreparsedData(ppd);
            }
        }
        CloseHandle(h);
        if (!match) continue;
        HANDLE rw = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (rw != INVALID_HANDLE_VALUE) { result = rw; break; }
    }
    SetupDiDestroyDeviceInfoList(hdev);
    return result;
}
```

## 2. Timeout-bounded GetFeature (NEVER call raw — hangs on bad report IDs)

```cpp
struct FeatArg { HANDLE h; UCHAR rid; BYTE* buf; int n; BOOL* ok; };
static DWORD WINAPI GetFeatThr(LPVOID p) {
    auto* a = (FeatArg*)p;
    a->buf[0] = a->rid;
    *(a->ok) = HidD_GetFeature(a->h, a->buf, a->n);
    return 0;
}
// returns bytes read, or -1 on timeout/failure
static int GetFeatureBounded(HANDLE h, UCHAR rid, std::vector<BYTE>& out, int n, DWORD ms = 200) {
    out.assign(n, 0);
    BOOL ok = FALSE;
    FeatArg a{h, rid, out.data(), n, &ok};
    HANDLE th = CreateThread(NULL, 0, GetFeatThr, &a, 0, NULL);
    if (!th) return -1;
    if (WaitForSingleObject(th, ms) == WAIT_TIMEOUT) {
        // can't cancel HidD_GetFeature reliably; just give up on this rid
        CloseHandle(th);
        return -1; // mark rid as "skip"
    }
    CloseHandle(th);
    return ok ? n : -1;
}
```

## 3. ReadFeature → patch known fields → SetFeature (preserve unknown bytes)

```cpp
static const BYTE kSig0 = 0x05, kSig1 = 0x0D;
static bool ReadConfig(HANDLE dev, BYTE* out, int len) {
    for (int i = 0; i < 24; i++) {              // config is transient: retry
        std::vector<BYTE> b(len, 0); b[0] = kReportID;
        if (HidD_GetFeature(dev, b.data(), len) && b[0] == kSig0 && b[1] == kSig1) {
            memcpy(out, b.data(), len); return true;
        }
        if (i < 23) Sleep(60);
    }
    return false;
}
static bool ApplyConfig(HANDLE dev, const BYTE* patch, int len) {
    BYTE buf[len];
    if (!ReadConfig(dev, buf, len)) memcpy(buf, kDefaultTemplate, len); // fallback
    memcpy(buf + OFF_KNOWN_START, patch, OFF_KNOWN_COUNT);              // only known bytes
    return HidD_SetFeature(dev, buf, len) != FALSE;
}
```

## 4. Battery read on a blocking Input thread (idle CPU = 0)

```cpp
static HANDLE g_mon = INVALID_HANDLE_VALUE;     // UsagePage 0x000A collection
static DWORD WINAPI BatteryThread(LPVOID) {
    int last = -1;
    for (;;) {
        if (g_mon == INVALID_HANDLE_VALUE) {
            g_mon = OpenMonitorCollection(0x000A);   // like OpenFeatureDevice but match UsagePage
            if (g_mon == INVALID_HANDLE_VALUE) { Sleep(3000); continue; }
            last = -1;
        }
        BYTE buf[64] = {0}; DWORD rd = 0;
        if (ReadFile(g_mon, buf, sizeof(buf), &rd, NULL) && rd >= 5) {
            if (buf[0] == 0x03 && buf[4] <= 10) {     // 03 00 40 01 NN  -> NN*10 %
                int pct = buf[4] * 10;
                if (pct != last) { last = pct; g_battery = pct; PostMessageW(g_hwnd, WM_APP_BATT, 0, 0); }
            }
        } else {
            CloseHandle(g_mon); g_mon = INVALID_HANDLE_VALUE; Sleep(2000);  // device unplugged
        }
    }
}
```

## 5. Draw a battery % tray icon at runtime (no .ico needed)

Use `BITMAPV5HEADER` + `CreateDIBSection`, paint a 16×16 battery with the percentage in a 3×5
pixel font, then `CreateIconIndirect`. Full implementation in `assets/main.cpp` (`MakeBatteryIcon`).
Key points: set `bi.bV5AlphaMask = 0xFF000000`; write pixels as `0xFF000000 | (B<<16)|(G<<8)|R`;
transparent = `0x00000000`.

## 6. Single instance + hidden window (for WM_DEVICECHANGE)

```cpp
HANDLE mx = CreateMutexW(NULL, TRUE, L"Local\\MyMouseDriver_SingleInstance_2026");
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    PostMessageW(HWND_BROADCAST, g_msgWake, 0, 0);  // wake existing instance
    return 0;
}
// hidden toolwindow so it never shows in Alt-Tab but still gets device events
g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Cls", L"", WS_POPUP, 0,0,0,0, NULL, NULL, hInst, NULL);
DEV_BROADCAST_DEVICEINTERFACE_W f{0}; f.dbcc_size = sizeof(f);
f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE; HidD_GetHidGuid(&f.dbcc_classguid);
RegisterDeviceNotificationW(g_hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
```

On `WM_DEVICECHANGE` with `w == DBT_DEVICEARRIVAL`: close old handle, reopen, re-`ApplyConfig()`;
on `DBT_DEVICEREMOVECOMPLETE`: close the handle. No polling.

## 7. Registry config (save / load / autostart)

```cpp
static void RegSetDWORD(const wchar_t* name, DWORD v) {
    HKEY hk; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MyMouseDriver", 0, NULL, 0,
        KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(hk);
    }
}
// autostart via HKCU\Software\Microsoft\Windows\CurrentVersion\Run: write the quoted exe path (enable)
// or RegDeleteValueW the named value (disable).
```

## 8. Build gotchas

- **`windres` needs `--use-temp-file`** under MinGW or it errors: `windres --use-temp-file app.rc -O coff -o app_res.o`.
- **Keep `.bat` files ASCII.** Chinese in a batch file is mangled by the GBK console and the
  commands fail with cryptic errors. `build.bat`/`clean.bat` in `assets/` are ASCII and
  auto-detect g++ / clang++ / cl.
- Link with `-static -s -Wl,--gc-sections -ffunction-sections -fdata-sections` → ~300 KB.
- If the device is silent after a write, flip the **write-format** toggle (first byte = report-id
  vs first byte = raw data) — see `references/protocol-discovery.md` §4.
