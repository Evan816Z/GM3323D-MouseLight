// GM3323D DPI locator v4 - CONTROLLED capture
// Reads feature report 0x04 on col#2 (UP=0x000B), which mirrors the device
// config. Catches the transient coherent '04 32 01 08' (CFG_A) block that
// carries the DPI candidate at byte[10] (mirrored at [18]) and the
// return-rate byte at [24]. User sets EXACT DPI values with pauses so we can
// map the byte to real DPI. Prints a live decode and logs every distinct
// coherent state to dpi_probe_<ts>.txt.
#include <windows.h>
#include <initguid.h>
#include <hidsdi.h>
#include <setupapi.h>

// HID class interface GUID (allocating definition, requires initguid.h above)
DEFINE_GUID(GUID_DEVINTERFACE_HID, 0x4d1e55b2L, 0xf16f, 0x11cf, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static const DWORD VID = 0x1D57, PID = 0xFA60;
static const int kCap = 600;
static const ULONGLONG kWindowMs = 120000; // 2 minutes, ESC to stop early

struct Col {
    HANDLE h = INVALID_HANDLE_VALUE;
    int idx = 0;
    std::vector<UCHAR> featRids;
    std::map<UCHAR,int> fHits;   // live rid size (<=0 = not live)
    std::set<std::vector<BYTE>> fState;
};

static std::string BufHex(const BYTE* b, int n) {
    std::string s; char t[4];
    for (int i=0;i<n;i++){ snprintf(t,4,"%02x ",b[i]); s+=t; }
    return s;
}
static void logln(const std::string& s){
    printf("%s\n", s.c_str()); fflush(stdout);
}
static std::string ts(ULONGLONG ms){
    char t[16]; snprintf(t,16,"T+%.1fs", ms/1000.0); return t;
}

struct FeatArg { Col* c; UCHAR rid; BYTE* buf; int n; BOOL* ok; };

// time-bounded GetFeature
static DWORD WINAPI GetFeatThr(LPVOID p){
    FeatArg* a=(FeatArg*)p;
    a->buf[0]=a->rid;
    *(a->ok)=HidD_GetFeature(a->c->h, a->buf, a->n);
    return 0;
}
static int GetFeat(Col* c, UCHAR rid, std::vector<BYTE>& out){
    int n=c->fHits[(UCHAR)rid]; if(n<=0) return -1;
    std::vector<BYTE> b(n,0);
    BOOL ok=FALSE;
    FeatArg a={c,rid,b.data(),n,&ok};
    HANDLE th=CreateThread(NULL,0,GetFeatThr,&a,0,NULL);
    if(!th) return -1;
    DWORD wr=WaitForSingleObject(th,200);
    if(wr==WAIT_TIMEOUT){ /* still pending; treat as no-hit */ }
    else { CloseHandle(th); }
    if(!ok) return 0;
    out=b; return n;
}

// classify a buffer
enum Kind { FLASH, CFG_A, CFG_B, LIGHT };
static Kind classify(const std::vector<BYTE>& b){
    if(b.size()<4) return FLASH;
    if(b[1]==0x32 && b[2]==0x01 && b[3]==0x08) return CFG_A;
    if(b[0]==0x23 && b[1]==0x43 && b[2]==0x1e && b[3]==0xde) return CFG_B;
    if(b[1]==0x0d && b[2]==0x01) return LIGHT;
    return FLASH;
}

int main(){
    // log file
    char tm[32];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(tm,32,"dpi_probe_%04d%02d%02d_%02d%02d%02d.txt",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    std::string logpath = std::string(getenv("TEMP")?getenv("TEMP"):".") + "\\" + tm;
    FILE* lf = fopen(logpath.c_str(),"w");
    if(!lf){ logpath = tm; lf=fopen(logpath.c_str(),"w"); }

    logln("=== GM3323D DPI locator v4 (controlled) ===");
    fprintf(lf,"=== GM3323D DPI locator v4 (controlled) ===\n");

    // enumerate collections
    HDEVINFO hdi = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    std::vector<Col> cols;
    int idx=0;
    for(DWORD i=0;;i++){
        SP_DEVICE_INTERFACE_DATA did; did.cbSize=sizeof(did);
        if(!SetupDiEnumDeviceInterfaces(hdi,NULL,&GUID_DEVINTERFACE_HID,i,&did)) break;
        DWORD sz=0; SetupDiGetDeviceInterfaceDetail(hdi,&did,NULL,0,&sz,NULL);
        std::vector<BYTE> buf(sz);
        SP_DEVICE_INTERFACE_DETAIL_DATA* dd=(SP_DEVICE_INTERFACE_DETAIL_DATA*)buf.data();
        dd->cbSize=sizeof(*dd);
        if(!SetupDiGetDeviceInterfaceDetail(hdi,&did,dd,sz,NULL,NULL)) continue;
        HANDLE h=CreateFile(dd->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(h==INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr; attr.Size=sizeof(attr);
        if(!HidD_GetAttributes(h,&attr)){ CloseHandle(h); continue; }
        if(attr.VendorID!=VID || attr.ProductID!=PID){ CloseHandle(h); continue; }
        PHIDP_PREPARSED_DATA ppd=NULL;
        if(!HidD_GetPreparsedData(h,&ppd)){ CloseHandle(h); continue; }
        HIDP_CAPS cap; HidP_GetCaps(ppd,&cap);
        // collection has feature reports?
        int featLen = (int)cap.FeatureReportByteLength;
        if(featLen>=64){
            cols.push_back(Col()); cols.back().h=h; cols.back().idx=idx++;
            // scan rids 1..32 for live ones
            for(UCHAR r=1;r<=32;r++){
                std::vector<BYTE> b(featLen,0); b[0]=r;
                BOOL ok=HidD_GetFeature(h,b.data(),featLen);
                if(ok) cols.back().fHits[r]=featLen;
            }
        } else { CloseHandle(h); }
        HidD_FreePreparsedData(ppd);
    }
    if(cols.empty()){
        logln("no matching device found. Make sure the mouse is plugged in and close the factory app.");
        if(lf) fclose(lf);
        return 1;
    }
    logln("device opened. collections with feature reports:");
    for(auto& c: cols){
        std::string s="  col#"+std::to_string(c.idx)+" live rids:";
        for(auto& kv:c.fHits) if(kv.second>0){ char t[8]; snprintf(t,8," 0x%02X",kv.first); s+=t; }
        logln(s);
        fprintf(lf,"%s\n",s.c_str());
    }

    // pick the collection with the most live rids
    Col* dev=NULL; int best=0;
    for(auto& c: cols){ int n=0; for(auto&kv:c.fHits) if(kv.second>0) n++; if(n>best){best=n;dev=&c;} }
    if(!dev){ logln("no live feature rids?"); if(lf)fclose(lf); return 1; }

    // choose rid to poll: prefer 0x04, else first live
    UCHAR pollRid=0x04;
    if(dev->fHits[pollRid]<=0){ for(auto&kv:dev->fHits) if(kv.second>0){pollRid=kv.first;break;} }
    logln(std::string("polling rid=0x")+(pollRid<0x10?"0":"")+std::to_string(pollRid));
    fprintf(lf,"polling rid=0x%02X\n",pollRid);

    logln("");
    logln(">>> CONTROLLED EXPERIMENT <<<");
    logln("Leave the factory app OPEN on the DPI page.");
    logln("Set DPI #1 to EXACTLY these values, pausing ~3s after each:");
    logln("   1) 400    2) 800    3) 1600   4) 3200   5) 5100");
    logln("Watch the CFG_A line below update. Then press ESC to stop & save.");
    logln("");

    std::set<std::vector<BYTE>> seenA, seenB;
    std::vector<BYTE> lastFull;
    Kind lastKind=FLASH;
    int liveCount=0;
    ULONGLONG t0=GetTickCount64();
    ULONGLONG lastPrint=0;
    std::string lastLiveLine;

    while(GetTickCount64()-t0 < kWindowMs){
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000){ logln("--- ESC ---"); break; }
        std::vector<BYTE> b;
        int n=GetFeat(dev,pollRid,b);
        if(n<=0) continue;
        Kind k=classify(b);
        if(k==FLASH) { Sleep(1); continue; }
        // coherent snapshot
        if(k==CFG_A){
            if(seenA.find(b)!=seenA.end()){ // already logged, but still update live line
                // update live decode line throttled
            } else {
                seenA.insert(b);
                ULONGLONG el=GetTickCount64()-t0;
                char line[256];
                snprintf(line,256,"CFG_A @%s: [6][7]=%02x %02x  [10]=%02x(%d)  [18]=%02x(%d)  [24]=%02x   full[:24]=%s",
                    ts(el).c_str(), b[6],b[7], b[10],b[10], b[18],b[18], b[24],
                    BufHex(b.data(),24).c_str());
                logln(line);
                fprintf(lf,"%s\n",line);
                fprintf(lf,"   CFG_A full: %s\n", BufHex(b.data(),(int)b.size()).c_str());
            }
            // live decode line (throttled)
            char ll[128];
            snprintf(ll,128,"\rCFG_A live: [6][7]=%02x %02x  [10]=%02x(%d)  [18]=%02x(%d)  [24]=%02x   ",
                b[6],b[7], b[10],b[10], b[18],b[18], b[24]);
            lastLiveLine=ll;
            ULONGLONG el=GetTickCount64()-t0;
            if(el-lastPrint>=120){ lastPrint=el; printf("%s",ll); fflush(stdout); }
        } else if(k==CFG_B){
            if(seenB.find(b)==seenB.end()){
                seenB.insert(b);
                ULONGLONG el=GetTickCount64()-t0;
                std::string line="CFG_B @";
                line+=ts(el);
                // print tail region (the varying part is near the end before 7d 9e 6d 6d 41)
                int tail=64; if((int)b.size()>tail) tail=(int)b.size();
                line += "  tail(=-12): "+BufHex(b.data()+(b.size()<12?0:b.size()-12), (b.size()<12?(int)b.size():12));
                logln(line);
                fprintf(lf,"%s\n",line.c_str());
                fprintf(lf,"   CFG_B full: %s\n", BufHex(b.data(),(int)b.size()).c_str());
            }
        } else if(k==LIGHT){
            // just note it happened; not our target
        }
        lastKind=k; lastFull=b;
        Sleep(1);
    }
    printf("\n");

    logln("");
    logln("=== SUMMARY ===");
    char s1[64]; snprintf(s1,64,"distinct CFG_A states: %d", (int)seenA.size());
    logln(s1); fprintf(lf,"%s\n",s1);
    char s2[64]; snprintf(s2,64,"distinct CFG_B states: %d", (int)seenB.size());
    logln(s2); fprintf(lf,"%s\n",s2);
    logln("log saved: "+logpath);
    fprintf(lf,"=== END ===\n");
    if(lf) fclose(lf);
    logln("press any key to exit...");
    getchar();
    return 0;
}
