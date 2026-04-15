#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <stdarg.h>
#include <algorithm> // for std::min_element, std::max_element
#include <TlHelp32.h> // for thread enumeration (HW breakpoint hook)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

//  log â”€
static std::vector<std::string> g_logLines;
static bool g_logScroll = true;
static void Log(const char* fmt, ...) {
    char buf[512];
    va_list a; va_start(a,fmt); vsnprintf(buf,sizeof(buf),fmt,a); va_end(a);
    g_logLines.push_back(std::string(buf));
    if (g_logLines.size()>300) g_logLines.erase(g_logLines.begin());
}

//  offsets â”€
// NameDllOff: offset inside citizen-playernames-five.dll where a
//   { uint32_t count; uint8_t pad[4]; uintptr_t listHead; } pair lives.
//   0 = unknown for this build â€” runtime scanner will find it automatically
//   and log "[Names] Scan found: ... off=0x?????" so you can fill it in.
struct FiveMOffsets {
    const char* name; int build;
    //  global (base + offset = ptr) 
    uintptr_t World, ReplayInterface, Viewport, Camera;
    //  ped member offsets (ped + offset) â”€
    uintptr_t PlayerInfo, PlayerID, Health, Armor, BoneManager;
    uintptr_t EntityType, FrameFlag, VisibleFlag, ConfigFlags;
    uintptr_t VehicleInterface, WeaponManager;
    uintptr_t MaxHealth, GodMode, Blip;
    //  name DLL 
    uintptr_t NameDllOff;       // 0 = auto-scan
    //  ped interface chain (changed b3258+) 
    uintptr_t PedIfaceOff;      // replay + this = CPedInterface*
    uintptr_t PedArrayOff;      // CPedInterface + this = ped array ptr
    uintptr_t PedCountOff;      // CPedInterface + this = uint16 count
    //  function pointers (base + offset = fn ptr) 
    uintptr_t HandleBullet;     // magic bullet hook target
    uintptr_t RequestRagdoll;   // instant ragdoll call (0 = use task hack)
    uintptr_t GameplayCamOff;   // Camera + this = CGameplayCamDirector ptr
    //  misc per-build offsets 
    uintptr_t WaypointOff;      // base + this = Vec3 waypoint world pos
    uintptr_t PedTaskOff;       // ped + this = task field (write 0 = TASK_NONE)
    uintptr_t FreecamByte;      // base + this = byte, write 1 to disable game camera update
    uintptr_t CamTarget;        // base + this = Vec3 world pos camera is looking at
    uintptr_t CamHolder;        // base + this = CGameplayCamDirector* directly (bypasses Camera+0x3C0 chain)
    uintptr_t AimedCPed;        // base + this = write target ped ptr here for silent aim (b3258: 0x202C8D0)
};

// Sources: UnknownCheats thread #340232 pages 1-198 (scraped Apr 2026)
// 0 = unknown for this build / use pattern scan / not applicable
static const FiveMOffsets g_offsets[] = {
// b1604 â”€
// World Replay       Viewport     Camera
// PInfo PID    HP      Armor  Bone   EntityT  FrameF   VisFlag  CfgFlag
// VehIf  WepMgr  MaxHP  God   Blip
// NameDll  PedIf  PedArr PedCnt
// HandleBullet  ReqRagdoll  CamOff   WaypointOff  PedTaskOff
{"b1604",1604,
 0x247F840,0x1EC3828,0x2087780,0x1F6B940,
 0x10C8,0x88,0x280,0x14B8,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x0,
 0,  0x18,0x100,0x110,
 0xFE7EF8,0,0x3C0, 0,0x146B,0, 0},

// b2060
{"b2060",2060,
 0x24C8858,0x1EC3828,0x1F6A7E0,0x1F6B3C0,
 0x10C8,0x88,0x280,0x14B8,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x0,
 0,  0x18,0x100,0x110,
 0,0,0x3C0, 0,0x146B,0, 0},

// b2189
{"b2189",2189,
 0x24E6D90,0x1EE18A8,0x1F888C0,0x1F89498,
 0x10C8,0x88,0x280,0x14B8,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x0,
 0,  0x18,0x100,0x110,
 0,0,0x3C0, 0,0x146B,0, 0},

// b2372 â”€
{"b2372",2372,
 0x252DCD8,0x1F05208,0x1F9E9F0,0x1F9F898,
 0x10C8,0x88,0x280,0x14B8,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x0,
 0,  0x18,0x100,0x110,
 0xFF716C,0,0x3C0, 0,0x146B,0},

// b2545 â”€
{"b2545",2545,
 0x2593320,0x1F58B58,0x20019E0,0x20025B8,
 0x10A8,0x88,0x280,0x150C,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x1FDF560,
 0,  0x18,0x100,0x110,
 0,0,0x3C0, 0,0x146B,0, 0},

// b2612
{"b2612",2612,
 0x2567DB0,0x1F77EF0,0x1FD8570,0x1FD9148,
 0x10A8,0x88,0x280,0x150C,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x0,
 0,  0x18,0x100,0x110,
 0,0,0x3C0, 0,0x146B,0, 0},

// b2699 â”€
{"b2699",2699,
 0x26684D8,0x20304C8,0x20D8C90,0x20D9B38,
 0x10A8,0x88,0x280,0x150C,0x410,   0x10B8,0x0,0x2C,0x1414,
 0xD10,0x10D8, 0x284,0x189,0x20E1420,
 0,  0x18,0x100,0x110,
 0xFF9D90,0,0x3C0, 0x20F9750,0x146B,0, 0},

// b2802 â”€
{"b2802",2802,
 0x254D448,0x1F5B820,0x1FBC100,0x1FBCFA8,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x2C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x1FBD6E0,
 0,  0x18,0x100,0x110,
 0x1003F80,0,0x3C0, 0,0x144B,0},

// b2944 â”€
{"b2944",2944,
 0x257BEA0,0x1F42068,0x1FEAAC0,0x1FEB968,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x2C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x1FF3130,
 0,  0x18,0x100,0x110,
 0x100F5A4,0,0x3C0, 0,0x144B,0},

// b3095 â”€
{"b3095",3095,
 0x2593320,0x1F58B58,0x20019E0,0x2002888,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x2C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x2002FA0,
 0x30D38,  0x18,0x100,0x110,
 0x100F5A4,0,0x3C0, 0x2022DE0,0x144B,0, 0x20D9E28},

// b3337 â”€
{"b3337",3337,
 0x25C15B0,0x1F85458,0x202DC50,0x202E878,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x2C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x2008120,
 0x30D38,  0x100,0x108,0x118,
 0x102FF8C,0,0x3C0, 0x20333E0,0x144B,0},

// b3323 (same binary as b3337)
{"b3323",3323,
 0x25C15B0,0x1F85458,0x202DC50,0x202E878,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x145C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x2008120,
 0x30D38,  0x100,0x108,0x118,
 0x102FF8C,0,0x3C0, 0x20333E0,0x144B,0},

// b3258 â”€
// OFFSETS.txt: b3258+ use new ped chain (replay+0x100 / pi+0x108 / pi+0x118)
{"b3258",3258,
 0x25B14B0,0x1FBD4F0,0x201DBA0,0x201E7D0,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x145C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x2023400,
 0x30D38,  0x100,0x108,0x118,
 0x101A5F4,0x11AECD0,0x3C0, 0x2022DD0,0x144B,0x27BD43, 0x2002B78, 0x201ED90, 0x202C8D0},

// b3407 â”€
{"b3407",3407,
 0x25D7108,0x1F9A9D8,0x20431C0,0x2043DF8,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x145C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x2047D50,
 0x30D38,  0x100,0x108,0x118,
 0x102FF8C,0,0x3C0, 0,0x144B,0},

// b3507 â”€
{"b3507",3507,
 0x25EC580,0x1FB0418,0x2058BA0,0x2059778,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x145C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x205E000,
 0x30D38,  0x100,0x108,0x118,
 0,0,0x3C0, 0,0x144B,0},

// b3570
{"b3570",3570,
 0x2603908,0x1FC38A8,0x206C060,0x206D1C0,
 0x10A8,0xE8,0x280,0x150C,0x430,   0x1098,0x270,0x145C,0x1444,
 0xD10,0x10B8, 0x284,0x189,0x206D600,
 0x30D38,  0x100,0x108,0x118,
 0x102B560,0,0x3C0, 0,0x144B,0},
};
static const int     g_offsets_count   = sizeof(g_offsets)/sizeof(g_offsets[0]);
static FiveMOffsets* g_current_offsets = nullptr;
static int           g_detected_build  = -1;
static uintptr_t     g_replayIfaceOverride = 0; // non-zero = override g_current_offsets->ReplayInterface
static uintptr_t     g_worldOverride = 0;        // non-zero = override g_current_offsets->World
static uintptr_t     g_viewportOverride = 0;      // non-zero = override g_current_offsets->Viewport

//  build detection
static int BuildFromModuleEnum() {
    HMODULE mods[512]; DWORD needed=0;
    if(!EnumProcessModules(GetCurrentProcess(),mods,sizeof(mods),&needed)) return -1;
    int count=(int)(needed/sizeof(HMODULE));
    for(int i=0;i<count;i++){
        char name[MAX_PATH]{};
        if(!GetModuleFileNameA(mods[i],name,MAX_PATH)) continue;
        const char* fn=name;
        for(const char* p=name;*p;p++) if(*p=='\\'||*p=='/') fn=p+1;
        const char* bp=nullptr;
        if(strncmp(fn,"FiveM_b",7)==0) bp=fn+7;
        else if(strncmp(fn,"FiveM_cl2_b",11)==0) bp=fn+11;
        if(bp && isdigit((unsigned char)*bp)){
            int b=0;
            while(isdigit((unsigned char)*bp)){b=b*10+(*bp-'0');bp++;}
            if(b>1000){
                Log("[Build] module enum: %s -> build %d",fn,b);
                return b;
            }
        }
    }
    Log("[Build] no FiveM_bXXXX module found via enum");
    return -1;
}
static int BuildFromGtaCoreFive() {
    // Scan gta-core-five.dll for: 8B 05 ?? ?? ?? ?? 83 F8 FF 75 0B
    // Resolves rip-relative to a static int = the game build the engine is running
    HMODULE hMod=GetModuleHandleA("gta-core-five.dll");
    if(!hMod){ Log("[Build] gta-core-five.dll not loaded"); return -1; }
    MODULEINFO mi{};
    if(!GetModuleInformation(GetCurrentProcess(),hMod,&mi,sizeof(mi))) return -1;
    uint8_t* base=(uint8_t*)mi.lpBaseOfDll;
    size_t size=mi.SizeOfImage;
    for(size_t i=0;i+11<=size;i++){
        if(base[i]==0x8B&&base[i+1]==0x05&&
           base[i+6]==0x83&&base[i+7]==0xF8&&base[i+8]==0xFF&&
           base[i+9]==0x75&&base[i+10]==0x0B){
            int32_t rel=*(int32_t*)(base+i+2);
            int* bp=(int*)(base+i+6+rel);
            MEMORY_BASIC_INFORMATION mbi{};
            if(!VirtualQuery(bp,&mbi,sizeof(mbi))||!(mbi.Protect&0xEE)) continue;
            int b=*bp;
            if(b>1000&&b<10000){
                Log("[Build] gta-core-five: %d @ %p",b,(void*)bp);
                return b;
            }
        }
    }
    Log("[Build] gta-core-five pattern not found");
    return -1;
}
static int BuildFromFileVersion() {
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,path,MAX_PATH)) return -1;
    DWORD d=0,sz=GetFileVersionInfoSizeW(path,&d); if(!sz) return -1;
    void* buf=VirtualAlloc(nullptr,sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); if(!buf) return -1;
    int build=-1;
    if(GetFileVersionInfoW(path,0,sz,buf)){
        VS_FIXEDFILEINFO* fi=nullptr; UINT len=0;
        if(VerQueryValueW(buf,L"\\",(void**)&fi,&len)&&fi){
            build=(int)LOWORD(fi->dwFileVersionLS);
            if(build<=0) build=(int)HIWORD(fi->dwFileVersionLS);
        }
    }
    VirtualFree(buf,0,MEM_RELEASE); return build;
}
static int BuildFromCmdLine() {
    wchar_t* cmd=GetCommandLineW(); if(!cmd) return -1;
    const wchar_t* pats[]={L"-b",L"/b",L"--build=",L"-build="};
    for(auto pat:pats){
        wchar_t* p=wcsstr(cmd,pat); if(!p) continue;
        p+=wcslen(pat); while(*p==L' '||*p==L'=') p++;
        if(iswdigit(*p)){int b=0;while(iswdigit(*p)){b=b*10+(*p-L'0');p++;}if(b>1000)return b;}
    }
    return -1;
}
static int BuildFromPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr,path,MAX_PATH);
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8,0,path,-1,narrow,MAX_PATH,NULL,NULL);
    Log("[Build] exe path: %s",narrow);
    for(size_t i=0;path[i];++i)
        if(wcsncmp(&path[i],L"FiveM_",6)==0){
            size_t o=i+6; if(wcsncmp(&path[o],L"cl2_",4)==0) o+=4;
            if(path[o]==L'b'&&iswdigit(path[o+1])){
                int b=0; size_t j=o+1;
                while(iswdigit(path[j])){b=b*10+(path[j]-L'0');j++;}
                if(b>1000) return b;
            }
        }
    return -1;
}
static int DetectGameBuild() {
    // 1. Module name (FiveM_bXXXX_GTAProcess.exe)
    int build=BuildFromModuleEnum();
    // 2. gta-core-five.dll engine build (what FiveM says the game build is)
    if(build<=1000) build=BuildFromGtaCoreFive();
    // 3. Path / cmdline / PE version
    if(build<=1000) build=BuildFromPath();
    if(build<=1000) build=BuildFromCmdLine();
    if(build<=1000) build=BuildFromFileVersion();
    if(build<=1000){ Log("[Build] all methods failed, defaulting to 3258"); build=3258; }
    Log("Build: %d",build);
    for(int i=0;i<g_offsets_count;i++)
        if(g_offsets[i].build==build){
            g_current_offsets=(FiveMOffsets*)&g_offsets[i];
            g_detected_build=i; Log("Offsets: %s",g_offsets[i].name); return i;
        }
    int best=0;
    for(int i=1;i<g_offsets_count;i++)
        if(g_offsets[i].build<=build&&g_offsets[i].build>g_offsets[best].build) best=i;
    g_current_offsets=(FiveMOffsets*)&g_offsets[best];
    g_detected_build=best; Log("Fallback: %s (build %d, wanted %d)",g_offsets[best].name,g_offsets[best].build,build); return best;
}

//  globals â”€
typedef HRESULT(__stdcall* SteamPresent_t)(IDXGISwapChain*,UINT,UINT);
static SteamPresent_t          oSteamPresent = nullptr;
static ID3D11Device*           g_dev       = nullptr;
static ID3D11DeviceContext*    g_ctx       = nullptr;
static ID3D11RenderTargetView* g_rtv       = nullptr;
static HWND                    g_hwnd      = nullptr;
static bool                    g_init      = false;
static bool                    g_show      = true;
static float                   g_time      = 0.f;
static volatile long           g_init_lock = 0;
static uintptr_t               g_base      = 0;
static float                   g_screenW   = 1920.f;
static float                   g_screenH   = 1080.f;

static bool  g_esp          = true;
static bool  g_espBox       = true;
static bool  g_espName      = true;
static bool  g_espHealth    = true;
static bool  g_espDistance  = true;
static bool  g_espLine      = true;
static bool  g_espNpc       = false;   // draw NPCs too (grey boxes)
static float g_espMaxDist   = 300.f;
static bool  g_aimbot          = false;
static float g_aimbotFov       = 10.f;
static bool  g_aimbotFovCircle  = true;
static float g_aimbotSmooth     = 0.15f;  // fraction of pixel delta per frame
static int   g_aimbotBone       = 7;      // bone index: 7=Head
static int   g_aimbotTargetMode = 0;      // 0=closest to crosshair, 1=closest distance
static bool  g_aimbotAliveOnly  = true;   // only target alive players
static bool  g_aimbotPlayersOnly= true;   // only target players (not NPCs)
static bool  g_godMode      = false;
static bool  g_noWanted     = false;
static bool  g_infArmor      = false;
static bool  g_invisible     = false;
static bool  g_ragdoll       = false;
static bool  g_freecam       = false;
static float g_freecamSpeed  = 45.f;
static bool  g_speedBoost    = false;
static float g_speedMult     = 1.5f;
// Vehicle
static bool  g_vehGod        = false;
static bool  g_vehRepair     = false;
static bool  g_vehBoost      = false;
static float g_vehBoostMult  = 1.5f;
static bool  g_vehLock       = false;
// Teleport
static bool  g_tpWaypoint    = false;
// Spawn
static char  g_spawnModel[64] = "adder";
static bool  g_spawnReq      = false;
// AOB-scanned function ptrs
static uintptr_t g_fnCreateVehicle  = 0;
static uint32_t  g_spawnHashQueued  = 0; // async spawn state
static int       g_spawnFrameWait   = 0;
static uintptr_t g_fnRequestModel   = 0;
static uintptr_t g_fnHasModel       = 0;
static uintptr_t g_fnHandleBullet   = 0;
static uintptr_t g_fnAttachEntity   = 0; // fpAttachEntityToEntity

//  Lua Executor 
// Strategy:
//   1. Pattern-scan luaL_loadbufferx in citizen-scripting-lua.dll (DLL strips
//      its export names so GetProcAddress returns 0 for everything).
//   2. Find lua_pcall by scanning for callers of luaL_loadbufferx in the DLL
//      and taking the next CALL after it (caller does: loadbuf â†’ test â†’ pcall).
//   3. Install VEH + HW BP on luaL_loadbufferx as early as possible so we
//      capture lua_State when FiveM loads/reloads any script.
//   4. Once lua_State is captured, ExecLua calls loadbufferx+pcall directly â€“
//      no need to wait for another script load.
static HMODULE   g_hLuaDll         = nullptr;
static uintptr_t g_fnLuaLoadBuf    = 0;  // luaL_loadbufferx (pattern scan) -- for CALLING
static uintptr_t g_fnLuaPcall      = 0;  // lua_pcall        (xref from caller)
static uintptr_t g_fnLuaBpLoadBuf  = 0;  // scripting-lua loadbufferx (HWBP target only)
static void*     g_luaState        = nullptr;
static bool      g_luaHooked       = false;
static bool      g_luaEarlyDone    = false;
static bool      g_usingMetadataLua= false; // true when loadbuf/pcall are from metadata-lua
static std::string g_luaQueue;

using LuaLBX_t  = int(__fastcall*)(void*,const char*,size_t,const char*,const char*);
using LuaPcall_t = int(__fastcall*)(void*,int,int,int,int);

static uintptr_t g_fnLuaLoad    = 0;  // lua_load internal (LTO-surviving, replaces luaL_loadbufferx)
static uintptr_t g_fnLuaSettop  = 0;  // lua_settop (state-capture via VEH+HWBP)
using LuaLoad_t = int(__fastcall*)(void* L, const char*(*reader)(void*,void*,size_t*), void* ud, const char* name, const char* mode);
struct LuaLoadS { const char* buf; size_t sz; };
static const char* LuaGetS(void* /*L*/, void* ud, size_t* sz) {
    LuaLoadS* s = (LuaLoadS*)ud;
    if(!s->sz) { *sz = 0; return nullptr; }
    *sz = s->sz; s->sz = 0; return s->buf;
}

// ── Deferred Lua execution (thread-safe via settop return-address hijack) ──
// ExecLua queues code + re-arms settop BP.  When settop fires on the Lua
// thread, we replace its return address with a trampoline.  settop runs to
// completion (Lua state consistent), then the trampoline calls loadbufferx+pcall
// and jumps back to the original caller.
static volatile bool      g_luaExecPending  = false;
static char*              g_luaExecBuf      = nullptr;
static size_t             g_luaExecSz       = 0;
static uint8_t*           g_trampCode       = nullptr;   // VirtualAlloc'd RWX trampoline
static volatile uintptr_t g_trampOrigRet    = 0;         // saved original return address
static volatile void*     g_trampL          = nullptr;   // lua_State for deferred exec
static volatile bool      g_settopRearmStep = false;     // single-stepping to re-arm settop BP

// Forward declare (defined later, after FLog)
static void DoLuaDeferred();
static void InitTrampoline();

//  HW breakpoint helpers (avoid .text patching that AC detects) â”€
static void* g_vehHandle = nullptr;

static void SetHardwareBP(uintptr_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    DWORD pid = GetCurrentProcessId();
    DWORD myTid = GetCurrentThreadId();
    if(Thread32First(snap, &te)) {
        do {
            if(te.th32OwnerProcessID == pid) {
                HANDLE ht = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if(ht) {
                    bool isCurrent = (te.th32ThreadID == myTid);
                    if(!isCurrent) SuspendThread(ht);
                    CONTEXT ctx = {0};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    GetThreadContext(ht, &ctx);
                    ctx.Dr0 = addr;
                    ctx.Dr7 = (ctx.Dr7 & ~0xFULL) | 1ULL; // Enable DR0 execute BP
                    SetThreadContext(ht, &ctx);
                    if(!isCurrent) ResumeThread(ht);
                    CloseHandle(ht);
                }
            }
        } while(Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void ClearHardwareBP() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    DWORD pid = GetCurrentProcessId();
    DWORD myTid = GetCurrentThreadId();
    if(Thread32First(snap, &te)) {
        do {
            if(te.th32OwnerProcessID == pid) {
                HANDLE ht = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if(ht) {
                    if(te.th32ThreadID != myTid) SuspendThread(ht);
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    GetThreadContext(ht, &ctx);
                    ctx.Dr0 = 0;
                    ctx.Dr7 &= ~1ULL;
                    SetThreadContext(ht, &ctx);
                    if(te.th32ThreadID != myTid) ResumeThread(ht);
                    CloseHandle(ht);
                }
            }
        } while(Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void ScanLuaFunctions() {
    if(!g_hLuaDll) {
        const char* dllName = "citizen-scripting-lua.dll";
        g_hLuaDll = GetModuleHandleA(dllName);
        if(!g_hLuaDll) { dllName = "citizen-scripting-lua54.dll"; g_hLuaDll = GetModuleHandleA(dllName); }
        if(!g_hLuaDll) { Log("[Lua] DLL not loaded yet"); return; }
        Log("[Lua] DLL found (%s): %p", dllName, g_hLuaDll);
    }

    uintptr_t base = (uintptr_t)g_hLuaDll;

    // Parse PE sections â€” only scan executable ranges to avoid bad-page crashes
    struct ScanRange { uintptr_t start; uint32_t size; };
    ScanRange execRanges[8]; int nExec = 0;
    {
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if(dos->e_magic == IMAGE_DOS_SIGNATURE && !IsBadReadPtr((void*)(base+dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64))) {
            auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            int   ns  = nt->FileHeader.NumberOfSections;
            for(int s = 0; s < ns && nExec < 8; s++) {
                if(!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                uintptr_t sb = base + sec[s].VirtualAddress;
                uint32_t  sz = sec[s].Misc.VirtualSize;
                if(sz < 64 || IsBadReadPtr((void*)sb, 8)) continue;
                execRanges[nExec++] = {sb, sz};
                Log("[Lua] .text[%d] %.8s DLL+0x%X sz=0x%X", s, (char*)sec[s].Name, sec[s].VirtualAddress, sz);
                // Dump 16 bytes at several offsets for pattern analysis
                const uint32_t dumpOff[] = {0, 0x10000, 0x50000, 0x100000, 0x200000};
                for(uint32_t dj : dumpOff) {
                    if(dj >= sz) break;
                    uint8_t* b = (uint8_t*)(sb + dj);
                    if(IsBadReadPtr(b, 16)) continue;
                    Log("[Lua]   +0x%05X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                        sec[s].VirtualAddress+dj,
                        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                        b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
                }
            }
        }
    }
    if(nExec == 0) {
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), g_hLuaDll, &mi, sizeof(mi));
        execRanges[nExec++] = {base, mi.SizeOfImage};
        Log("[Lua] No exec sections â€” full image 0x%X bytes", (unsigned)mi.SizeOfImage);
    }

    if(!g_fnLuaLoadBuf) {
        //  Prologue-pattern scan with validation 
        // After matching, validate the candidate:
        // - Function body > 64 bytes (not a stub)
        // - Contains CALL (E8) within 128 bytes (calls lua_load internally)
        // - Accesses [RSP+0x28/0x30] = 5th param (mode) within prologue
        auto ValidateLoadBuf = [&](uintptr_t cand) -> bool {
            if(IsBadReadPtr((void*)cand, 256)) return false;
            int funcLen = 0;
            for(int i = 4; i < 512; i++) {
                uint8_t b = ((uint8_t*)cand)[i];
                if(b == 0xC3 || b == 0xCC) { funcLen = i; break; }
            }
            if(funcLen < 64) {
                Log("[Lua] Candidate DLL+0x%llX REJECTED: too short (%d bytes)", (unsigned long long)(cand-base), funcLen);
                return false;
            }
            bool hasCall = false;
            int scanLen = funcLen < 128 ? funcLen : 128;
            for(int i = 0; i < scanLen; i++) {
                if(((uint8_t*)cand)[i] == 0xE8) { hasCall = true; break; }
            }
            if(!hasCall) {
                Log("[Lua] Candidate DLL+0x%llX REJECTED: no CALL found", (unsigned long long)(cand-base));
                return false;
            }
            // Check 5th param access [RSP+offset] where offset >= 0x28 (mode parameter)
            // After any sub rsp,N the original [RSP+0x28] is now at [RSP+0x28+N].
            // Scan wider (256 bytes) and accept any aligned offset >= 0x28.
            bool has5thParam = false;
            for(int i = 0; i < 256 && i < funcLen - 4; i++) {
                uint8_t* b = (uint8_t*)(cand + i);
                // MOV reg,[RSP+disp] or MOV [RSP+disp],reg with disp >= 0x28
                if(b[3] == 0x24 && b[4] >= 0x28) {
                    if((b[0] == 0x44 || b[0] == 0x48 || b[0] == 0x4C) && (b[1] == 0x8B || b[1] == 0x89)) {
                        has5thParam = true; break;
                    }
                }
                // Also: MOV [RSP+xx],reg where xx >= 0x28
                if(b[0] == 0x48 && b[1] == 0x89 && (b[2] & 0x47) == 0x44 && b[3] == 0x24 && b[4] >= 0x28) {
                    has5thParam = true; break;
                }
            }
            if(!has5thParam) {
                Log("[Lua] Candidate DLL+0x%llX REJECTED: no 5th param access in prologue", (unsigned long long)(cand-base));
                return false;
            }
            Log("[Lua] Candidate DLL+0x%llX VALIDATED: funcLen=%d", (unsigned long long)(cand-base), funcLen);
            return true;
        };
        const uint8_t p1[]={0x4C,0x8B,0xDC,0x53,0x48,0x83,0xEC,0x60};
        const uint8_t p2[]={0x4C,0x8B,0xD4,0x53,0x48,0x83,0xEC,0x60};
        const uint8_t p3[]={0x53,0x48,0x83,0xEC,0x40,0x4C,0x8B,0xDA};
        const uint8_t p4[]={0x48,0x89,0x4C,0x24,0x08,0x48,0x89,0x54,0x24,0x10,0x53};
        const uint8_t p5[]={0x48,0x83,0xEC,0x58,0x48,0x89,0x5C,0x24};
        const uint8_t p6[]={0x4C,0x8B,0xDC,0x57,0x48,0x83,0xEC,0x50};
        const uint8_t p7[]={0x48,0x83,0xEC,0x38,0x48,0x89,0x54,0x24,0x20};
        const uint8_t p8[]={0x48,0x83,0xEC,0x48,0x48,0x89,0x54,0x24,0x20};
        const uint8_t p9[]={0x40,0x53,0x48,0x83,0xEC,0x50,0x33,0xC0};
        const uint8_t p10[]={0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x08};
        const uint8_t p11[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10};
        struct PI { const uint8_t* b; int n; };
        const PI pats[] = {{p1,8},{p2,8},{p3,8},{p4,11},{p5,8},{p6,8},{p7,9},{p8,9},{p9,8},{p10,7},{p11,11}};
        for(const auto& pi : pats) {
            for(int ri = 0; ri < nExec; ri++) {
                uintptr_t rs = execRanges[ri].start, re = rs + execRanges[ri].size;
                for(uintptr_t p = rs; p < re - (uintptr_t)pi.n; p++) {
                    if(!memcmp((void*)p, pi.b, pi.n)){
                        if(ValidateLoadBuf(p)) {
                            g_fnLuaLoadBuf = p;
                            Log("[Lua] luaL_loadbufferx at DLL+0x%llX (validated pattern)", (unsigned long long)(p-base));
                        }
                        // Don't break - try remaining matches in this exec range for this pattern
                        if(g_fnLuaLoadBuf) break;
                    }
                }
                if(g_fnLuaLoadBuf) break;
            }
            if(g_fnLuaLoadBuf) break;
        }
        if(!g_fnLuaLoadBuf) {
            g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(g_hLuaDll, "luaL_loadbufferx");
            if(g_fnLuaLoadBuf) Log("[Lua] luaL_loadbufferx via export");
        }
        //  getS indirect scan 
        // luaL_loadbufferx is the only Lua function that contains the static
        // getS reader. Even inlined, MOV RAX,[RDX+8]; TEST RAX,RAX; JZ  appears
        // inside the function body. Find it, trace back to prev RET = func start.
        if(!g_fnLuaLoadBuf) {
            const uint8_t gs[]={0x48,0x8B,0x42,0x08,0x48,0x85,0xC0,0x74};
            for(int ri = 0; ri < nExec && !g_fnLuaLoadBuf; ri++) {
                uintptr_t rs = execRanges[ri].start, re = rs + execRanges[ri].size;
                for(uintptr_t p = rs; p < re - 8; p++) {
                    if(memcmp((void*)p, gs, 8)) continue;
                    Log("[Lua] getS body at DLL+0x%llX", (unsigned long long)(p-base));
                    // Trace backward to previous RET/INT3 = end of prior function
                    uintptr_t funcStart = 0;
                    for(int back = 1; back < 0x400; back++) {
                        if(IsBadReadPtr((void*)(p - back), 1)) break;
                        uint8_t c = *(uint8_t*)(p - back);
                        if(c == 0xC3 || c == 0xCC || c == 0xC2) {
                            funcStart = p - back + 1;
                            break;
                        }
                    }
                    if(funcStart && !IsBadReadPtr((void*)funcStart, 16)) {
                        uint8_t* fb = (uint8_t*)funcStart;
                        Log("[Lua] âœ“ luaL_loadbufferx (getS trace) DLL+0x%llX bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                            (unsigned long long)(funcStart-base),
                            fb[0],fb[1],fb[2],fb[3],fb[4],fb[5],fb[6],fb[7]);
                        g_fnLuaLoadBuf = funcStart;
                    }
                    break; // only use first match
                }
            }
        }
        // -- Extra attempts: alternate export names + lua54.dll + export dump --
        if(!g_fnLuaLoadBuf) {
            g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(g_hLuaDll, "luaL_loadbuffer");
            if(g_fnLuaLoadBuf) Log("[Lua] [OK] luaL_loadbuffer (no-x) export");
        }
        if(!g_fnLuaLoadBuf) {
            HMODULE h54 = GetModuleHandleA("citizen-scripting-lua54.dll");
            if(h54 && h54 != g_hLuaDll) {
                g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(h54, "luaL_loadbufferx");
                if(!g_fnLuaLoadBuf) g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(h54, "luaL_loadbuffer");
                if(g_fnLuaLoadBuf) { g_hLuaDll = h54; Log("[Lua] [OK] found in lua54.dll"); }
            }
        }
        { static bool s_expDone=false; if(!s_expDone){ s_expDone=true;
            auto* dos2=(IMAGE_DOS_HEADER*)base;
            if(dos2->e_magic==IMAGE_DOS_SIGNATURE){
                auto* nt2=(IMAGE_NT_HEADERS*)(base+dos2->e_lfanew);
                auto& ed=nt2->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                if(ed.VirtualAddress){ auto* exp2=(IMAGE_EXPORT_DIRECTORY*)(base+ed.VirtualAddress);
                    auto* nms=(DWORD*)(base+exp2->AddressOfNames);
                    Log("[Lua] Exports (%u):",exp2->NumberOfNames);
                    DWORD show=exp2->NumberOfNames<50?exp2->NumberOfNames:50;
                    for(DWORD i=0;i<show;i++){const char* nm=(const char*)(base+nms[i]);if(!IsBadReadPtr(nm,4))Log("[Lua]  [%u] %s",i,nm);}
                } else Log("[Lua] No export dir");
            }
        }}
        if(!g_fnLuaLoadBuf) {
            Log("[Lua] luaL_loadbufferx NOT found -- trying lua_load/settop fallbacks");
        }
    }

    //  lua_pcall â”€
    if(!g_fnLuaPcall) {
        g_fnLuaPcall = (uintptr_t)GetProcAddress(g_hLuaDll, "lua_pcall");
        if(!g_fnLuaPcall) g_fnLuaPcall = (uintptr_t)GetProcAddress(g_hLuaDll, "lua_pcallk");
        if(g_fnLuaPcall) Log("[Lua] âœ“ lua_pcall via export");
    }
    if(!g_fnLuaPcall && g_fnLuaLoadBuf) {
        for(int ri = 0; ri < nExec && !g_fnLuaPcall; ri++) {
            uintptr_t rs = execRanges[ri].start, re = rs + execRanges[ri].size;
            for(uintptr_t p = rs; p < re - 5; p++) {
                uint8_t* b = (uint8_t*)p;
                if(b[0] != 0xE8) continue;
                uintptr_t tgt = p + 5 + (int32_t)(b[1]|(b[2]<<8)|(b[3]<<16)|(b[4]<<24));
                if(tgt != g_fnLuaLoadBuf) continue;
                for(uintptr_t q = p+5; q < p+100 && q < re-5; q++) {
                    uint8_t* s = (uint8_t*)q;
                    if(s[0]==0xC3||s[0]==0xC2) break;
                    if(s[0]==0xE8) {
                        uintptr_t cand = q+5+(int32_t)(s[1]|(s[2]<<8)|(s[3]<<16)|(s[4]<<24));
                        if(cand >= rs && cand < re && cand != g_fnLuaLoadBuf) {
                            g_fnLuaPcall = cand;
                            Log("[Lua] âœ“ lua_pcall xref DLL+0x%llX", (unsigned long long)(cand-base));
                            break;
                        }
                    }
                }
                if(g_fnLuaPcall) break;
            }
        }
        if(!g_fnLuaPcall) Log("[Lua] âœ— lua_pcall not found");
    }
    // ── lua_settop (for lua_State capture via VEH+HWBP) ─────────────────────
    // Primary: find via xref from lua_fx_opendebug export.
    // lua_fx_opendebug calls lua_settop(L, 0) near the end.
    // Scan ALL CALL instructions in the export body; the last CALL before
    // the function epilogue is very likely lua_settop(L, 0).
    if(!g_fnLuaSettop) {
        uintptr_t dllBase = (uintptr_t)g_hLuaDll;
        uintptr_t dllEnd  = dllBase;
        for(int ri = 0; ri < nExec; ri++) dllEnd = execRanges[ri].start + execRanges[ri].size;
        static const char* s_settopExports[] = {
            "?lua_fx_opendebug@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openio@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openos@fx@@YAHPEAUlua_State@@@Z",
        };
        for(int xi = 0; xi < 3 && !g_fnLuaSettop; xi++) {
            uintptr_t fn = (uintptr_t)GetProcAddress(g_hLuaDll, s_settopExports[xi]);
            if(!fn || IsBadReadPtr((void*)fn, 0x200)) continue;
            Log("[Lua] settop xref: scanning %s @ DLL+0x%llX",
                s_settopExports[xi], (unsigned long long)(fn - dllBase));
            // Dump first 32 bytes for debugging
            {
                uint8_t* d = (uint8_t*)fn;
                Log("[Lua]   bytes: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
                Log("[Lua]          %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    d[16],d[17],d[18],d[19],d[20],d[21],d[22],d[23],d[24],d[25],d[26],d[27],d[28],d[29],d[30],d[31]);
            }
            // Collect ALL E8 call targets within the export body (up to 512 bytes)
            // Validate each: lua_settop checks idx (edx) with test/cmp in first 40 bytes
            // lua_settop(L, idx) checks idx (edx) early.  LTO may use:
            //   test edx,edx (85 D2)  |  cmp edx,imm8 (83 FA)
            //   movsxd rXX,edx (63 xx where xx&0xC7==0xC2)  — sign-extend before test
            auto ValidateSettopXref = [](uintptr_t fn) -> bool {
                if(IsBadReadPtr((void*)fn, 80)) return false;
                const uint8_t* b = (const uint8_t*)fn;
                for(int i = 0; i < 60; i++) {
                    if(i < 59 && b[i] == 0x85 && b[i+1] == 0xD2) return true; // test edx,edx
                    if(i < 58 && b[i] == 0x83 && b[i+1] == 0xFA) return true; // cmp edx, imm8
                    // movsxd rXX, edx — compiler sign-extends idx before branching
                    if(i < 59 && b[i] == 0x63 && (b[i+1] & 0xC7) == 0xC2) return true;
                    if(i > 0 && i < 59 && (b[i-1]&0xF0)==0x40 && b[i]==0x63 && (b[i+1]&0xC7)==0xC2) return true;
                    // test esi,esi / test edi,edi after mov from edx
                    if(i < 59 && b[i] == 0x85 && (b[i+1] == 0xF6 || b[i+1] == 0xFF)) return true;
                }
                return false;
            };
            int callCount = 0;
            for(int off = 0; off < 512; off++) {
                uint8_t* b = (uint8_t*)(fn + off);
                if(IsBadReadPtr(b, 6)) break;
                if(b[0] == 0xE8) {
                    int32_t rel = (int32_t)(b[1]|(b[2]<<8)|(b[3]<<16)|(b[4]<<24));
                    uintptr_t tgt = (uintptr_t)(b) + 5 + rel;
                    if(tgt > dllBase && tgt < dllEnd) {
                        bool valid = ValidateSettopXref(tgt);
                        // Dump first 16 bytes of every target for diagnostics
                        if(!IsBadReadPtr((void*)tgt, 16)) {
                            const uint8_t* d = (const uint8_t*)tgt;
                            Log("[Lua]   CALL at +0x%X -> DLL+0x%llX %s  [%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                                off, (unsigned long long)(tgt - dllBase),
                                valid ? "<-- settop" : "",
                                d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],
                                d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
                        }
                        callCount++;
                        if(valid && !g_fnLuaSettop) {
                            g_fnLuaSettop = tgt;
                            Log("[Lua] lua_settop xref (validated): DLL+0x%llX",
                                (unsigned long long)(tgt - dllBase));
                        }
                    }
                    off += 4;
                }
            }
            Log("[Lua]   scanned %d calls", callCount);
        }
        // Fallback: pattern scan with validation
        if(!g_fnLuaSettop) {
            struct SettopPat { const uint8_t* b; const char* m; int n; };
            const uint8_t psA[] = {0x48,0x89,0x5C,0x24,0,0x48,0x89,0x6C,0x24,0,0x57,0x48,0x83,0xEC,0,0x48,0x8B,0xE9};
            const uint8_t psB[] = {0x48,0x89,0x5C,0x24,0,0x48,0x89,0x74,0x24,0,0x57,0x48,0x83,0xEC,0};
            const SettopPat stPats[] = {
                {psB, "xxxx?xxxx?xxxx?", 15},
                {psA, "xxxx?xxxx?xxxx?xxx", 18},
            };
            auto ValidateSettop = [](uintptr_t fn) -> bool {
                if(IsBadReadPtr((void*)fn, 60)) return false;
                const uint8_t* b = (const uint8_t*)fn;
                for(int i = 0; i < 40; i++) {
                    if(i < 39 && b[i] == 0x85 && b[i+1] == 0xD2) return true;
                    if(i < 38 && b[i] == 0x83 && b[i+1] == 0xFA && b[i+2] == 0x00) return true;
                }
                return false;
            };
            for(const auto& sp : stPats) {
                for(int ri = 0; ri < nExec && !g_fnLuaSettop; ri++) {
                    uintptr_t rs = execRanges[ri].start, re = rs + execRanges[ri].size;
                    for(uintptr_t p = rs; p < re - (uintptr_t)sp.n; p++) {
                        bool ok = true;
                        for(int j = 0; j < sp.n; j++) if(sp.m[j]=='x' && ((uint8_t*)p)[j]!=sp.b[j]){ok=false;break;}
                        if(!ok) continue;
                        uintptr_t off = p - (uintptr_t)g_hLuaDll;
                        if(ValidateSettop(p)) {
                            g_fnLuaSettop = p;
                            Log("[Lua] lua_settop DLL+0x%llX (pat, validated)", (unsigned long long)off);
                            break;
                        }
                    }
                }
                if(g_fnLuaSettop) break;
            }
        }
        if(!g_fnLuaSettop) Log("[Lua] lua_settop not found");
    }
    // ── lua_load internal (LTO-surviving replacement for luaL_loadbufferx) ──
    if(!g_fnLuaLoad) {
        // 48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC
        const uint8_t pl[] = {0x48,0x89,0x5C,0x24,0,0x48,0x89,0x74,0x24,0,0x48,0x89,0x7C,0x24,0,
                              0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0,0x48,0x81,0xEC};
        const char*   plm  = "xxxx?xxxx?xxxx?xxxxxxxxxx?xxx";
        const int     plen = 28;
        for(int ri = 0; ri < nExec && !g_fnLuaLoad; ri++) {
            uintptr_t rs = execRanges[ri].start, re = rs + execRanges[ri].size;
            for(uintptr_t p = rs; p < re - plen; p++) {
                bool ok = true;
                for(int j = 0; j < plen; j++) if(plm[j]=='x' && ((uint8_t*)p)[j]!=pl[j]){ok=false;break;}
                if(ok){ g_fnLuaLoad = p; Log("[Lua] lua_load(internal) DLL+0x%llX",(unsigned long long)(p-(uintptr_t)g_hLuaDll)); break; }
            }
        }
        if(!g_fnLuaLoad) Log("[Lua] lua_load(internal) not found");
    }
    // ── fallback: use exported lua_fx_open* directly as HWBP target ──
    // If xref scan didn't find settop, put HWBP on the export itself.
    // The export takes lua_State* as RCX, so VEH captures it on hit.
    if(!g_fnLuaSettop) {
        static const char* s_stateExports[] = {
            "?lua_fx_opendebug@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openio@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openos@fx@@YAHPEAUlua_State@@@Z",
        };
        for(int xi = 0; xi < 3 && !g_fnLuaSettop; xi++) {
            uintptr_t fn = (uintptr_t)GetProcAddress(g_hLuaDll, s_stateExports[xi]);
            if(fn) {
                g_fnLuaSettop = fn;
                Log("[Lua] HWBP target (export fallback): DLL+0x%llX (%s)",
                    (unsigned long long)(fn - (uintptr_t)g_hLuaDll), s_stateExports[xi]);
            }
        }
        if(!g_fnLuaSettop) Log("[Lua] No state-capture HWBP target found");
    }
    // ── pcall discovery: scan bodies of lua_fx_open* for CALL xrefs ──────────
    // lua_fx_openX calls luaL_requiref which calls lua_pcall(L,1,1,0). We scan
    // up to 512 bytes of each open-function body, collecting all CALL targets.
    // For each target, check if its first 20 bytes match a near-ret (tiny func)
    // or if a deeper scan finds a pattern near xor-r9d (4th arg = 0 = errfunc).
    if(!g_fnLuaPcall) {
        static const char* s_pcallExports[] = {
            "?lua_fx_opendebug@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openio@fx@@YAHPEAUlua_State@@@Z",
            "?lua_fx_openos@fx@@YAHPEAUlua_State@@@Z",
        };
        uintptr_t dllBase = (uintptr_t)g_hLuaDll;
        uintptr_t dllEnd  = dllBase;
        // approximate DLL text end from first exec range
        for(int ri = 0; ri < nExec; ri++) dllEnd = execRanges[ri].start + execRanges[ri].size;
        for(int xi = 0; xi < 3 && !g_fnLuaPcall; xi++) {
            uintptr_t fn = (uintptr_t)GetProcAddress(g_hLuaDll, s_pcallExports[xi]);
            if(!fn || IsBadReadPtr((void*)fn, 0x200)) continue;
            // Scan body of this export for CALL instructions
            for(int off = 0; off < 512 && !g_fnLuaPcall; off++) {
                uint8_t* b = (uint8_t*)(fn + off);
                if(IsBadReadPtr(b, 6)) break;
                if(b[0] == 0xC3 || b[0] == 0xC2) break; // ret
                if(b[0] != 0xE8) continue;
                uintptr_t tgt = (fn + off) + 5 + (int32_t)(b[1]|(b[2]<<8)|(b[3]<<16)|(b[4]<<24));
                if(tgt < dllBase || tgt >= dllEnd) continue;
                // Scan tgt's body for a call that is preceded by xor r9d,r9d (errfunc=0)
                // xor r9d,r9d = 45 33 C9
                for(int off2 = 0; off2 < 256 && !g_fnLuaPcall; off2++) {
                    uint8_t* s2 = (uint8_t*)(tgt + off2);
                    if(IsBadReadPtr(s2, 6)) break;
                    if(s2[0] == 0xC3 || s2[0] == 0xC2) break;
                    // xor r9d,r9d (45 33 C9) within 6 bytes before a CALL
                    if(s2[0] == 0x45 && s2[1] == 0x33 && s2[2] == 0xC9) {
                        // look for CALL within next 10 bytes
                        for(int ck = 1; ck < 10; ck++) {
                            uint8_t* sc = s2 + ck;
                            if(IsBadReadPtr(sc, 6)) break;
                            if(sc[0] == 0xE8) {
                                uintptr_t cand = (uintptr_t)(sc) + 5 + (int32_t)(sc[1]|(sc[2]<<8)|(sc[3]<<16)|(sc[4]<<24));
                                if(cand > dllBase && cand < dllEnd) {
                                    g_fnLuaPcall = cand;
                                    Log("[Lua] lua_pcall xref via export body: DLL+0x%llX",
                                        (unsigned long long)(cand - dllBase));
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        if(!g_fnLuaPcall) Log("[Lua] lua_pcall not found via export xref");
    }

    // ── Record scripting-lua loadbufferx for HWBP (state capture + injection) ──
    // Only set once — never overwrite with metadata-lua's address on re-scan
    static bool s_bpLoadBufSet = false;
    if(!s_bpLoadBufSet) {
        s_bpLoadBufSet = true;
        g_fnLuaBpLoadBuf = g_fnLuaLoadBuf; // 0 if scripting-lua didn't yield it
    }

    // ── citizen-resources-metadata-lua.dll fallback ──────────────────────────
    // This DLL embeds its own Lua 5.4 build for parsing fxmanifest.lua.
    // Its Lua functions share the same ABI, so we can call them with a
    // scripting-lua lua_State. Only used for CALLING, never for HWBP.
    if(!g_fnLuaLoadBuf || !g_fnLuaPcall) {
        HMODULE hMeta = GetModuleHandleA("citizen-resources-metadata-lua.dll");
        if(hMeta && hMeta != g_hLuaDll) {
            uintptr_t mBase = (uintptr_t)hMeta;
            Log("[Lua] Fallback: scanning citizen-resources-metadata-lua.dll @ %p", hMeta);

            // Parse PE exec sections
            ScanRange mRanges[8]; int nMR = 0;
            {
                auto* mdos = (IMAGE_DOS_HEADER*)mBase;
                if(mdos->e_magic == IMAGE_DOS_SIGNATURE && !IsBadReadPtr((void*)(mBase+mdos->e_lfanew), sizeof(IMAGE_NT_HEADERS64))) {
                    auto* mnt = (IMAGE_NT_HEADERS*)(mBase + mdos->e_lfanew);
                    auto* msec = IMAGE_FIRST_SECTION(mnt);
                    for(int s = 0; s < mnt->FileHeader.NumberOfSections && nMR < 8; s++) {
                        if(!(msec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                        uintptr_t sb = mBase + msec[s].VirtualAddress;
                        uint32_t  sz = msec[s].Misc.VirtualSize;
                        if(sz < 64 || IsBadReadPtr((void*)sb, 8)) continue;
                        mRanges[nMR++] = {sb, sz};
                    }
                }
            }
            if(nMR == 0) {
                MODULEINFO mi2{};
                GetModuleInformation(GetCurrentProcess(), hMeta, &mi2, sizeof(mi2));
                mRanges[nMR++] = {mBase, mi2.SizeOfImage};
            }

            // Pattern scan for luaL_loadbufferx in metadata-lua
            if(!g_fnLuaLoadBuf) {
                const uint8_t mp1[]={0x4C,0x8B,0xDC,0x53,0x48,0x83,0xEC,0x60};
                const uint8_t mp2[]={0x4C,0x8B,0xD4,0x53,0x48,0x83,0xEC,0x60};
                const uint8_t mp3[]={0x53,0x48,0x83,0xEC,0x40,0x4C,0x8B,0xDA};
                const uint8_t mp4[]={0x48,0x89,0x4C,0x24,0x08,0x48,0x89,0x54,0x24,0x10,0x53};
                const uint8_t mp5[]={0x48,0x83,0xEC,0x58,0x48,0x89,0x5C,0x24};
                const uint8_t mp6[]={0x4C,0x8B,0xDC,0x57,0x48,0x83,0xEC,0x50};
                const uint8_t mp7[]={0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x08};
                const uint8_t mp8[]={0x40,0x53,0x48,0x83,0xEC,0x50,0x33,0xC0};
                const uint8_t mp9[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10};
                struct MPI { const uint8_t* b; int n; };
                const MPI mpats[] = {{mp1,8},{mp2,8},{mp3,8},{mp4,11},{mp5,8},{mp6,8},{mp7,7},{mp8,8},{mp9,11}};
                for(const auto& pi : mpats) {
                    for(int ri = 0; ri < nMR && !g_fnLuaLoadBuf; ri++) {
                        uintptr_t rs = mRanges[ri].start, re = rs + mRanges[ri].size;
                        for(uintptr_t p = rs; p < re - (uintptr_t)pi.n; p++) {
                            if(!memcmp((void*)p, pi.b, pi.n)){
                                g_fnLuaLoadBuf = p;
                                Log("[Lua] loadbufferx in metadata-lua DLL+0x%llX (pattern)", (unsigned long long)(p-mBase));
                                break;
                            }
                        }
                    }
                    if(g_fnLuaLoadBuf) break;
                }
                // Export fallback
                if(!g_fnLuaLoadBuf) g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(hMeta, "luaL_loadbufferx");
                if(!g_fnLuaLoadBuf) g_fnLuaLoadBuf = (uintptr_t)GetProcAddress(hMeta, "luaL_loadbuffer");
                if(g_fnLuaLoadBuf) Log("[Lua] loadbufferx via metadata-lua export");
                // getS indirect scan in metadata-lua
                if(!g_fnLuaLoadBuf) {
                    const uint8_t gs[]={0x48,0x8B,0x42,0x08,0x48,0x85,0xC0,0x74};
                    for(int ri = 0; ri < nMR && !g_fnLuaLoadBuf; ri++) {
                        uintptr_t rs = mRanges[ri].start, re = rs + mRanges[ri].size;
                        for(uintptr_t p = rs; p < re - 8; p++) {
                            if(memcmp((void*)p, gs, 8)) continue;
                            uintptr_t funcStart = 0;
                            for(int back = 1; back < 0x400; back++) {
                                if(IsBadReadPtr((void*)(p - back), 1)) break;
                                uint8_t c = *(uint8_t*)(p - back);
                                if(c == 0xC3 || c == 0xCC || c == 0xC2) { funcStart = p - back + 1; break; }
                            }
                            if(funcStart && !IsBadReadPtr((void*)funcStart, 16)) {
                                g_fnLuaLoadBuf = funcStart;
                                Log("[Lua] loadbufferx (getS trace) metadata-lua DLL+0x%llX", (unsigned long long)(funcStart-mBase));
                            }
                            break;
                        }
                    }
                }
            }

            // pcall scan in metadata-lua
            if(!g_fnLuaPcall) {
                g_fnLuaPcall = (uintptr_t)GetProcAddress(hMeta, "lua_pcall");
                if(!g_fnLuaPcall) g_fnLuaPcall = (uintptr_t)GetProcAddress(hMeta, "lua_pcallk");
                if(g_fnLuaPcall) Log("[Lua] pcall via metadata-lua export");
            }
            // pcall xref from loadbufferx callers in metadata-lua
            if(!g_fnLuaPcall && g_fnLuaLoadBuf) {
                for(int ri = 0; ri < nMR && !g_fnLuaPcall; ri++) {
                    uintptr_t rs = mRanges[ri].start, re = rs + mRanges[ri].size;
                    for(uintptr_t p = rs; p < re - 5; p++) {
                        uint8_t* b = (uint8_t*)p;
                        if(b[0] != 0xE8) continue;
                        uintptr_t tgt = p + 5 + (int32_t)(b[1]|(b[2]<<8)|(b[3]<<16)|(b[4]<<24));
                        if(tgt != g_fnLuaLoadBuf) continue;
                        for(uintptr_t q = p+5; q < p+100 && q < re-5; q++) {
                            uint8_t* s = (uint8_t*)q;
                            if(s[0]==0xC3||s[0]==0xC2) break;
                            if(s[0]==0xE8) {
                                uintptr_t cand = q+5+(int32_t)(s[1]|(s[2]<<8)|(s[3]<<16)|(s[4]<<24));
                                if(cand >= rs && cand < re && cand != g_fnLuaLoadBuf) {
                                    g_fnLuaPcall = cand;
                                    Log("[Lua] pcall xref in metadata-lua DLL+0x%llX", (unsigned long long)(cand-mBase));
                                    break;
                                }
                            }
                        }
                        if(g_fnLuaPcall) break;
                    }
                }
            }
            // xor r9d,r9d pcall scan in metadata-lua (same as scripting-lua export body scan)
            if(!g_fnLuaPcall) {
                uintptr_t fn = (uintptr_t)GetProcAddress(hMeta, "CreateComponent");
                if(fn && !IsBadReadPtr((void*)fn, 0x200)) {
                    uintptr_t mEnd = mRanges[0].start + mRanges[0].size;
                    for(int off = 0; off < 512 && !g_fnLuaPcall; off++) {
                        uint8_t* b = (uint8_t*)(fn + off);
                        if(IsBadReadPtr(b, 6)) break;
                        if(b[0] == 0xC3 || b[0] == 0xC2) break;
                        if(b[0] != 0xE8) continue;
                        uintptr_t tgt = (fn + off) + 5 + (int32_t)(b[1]|(b[2]<<8)|(b[3]<<16)|(b[4]<<24));
                        if(tgt < mBase || tgt >= mEnd) continue;
                        for(int off2 = 0; off2 < 256 && !g_fnLuaPcall; off2++) {
                            uint8_t* s2 = (uint8_t*)(tgt + off2);
                            if(IsBadReadPtr(s2, 6)) break;
                            if(s2[0] == 0xC3 || s2[0] == 0xC2) break;
                            if(s2[0] == 0x45 && s2[1] == 0x33 && s2[2] == 0xC9) {
                                for(int ck = 1; ck < 10; ck++) {
                                    uint8_t* sc = s2 + ck;
                                    if(IsBadReadPtr(sc, 6)) break;
                                    if(sc[0] == 0xE8) {
                                        uintptr_t cand = (uintptr_t)(sc) + 5 + (int32_t)(sc[1]|(sc[2]<<8)|(sc[3]<<16)|(sc[4]<<24));
                                        if(cand > mBase && cand < mEnd) {
                                            g_fnLuaPcall = cand;
                                            Log("[Lua] pcall xref via metadata-lua CreateComponent: DLL+0x%llX",
                                                (unsigned long long)(cand - mBase));
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if(g_fnLuaLoadBuf && !g_fnLuaBpLoadBuf) {
                g_usingMetadataLua = true;
                Log("[Lua] Using metadata-lua for Lua calls (settop will be used as exec trigger)");
            }
        }
    }
}

static int __fastcall HkLuaLoadBufX(void* L, const char* buf, size_t sz,
                                      const char* name, const char* mode){
    if(!g_luaState) {
        g_luaState = L;
        Log("[Lua] lua_State captured: %p (from script '%s')", L, name?name:"?");
    }
    if(!g_luaQueue.empty()){
        std::string code = g_luaQueue;
        g_luaQueue.clear();
        g_luaHooked = false;
        ClearHardwareBP();
        Log("[Lua] *** Injecting %zu bytes into '%s' (orig buf=%p sz=%zu) ***",
            code.size(), name?name:"?", buf, sz);

        // Validate original buffer — if bad, just run our code alone
        bool origOk = (buf != nullptr && sz > 0 && sz < 0x1000000 && !IsBadReadPtr(buf, sz));

        std::string combined;
        try {
            combined.reserve(code.size() + 80 + (origOk ? sz : 0));
            combined += "local _ok,_er=pcall(function()\n";
            combined += code;
            combined += "\nend) if not _ok then print('[QWAK]'..tostring(_er)) end\n";
            if(origOk) combined.append(buf, sz);
        } catch(...) {
            Log("[Lua] ERROR: string build failed, running code only");
            combined = "local _ok,_er=pcall(function()\n";
            combined += code;
            combined += "\nend) if not _ok then print('[QWAK]'..tostring(_er)) end\n";
        }

        Log("[Lua] Buffer injection done (%zu bytes total).", combined.size());
        return ((LuaLBX_t)g_fnLuaLoadBuf)(L, combined.c_str(), combined.size(), name, mode);
    }
    return ((LuaLBX_t)g_fnLuaLoadBuf)(L, buf, sz, name, mode);
}

// Pre-built injection buffer (built in ExecLua, swapped into registers by VEH)
static char*    g_luaInjBuf  = nullptr;   // malloc'd prefix: pcall(function()\n <code> \nend)...
static size_t   g_luaInjSz   = 0;
static char*    g_luaCombBuf = nullptr;   // malloc'd combined: prefix + original script
static size_t   g_luaCombSz  = 0;

// Forward declarations for VEH handler (defined later in file)
struct Vec3 { float x,y,z; };
static inline bool AddrOk(uintptr_t a);
static Vec3 GetPos(uintptr_t ped);
static Vec3 GetBonePos(uintptr_t ped, int boneIdx);
static volatile bool g_bulletBpStepping = false;
static bool          g_magicBullet      = false;
static uintptr_t     g_aimTarget_early  = 0;

// Simple file log for crash diagnostics (survives game close)
static void FLog(const char* fmt, ...) {
    char logPath[MAX_PATH];
    GetTempPathA(MAX_PATH, logPath);
    strcat_s(logPath, "qwak_lua.log");
    FILE* f = fopen(logPath, "a");
    if(!f) return;
    va_list a; va_start(a,fmt); vfprintf(f,fmt,a); va_end(a);
    fprintf(f, "\n"); fflush(f); fclose(f);
}

// DoLuaDeferred / InitTrampoline — implementations (forward-declared above)
static void DoLuaDeferred() {
    void* L = (void*)g_trampL;
    FLog("DoLuaDeferred: L=%p sz=%zu", L, g_luaExecSz);
    if(!L || !g_luaExecBuf || !g_luaExecSz
       || !g_fnLuaLoadBuf || !g_fnLuaPcall) {
        FLog("DoLuaDeferred: SKIP (L=%p buf=%p sz=%zu lbx=%llX pc=%llX)",
             L, g_luaExecBuf, g_luaExecSz,
             (unsigned long long)g_fnLuaLoadBuf, (unsigned long long)g_fnLuaPcall);
        return;
    }
    int loadRet = ((LuaLBX_t)g_fnLuaLoadBuf)(L, g_luaExecBuf, g_luaExecSz, "qwak", nullptr);
    FLog("loadbufferx=%d", loadRet);
    Log("[Lua] loadbufferx=%d", loadRet);
    if(loadRet == 0) {
        // lua_pcallk: L, nargs, nresults, errfunc, ctx, k  (6 params)
        using Pcallk_t = int(__fastcall*)(void*,int,int,int,intptr_t,void*);
        int pcRet = ((Pcallk_t)g_fnLuaPcall)(L, 0, 0, 0, 0, nullptr);
        FLog("pcall=%d", pcRet);
        Log("[Lua] pcall=%d", pcRet);
        if(pcRet != 0 && g_fnLuaSettop) {
            // Pop error message to keep Lua stack clean
            using Settop_t = void(__fastcall*)(void*, int);
            ((Settop_t)g_fnLuaSettop)(L, -2);
        }
    } else {
        // Pop error from failed load
        if(g_fnLuaSettop) {
            using Settop_t = void(__fastcall*)(void*, int);
            ((Settop_t)g_fnLuaSettop)(L, -2);
        }
        FLog("loadbufferx FAILED (%d)", loadRet);
        Log("[Lua] load FAILED (%d)", loadRet);
    }
    FLog("DoLuaDeferred DONE");
}

static void InitTrampoline() {
    if(g_trampCode) return;
    g_trampCode = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(!g_trampCode) { FLog("TRAMPOLINE: VirtualAlloc failed"); return; }
    uint8_t* p = g_trampCode;
    int i = 0;
    // After settop's RET, RSP is 16-aligned
    // sub rsp, 0x20  (shadow space, keeps 16-byte alignment)
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xEC; p[i++] = 0x20;
    // mov rax, <DoLuaDeferred>
    p[i++] = 0x48; p[i++] = 0xB8;
    *(uintptr_t*)(p + i) = (uintptr_t)&DoLuaDeferred; i += 8;
    // call rax
    p[i++] = 0xFF; p[i++] = 0xD0;
    // add rsp, 0x20
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xC4; p[i++] = 0x20;
    // mov rax, [&g_trampOrigRet]  (load the saved original return address)
    p[i++] = 0x48; p[i++] = 0xB8;
    *(uintptr_t*)(p + i) = (uintptr_t)&g_trampOrigRet; i += 8;
    p[i++] = 0x48; p[i++] = 0x8B; p[i++] = 0x00;  // mov rax, [rax]
    // jmp rax
    p[i++] = 0xFF; p[i++] = 0xE0;
    FLog("Trampoline at %p (%d bytes)", g_trampCode, i);
}

// VEH handler -- fires on DR0 (lua) and DR2 (magic bullet) execute breakpoints
static LONG CALLBACK LuaVehHandler(PEXCEPTION_POINTERS pEx) {
    if(pEx->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        uintptr_t addr = (uintptr_t)pEx->ExceptionRecord->ExceptionAddress;

        // Re-arm DR2 after single-stepping past HandleBullet BP
        if(g_bulletBpStepping) {
            g_bulletBpStepping = false;
            pEx->ContextRecord->Dr2 = g_fnHandleBullet;
            pEx->ContextRecord->Dr7 |= (1ULL << 4);
            pEx->ContextRecord->EFlags &= ~(DWORD64)0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Re-arm DR0 after single-stepping past settop (keeps BP permanently alive)
        if(g_settopRearmStep) {
            g_settopRearmStep = false;
            pEx->ContextRecord->Dr0 = g_fnLuaSettop;
            pEx->ContextRecord->Dr7 = (pEx->ContextRecord->Dr7 & ~0xFULL) | 1ULL;
            pEx->ContextRecord->EFlags &= ~(DWORD64)0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Hit on luaL_loadbufferx: swap buf/sz registers to inject our code
        if(g_fnLuaBpLoadBuf && addr == g_fnLuaBpLoadBuf) {
            pEx->ContextRecord->Dr0 = 0;
            pEx->ContextRecord->Dr7 &= ~1ULL;

            // Capture lua_State from RCX
            void* L = (void*)pEx->ContextRecord->Rcx;
            if(!g_luaState && L) {
                g_luaState = L;
            }

            // Replace the entire buffer with our pre-built injection code.
            // NO malloc/free here — VEH handlers must not use heap functions.
            // The original script is skipped this one call; it reloads next time.
            if(g_luaInjBuf && g_luaInjSz > 0) {
                pEx->ContextRecord->Rdx = (DWORD64)g_luaInjBuf;
                pEx->ContextRecord->R8  = (DWORD64)g_luaInjSz;
                g_luaHooked = false;
                // Don't free g_luaInjBuf here — it's used by the function we're returning to.
                // It will be freed on the next ExecLua call.
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Hit on pcallk (metadata-lua): run deferred Lua exec via trampoline
        // pcallk is called every Lua tick — much more reliable than settop
        if(g_fnLuaPcall && addr == g_fnLuaPcall) {
            // Disable DR0 on this thread (one-shot per ExecLua call)
            pEx->ContextRecord->Dr0 = 0;
            pEx->ContextRecord->Dr7 &= ~1ULL;

            if(g_luaExecPending && g_trampCode) {
                g_luaExecPending = false; // clear FIRST to prevent re-entry
                void* L = (void*)pEx->ContextRecord->Rcx;
                g_trampL = L;
                uintptr_t* rsp = (uintptr_t*)pEx->ContextRecord->Rsp;
                g_trampOrigRet = *rsp;
                *rsp = (uintptr_t)g_trampCode;
                FLog("pcallk trampoline: origRet=%llX L=%p",
                    (unsigned long long)g_trampOrigRet, L);
                Log("[Lua] pcallk hijacked -> trampoline (L=%p)", L);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Hit on lua_settop: capture state, and run deferred exec via trampoline when pending
        if(g_fnLuaSettop && addr == g_fnLuaSettop) {
            static int s_stHits = 0;
            s_stHits++;
            if(s_stHits <= 3) {
                FLog("settop hit #%d L=%p", s_stHits, (void*)pEx->ContextRecord->Rcx);
                Log("[Lua] settop BP hit #%d L=%p", s_stHits, (void*)pEx->ContextRecord->Rcx);
            }

            void* L = (void*)pEx->ContextRecord->Rcx;
            if(L && !g_luaState) {
                g_luaState = L;
                Log("[Lua] State captured: %p", L);
            }

            // Disable DR0 on this context (one-shot; ExecLua re-arms for next call)
            pEx->ContextRecord->Dr0 = 0;
            pEx->ContextRecord->Dr7 &= ~1ULL;

            // Deferred exec: hijack settop's return address so DoLuaDeferred runs
            // after settop finishes (Lua stack is consistent at that point).
            if(g_luaExecPending && g_trampCode && L) {
                g_luaExecPending = false;
                g_trampL = L;
                uintptr_t* rsp = (uintptr_t*)pEx->ContextRecord->Rsp;
                g_trampOrigRet = *rsp;
                *rsp = (uintptr_t)g_trampCode;
                FLog("settop trampoline: origRet=%llX L=%p",
                    (unsigned long long)g_trampOrigRet, L);
                Log("[Lua] settop hijacked -> trampoline (L=%p)", L);
            } else if(!g_luaState) {
                // State not yet captured: single-step to keep BP alive for next hit
                pEx->ContextRecord->EFlags |= 0x100;
                g_settopRearmStep = true;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Hit on HandleBullet (DR2): redirect bullet toward aimbot target
        if(g_fnHandleBullet && addr == g_fnHandleBullet) {
            static int s_mbHits = 0;
            if(++s_mbHits <= 5) Log("[MB] HandleBullet BP hit #%d target=%llX mb=%d", s_mbHits, (unsigned long long)g_aimTarget_early, (int)g_magicBullet);
            if(g_magicBullet && AddrOk(g_aimTarget_early)) {
                // r9 = 4th param, points to bullet direction/target Vec3
                float* r9data = (float*)pEx->ContextRecord->R9;
                if(r9data && !IsBadWritePtr(r9data, 12)) {
                    Vec3 tgt = GetBonePos(g_aimTarget_early, g_aimbotBone);
                    if(tgt.x == 0.f && tgt.y == 0.f && tgt.z == 0.f) {
                        Vec3 p = GetPos(g_aimTarget_early);
                        tgt = {p.x, p.y, p.z + 0.65f};
                    }
                    if(tgt.x != 0.f || tgt.y != 0.f || tgt.z != 0.f) {
                        r9data[0] = tgt.x;
                        r9data[1] = tgt.y;
                        r9data[2] = tgt.z;
                    }
                }
            }
            // Single-step past this instruction, then re-arm DR2
            pEx->ContextRecord->Dr2 = 0;
            pEx->ContextRecord->Dr7 &= ~(1ULL << 4);
            pEx->ContextRecord->EFlags |= 0x100; // TF
            g_bulletBpStepping = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Called once when g_gameReady becomes true -- installs BP so we catch first script load
static void LuaEarlyInit() {
    if(g_luaEarlyDone) return;
    g_luaEarlyDone = true;
    ScanLuaFunctions();
    if(!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, LuaVehHandler);
        Log("[Lua] VEH handler installed");
    }
    if(g_fnLuaBpLoadBuf) {
        SetHardwareBP(g_fnLuaBpLoadBuf);
        Log("[Lua] BP armed on luaL_loadbufferx -- waiting for first script load...");
    } else if(g_fnLuaSettop) {
        SetHardwareBP(g_fnLuaSettop);
        Log("[Lua] BP armed on lua_settop (state capture only)...");
    } else {
        Log("[Lua] No BP target found -- lua_State must be captured via ExecLua trigger");
    }
}

static void ExecLua(const char* code){
    if(!code || !code[0]) { Log("[Lua] ERROR: empty code"); return; }
    Log("\n[Lua] ====== ExecLua %zu bytes ======", strlen(code));
    FLog("ExecLua called, %zu bytes", strlen(code));

    ScanLuaFunctions();

    if(!g_fnLuaLoadBuf) { Log("[Lua] No loadbufferx found"); FLog("ABORT: no loadbufferx"); return; }
    if(!g_fnLuaPcall)   { Log("[Lua] No pcall found"); FLog("ABORT: no pcall"); return; }
    if(!g_fnLuaSettop)  { Log("[Lua] Warning: no settop -- error stack cleanup disabled"); FLog("Warning: no settop"); }
    // Ensure VEH handler is installed before arming any breakpoint
    if(!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, LuaVehHandler);
        Log("[Lua] VEH handler installed (from ExecLua)");
        FLog("VEH installed from ExecLua");
    }
    if(!g_vehHandle) { Log("[Lua] VEH install FAILED"); FLog("ABORT: VEH install failed"); return; }

    InitTrampoline();
    if(!g_trampCode) { Log("[Lua] Trampoline alloc failed"); FLog("ABORT: no trampoline"); return; }

    // Build pcall-wrapped code buffer
    std::string buf = "local _ok,_er=pcall(function()\n";
    buf += code;
    buf += "\nend) if not _ok then print('[QWAK] '..tostring(_er)) end\n";

    // Store in pre-allocated buffer (render thread — safe to malloc here)
    if(g_luaExecBuf) { free(g_luaExecBuf); g_luaExecBuf = nullptr; }
    g_luaExecBuf = (char*)malloc(buf.size() + 1);
    if(!g_luaExecBuf) { Log("[Lua] malloc failed"); FLog("ABORT: malloc failed"); return; }
    memcpy(g_luaExecBuf, buf.c_str(), buf.size() + 1);
    g_luaExecSz = buf.size();

    Log("[Lua] Queued %zu bytes for Lua thread (state=%p)", buf.size(), g_luaState);
    FLog("Queued %zu bytes, state=%p", buf.size(), g_luaState);
    g_luaExecPending = true;
    if(g_usingMetadataLua) {
        // metadata-lua pcall is only invoked during resource loading, never on a Lua tick.
        // Use settop (from scripting-lua) as the trigger — it fires every frame.
        if(!g_fnLuaSettop) { Log("[Lua] No settop trigger available"); FLog("ABORT: no settop trigger"); g_luaExecPending = false; return; }
        SetHardwareBP(g_fnLuaSettop);
        Log("[Lua] BP armed on settop (%llX) for deferred exec (metadata-lua path)", (unsigned long long)g_fnLuaSettop);
    } else {
        SetHardwareBP(g_fnLuaPcall);
        Log("[Lua] BP armed on pcallk (%llX) for deferred exec", (unsigned long long)g_fnLuaPcall);
    }
}
static bool g_gameReady     = false;

// Try all known World/Viewport offsets to find which ones are valid for this binary
static bool g_worldViewportDetected = false;
static void DetectWorldViewport() {
    if(g_worldViewportDetected) return;
    if(!g_base || !g_current_offsets) return;

    // Collect all unique World offsets from  offset table
    static const uintptr_t worldOffs[] = {
        0x247F840, 0x24C8858, 0x24E6D90, 0x252DCD8, 0x2593320, 0x2567DB0,
        0x26684D8, 0x254D448, 0x257BEA0, 0x25B14B0, 0x25C15B0, 0x25D7108,
        0x25EC580, 0x2603908
    };
    static const uintptr_t viewportOffs[] = {
        0x2087780, 0x1F6A7E0, 0x1F888C0, 0x1F9E9F0, 0x20019E0, 0x1FD8570,
        0x20D8C90, 0x1FBC100, 0x1FEAAC0, 0x201DBA0, 0x202DC50, 0x20431C0,
        0x2058BA0, 0x206C060
    };

    // Try current offsets first, then all known offsets
    auto TryWorld = [&](uintptr_t off) -> bool {
        uintptr_t addr = g_base + off;
        if(IsBadReadPtr((void*)addr,8)) return false;
        uintptr_t world = *(uintptr_t*)addr;
        if(!AddrOk(world)) return false;
        // Validate: world+0x08 should be a ped pointer (or zero if not spawned)
        if(IsBadReadPtr((void*)(world+0x08),8)) return false;
        uintptr_t ped = *(uintptr_t*)(world + 0x08);
        if(ped && AddrOk(ped) && !IsBadReadPtr((void*)(ped+0x90),12)) {
            Vec3 pos = *(Vec3*)(ped + 0x90);
            // Valid position: GTA map coords are roughly -4000..8000
            if(fabsf(pos.x) > 1.f && fabsf(pos.x) < 10000.f &&
               fabsf(pos.y) > 1.f && fabsf(pos.y) < 10000.f) {
                g_worldOverride = off;
                g_worldViewportDetected = true;
                // If a different build's World offset works, switch g_current_offsets
                for(int bi = 0; bi < g_offsets_count; bi++) {
                    if(g_offsets[bi].World == off && &g_offsets[bi] != g_current_offsets) {
                        Log("[Detect] Switching offsets from %s to %s (World match)",
                            g_current_offsets->name, g_offsets[bi].name);
                        g_current_offsets = (FiveMOffsets*)&g_offsets[bi];
                        g_detected_build = bi;
                        break;
                    }
                }
                Log("[Detect] World found at +0x%llX (world=0x%llX ped=0x%llX pos=%.1f,%.1f,%.1f)",
                    (unsigned long long)off, (unsigned long long)world,
                    (unsigned long long)ped, pos.x, pos.y, pos.z);
                return true;
            }
        }
        return false;
    };

    // Try current offsets first
    if(TryWorld(g_current_offsets->World)) {} else {
        for(auto off : worldOffs) {
            if(off == g_current_offsets->World) continue;
            if(TryWorld(off)) break;
        }
    }
    // If still not found, do a nearby scan around known offsets
    if(!g_worldOverride) {
        for(auto off : worldOffs) {
            for(int d = -0x200; d <= 0x200; d += 8) {
                if(TryWorld(off + d)) goto worldDone;
            }
        }
        worldDone:;
    }

    // Viewport detection
    auto TryViewport = [&](uintptr_t off) -> bool {
        uintptr_t vaddr = g_base + off;
        if(IsBadReadPtr((void*)vaddr,8)) return false;
        uintptr_t vp = *(uintptr_t*)vaddr;
        if(!AddrOk(vp)) return false;
        if(IsBadReadPtr((void*)(vp + 0x24C), 64)) return false;
        // Validate: view-projection matrix should have reasonable float values
        float* m = (float*)(vp + 0x24C);
        bool hasNonZero = false;
        for(int j=0;j<16;j++) {
            if(!std::isfinite(m[j]) || fabsf(m[j]) > 1e6f) return false;
            if(m[j] != 0.f) hasNonZero = true;
        }
        if(!hasNonZero) return false;
        g_viewportOverride = off;
        Log("[Detect] Viewport found at +0x%llX (vp=0x%llX)", (unsigned long long)off, (unsigned long long)vp);
        return true;
    };

    if(TryViewport(g_current_offsets->Viewport)) {} else {
        for(auto off : viewportOffs) {
            if(off == g_current_offsets->Viewport) continue;
            if(TryViewport(off)) break;
        }
    }
    if(!g_viewportOverride) {
        for(auto off : viewportOffs) {
            for(int d = -0x200; d <= 0x200; d += 8) {
                if(TryViewport(off + d)) goto vpDone;
            }
        }
        vpDone:;
    }

    if(g_worldOverride || g_viewportOverride)
        g_worldViewportDetected = true;
}

//  memory 
// Vec3 defined before VEH handler
// Windows 64-bit user-space: 0x10000 â€“ 0x7FFFFFFFFFFF. Values outside this range are kernel addresses or garbage.
// Rd/Wr use IsBadReadPtr/IsBadWritePtr to safely access any valid user-space address.
static inline bool AddrOk(uintptr_t a){ return a>=0x10000 && a<=0x7FFFFFFFFFFF; }
template<typename T> static inline T    Rd(uintptr_t a){ if(!AddrOk(a)||IsBadReadPtr((void*)a,sizeof(T))) return T{}; return *(T*)a; }
template<typename T> static inline void Wr(uintptr_t a,T v){ if(!AddrOk(a)||IsBadWritePtr((void*)a,sizeof(T))) return; *(T*)a=v; }
static inline uintptr_t RdPtr(uintptr_t a){ return Rd<uintptr_t>(a); }

static uintptr_t GetLocalPed() {
    if(!g_current_offsets||!g_base||!g_gameReady) return 0;
    uintptr_t worldOff = g_worldOverride ? g_worldOverride : g_current_offsets->World;
    uintptr_t world=RdPtr(g_base+worldOff);
    if(!world) return 0;
    return RdPtr(world+0x08);
}
static Vec3 GetPos(uintptr_t ped) {
    if(!ped) return {};
    // Use direct reads â€” ped pointers can be above AddrOk's 0x7FFFFFFF0000 limit in FiveM
    if(IsBadReadPtr((void*)(ped+0x90), 12)) return {};
    Vec3 p = *(Vec3*)(ped+0x90);
    if(p.x!=0.f||p.y!=0.f||p.z!=0.f) return p;
    if(IsBadReadPtr((void*)(ped+0x30), 8)) return {};
    uintptr_t nav = *(uintptr_t*)(ped+0x30);
    if(nav && !IsBadReadPtr((void*)(nav+0x50), 12)) return *(Vec3*)(nav+0x50);
    return {};
}

static bool WorldToScreen(float worldX, float worldY, float worldZ, float& screenX, float& screenY) {
    if(!g_current_offsets || !g_base || !g_gameReady) return false;
    float w = (float)g_screenW, h = (float)g_screenH;
    if(w <= 0.f || h <= 0.f) return false;

    uintptr_t vpOff = g_viewportOverride ? g_viewportOverride : g_current_offsets->Viewport;
    uintptr_t viewport = RdPtr(g_base + vpOff);
    if(!viewport || IsBadReadPtr((void*)(viewport + 0x24C), 64)) return false;

    // View-projection matrix sub-offset 0x24C is a RAGE engine constant (same across all builds)
    // Row-major layout, column vectors: m[col*4 + row]
    // Method: use columns 1,2,3 (Device9999 convention)
    float* m = (float*)(viewport + 0x24C);

    float sx = m[1]*worldX + m[5]*worldY + m[9] *worldZ + m[13];
    float sy = m[2]*worldX + m[6]*worldY + m[10]*worldZ + m[14];
    float sw = m[3]*worldX + m[7]*worldY + m[11]*worldZ + m[15];

    if(sw <= 0.f) return false;  // behind camera

    screenX = (w * 0.5f) * (1.f + sx / sw);
    screenY = (h * 0.5f) * (1.f - sy / sw);
    return true;
}

//  bone reading â”€
// Bone TAG hashes (from UC thread #340232 â€” use these with GetBonePosByTag).
// These are the values FiveM/RAGE uses to look up bones by name hash.
//   SKEL_Head        = 0x796E
//   SKEL_Neck_1      = 0x9995
//   SKEL_Spine3      = 0x9D4D   (upper chest)
//   SKEL_Spine2      = 0xB1C5
//   SKEL_Spine1      = 0x29D2
//   SKEL_Pelvis      = 0x2E28
//   SKEL_R_Upperarm  = 0x9D4D   (R shoulder/upper arm)
//   SKEL_R_Forearm   = 0x6E5C
//   SKEL_R_Hand      = 0xDEAD
//   SKEL_L_Upperarm  = 0xB1C5
//   SKEL_L_Forearm   = 0xEEEB
//   SKEL_L_Hand      = 0x49D9
//   SKEL_R_Thigh     = 0xCA72
//   SKEL_R_Calf      = 0x9000
//   SKEL_R_Foot      = 0xCC4D
//   SKEL_L_Thigh     = 0xE39F
//   SKEL_L_Calf      = 0xF9BB
//   SKEL_L_Foot      = 0x3779
//   Clavicle_R       = 0x29D2
//   Clavicle_L       = 0xFCD9
//   Elbow_L          = 0x58B7
//
// Sequential bone INDICES (matrix array index, used by GetBonePos below):
//   Pelvis=1  UpperChest=5  Neck=6  Head=7
//   R_UpperArm=8  R_Forearm=9   R_Hand=10
//   L_UpperArm=11 L_Forearm=12  L_Hand=13
//   R_Thigh=16 R_Knee=17 R_Foot=18
//   L_Thigh=19 L_Knee=20 L_Foot=21
//
//  bone position (CONFIRMED from UC thread pages 56/68/90/115/119) â”€
// Method (all builds b2802+, confirmed multi-page):
//   Local bone positions are stored INLINE (no deref) at:
//     ped + BoneManager + boneIndex * 0x10  (Vec3, stride=0x10, LOCAL space)
//   The entity world-transform 4x4 matrix (col-major) is accessed via:
//     Approach 0: matrix is INLINE at ped + 0x60 (external-cheat style, older builds)
//     Approach 1: *(ped + 0x60) = pointer to matrix elsewhere (newer builds)
//   World position = matrix * localBonePos
//   Validation: result must be 0.1â€“10m from entity origin (ped + 0x90)
//
// Sequential bone indices (OFFSETS.txt):
//   0=Root 1=Pelvis 2=Spine0 3=Spine1 4=Spine2 5=Chest 6=Neck 7=Head
//   8=R_UpperArm 9=R_Forearm 10=R_Hand
//   11=L_UpperArm 12=L_Forearm 13=L_Hand
//   16=R_Thigh 17=R_Knee 18=R_Foot
//   19=L_Thigh 20=L_Knee 21=L_Foot
static int      g_boneMode   = -1;  // -1=undetected, 0=inline-mat, 1=ptr-mat, 2=fragInst

// Get FragInsNmGTA offset for current build
static uintptr_t GetFragInstOff() {
    if(!g_current_offsets) return 0;
    if(g_current_offsets->build >= 2802) return 0x1430;
    if(g_current_offsets->build >= 2545) return 0x1450;
    if(g_current_offsets->build >= 2060) return 0x1400;
    return 0x13E0;
}

// Map our simple UI bone index to RAGE bone tag for skeleton lookup
static int SimpleBoneToTag(int boneIdx) {
    switch(boneIdx) {
        case 0:  return 0;      // SKEL_ROOT
        case 1:  return 11816;  // SKEL_Pelvis (SKEL_ROOT)
        case 5:  return 24818;  // SKEL_Spine3 (chest)
        case 6:  return 39317;  // SKEL_Neck_1
        case 7:  return 31086;  // SKEL_Head
        case 8:  return 40269;  // SKEL_R_UpperArm
        case 9:  return 28252;  // SKEL_R_Forearm
        case 10: return 57005;  // SKEL_R_Hand
        case 11: return 45509;  // SKEL_L_UpperArm
        case 12: return 61163;  // SKEL_L_Forearm
        case 13: return 18905;  // SKEL_L_Hand
        case 16: return 51826;  // SKEL_R_Thigh
        case 17: return 36864;  // SKEL_R_Calf
        case 18: return 52301;  // SKEL_R_Foot
        case 19: return 58271;  // SKEL_L_Thigh
        case 20: return 63931;  // SKEL_L_Calf
        case 21: return 14201;  // SKEL_L_Foot
        default: return -1;
    }
}

// Hash table lookup: bone tag → skeleton matrix index via crSkeletonData
static int BoneTagToSkelIdx(uintptr_t skeleton, int boneTag) {
    if(boneTag < 0) return -1;
    uintptr_t skelData = RdPtr(skeleton + 0x0);
    if(!AddrOk(skelData) || IsBadReadPtr((void*)skelData, 0x20)) return -1;
    uint16_t numBones = Rd<uint16_t>(skelData + 0x1A);
    uint16_t hashSize = Rd<uint16_t>(skelData + 0x18);
    if(numBones == 0 || hashSize == 0 || numBones > 300) return -1;
    uintptr_t hashTable = RdPtr(skelData + 0x10);
    if(!AddrOk(hashTable) || IsBadReadPtr((void*)hashTable, (size_t)hashSize * 8)) return -1;
    uintptr_t node = RdPtr(hashTable + 8 * ((unsigned)boneTag % hashSize));
    for(int i = 0; i < 20 && node != 0 && AddrOk(node); i++) {
        if(IsBadReadPtr((void*)node, 16)) break;
        if(Rd<int>(node + 0) == boneTag) {
            int idx = Rd<int>(node + 4);
            return (idx >= 0 && idx < numBones) ? idx : -1;
        }
        node = RdPtr(node + 0x8);
    }
    return -1;
}

static Vec3 GetBonePos(uintptr_t ped, int boneIdx) {
    if(!g_current_offsets || !ped) return {};

    Vec3 ref = GetPos(ped);
    if(fabsf(ref.x) < 2.f && fabsf(ref.y) < 2.f) return {}; // entity not spawned

    auto ValidResult = [&](float wx, float wy, float wz) -> bool {
        if(!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) return false;
        float d2 = (wx-ref.x)*(wx-ref.x)+(wy-ref.y)*(wy-ref.y)+(wz-ref.z)*(wz-ref.z);
        return d2 > 0.001f && d2 < 400.f;
    };

    // Approach 2 (best): FragInst → crSkeleton → animated per-bone world matrices
    {
        uintptr_t fragOff = GetFragInstOff();
        uintptr_t fragInst = fragOff ? RdPtr(ped + fragOff) : 0;
        if(AddrOk(fragInst) && !IsBadReadPtr((void*)fragInst, 0x70)) {
            uintptr_t primo = RdPtr(fragInst + 0x68);
            if(AddrOk(primo) && !IsBadReadPtr((void*)primo, 0x180)) {
                uintptr_t skeleton = RdPtr(primo + 0x178);
                if(AddrOk(skeleton) && !IsBadReadPtr((void*)skeleton, 0x20)) {
                    // Convert our simple bone index to a RAGE bone tag, then look up skeleton index
                    int boneTag = SimpleBoneToTag(boneIdx);
                    int skelIdx = BoneTagToSkelIdx(skeleton, boneTag);
                    if(skelIdx >= 0) {
                        uintptr_t parentPtr = RdPtr(skeleton + 0x8);
                        uintptr_t boneArr   = RdPtr(skeleton + 0x18);
                        uintptr_t boneMatAddr = boneArr + (uintptr_t)skelIdx * 64;
                        if(AddrOk(parentPtr) && !IsBadReadPtr((void*)parentPtr, 64) &&
                           AddrOk(boneArr)   && !IsBadReadPtr((void*)boneMatAddr, 64)) {
                            float* P = (float*)parentPtr;
                            float* B = (float*)boneMatAddr;
                            float bx = B[12], by = B[13], bz = B[14];
                            float wx = P[0]*bx + P[4]*by + P[ 8]*bz + P[12];
                            float wy = P[1]*bx + P[5]*by + P[ 9]*bz + P[13];
                            float wz = P[2]*bx + P[6]*by + P[10]*bz + P[14];
                            if(ValidResult(wx, wy, wz)) {
                                if(g_boneMode != 2) { Log("[Bone] Mode 2 (fragInst animated) uiIdx=%d tag=%d skelIdx=%d", boneIdx, boneTag, skelIdx); g_boneMode = 2; }
                                return {wx, wy, wz};
                            }
                        }
                    }
                }
            }
        }
    }

    // Fallback: rest-pose bones from BoneManager + entity matrix
    uintptr_t boneOff = g_current_offsets->BoneManager;
    uintptr_t boneAddr = ped + boneOff + (uintptr_t)boneIdx * 0x10;
    if(IsBadReadPtr((void*)boneAddr, 12)) return {};

    float bx = *(float*)(boneAddr + 0);
    float by = *(float*)(boneAddr + 4);
    float bz = *(float*)(boneAddr + 8);

    auto ApplyMat = [&](float* M) -> Vec3 {
        float wx = M[0]*bx + M[4]*by + M[ 8]*bz + M[12];
        float wy = M[1]*bx + M[5]*by + M[ 9]*bz + M[13];
        float wz = M[2]*bx + M[6]*by + M[10]*bz + M[14];
        if(ValidResult(wx, wy, wz)) return Vec3{wx,wy,wz};
        return {};
    };

    // Approach 1: ped+0x60 = pointer to world-transform matrix
    {
        uintptr_t matPtr = RdPtr(ped + 0x60);
        if(AddrOk(matPtr) && !IsBadReadPtr((void*)matPtr, 64)) {
            Vec3 r = ApplyMat((float*)matPtr);
            if(r.x != 0.f || r.y != 0.f || r.z != 0.f) {
                if(g_boneMode == -1) { Log("[Bone] Mode 1 (ptr mat) boneOff=0x%X idx=%d", (unsigned)boneOff, boneIdx); g_boneMode = 1; }
                return r;
            }
        }
    }

    // Approach 0: world-transform matrix INLINE at ped+0x60
    {
        if(!IsBadReadPtr((void*)(ped + 0x60), 64)) {
            Vec3 r = ApplyMat((float*)(ped + 0x60));
            if(r.x != 0.f || r.y != 0.f || r.z != 0.f) {
                if(g_boneMode == -1) { Log("[Bone] Mode 0 (inline mat) boneOff=0x%X idx=%d", (unsigned)boneOff, boneIdx); g_boneMode = 0; }
                return r;
            }
        }
    }

    if(g_boneMode == -1) {
        static int s_boneFail = 0;
        if(++s_boneFail == 1 || s_boneFail % 600 == 0) {
            Log("[Bone] FAIL #%d ped=0x%llX ref=(%.1f,%.1f,%.1f) idx=%d",
                s_boneFail, (unsigned long long)ped, ref.x, ref.y, ref.z, boneIdx);
        }
    }
    return {};
}


//  player names 
// citizen-playernames-five.dll stores a linked list of {next*, ?, id, ?, nameStr*}.
// The list head pointer and count sit at a fixed offset inside the DLL's writable
// data section. That offset changes every FiveM update, so we scan once at runtime:
//   scan walks every writable section looking for uint32 count [1..128] followed by
//   a valid heap pointer whose first node passes an id+name sanity check.
// Once found the offset is logged â†’ copy it into NameDllOff for your build entry.

struct NameTable { uintptr_t listHead; uint32_t count; bool valid; };
static NameTable g_nameTable         = {};
static bool      g_nameScanned       = false;
static float     g_nameScanRetryIn   = 5.f; // seconds until next scan attempt
static int       g_nameNodeIdOff      = 0x10; // offset of playerId within each node
static int       g_nameNodeNamePtrOff = 0x18; // offset of namePtr within each node
static int       g_nameVecStride      = 0;    // 0 = linked list (next at +0x00), >0 = vector stride

// Returns true if the entire range [addr, addr+size) is in a committed, readable page.
static bool PageReadable(uintptr_t addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if(mbi.State != MEM_COMMIT) return false;
    if(mbi.Protect & PAGE_NOACCESS)    return false;
    if(mbi.Protect & PAGE_GUARD)       return false;
    // Check the region covers our entire range
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (addr + size) <= regionEnd;
}

static void ScanNameTable(HMODULE hMod) {
    uintptr_t base = (uintptr_t)hMod;
    if(!PageReadable(base, sizeof(IMAGE_DOS_HEADER))) return;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if(dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if(!PageReadable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec  = IMAGE_FIRST_SECTION(nt);
    int   nSec = nt->FileHeader.NumberOfSections;
    Log("[Names] Scanning %d sections in DLL 0x%llX", nSec, (unsigned long long)base);

    // Validate a std::map BST node (MSVC layout from OFFSETS.txt):
    //   +0x00 Left, +0x08 Parent, +0x10 Right (ptrs)
    //   +0x18 _Color(byte), +0x19 _Isnil(byte)
    //   +0x20 key=int32  +0x28 std::string  +0x38 string_len uint64
    auto ValidNode = [&](uintptr_t node, bool allowNil) -> bool {
        if(!AddrOk(node) || !PageReadable(node, 0x40)) return false;
        uint8_t isnil = Rd<uint8_t>(node + 0x19);
        if(isnil && !allowNil) return false;
        if(isnil) return true; // sentinel OK when allowNil
        int id = Rd<int>(node + 0x20);
        if(id < 0 || id > 1023) return false;
        // Check std::string: either SSO (len<16, chars inline) or heap ptr
        uint64_t len = Rd<uint64_t>(node + 0x38);
        if(len == 0 || len > 64) return false;
        uintptr_t strData;
        if(len < 16) {
            strData = node + 0x28; // SSO inline
        } else {
            strData = RdPtr(node + 0x28);
            if(!AddrOk(strData) || !PageReadable(strData, 4)) return false;
        }
        char c0 = Rd<char>(strData);
        return c0 >= 0x20 && c0 <= 0x7E;
    };

    // Scan writable non-exec sections for std::map<int,string> layout:
    //   offset+0x0: _Myhead ptr  (sentinel node, _Isnil==1)
    //   offset+0x8: _Mysize uint64 [1..512]
    //   sentinel+0x8: _Parent ptr = tree root
    for(int s = 0; s < nSec; s++) {
        DWORD ch = sec[s].Characteristics;
        if(!(ch & IMAGE_SCN_MEM_WRITE))   continue;
        if(!(ch & IMAGE_SCN_MEM_READ))    continue;
        if(  ch & IMAGE_SCN_MEM_EXECUTE)  continue;
        uintptr_t sb  = base + sec[s].VirtualAddress;
        uint32_t  ssz = sec[s].Misc.VirtualSize;
        if(ssz < 0x10 || !PageReadable(sb, 0x10)) continue;

        for(uint32_t off = 0; off + 0x10 <= ssz; off += 8) {
            if(!PageReadable(sb + off, 0x10)) continue;
            uintptr_t sentinel = RdPtr(sb + off);
            uint64_t  count    = Rd<uint64_t>(sb + off + 0x8);
            if(count < 1 || count > 512) continue;
            if(!ValidNode(sentinel, true)) continue;
            if(Rd<uint8_t>(sentinel + 0x19) != 1) continue; // must be nil/sentinel
            uintptr_t root = RdPtr(sentinel + 0x8); // _Parent = tree root
            if(!ValidNode(root, false)) continue;
            // validate 2 BST child pointers too
            uintptr_t L = RdPtr(root + 0x00);
            uintptr_t R = RdPtr(root + 0x10);
            if(!ValidNode(L, true) || !ValidNode(R, true)) continue;

            // Read first real node name
            uint64_t len = Rd<uint64_t>(root + 0x38);
            char preview[32]={};
            uintptr_t sd = (len < 16) ? (root + 0x28) : RdPtr(root + 0x28);
            for(int j = 0; j < 31 && j < (int)len; j++) preview[j] = Rd<char>(sd + j);

            Log("[Names] âœ“ Found std::map at DLL+0x%X: sentinel=%llX root=%llX cnt=%llu first='%s'",
                sec[s].VirtualAddress + off,
                (unsigned long long)sentinel,
                (unsigned long long)root,
                (unsigned long long)count,
                preview);

            g_nameTable = { root, (uint32_t)count, true };
            return;
        }
    }
    Log("[Names] âœ— Scan failed â€” will use hardcoded NameDllOff");
}

static std::string GetPlayerName(uintptr_t ped) {
    if(!ped) return "Unknown";

    uintptr_t playerInfo = RdPtr(ped + g_current_offsets->PlayerInfo);
    if(!AddrOk(playerInfo)) return "Player";

    int playerId = Rd<int>(playerInfo + g_current_offsets->PlayerID);

    // Retry DLL handle every call until loaded
    static HMODULE hNames = nullptr;
    if(!hNames) hNames = GetModuleHandleA("citizen-playernames-five.dll");
    if(!hNames) return "ID " + std::to_string(playerId);

    // Run the section scanner once â€” it finds the correct offset automatically
    // regardless of DLL version and logs "[Names] Scan found: ... DLL+0x?????"
    if(!g_nameScanned) {
        g_nameScanned = true;
        ScanNameTable(hNames);
    }

    // If scan succeeded use it, otherwise try the hardcoded offset from the build table
    if(!g_nameTable.valid && g_current_offsets->NameDllOff) {
        uintptr_t base   = (uintptr_t)hNames;
        uintptr_t myHead = RdPtr(base + g_current_offsets->NameDllOff);
        uint64_t  mySize = Rd<uint64_t>(base + g_current_offsets->NameDllOff + 0x8);
        if(AddrOk(myHead) && mySize > 0 && mySize < 512) {
            uintptr_t root = RdPtr(myHead + 0x8);
            if(AddrOk(root) && root != myHead)
                g_nameTable = {root, (uint32_t)mySize, true};
        }
    }

    if(!g_nameTable.valid) return "ID " + std::to_string(playerId);

    // Walk the std::map BST.
    // MSVC _Tree_node layout: Left+0x00, Parent+0x08, Right+0x10,
    //   Color/Isnil+0x18, key(int)+0x20, std::string+0x28, string_len+0x38
    uintptr_t node = g_nameTable.listHead; // listHead = root node
    uintptr_t sentinel = 0;                // lazily resolved if needed
    int safety = 0;
    while(AddrOk(node) && !IsBadReadPtr((void*)node, 0x50) && safety++ < 512) {
        // Skip nil/sentinel nodes (_Isnil == 1)
        if(Rd<uint8_t>(node + 0x19)) break;

        int id = Rd<int>(node + 0x20);
        if(id == playerId) {
            uint64_t len = Rd<uint64_t>(node + 0x38);
            char name[64] = {};
            if(len < 16) {
                // SSO: chars stored inline at +0x28
                for(int i = 0; i < (int)len && i < 63; i++)
                    name[i] = Rd<char>(node + 0x28 + i);
            } else {
                // Heap: ptr to chars at +0x28
                uintptr_t ptr = RdPtr(node + 0x28);
                if(AddrOk(ptr)) {
                    for(int i = 0; i < 63; i++) {
                        name[i] = Rd<char>(ptr + i);
                        if(!name[i]) break;
                    }
                }
            }
            if(name[0]) return std::string(name);
        }
        node = RdPtr(node + (playerId < id ? 0x00 : 0x10)); // left or right
    }

    return "ID " + std::to_string(playerId);
}

static inline bool ValidCoordinate(float v) {
    return (v == v) && (v > -200000.f) && (v < 200000.f);
}

//  ped interface auto-detection â”€
// Scans candidate offsets inside CReplayInterface to find the one that
// gives a valid CPedInterface â†’ valid ped array.  Runs once per session.
static uintptr_t g_pedIfaceOff  = 0;  // detected offset from replay ptr
static uintptr_t g_pedArrayOff  = 0;  // detected offset from pedIface
static uintptr_t g_pedCountOff  = 0;  // detected offset for count
static bool      g_pedIfaceKnown = false;

static bool DetectPedInterface() {
    static bool s_logged = false;
    
    // MULTI-BUILD FALLBACK:
    // Try detected build first, then iterate through ALL builds (both forward and backward).
    // If we find a valid ReplayInterface + PedIface chain, we permanently switch
    // g_current_offsets to that build. The user's system couldn't detect the build
    // explicitly so we use the offset chain validity as the absolute source of truth!
    
    int startIdx = g_detected_build;
    if(startIdx < 0) startIdx = 0;
    
    // Helper to check if a specific build's offsets are valid
    auto TryBuildTable = [&](int buildIdx) -> bool {
        const FiveMOffsets& offsets = g_offsets[buildIdx];
        if(offsets.ReplayInterface == 0) return false;
        
        uintptr_t replay = RdPtr(g_base + offsets.ReplayInterface);
        if(!AddrOk(replay) || IsBadReadPtr((void*)replay, 0x200)) {
            Log("[ESP] TryBuild %s FAIL: replay=%llX", offsets.name, (unsigned long long)replay);
            return false;
        }
        
        if(offsets.PedIfaceOff != 0 && offsets.PedArrayOff != 0 && offsets.PedCountOff != 0) {
            if(IsBadReadPtr((void*)(replay + offsets.PedIfaceOff), 8)) {
                Log("[ESP] TryBuild %s FAIL: replay+0x%llX not readable", offsets.name, (unsigned long long)offsets.PedIfaceOff);
                return false;
            }
            uintptr_t pi = RdPtr(replay + offsets.PedIfaceOff);
            if(!AddrOk(pi) || IsBadReadPtr((void*)(pi + 0x10), 0x10)) {
                Log("[ESP] TryBuild %s FAIL: pi=%llX (replay+0x%llX)", offsets.name, (unsigned long long)pi, (unsigned long long)offsets.PedIfaceOff);
                return false;
            }
            
            if(IsBadReadPtr((void*)(pi + offsets.PedArrayOff), 8)) {
                Log("[ESP] TryBuild %s FAIL: pi+arrOff not readable", offsets.name);
                return false;
            }
            uintptr_t arr = RdPtr(pi + offsets.PedArrayOff);
            
            if(IsBadReadPtr((void*)(pi + offsets.PedCountOff), 2)) {
                Log("[ESP] TryBuild %s FAIL: pi+cntOff not readable", offsets.name);
                return false;
            }
            uint16_t cnt = Rd<uint16_t>(pi + offsets.PedCountOff);
            
            Log("[ESP] TryBuild %s: pi=%llX arr=%llX cnt=%u", offsets.name, (unsigned long long)pi, (unsigned long long)arr, (unsigned)cnt);
            if(AddrOk(arr) && !IsBadReadPtr((void*)arr, 0x10) && cnt >= 1 && cnt <= 500) {
                // Validate: slots readable AND at least 1 non-null entry with valid ped position
                bool valid = true;
                int check = cnt < 16 ? (int)cnt : 16;
                int validPeds = 0;
                for(int s = 0; s < check; s++) {
                    if(IsBadReadPtr((void*)(arr + s * 0x10), 8)) { valid = false; break; }
                    uintptr_t entry = RdPtr(arr + s * 0x10);
                    if(!entry || !AddrOk(entry)) continue;
                    if(IsBadReadPtr((void*)(entry + 0x90), 12)) continue;
                    float* epos = (float*)(entry + 0x90);
                    if(ValidCoordinate(epos[0]) && ValidCoordinate(epos[1]) &&
                       (epos[0] != 0.f || epos[1] != 0.f || epos[2] != 0.f))
                        validPeds++;
                }
                Log("[ESP] TryBuild %s: pi=%llX arr=%llX cnt=%u valid=%d validPeds=%d",
                    offsets.name, (unsigned long long)pi,
                    (unsigned long long)arr, (unsigned)cnt, (int)valid, validPeds);
                if(valid && validPeds > 0) {
                    // We found a completely valid chain using THIS build's offsets.
                    // Switch our global offsets to this build!
                    g_current_offsets = (FiveMOffsets*)&g_offsets[buildIdx];
                    g_pedIfaceOff   = offsets.PedIfaceOff;
                    g_pedArrayOff   = offsets.PedArrayOff;
                    g_pedCountOff   = offsets.PedCountOff;
                    g_pedIfaceKnown = true;
                    if(!s_logged) {
                        s_logged = true;
                        if(buildIdx != startIdx) {
                            Log("[ESP] âœ“ Switched build offsets to %s (fallback from %s)", 
                                offsets.name, g_offsets[startIdx].name);
                        }
                        Log("[ESP] âœ“ PedIface (TABLE %s): +0x%llX, +0x%llX, +0x%llX | cnt=%u",
                            offsets.name, offsets.PedIfaceOff, offsets.PedArrayOff, offsets.PedCountOff, cnt);
                    }
                    return true;
                }
            }
        }
        return false;
    };
    
    // PASS 1: Try detected build and forward (higher build numbers)
    for(int buildIdx = startIdx; buildIdx < g_offsets_count; buildIdx++) {
        if(TryBuildTable(buildIdx)) return true;
    }
    
    // PASS 2: Try backward from detected build (lower build numbers)
    for(int buildIdx = startIdx - 1; buildIdx >= 0; buildIdx--) {
        if(TryBuildTable(buildIdx)) return true;
    }
    
    // PASS 3: Auto-detect using ALL known ReplayInterface offsets (not just current)
    // Tries each known ReplayInterface, plus nearby offsets, with ped content validation
    {
        // Collect unique ReplayInterface offsets from all known builds
        uintptr_t replayOffs[32]; int nReplay = 0;
        for(int bi = 0; bi < g_offsets_count && nReplay < 32; bi++) {
            if(g_offsets[bi].ReplayInterface == 0) continue;
            bool dup = false;
            for(int d = 0; d < nReplay; d++) if(replayOffs[d] == g_offsets[bi].ReplayInterface) { dup = true; break; }
            if(!dup) replayOffs[nReplay++] = g_offsets[bi].ReplayInterface;
        }

        const uintptr_t ifaceCands[] = {0x18,0x20,0x28,
                                        0x100,0x108,0x110,0x118,0x120,0x128,0x130,0x138,
                                        0x140,0x148,0x150,0x158,0x160,0x168,0x170,0x178,
                                        0x180,0x188,0x190,0x198};
        const uintptr_t arrCands[]   = {0x100,0x108,0x110,0x118,0x120,0x128};
        const uintptr_t cntCands[]   = {0x110,0x118,0x120,0x128,0x108,0x130};
        const int nIface = sizeof(ifaceCands)/sizeof(ifaceCands[0]);
        const int nArr = sizeof(arrCands)/sizeof(arrCands[0]);
        const int nCnt = sizeof(cntCands)/sizeof(cntCands[0]);

        // Lambda to try one ReplayInterface offset with all sub-offset combos
        auto TryScanReplay = [&](uintptr_t replayOff) -> bool {
            uintptr_t replay = RdPtr(g_base + replayOff);
            if(!AddrOk(replay) || IsBadReadPtr((void*)replay, 0x200)) return false;
            for(int i = 0; i < nIface; i++) {
                uintptr_t io = ifaceCands[i];
                if(IsBadReadPtr((void*)(replay + io), 8)) continue;
                uintptr_t pi = RdPtr(replay + io);
                if(!AddrOk(pi) || IsBadReadPtr((void*)(pi + 0x10), 0x20)) continue;
                for(int j = 0; j < nArr; j++) {
                    if(IsBadReadPtr((void*)(pi + arrCands[j]), 8)) continue;
                    uintptr_t arr = RdPtr(pi + arrCands[j]);
                    if(!AddrOk(arr)) continue;
                    for(int k = 0; k < nCnt; k++) {
                        if(arrCands[j] == cntCands[k]) continue;
                        if(IsBadReadPtr((void*)(pi + cntCands[k]), 2)) continue;
                        uint16_t cnt = Rd<uint16_t>(pi + cntCands[k]);
                        if(cnt < 1 || cnt > 500) continue;
                        if(IsBadReadPtr((void*)arr, cnt < 32 ? cnt * 0x10 : 256)) continue;
                        // Validate: require at least 1 non-null entry with valid ped position
                        int check = cnt < 16 ? (int)cnt : 16;
                        int validPeds = 0;
                        for(int s = 0; s < check; s++) {
                            if(IsBadReadPtr((void*)(arr + s * 0x10), 8)) break;
                            uintptr_t entry = RdPtr(arr + s * 0x10);
                            if(!entry || !AddrOk(entry)) continue;
                            if(IsBadReadPtr((void*)(entry + 0x90), 12)) continue;
                            float* epos = (float*)(entry + 0x90);
                            if(ValidCoordinate(epos[0]) && ValidCoordinate(epos[1]) &&
                               (epos[0] != 0.f || epos[1] != 0.f || epos[2] != 0.f))
                                validPeds++;
                        }
                        if(validPeds < 3) continue;
                        g_pedIfaceOff  = io;
                        g_pedArrayOff  = arrCands[j];
                        g_pedCountOff  = cntCands[k];
                        g_pedIfaceKnown = true;
                        g_replayIfaceOverride = replayOff;
                        if(!s_logged) {
                            s_logged = true;
                            Log("[ESP] PedIface (AUTO-DETECTED): +0x%llX, +0x%llX, +0x%llX | cnt=%u validPeds=%d replayOff=0x%llX",
                                io, arrCands[j], cntCands[k], cnt, validPeds, (unsigned long long)replayOff);
                        }
                        return true;
                    }
                }
            }
            return false;
        };

        // Try all known ReplayInterface offsets
        for(int ri = 0; ri < nReplay; ri++) {
            if(TryScanReplay(replayOffs[ri])) return true;
        }

        // PASS 4: Nearby scan - try offsets +/-0x10000 at 8-byte steps around each known value
        // Only run once (expensive scan ~16K candidates per known offset)
        static bool s_nearbyDone = false;
        if(!s_nearbyDone) {
            s_nearbyDone = true;
            Log("[ESP] Starting nearby ReplayInterface scan...");
            for(int ri = 0; ri < nReplay; ri++) {
            uintptr_t base_off = replayOffs[ri];
            uintptr_t lo = base_off > 0x10000 ? base_off - 0x10000 : 0;
            uintptr_t hi = base_off + 0x10000;
            for(uintptr_t scan = lo; scan <= hi; scan += 8) {
                // Skip if this is an already-tested known offset
                bool isKnown = false;
                for(int d = 0; d < nReplay; d++) if(replayOffs[d] == scan) { isKnown = true; break; }
                if(isKnown) continue;
                if(TryScanReplay(scan)) {
                    Log("[ESP] Found ReplayInterface at base+0x%llX (near known 0x%llX)",
                        (unsigned long long)scan, (unsigned long long)base_off);
                    return true;
                }
            }
        }
        Log("[ESP] Nearby scan complete, no match found");
        } // end s_nearbyDone
    }
    
    //  Nothing worked â€” dump diagnostic info once per 120 frames 
    static int s_detectFail = 0;
    s_detectFail++;
    if(s_detectFail == 1 || s_detectFail % 120 == 0) {
        uintptr_t replay = (g_base && g_current_offsets) ?
            RdPtr(g_base + g_current_offsets->ReplayInterface) : 0;
        Log("[ESP] DetectPedInterface FAIL #%d", s_detectFail);
        Log("[ESP]   base=0x%llX replayOff=0x%llX replay=0x%llX",
            (unsigned long long)g_base,
            g_current_offsets ? (unsigned long long)g_current_offsets->ReplayInterface : 0ULL,
            (unsigned long long)replay);
        if(AddrOk(replay)) {
            uintptr_t pi_new = RdPtr(replay + 0x100);
            uintptr_t pi_old = RdPtr(replay + 0x18);
            Log("[ESP]   replay+0x100=0x%llX  replay+0x18=0x%llX (new/old pedIface)",
                (unsigned long long)pi_new, (unsigned long long)pi_old);
            if(AddrOk(pi_new)) {
                uintptr_t arr = RdPtr(pi_new + 0x108);
                uint16_t  cnt = Rd<uint16_t>(pi_new + 0x118);
                Log("[ESP]   new: pi+0x108(arr)=0x%llX  pi+0x118(cnt)=%u",
                    (unsigned long long)arr, (unsigned)cnt);
            }
            if(AddrOk(pi_old)) {
                uintptr_t arr = RdPtr(pi_old + 0x100);
                uint16_t  cnt = Rd<uint16_t>(pi_old + 0x110);
                Log("[ESP]   old: pi+0x100(arr)=0x%llX  pi+0x110(cnt)=%u",
                    (unsigned long long)arr, (unsigned)cnt);
            }
        } else {
            Log("[ESP]   ReplayInterface pointer is INVALID â€” offsets wrong or world not loaded");
        }
    }
    return false;
}

static void DrawESP() {
    if(!g_esp || !g_gameReady) return;
    if(!g_pedIfaceKnown) {
        static int s_dn=0; if(++s_dn%180==0) Log("[ESP] DrawESP skip: pedIfaceKnown=false");
        return;
    }

    uintptr_t replayOff = g_replayIfaceOverride ? g_replayIfaceOverride : g_current_offsets->ReplayInterface;
    uintptr_t replay = RdPtr(g_base + replayOff);
    if(!AddrOk(replay)) {
        static int s_r=0; if(++s_r%90==0) Log("[ESP] ReplayInterface lost");
        g_pedIfaceKnown = false;
        g_replayIfaceOverride = 0;
        return;
    }

    uintptr_t pedIface = RdPtr(replay + g_pedIfaceOff);
    if(!AddrOk(pedIface)) {
        static int s_pedIfaceFail = 0;
        s_pedIfaceFail++;
        if(s_pedIfaceFail > 300) {  // ~5 seconds of failures before reset
            g_pedIfaceKnown = false;
            g_replayIfaceOverride = 0;
            s_pedIfaceFail = 0;
            Log("[ESP] PedIface reset after 300 consecutive failures");
        }
        return;
    }

    uintptr_t pedArr = RdPtr(pedIface + g_pedArrayOff);
    uint16_t count   = Rd<uint16_t>(pedIface + g_pedCountOff);

    if(!AddrOk(pedArr) || count == 0 || count > 500) {
        static int s_ba=0; if(++s_ba%180==0) Log("[ESP] DrawESP: bad arr=%llX cnt=%u", (unsigned long long)pedArr, (unsigned)count);
        return;
    }

    static bool s_drawOnce=false;
    if(!s_drawOnce){
        s_drawOnce=true;
        Log("[ESP] DrawESP running: arr=%llX cnt=%u piOff=%llX", (unsigned long long)pedArr, (unsigned)count, (unsigned long long)g_pedIfaceOff);
        // Dump first 8 entries raw to diagnose layout
        int dumpN = count < 8 ? count : 8;
        for(int d = 0; d < dumpN; d++) {
            uintptr_t off = pedArr + (uintptr_t)d * 0x10;
            uintptr_t v0 = RdPtr(off);
            uintptr_t v8 = RdPtr(off + 0x8);
            Log("[ESP]   entry[%d] +0x0=%llX  +0x8=%llX", d, (unsigned long long)v0, (unsigned long long)v8);
        }
    }

    uintptr_t localPed = GetLocalPed();
    Vec3 localPos = localPed ? GetPos(localPed) : Vec3{};

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImVec2 center = {displaySize.x * 0.5f, displaySize.y * 0.5f};

    int drawn = 0, sNull = 0, sLocal = 0, sPos = 0, sHp = 0, sW2S = 0, sNpc = 0;

    for(uint16_t i = 0; i < count; i++) {
        uintptr_t ped = RdPtr(pedArr + (uintptr_t)i * 0x10);
        // Fallback: if offset 0x0 is null, try offset 0x8 (some builds store pointer there)
        if(!ped) {
            ped = RdPtr(pedArr + (uintptr_t)i * 0x10 + 0x8);
        }
        if(ped == localPed) { sLocal++; continue; }
        if(!ped) { sNull++; continue; }

        // Draw ALL peds (players + NPCs) -- no server ID filter
        // Server ID is only used for labeling, not filtering

        Vec3 pos = GetPos(ped);
        if(pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) { sPos++; continue; }

        float hp = Rd<float>(ped + g_current_offsets->Health);

        // Check server ID for labeling (NOT filtering)
        uintptr_t pInfo = RdPtr(ped + g_current_offsets->PlayerInfo);
        int netId = (pInfo && !IsBadReadPtr((void*)pInfo, 0x10))
                  ? Rd<int>(pInfo + g_current_offsets->PlayerID) : -1;
        bool isPlayer = (netId >= 0);
        if(!isPlayer) sNpc++;

        float dist = sqrtf((pos.x - localPos.x)*(pos.x - localPos.x) +
                           (pos.y - localPos.y)*(pos.y - localPos.y) +
                           (pos.z - localPos.z)*(pos.z - localPos.z));

        // Trigger bone detection early (first valid ped) so it works even with skeleton off
        if(g_boneMode < 0 && drawn == 0) GetBonePos(ped, 7);

        float feetScreenX, feetScreenY, headScreenX, headScreenY;

        bool feetOnScreen = WorldToScreen(pos.x, pos.y, pos.z - 1.0f, feetScreenX, feetScreenY);
        bool headOnScreen = WorldToScreen(pos.x, pos.y, pos.z + 0.8f, headScreenX, headScreenY);

        // Fallback: try center position if both feet and head fail
        if(!feetOnScreen && !headOnScreen) {
            float csx, csy;
            if(WorldToScreen(pos.x, pos.y, pos.z, csx, csy)) {
                // Estimate head/feet from center
                float estH = displaySize.y * (50.f / (dist + 10.f));
                if(estH < 15.f) estH = 15.f;
                if(estH > 400.f) estH = 400.f;
                headScreenX = csx; headScreenY = csy - estH * 0.4f;
                feetScreenX = csx; feetScreenY = csy + estH * 0.6f;
                headOnScreen = true; feetOnScreen = true;
            }
        }

        // Skip if all three points off-screen
        if(!feetOnScreen && !headOnScreen) { sW2S++; continue; }

        // Reject extreme screen coords â€” W2S can return huge values for near-edge-of-frustum players
        const float margin = 1200.f;
        if(feetOnScreen && (feetScreenX < -margin || feetScreenX > displaySize.x + margin ||
                            feetScreenY < -margin || feetScreenY > displaySize.y + margin)) {
            feetOnScreen = false;
        }
        if(headOnScreen && (headScreenX < -margin || headScreenX > displaySize.x + margin ||
                            headScreenY < -margin || headScreenY > displaySize.y + margin)) {
            headOnScreen = false;
        }
        if(!feetOnScreen && !headOnScreen) { sW2S++; continue; }

        // If only one point is on screen, estimate the other from distance
        if(!headOnScreen) {
            float estH = displaySize.y * (50.f / (dist + 10.f));
            if(estH < 15.f) estH = 15.f;
            if(estH > 400.f) estH = 400.f;
            headScreenX = feetScreenX;
            headScreenY = feetScreenY - estH;
        } else if(!feetOnScreen) {
            float estH = displaySize.y * (50.f / (dist + 10.f));
            if(estH < 15.f) estH = 15.f;
            if(estH > 400.f) estH = 400.f;
            feetScreenX = headScreenX;
            feetScreenY = headScreenY + estH;
        }

        float hpR = hp / 200.f;
        ImU32 boxCol = isPlayer
            ? IM_COL32((int)((1.f - hpR) * 255), (int)(hpR * 255), 0, 235)
            : IM_COL32(120, 120, 120, 120);  // grey for NPCs

        if(!isPlayer && !g_espNpc) continue;  // skip NPCs if toggle is off

        if(g_espLine) dl->AddLine(center, {feetScreenX, feetScreenY}, IM_COL32(0, 255, 255, 160), 1.5f);

        if(g_espBox) {
            float boxH = fabsf(feetScreenY - headScreenY);
            if(boxH < 5.f) boxH = 5.f;
            float boxW = boxH * 0.42f;
            float x = headScreenX - boxW * 0.5f;
            dl->AddRect({x-3,headScreenY-3}, {x+boxW+3,headScreenY+boxH+3}, IM_COL32(0,0,0,170), 4.f, 0, 3.f);
            dl->AddRect({x,headScreenY}, {x+boxW,headScreenY+boxH}, boxCol, 1.8f, 0, 2.f);
        }

        if(g_espHealth) {
            float boxH = fabsf(feetScreenY - headScreenY);
            if(boxH < 5.f) boxH = 5.f;
            float barX = headScreenX - (boxH * 0.42f * 0.5f) - 11.f;
            float fillH = boxH * hpR;
            dl->AddRectFilled({barX, headScreenY + boxH - fillH}, {barX + 5.f, headScreenY + boxH}, IM_COL32(0,255,0,220));
            dl->AddRect({barX, headScreenY}, {barX + 5.f, headScreenY + boxH}, IM_COL32(255,255,255,100));
        }

        std::string name = GetPlayerName(ped);
        float midX = (headScreenX + feetScreenX) * 0.5f;

        if(g_espName) dl->AddText({midX - 25, headScreenY - 28}, IM_COL32(255, 215, 80, 255), name.c_str());
        if(g_espDistance) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0fm", dist);
            dl->AddText({midX - ImGui::CalcTextSize(buf).x / 2, headScreenY - 12}, IM_COL32(255,255,255,240), buf);
        }

        drawn++;
    }

    // -- FOV circle overlay (when aimbot enabled) --
    if(g_aimbot && g_aimbotFovCircle) {
        ImVec2 io_disp = ImGui::GetIO().DisplaySize;
        ImVec2 center = {io_disp.x * 0.5f, io_disp.y * 0.5f};
        float radius = g_aimbotFov / 100.f * io_disp.x;
        // outer faint ring + inner solid ring for visibility
        dl->AddCircle(center, radius, IM_COL32(0,0,0,100), 64, 3.f);
        dl->AddCircle(center, radius, IM_COL32(255,255,100,220), 64, 1.5f);
        // crosshair dot
        dl->AddCircleFilled(center, 2.5f, IM_COL32(255,255,100,220));
    }

    static int ef = 0;
    if(++ef >= 90) {
        ef = 0;
        Log("[ESP] Active: drawn=%d | total=%d | null=%d local=%d npc=%d pos=%d hp=%d w2s=%d",
            drawn, count, sNull, sLocal, sNpc, sPos, sHp, sW2S);
    }

    // Retry ped-interface detection only if offsets seem genuinely wrong.
    // drawn=0 with high w2s just means all peds are behind the camera (normal).
    // Only retry when drawn=0 AND very few w2s fails (= no peds found at all).
    static int s_zeroDrawn = 0;
    if(drawn == 0 && sW2S < 3 && sPos == 0 && count > 5) {
        s_zeroDrawn++;
        if(s_zeroDrawn >= 600) { // ~10 seconds at 60fps
            s_zeroDrawn = 0;
            g_pedIfaceKnown = false;
            g_replayIfaceOverride = 0;
            Log("[ESP] Retry: no peds rendered & low w2s for 600 frames, resetting detection");
        }
    } else {
        s_zeroDrawn = 0;
    }

    // Teleport detection: if local player moved >200m in one frame, force re-detect.
    // After teleporting, the ped array may contain only old peds from the prev location.
    // They all fail W2S (behind camera), so drawn=0 but sW2S is high — our normal retry
    // won't trigger. Force reset so the game can repopulate the ped array.
    static Vec3 s_lastLocalPos = {};
    if(localPed && localPos.x != 0.f) {
        float dx = localPos.x - s_lastLocalPos.x;
        float dy = localPos.y - s_lastLocalPos.y;
        float dz = localPos.z - s_lastLocalPos.z;
        float moveDist = dx*dx + dy*dy + dz*dz;
        if(s_lastLocalPos.x != 0.f && moveDist > 200.f*200.f) {
            g_pedIfaceKnown = false;
            g_replayIfaceOverride = 0;
            s_zeroDrawn = 0;
            Log("[ESP] Teleport detected (%.0fm), resetting ped detection", sqrtf(moveDist));
        }
        s_lastLocalPos = localPos;
    }
}

// magic bullet target (defined before VEH handler)

// -- RawInput IAT hook (aimbot mouse injection, bypasses FiveM SendInput detection) --
typedef UINT (WINAPI *PFN_RID_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static PFN_RID_t     g_origRID      = nullptr;
static volatile LONG g_aimDeltaX   = 0;
static volatile LONG g_aimDeltaY   = 0;

static UINT WINAPI HkRawInputData(HRAWINPUT hRI, UINT cmd, LPVOID pData, PUINT pcbSize, UINT cbHdr) {
    UINT ret = g_origRID(hRI, cmd, pData, pcbSize, cbHdr);
    if(ret != (UINT)-1 && pData && cmd == RID_INPUT) {
        auto* ri = (RAWINPUT*)pData;
        if(ri->header.dwType == RIM_TYPEMOUSE) {
            if(g_show) {
                // Block ALL mouse input from reaching the game when menu is open
                ri->data.mouse.lLastX = 0;
                ri->data.mouse.lLastY = 0;
                ri->data.mouse.usButtonFlags = 0;  // block clicks (punch, shoot, etc)
                ri->data.mouse.usButtonData = 0;   // block scroll
            } else if(!(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                ri->data.mouse.lLastX += InterlockedExchange(&g_aimDeltaX, 0);
                ri->data.mouse.lLastY += InterlockedExchange(&g_aimDeltaY, 0);
            }
        }
    }
    return ret;
}

static void InstallRIDHook() {
    if(g_origRID || !g_base) return;
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    if(IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    if(IsBadReadPtr(nt, 4)) return;
    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!impDir.VirtualAddress) return;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + impDir.VirtualAddress);
    for(; !IsBadReadPtr(desc, sizeof(*desc)) && desc->Name; desc++) {
        const char* dllName = (const char*)(g_base + desc->Name);
        if(IsBadReadPtr(dllName, 8) || _stricmp(dllName, "user32.dll") != 0) continue;
        if(!desc->FirstThunk || !desc->OriginalFirstThunk) continue;
        auto* iat  = (uintptr_t*)(g_base + desc->FirstThunk);
        auto* orig = (IMAGE_THUNK_DATA*)(g_base + desc->OriginalFirstThunk);
        for(; !IsBadReadPtr(iat, 8) && *iat; iat++, orig++) {
            if(IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            uintptr_t nameRva = (uintptr_t)orig->u1.Function & ~(uintptr_t)IMAGE_ORDINAL_FLAG;
            if(!nameRva || IsBadReadPtr((void*)(g_base+nameRva), 8)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(g_base + nameRva);
            if(strcmp((const char*)ibn->Name, "GetRawInputData") != 0) continue;
            DWORD old = 0;
            VirtualProtect(iat, 8, PAGE_EXECUTE_READWRITE, &old);
            g_origRID = (PFN_RID_t)*iat;
            *iat = (uintptr_t)HkRawInputData;
            VirtualProtect(iat, 8, old, &old);
            Log("[Aim] RID hook installed (orig=0x%llX)", (unsigned long long)(uintptr_t)g_origRID);
            return;
        }
    }
    Log("[Aim] GetRawInputData not found in IAT");
}

//  aimbot 
static void TickAimbot() {
    if(!g_aimbot && !g_magicBullet) return;
    if(!g_gameReady || !g_current_offsets || !g_base) return;
    if(!(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) { g_aimTarget_early = 0; return; }

    uintptr_t localPed = GetLocalPed();
    if(!localPed || !g_pedIfaceKnown) return;
    uintptr_t replay = RdPtr(g_base + (g_replayIfaceOverride ? g_replayIfaceOverride : g_current_offsets->ReplayInterface));
    if(!AddrOk(replay)) return;
    uintptr_t pedIface = RdPtr(replay + g_pedIfaceOff);
    if(!AddrOk(pedIface)) return;
    uintptr_t pedArr = RdPtr(pedIface + g_pedArrayOff);
    uint16_t  count  = Rd<uint16_t>(pedIface + g_pedCountOff);
    if(!AddrOk(pedArr) || count == 0 || count > 500) return;

    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float cx = disp.x * 0.5f, cy = disp.y * 0.5f;
    float bestDist = (g_aimbotTargetMode == 1) ? 99999.f : g_aimbotFov;
    uintptr_t bestPed = 0;
    float bestSx = 0.f, bestSy = 0.f;

    Vec3 localPos = GetPos(localPed);

    for(uint16_t i = 0; i < count; i++) {
        uintptr_t ped = RdPtr(pedArr + (uintptr_t)i * 0x10);
        if(!AddrOk(ped) || ped == localPed) continue;

        // Player-only filter
        if(g_aimbotPlayersOnly) {
            uintptr_t pInf = RdPtr(ped + g_current_offsets->PlayerInfo);
            if(!AddrOk(pInf) || Rd<int>(pInf + g_current_offsets->PlayerID) < 0) continue;
        }

        float hp = Rd<float>(ped + g_current_offsets->Health);
        if(g_aimbotAliveOnly && (hp <= 0.01f || hp > 100000.f)) continue;
        if(hp > 100000.f) continue; // always skip garbage hp values

        Vec3 tgt = GetBonePos(ped, g_aimbotBone);
        if(tgt.x == 0.f && tgt.y == 0.f && tgt.z == 0.f) {
            Vec3 p = GetPos(ped); tgt = {p.x, p.y, p.z + 0.65f};
        }
        float sx, sy;
        if(!WorldToScreen(tgt.x, tgt.y, tgt.z, sx, sy)) continue;

        float ddx = (sx-cx)/disp.x*100.f, ddy = (sy-cy)/disp.x*100.f;
        float fovDist = sqrtf(ddx*ddx + ddy*ddy);
        if(fovDist > g_aimbotFov) continue; // must be within FOV circle

        float score;
        if(g_aimbotTargetMode == 1) {
            // Closest world distance to local player
            float dx = tgt.x - localPos.x, dy = tgt.y - localPos.y, dz = tgt.z - localPos.z;
            score = sqrtf(dx*dx + dy*dy + dz*dz);
        } else {
            // Closest to crosshair (screen distance)
            score = fovDist;
        }
        if(score < bestDist) { bestDist = score; bestPed = ped; bestSx = sx; bestSy = sy; }
    }

    g_aimTarget_early = bestPed;
    if(!bestPed) return;

    // Mouse-based aiming: set deltas for RawInput hook, then nudge input pipeline
    if(g_aimbot && g_origRID) {
        float rawDx = bestSx - cx;
        float rawDy = bestSy - cy;
        float mag = sqrtf(rawDx*rawDx + rawDy*rawDy);
        if(mag > 0.5f) {
            LONG dx, dy;
            if(mag < 3.f) {
                dx = rawDx > 0.f ? 1 : (rawDx < 0.f ? -1 : 0);
                dy = rawDy > 0.f ? 1 : (rawDy < 0.f ? -1 : 0);
            } else {
                dx = (LONG)(rawDx * g_aimbotSmooth);
                dy = (LONG)(rawDy * g_aimbotSmooth);
                if(dx == 0 && rawDx > 0.f) dx = 1;
                if(dx == 0 && rawDx < 0.f) dx = -1;
                if(dy == 0 && rawDy > 0.f) dy = 1;
                if(dy == 0 && rawDy < 0.f) dy = -1;
            }
            InterlockedExchange(&g_aimDeltaX, dx);
            InterlockedExchange(&g_aimDeltaY, dy);
            // Nudge: generate a zero-movement mouse event so the game calls
            // GetRawInputData (which our hook intercepts to inject the deltas).
            // mouse_event is a legacy API that FiveM does not block.
            mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
        }
    }
}
//  freecam (safe) 
static Vec3 g_fcPos{};
static bool g_fcInit=false;

//  magic bullet hook 
static bool      g_hbHooked         = false;
#define g_aimTarget g_aimTarget_early
// Weapon mod globals (used by both Aimbot tab and ApplyFeatures)
bool  g_wepNoRecoil  = false;
bool  g_wepNoSpread  = false;
bool  g_wepInfAmmo   = false;
float g_wepDmgMult   = 1.f;
float g_wepRangeMult = 1.f;

static void InstallBulletHook() {
    if(g_hbHooked || !g_fnHandleBullet) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    DWORD pid = GetCurrentProcessId();
    DWORD myTid = GetCurrentThreadId();
    if(Thread32First(snap, &te)) {
        do {
            if(te.th32OwnerProcessID == pid) {
                HANDLE ht = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if(ht) {
                    bool isCurrent = (te.th32ThreadID == myTid);
                    if(!isCurrent) SuspendThread(ht);
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    GetThreadContext(ht, &ctx);
                    ctx.Dr2 = g_fnHandleBullet;
                    ctx.Dr7 |= (1ULL << 4);
                    SetThreadContext(ht, &ctx);
                    if(!isCurrent) ResumeThread(ht);
                    CloseHandle(ht);
                }
            }
        } while(Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_hbHooked = true;
    Log("[MB] HWBP installed on HandleBullet DR2 (0x%llX)", (unsigned long long)g_fnHandleBullet);
}

static void RemoveBulletHook() {
    if(!g_hbHooked) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    DWORD pid = GetCurrentProcessId();
    DWORD myTid = GetCurrentThreadId();
    if(Thread32First(snap, &te)) {
        do {
            if(te.th32OwnerProcessID == pid) {
                HANDLE ht = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if(ht) {
                    if(te.th32ThreadID != myTid) SuspendThread(ht);
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    GetThreadContext(ht, &ctx);
                    ctx.Dr2 = 0;
                    ctx.Dr7 &= ~(1ULL << 4);
                    SetThreadContext(ht, &ctx);
                    if(te.th32ThreadID != myTid) ResumeThread(ht);
                    CloseHandle(ht);
                }
            }
        } while(Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_hbHooked = false;
    g_bulletBpStepping = false;
    Log("[MB] HWBP removed");
}

// WndProc hook removed â€” SetWindowLongPtrA is a known Adhesive detection vector.
// Camera will move while menu is open; this is standard for overlay-style menus.
WNDPROC g_origWndProc = nullptr;

//  AOB scanner â”€
static uintptr_t AobScan(const char* pattern, const char* mask) {
    if(!g_base) return 0;
    // Get actual module size so we don't overshoot
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), (HMODULE)g_base, &mi, sizeof(mi));
    uintptr_t end = g_base + (mi.SizeOfImage ? mi.SizeOfImage : 0x5000000ULL);
    size_t patLen = strlen(mask);
    MEMORY_BASIC_INFORMATION mbi{};
    for(uintptr_t addr = g_base; addr < end;) {
        if(!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        if(mbi.State == MEM_COMMIT &&
           (mbi.Protect == PAGE_EXECUTE_READ || mbi.Protect == PAGE_EXECUTE_READWRITE ||
            mbi.Protect == PAGE_EXECUTE_WRITECOPY)) {
            uintptr_t regionEnd = addr + mbi.RegionSize - patLen;
            for(uintptr_t p = addr; p < regionEnd; p++) {
                bool found = true;
                for(size_t i = 0; i < patLen; i++) {
                    if(mask[i] == 'x' && ((uint8_t*)p)[i] != (uint8_t)pattern[i])
                        { found = false; break; }
                }
                if(found) return p;
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 0;
}

static void ScanFunctions() {
    // HandleBullet: use per-build offset if known, fall back to AOB
    if(!g_fnHandleBullet && g_base && g_current_offsets && g_current_offsets->HandleBullet) {
        g_fnHandleBullet = g_base + g_current_offsets->HandleBullet;
        Log("[MB] HandleBullet from offsets: 0x%llX", (unsigned long long)g_fnHandleBullet);
    }
    if(!g_fnHandleBullet) {
        const char* pat = "\xF3\x41\x0F\x10\x19\xF3\x41\x0F\x10\x41\x04";
        const char* msk = "xxxxxxxxxxx";
        g_fnHandleBullet = AobScan(pat, msk);
        if(g_fnHandleBullet) Log("[Scan] HandleBullet AOB: 0x%llX", (unsigned long long)g_fnHandleBullet);
    }
    // (hook install removed â€” see note in ApplyFeatures)
    // CreateVehicle: AOB scan only (no per-build offset in table)
    if(!g_fnCreateVehicle) {
        const char* pat = "\x48\x89\x5C\x24\x08\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8B\xEC\x48\x83\xEC\x50\xF3\x0F\x10\x02";
        const char* msk = "xxxxxxxxxxxxxxxxxxxxxxxxxxx";
        g_fnCreateVehicle = AobScan(pat, msk);
        if(g_fnCreateVehicle) Log("[Scan] CreateVehicle: 0x%llX", (unsigned long long)g_fnCreateVehicle);
    }
    // RequestModel: AOB scan
    if(!g_fnRequestModel) {
        const char* pat = "\x48\x89\x5C\x24\x08\x48\x89\x7C\x24\x20\x55\x48\x8B\xEC\x48\x83\xEC\x50";
        const char* msk = "xxxxxxxxxxxxxxxxxx";
        g_fnRequestModel = AobScan(pat, msk);
        if(g_fnRequestModel) Log("[Scan] RequestModel: 0x%llX", (unsigned long long)g_fnRequestModel);
    }
    // fpAttachEntityToEntity (OFFSETS.txt confirmed AOB)
    if(!g_fnAttachEntity) {
        const char* pat = "\x48\x8B\xC4\x48\x89\x58\x00\x48\x89\x68\x00\x48\x89\x70\x00\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x00\x0F\x29\x70\x00\x45\x0F\xB7\xE1";
        const char* msk = "xxxxxx?xxx?xxx?xxxxxxxxxxxx?xxx?xxxx";
        g_fnAttachEntity = AobScan(pat, msk);
        if(g_fnAttachEntity) Log("[Scan] AttachEntity: 0x%llX", (unsigned long long)g_fnAttachEntity);
    }
    // RawInput IAT hook for aimbot (install once)
    if(!g_origRID) InstallRIDHook();
}

//  teleport / utils â”€
static void SetPedPos(uintptr_t ped, Vec3 pos) {
    // Write to both nav component and direct position for reliability
    uintptr_t nav = RdPtr(ped + 0x30);
    if(AddrOk(nav)) Wr<Vec3>(nav + 0x50, pos);
    Wr<Vec3>(ped + 0x90, pos);
}

static Vec3 GetWaypoint() {
    if(!g_base || !g_current_offsets) return {};
    uintptr_t off = g_current_offsets->WaypointOff;
    if(!off) {
        Log("[TP] WaypointOff=0 for this build â€” waypoint not available");
        return {};
    }
    float x = Rd<float>(g_base + off);
    float y = Rd<float>(g_base + off + 4);
    float z = Rd<float>(g_base + off + 8);
    // Z may be 0 for map waypoints â€” that's fine, we add offset on teleport
    if(x == 0.f && y == 0.f) {
        Log("[TP] Waypoint is (0,0) â€” place a waypoint on the map first");
        return {};
    }
    if(!ValidCoordinate(x) || !ValidCoordinate(y)) {
        Log("[TP] Waypoint coords invalid: %.1f, %.1f", x, y);
        return {};
    }
    Log("[TP] Waypoint: %.1f, %.1f (z=%.1f)", x, y, z);
    return {x, y, z > 0.f ? z : 0.f}; // Z=0 if not set, teleport adds +5 to avoid underground
}

static Vec3 g_frozenPedPos = {};
static uintptr_t g_spectateTarget = 0; // ped to follow when in spectate mode

// Teleport freeze: use ped+0x2E bit 1 to freeze entity during teleport
static Vec3 g_tpTarget = {};
static int  g_tpFrames = 0;  // frames remaining to hold freeze

static void TickFreecam() {
    if(!g_freecam || !g_gameReady) {
        if(g_fcInit && g_base && g_current_offsets && g_current_offsets->FreecamByte)
            Wr<uint8_t>(g_base + g_current_offsets->FreecamByte, 0); // re-enable game camera
        g_spectateTarget = 0;
        g_fcInit = false;
        return;
    }

    uintptr_t ped = GetLocalPed();
    if(!ped) return;

    uintptr_t camBase = RdPtr(g_base + g_current_offsets->Camera);
    if(!AddrOk(camBase)) return;

    if(!g_fcInit) {
        g_frozenPedPos = GetPos(ped);
        // Camera world position is at camBase + 0x3D0 (OFFSETS.txt confirmed)
        // NOT followCam+0x60 which is the look-at/crosshair target
        g_fcPos = Rd<Vec3>(camBase + 0x3D0);
        if(fabsf(g_fcPos.x) < 1.f && fabsf(g_fcPos.y) < 1.f) {
            // Camera pos invalid â€” fall back to ped position + eye height
            g_fcPos = g_frozenPedPos;
            g_fcPos.z += 1.8f;
        }
        // Write FreecamByte to disable game camera processing (OFFSETS.txt confirmed)
        // This prevents the game overwriting our camera position each frame.
        if(g_current_offsets->FreecamByte)
            Wr<uint8_t>(g_base + g_current_offsets->FreecamByte, 1);
        g_fcInit = true;
        Log("[Freecam] Init at (%.1f, %.1f, %.1f)", g_fcPos.x, g_fcPos.y, g_fcPos.z);
    }

    float dt = ImGui::GetIO().DeltaTime;
    if(dt <= 0.f || dt > 0.1f) dt = 0.016f;
    float speed = g_freecamSpeed * dt;

    // Direction: use camera yaw from follow cam (OFFSETS.txt: viewAngles at +0x40, y=yaw in degrees)
    uintptr_t followCam = RdPtr(camBase + g_current_offsets->GameplayCamOff);
    Vec3 right = {1.f,0.f,0.f}, fwd = {0.f,1.f,0.f};
    if(AddrOk(followCam)) {
        Vec3 angles = Rd<Vec3>(followCam + 0x40);
        // RAGE stores yaw in angles.z (heading), pitch in angles.x
        float yaw = angles.z * 3.14159265f / 180.f;
        right = {cosf(yaw), sinf(yaw), 0.f};
        fwd = {-sinf(yaw), cosf(yaw), 0.f};
    }

    // If spectating a target, track their position (3rd-person offset behind/above)
    if(g_spectateTarget && !IsBadReadPtr((void*)g_spectateTarget, 0x100)) {
        Vec3 tPos = GetPos(g_spectateTarget);
        if(tPos.x != 0.f || tPos.y != 0.f) {
            // Q/E adjust height offset
            if(GetAsyncKeyState('E') & 0x8000) g_fcPos.z += speed;
            if(GetAsyncKeyState('Q') & 0x8000) g_fcPos.z -= speed;
            // Lock XY to target, keep Z offset
            float zOff = g_fcPos.z - tPos.z;
            if(zOff < 0.5f) zOff = 0.5f;
            g_fcPos = {tPos.x, tPos.y, tPos.z + zOff};
        }
    } else {
        // Normal freecam WASD movement
        if(GetAsyncKeyState('W') & 0x8000) { g_fcPos.x += fwd.x*speed;   g_fcPos.y += fwd.y*speed; }
        if(GetAsyncKeyState('S') & 0x8000) { g_fcPos.x -= fwd.x*speed;   g_fcPos.y -= fwd.y*speed; }
        if(GetAsyncKeyState('A') & 0x8000) { g_fcPos.x -= right.x*speed; g_fcPos.y -= right.y*speed; }
        if(GetAsyncKeyState('D') & 0x8000) { g_fcPos.x += right.x*speed; g_fcPos.y += right.y*speed; }
        if(GetAsyncKeyState('E') & 0x8000) g_fcPos.z += speed;
        if(GetAsyncKeyState('Q') & 0x8000) g_fcPos.z -= speed;
    }

    // Write camera world position
    Wr<Vec3>(camBase + 0x3D0, g_fcPos);

    // Freeze ped in place (don't clear tasks - that causes bent knees)
    SetPedPos(ped, g_frozenPedPos);
}

//  features 
static void ApplyFeatures() {
    if(!g_gameReady || !g_current_offsets || !g_base) return;

    // Always try ped-interface detection every 30 frames regardless of g_esp,
    // so the Players tab can show data even when the ESP overlay is disabled.
    if(!g_pedIfaceKnown) {
        static int s_detectTick = 0;
        if(++s_detectTick % 30 == 0) DetectPedInterface();
    }

    uintptr_t ped = GetLocalPed();
    if(!ped) {
        static int s_noPed = 0;
        if(++s_noPed == 60) Log("[Apply] gameReady=1 but no local ped");
        return;
    }

    //  god mode 
    if(g_godMode) {
        if(g_current_offsets->GodMode) Wr<uint8_t>(ped + g_current_offsets->GodMode, 1);
        float mhp = Rd<float>(ped + g_current_offsets->MaxHealth);
        Wr<float>(ped + g_current_offsets->Health, mhp > 0.f ? mhp : 200.f);
    }

    //  infinite armor 
    if(g_infArmor) Wr<float>(ped + g_current_offsets->Armor, 100.f);

    //  never wanted 
    if(g_noWanted) {
        uintptr_t pi = RdPtr(ped + g_current_offsets->PlayerInfo);
        if(AddrOk(pi)) Wr<uint32_t>(pi + 0xEC, 0);
    }

    //  invisibility â€” use both alpha byte (client) and Lua SET_ENTITY_VISIBLE (server) 
    if(g_invisible) {
        Wr<uint8_t>(ped + 0xAC, 0); // client-side alpha
    }

    //  anti-ragdoll: ConfigFlags only (FragInsNmGTA kept intact for bone reading)
    if(g_ragdoll) {
        if(g_current_offsets->ConfigFlags) {
            uint32_t cf = Rd<uint32_t>(ped + g_current_offsets->ConfigFlags);
            cf &= ~(1u<<26); cf &= ~(1u<<19);
            Wr<uint32_t>(ped + g_current_offsets->ConfigFlags, cf);
        }
    }
    // Teleport freeze: use entity freeze byte to lock position
    if(g_tpFrames > 0) {
        // Freeze ped in place (bit 1 of ped+0x2E)
        uint8_t flags = Rd<uint8_t>(ped + 0x2E);
        Wr<uint8_t>(ped + 0x2E, flags | 2);
        SetPedPos(ped, g_tpTarget);
        g_tpFrames--;
        if(g_tpFrames == 0) {
            // Unfreeze
            flags = Rd<uint8_t>(ped + 0x2E);
            Wr<uint8_t>(ped + 0x2E, flags & ~2);
        }
    }

    //  teleport to waypoint (one-shot) 
    if(g_tpWaypoint) {
        g_tpWaypoint = false;
        Vec3 wp = GetWaypoint();
        if(wp.x != 0.f || wp.y != 0.f) {
            // Use wp.z if available, otherwise add offset to avoid underground
            float tpZ = wp.z > 0.f ? wp.z : 5.f;
            g_tpTarget = {wp.x, wp.y, tpZ};
            g_tpFrames = 15;
            SetPedPos(ped, g_tpTarget);
            Log("[TP] Teleported to waypoint: (%.1f, %.1f, %.1f)", wp.x, wp.y, tpZ);
        }
    }

    //  vehicle features 
    uintptr_t veh = RdPtr(ped + g_current_offsets->VehicleInterface);
    if(AddrOk(veh)) {
        if(g_vehGod) {
            Wr<float>(veh + 0x820, 1000.f); // body health
            Wr<float>(veh + 0x824, 1000.f); // tank health
        }
        if(g_vehRepair) {
            g_vehRepair = false;
            Wr<float>(veh + 0x820, 1000.f);
            Wr<float>(veh + 0x824, 1000.f);
        }
        if(g_vehBoost) {
            Wr<float>(veh + 0xD40, g_vehBoostMult);
        }
        if(g_vehLock) Wr<int>(veh + 0x13C0, 2);
    }

    //  speed boost (ped on foot) 
    if(g_speedBoost && !AddrOk(veh)) {
        // speed boost handled via velocity if needed
    }

    //  vehicle spawn 
    // Direct CreateVehicle calls crash because they must run inside GTA's
    // scripting fiber thread. Spawn is logged as pending; a future update
    //  vehicle spawn (async: RequestModel â†’ wait HasModelLoaded â†’ CreateVehicle) 
    // Vehicle spawn via Lua (CreateVehicle called from render thread crashes â€” use ExecLua)
    if(g_spawnReq) {
        g_spawnReq = false;
        Log("\n==================== USER: VEHICLE SPAWN CLICKED ====================");
        Vec3 sp = GetPos(ped);
        // Validate spawn position
        if(!ValidCoordinate(sp.x) || !ValidCoordinate(sp.y) || (sp.x == 0.f && sp.y == 0.f)) {
            Log("[Spawn] ERROR: Invalid ped position, cannot spawn");
        } else {
        Log("[Spawn] Model: %s | Position: (%.1f, %.1f, %.1f)", g_spawnModel, sp.x+5.f, sp.y, sp.z+1.f);
        // TEST: just print to F8 console to confirm Lua fires before doing anything
        const char* lua = "print('[QWAK] Lua test fired OK')";
        Log("[Spawn] *** CALLING ExecLua() ***");
        ExecLua(lua);
        Log("[Spawn] ========== COMPLETE ==========\n");
        } // end else (valid position)
    }

    // Install/remove magic bullet hook based on toggle
    {
        static bool s_mbLogged = false;
        if(g_magicBullet && !s_mbLogged) {
            s_mbLogged = true;
            Log("[MB] Toggle ON: fnHB=%llX hbHooked=%d", (unsigned long long)g_fnHandleBullet, (int)g_hbHooked);
        }
        if(!g_magicBullet) s_mbLogged = false;
    }
    if(g_magicBullet && !g_hbHooked) InstallBulletHook();
    if(!g_magicBullet && g_hbHooked) RemoveBulletHook();

    //  weapon mods (globals set by Aimbot tab UI) 
    extern bool  g_wepNoRecoil, g_wepNoSpread, g_wepInfAmmo;
    extern float g_wepDmgMult,  g_wepRangeMult;
    if(g_wepNoRecoil || g_wepNoSpread || g_wepInfAmmo || g_wepDmgMult != 1.f || g_wepRangeMult != 1.f) {
        uintptr_t wm = RdPtr(ped + g_current_offsets->WeaponManager);
        if(AddrOk(wm)) {
            uintptr_t cw = RdPtr(wm + 0x20); // ptr to current CWeapon (WeaponManager+0x20)
            if(AddrOk(cw)) {
                static uintptr_t s_lastWep = 0;
                bool wepChanged = (s_lastWep != cw);
                if(wepChanged) {
                    s_lastWep = cw;
                    Log("[Weapon] New weapon equipped at 0x%llX", (unsigned long long)cw);
                }
                // Spread/recoil are floats; 0 = none  (b2802+: Spread=0x84, Recoil=0x2F4)
                if(g_wepNoRecoil) Wr<float>(cw + 0x2F4, 0.f);
                if(g_wepNoSpread) Wr<float>(cw + 0x84,  0.f);
                // Ammo: traverse CWeapon -> CAmmoInfo -> CAmmoWrap -> CAmmo -> count
                // Chain: cw+0x60 -> +0x8 -> +0x0 -> +0x18 (per-instance, safe)
                if(g_wepInfAmmo) {
                    uintptr_t ammoInfo = RdPtr(cw + 0x60);
                    if(AddrOk(ammoInfo)) {
                        uintptr_t ammoWrap = RdPtr(ammoInfo + 0x8);
                        if(AddrOk(ammoWrap)) {
                            uintptr_t ammo = RdPtr(ammoWrap + 0x0);
                            if(AddrOk(ammo)) {
                                Wr<int>(ammo + 0x18, 9999);
                            }
                        }
                    }
                }
                // Damage: read default then scale  (OFFSETS.txt: BulletDamage=0xBC)
                if(g_wepDmgMult != 1.f) {
                    static float s_origDmg = 0.f;
                    static uintptr_t s_dmgCw = 0;
                    if(s_dmgCw != cw) {
                        s_origDmg = Rd<float>(cw + 0xBC);
                        s_dmgCw = cw;
                        if(s_origDmg > 0.01f) {
                            Log("[Weapon] Damage base: %.2f â†’ %.2f", s_origDmg, s_origDmg * g_wepDmgMult);
                        }
                    }
                    if(s_origDmg > 0.01f) {
                        Wr<float>(cw + 0xBC, s_origDmg * g_wepDmgMult);
                    }
                }
                // Range  (OFFSETS.txt: Range=0x28C)
                if(g_wepRangeMult != 1.f) {
                    static float s_origRng = 0.f;
                    static uintptr_t s_rngCw = 0;
                    if(s_rngCw != cw) {
                        s_origRng = Rd<float>(cw + 0x28C);
                        s_rngCw = cw;
                        if(s_origRng > 0.01f) {
                            Log("[Weapon] Range base: %.2f â†’ %.2f", s_origRng, s_origRng * g_wepRangeMult);
                        }
                    }
                    if(s_origRng > 0.01f) {
                        Wr<float>(cw + 0x28C, s_origRng * g_wepRangeMult);
                    }
                }
            } else {
                static int s_cwFail = 0;
                if(++s_cwFail % 300 == 0) Log("[Weapon] Current weapon ptr invalid");
            }
        } else {
            static int s_wmFail = 0;
            if(++s_wmFail % 300 == 0) Log("[Weapon] WeaponManager ptr invalid");
        }
    }

    TickAimbot();
    if(g_freecam) TickFreecam(); else g_fcInit = false;
    DrawESP();
}

//  UI & hook (FULL original UI - nothing cut) â”€
static ImVec4 Accent()   {return{0.20f,0.60f,1.00f,1.f};}
static ImVec4 AccentDim(){return{0.12f,0.35f,0.70f,1.f};}
static ImVec4 Good()     {return{0.20f,0.90f,0.50f,1.f};}
static ImVec4 Muted()    {return{0.38f,0.40f,0.50f,1.f};}

static void HRule(){
    ImDrawList* dl=ImGui::GetWindowDrawList(); ImVec2 p=ImGui::GetCursorScreenPos();
    dl->AddLine({p.x,p.y},{p.x+ImGui::GetContentRegionAvail().x,p.y},IM_COL32(50,70,120,120));
    ImGui::Dummy({0,1});
}
static void SectionLabel(const char* txt){
    ImGui::Dummy({0,4});ImGui::TextColored(AccentDim(),"  %s",txt);HRule();ImGui::Dummy({0,2});
}
static bool Toggle(const char* id,const char* label,bool* v){
    const float W=36.f,H=18.f,R=H*.5f;
    ImDrawList* dl=ImGui::GetWindowDrawList(); ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id,{W+ImGui::CalcTextSize(label).x+8.f,H});
    if(ImGui::IsItemClicked())*v=!*v;
    bool hov=ImGui::IsItemHovered();
    ImU32 bg=*v?IM_COL32(30,140,255,255):IM_COL32(38,40,52,255);
    ImU32 kn=*v?IM_COL32(255,255,255,255):IM_COL32(120,125,145,255);
    if(hov&&!*v) bg=IM_COL32(50,55,75,255);
    float kx=*v?(p.x+W-R):(p.x+R);
    dl->AddRectFilled({p.x,p.y+1},{p.x+W,p.y+H-1},bg,R);
    dl->AddCircleFilled({kx,p.y+R},R-2.5f,kn);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+(H-ImGui::GetTextLineHeight())*.5f);
    ImGui::TextColored(*v?ImVec4(.92f,.95f,1.f,1.f):ImVec4(.5f,.52f,.6f,1.f),"%s",label);
    return *v;
}
static void Badge(const char* txt,ImVec4 col){
    ImVec2 ts=ImGui::CalcTextSize(txt);
    ImVec2 p={ImGui::GetWindowPos().x+ImGui::GetWindowWidth()-ts.x-18.f,ImGui::GetWindowPos().y+8.f};
    ImDrawList* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled({p.x-6,p.y-2},{p.x+ts.x+6,p.y+ts.y+2},
        IM_COL32((int)(col.x*180),(int)(col.y*180),(int)(col.z*180),60),6.f);
    dl->AddText(p,IM_COL32((int)(col.x*255),(int)(col.y*255),(int)(col.z*255),255),txt);
}
static void CopyToClipboard(const std::string& t){
    if(!OpenClipboard(nullptr)) return; EmptyClipboard();
    HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,t.size()+1); if(!hg){CloseClipboard();return;}
    memcpy(GlobalLock(hg),t.c_str(),t.size()+1); GlobalUnlock(hg);
    SetClipboardData(CF_TEXT,hg); CloseClipboard();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

HRESULT __stdcall hkSteamPresent(IDXGISwapChain* sc,UINT sync,UINT flags){
    if(!g_init){
        if(InterlockedCompareExchange(&g_init_lock,1,0)==0){
            if(SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device),(void**)&g_dev))){
                g_dev->GetImmediateContext(&g_ctx);
                DXGI_SWAP_CHAIN_DESC desc; sc->GetDesc(&desc);
                g_hwnd=desc.OutputWindow;
                g_screenW=(float)desc.BufferDesc.Width; g_screenH=(float)desc.BufferDesc.Height;
                g_base=(uintptr_t)GetModuleHandleA(nullptr);
                if(!g_base) g_base=(uintptr_t)GetModuleHandleA("FiveM_GTAProcess.exe");
                Log("Base: 0x%llX",(unsigned long long)g_base);
                DetectGameBuild();
                // g_gameReady set by per-frame DetectWorldViewport() below
                IMGUI_CHECKVERSION(); ImGui::CreateContext();
                ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; io.MouseDrawCursor=true;
                ImGuiStyle& s=ImGui::GetStyle();
                s.WindowRounding=14.f;s.ChildRounding=10.f;s.FrameRounding=6.f;
                s.GrabRounding=4.f;s.TabRounding=6.f;s.WindowBorderSize=0.f;s.FrameBorderSize=0.f;
                s.WindowPadding={16,14};s.FramePadding={10,6};s.ItemSpacing={8,7};
                s.ScrollbarSize=6.f;s.GrabMinSize=10.f;
                ImVec4* c=s.Colors;
                c[ImGuiCol_WindowBg]           ={.055f,.058f,.090f,.97f};
                c[ImGuiCol_ChildBg]            ={.040f,.042f,.068f,1.f};
                c[ImGuiCol_PopupBg]            ={.060f,.062f,.095f,.98f};
                c[ImGuiCol_Border]             ={.12f,.16f,.30f,.60f};
                c[ImGuiCol_FrameBg]            ={.10f,.11f,.17f,1.f};
                c[ImGuiCol_FrameBgHovered]     ={.14f,.16f,.25f,1.f};
                c[ImGuiCol_FrameBgActive]      ={.18f,.20f,.32f,1.f};
                c[ImGuiCol_ScrollbarBg]        ={.03f,.03f,.05f,1.f};
                c[ImGuiCol_ScrollbarGrab]      ={.15f,.20f,.38f,1.f};
                c[ImGuiCol_ScrollbarGrabHovered]={.20f,.28f,.50f,1.f};
                c[ImGuiCol_ScrollbarGrabActive] ={.25f,.35f,.65f,1.f};
                c[ImGuiCol_CheckMark]          ={.20f,.90f,.55f,1.f};
                c[ImGuiCol_SliderGrab]         ={.20f,.55f,1.f,1.f};
                c[ImGuiCol_SliderGrabActive]   ={.30f,.70f,1.f,1.f};
                c[ImGuiCol_Button]             ={.13f,.20f,.42f,1.f};
                c[ImGuiCol_ButtonHovered]      ={.20f,.40f,.80f,1.f};
                c[ImGuiCol_ButtonActive]       ={.15f,.30f,.65f,1.f};
                c[ImGuiCol_Header]             ={.13f,.20f,.42f,.80f};
                c[ImGuiCol_HeaderHovered]      ={.20f,.38f,.75f,.90f};
                c[ImGuiCol_HeaderActive]       ={.25f,.45f,.85f,1.f};
                c[ImGuiCol_Separator]          ={.12f,.16f,.30f,.80f};
                c[ImGuiCol_Tab]                ={.08f,.10f,.18f,1.f};
                c[ImGuiCol_TabHovered]         ={.20f,.40f,.80f,1.f};
                c[ImGuiCol_TabActive]          ={.14f,.28f,.65f,1.f};
                c[ImGuiCol_TabUnfocusedActive] ={.10f,.18f,.40f,1.f};
                c[ImGuiCol_Text]               ={.88f,.90f,.96f,1.f};
                c[ImGuiCol_TextDisabled]       ={.35f,.37f,.48f,1.f};
                ImGui_ImplWin32_Init(g_hwnd); ImGui_ImplDX11_Init(g_dev,g_ctx);
                g_init=true; Log("Init complete");
                // WndProc subclassing removed (Adhesive detects it)
                // Background AOB scan for spawning etc.
                CreateThread(nullptr,0,[](LPVOID)->DWORD{
                    Sleep(3000); // wait for game to finish loading
                    ScanFunctions();
                    return 0;
                },nullptr,0,nullptr);
            }
        }
    }
    if(!g_init) return oSteamPresent(sc,sync,flags);


    static bool lh=false; bool home=(GetAsyncKeyState(VK_HOME)&0x8000)!=0;
    if(home&&!lh) g_show=!g_show; lh=home;

    if(!g_rtv){
        ID3D11Texture2D* back=nullptr;
        if(SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))){g_dev->CreateRenderTargetView(back,nullptr,&g_rtv);back->Release();}
    }
    DXGI_SWAP_CHAIN_DESC d2; sc->GetDesc(&d2);
    g_screenW=(float)d2.BufferDesc.Width; g_screenH=(float)d2.BufferDesc.Height;

    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    ImGuiIO& io=ImGui::GetIO();
    POINT mp; GetCursorPos(&mp); ScreenToClient(g_hwnd,&mp);
    io.MousePos={float(mp.x),float(mp.y)};
    if(g_show) {
        // Menu open: feed all mouse buttons, show cursor, block RMB from game
        io.MouseDown[0]=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
        io.MouseDown[1]=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
        io.MouseDrawCursor = true;
    } else {
        // Menu closed: hide cursor, don't steal mouse from game
        io.MouseDown[0] = false;
        io.MouseDown[1] = false;
        io.MouseDrawCursor = false;
    }
ImGui::NewFrame();

// Retry g_gameReady every frame until world pointer is valid
if(!g_gameReady && g_base && g_current_offsets) {
    DetectWorldViewport(); // try all known offsets
    uintptr_t worldOff2 = g_worldOverride ? g_worldOverride : g_current_offsets->World;
    uintptr_t world = RdPtr(g_base + worldOff2);
    if(AddrOk(world)) {
        g_gameReady = true;
        Log("GameReady (world=0x%llX worldOff=0x%llX)", (unsigned long long)world, (unsigned long long)worldOff2);
    }
}
if(g_gameReady) {
    LuaEarlyInit(); // arm luaL_loadbufferx BP as early as possible to capture lua_State
    // Re-arm settop BP for state capture if Lua thread started after initial arm
    if(!g_luaState && g_fnLuaSettop) {
        static DWORD s_lastRearm = 0;
        DWORD now = GetTickCount();
        if(now - s_lastRearm > 2000) { // every 2s, just for state capture
            s_lastRearm = now;
            SetHardwareBP(g_fnLuaSettop);
        }
    }
    ApplyFeatures();
}

    if(g_show){
        g_time+=io.DeltaTime; float pulse=.5f+.5f*sinf(g_time*1.8f);
        ImGui::SetNextWindowSize({480,700},ImGuiCond_Once);
        ImGui::SetNextWindowPos({60,60},ImGuiCond_Once);
        ImGui::Begin("##qwak",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl=ImGui::GetWindowDrawList(); ImVec2 wp=ImGui::GetWindowPos(); float ww=ImGui::GetWindowWidth();
        float barH=48.f;
        dl->AddRectFilledMultiColor({wp.x,wp.y},{wp.x+ww,wp.y+barH},
            IM_COL32(14,18,40,255),IM_COL32(20,28,65,255),IM_COL32(20,28,65,255),IM_COL32(14,18,40,255));
        float ap=.6f+.4f*pulse;
        dl->AddRectFilledMultiColor({wp.x,wp.y+barH-2},{wp.x+ww,wp.y+barH},
            IM_COL32(0,(int)(120*ap),(int)(255*ap),200),IM_COL32(40,(int)(160*ap),255,200),
            IM_COL32(40,(int)(160*ap),255,200),IM_COL32(0,(int)(120*ap),(int)(255*ap),200));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.3f+.2f*pulse,.7f+.15f*pulse,1.f,1.f));
        ImGui::SetWindowFontScale(1.8f);
        float tw=ImGui::CalcTextSize("QWAK").x;
        ImGui::SetCursorPos({(ww-tw)*.5f,(barH-ImGui::GetTextLineHeight())*.5f});
        ImGui::Text("QWAK"); ImGui::SetWindowFontScale(1.f); ImGui::PopStyleColor();
        Badge(g_current_offsets?g_current_offsets->name:"?",Accent());
        ImGui::SetCursorPosY(barH+8.f);

        {
            ImGui::BeginChild("##st",{0,32},false,ImGuiWindowFlags_NoScrollbar);
            ImVec2 cp=ImGui::GetCursorScreenPos(); float sw2=ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddRectFilled({cp.x,cp.y},{cp.x+sw2,cp.y+28},IM_COL32(25,28,48,255),6.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+5.f); ImGui::SetCursorPosX(8.f);
            auto Dot=[&](bool on,ImVec4 col,const char* lbl){
                ImVec2 p2=ImGui::GetCursorScreenPos();
                ImU32 cc=on?IM_COL32((int)(col.x*255),(int)(col.y*255),(int)(col.z*255),255):IM_COL32(50,52,68,255);
                ImGui::GetWindowDrawList()->AddCircleFilled({p2.x+6,p2.y+7},4.f,cc);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX()+16);
                ImGui::TextColored(on?col:Muted(),"%s",lbl); ImGui::SameLine(0,14);
            };
            Dot(g_esp,Good(),"ESP");   Dot(g_aimbot,Accent(),"AIM");
            Dot(g_godMode,{1.f,.7f,.2f,1.f},"GOD"); Dot(g_freecam,{.8f,.4f,1.f,1.f},"CAM");
            Dot(g_invisible,{.5f,.9f,1.f,1.f},"INV"); Dot(g_vehGod,{1.f,.5f,.2f,1.f},"VEH");
            ImGui::EndChild();
        }
        ImGui::Spacing();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{12,6});
        if(ImGui::BeginTabBar("##tabs",ImGuiTabBarFlags_NoTabListScrollingButtons)){

            //  ESP Tab â”€
            if(ImGui::BeginTabItem(" ESP ")){
                ImGui::Spacing(); Toggle("##esp","Enable ESP",&g_esp);
                if(g_esp){
                    SectionLabel("VISIBILITY"); ImGui::Indent(8);
                    Toggle("##eb","Bounding Box",&g_espBox);
                    Toggle("##en","Name Tag",    &g_espName);
                    Toggle("##eh","Health Bar",  &g_espHealth);
                    Toggle("##ed","Distance",    &g_espDistance);
                    Toggle("##line","Snaplines", &g_espLine);
                    Toggle("##npc","Show NPCs",  &g_espNpc);
                    ImGui::Unindent(8);
                    SectionLabel("RANGE"); ImGui::Indent(8);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##maxd",&g_espMaxDist,50.f,1500.f,"%.0f m");
                    ImGui::Unindent(8);
                }
                ImGui::EndTabItem();
            }

            //  Player Tab 
            if(ImGui::BeginTabItem(" Player ")){
                ImGui::Spacing();
                SectionLabel("COMBAT"); ImGui::Indent(8);
                Toggle("##god","God Mode",    &g_godMode);
                // Aimbot/Magic Bullet are in the Aimbot tab
                Toggle("##wnt","Never Wanted",&g_noWanted);
                ImGui::Unindent(8);
                SectionLabel("SURVIVAL"); ImGui::Indent(8);
                Toggle("##arm","Infinite Armor",&g_infArmor);
                Toggle("##inv","Invisible",     &g_invisible);
                ImGui::TextColored(Muted(),"  Client-side (alpha=0 each frame)");
                Toggle("##rdl","Force Ragdoll", &g_ragdoll);
                ImGui::TextColored(Muted(),"  Hold to ragdoll");
                ImGui::Unindent(8);
                SectionLabel("MOVEMENT"); ImGui::Indent(8);
                Toggle("##fc","FreeCam",&g_freecam);
                if(g_freecam){ ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##fcs",&g_freecamSpeed,5.f,200.f,"%.0f u/s"); ImGui::TextColored(Muted(),"  WASD + Q/E"); }
                ImGui::Unindent(8);
                SectionLabel("TELEPORT"); ImGui::Indent(8);
                if(ImGui::Button("Teleport to Waypoint",{-1,24})) g_tpWaypoint=true;
                ImGui::Unindent(8);
                ImGui::EndTabItem();
            }

            //  Vehicle Tab â”€
            if(ImGui::BeginTabItem(" Vehicle ")){
                ImGui::Spacing();
                SectionLabel("PROTECTION"); ImGui::Indent(8);
                Toggle("##vgod","Vehicle God Mode",&g_vehGod);
                if(ImGui::Button("Repair Vehicle",{-1,22})) g_vehRepair=true;
                ImGui::Unindent(8);
                SectionLabel("PERFORMANCE"); ImGui::Indent(8);
                Toggle("##vboost","Speed Boost",&g_vehBoost);
                if(g_vehBoost){ ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##vbm",&g_vehBoostMult,1.f,10.f,"%.1fx"); }
                Toggle("##vlock","Lock Doors",&g_vehLock);
                ImGui::Unindent(8);
                SectionLabel("SPAWN"); ImGui::Indent(8);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##model",g_spawnModel,sizeof(g_spawnModel));
                ImGui::TextColored(Muted(),"  Model name e.g. adder, t20, lazer");
                if(ImGui::Button("Spawn Vehicle",{-1,22})) g_spawnReq=true;
                ImGui::Unindent(8);
                ImGui::EndTabItem();
            }

            //  Players Tab â”€
            if(ImGui::BeginTabItem(" Players ")){
                uintptr_t localPed2 = GetLocalPed();
                Vec3 localPos2 = localPed2 ? GetPos(localPed2) : Vec3{};
                uintptr_t replay2   = (g_base&&g_current_offsets) ? RdPtr(g_base+(g_replayIfaceOverride?g_replayIfaceOverride:g_current_offsets->ReplayInterface)) : 0;
                uintptr_t pedIface2 = (AddrOk(replay2)&&g_pedIfaceKnown) ? RdPtr(replay2 + g_pedIfaceOff) : 0;
                uintptr_t pedArr2   = AddrOk(pedIface2) ? RdPtr(pedIface2 + g_pedArrayOff) : 0;
                uint16_t  cnt2      = AddrOk(pedIface2) ? Rd<uint16_t>(pedIface2 + g_pedCountOff) : 0;

                ImGui::Spacing();
                // Column headers
                ImGui::PushStyleColor(ImGuiCol_Text, Muted());
                ImGui::SetCursorPosX(12);
                ImGui::Text("%-3s  %-18s  %5s  %6s", "#", "Name", "HP", "Dist");
                ImGui::PopStyleColor();
                ImGui::Separator();

                //  Build sorted player list â”€
                struct PEntry { uintptr_t ped; float dist; float hp; Vec3 pos; int netId; };
                static std::vector<PEntry> s_plist;
                s_plist.clear();
                for(uint16_t i = 0; i < cnt2 && AddrOk(pedArr2); i++) {
                    uintptr_t p = RdPtr(pedArr2 + (uintptr_t)i * 0x10);
                    if(!AddrOk(p) || p == localPed2) continue;
                    uintptr_t pi = RdPtr(p + g_current_offsets->PlayerInfo);
                    int nid = AddrOk(pi) ? Rd<int>(pi + g_current_offsets->PlayerID) : -1;
                    if(nid < 0) continue;  // -1 = NPC/invalid; 0+ are valid player IDs
                    float hp = Rd<float>(p + g_current_offsets->Health);
                    if(hp <= 0.01f || hp > 100000.f) continue;
                    Vec3 pos = GetPos(p);
                    if(!ValidCoordinate(pos.x)) continue;
                    float d = sqrtf((pos.x-localPos2.x)*(pos.x-localPos2.x)+
                                    (pos.y-localPos2.y)*(pos.y-localPos2.y)+
                                    (pos.z-localPos2.z)*(pos.z-localPos2.z));
                    s_plist.push_back({p, d, hp, pos, nid});
                }
                std::sort(s_plist.begin(), s_plist.end(),
                    [](const PEntry& a, const PEntry& b){ return a.dist < b.dist; });

                float listH = ImGui::GetContentRegionAvail().y - 4.f;
                ImGui::BeginChild("##plist", {0, listH}, false);
                int row = 0;
                for(auto& e : s_plist) {
                    uintptr_t ped2 = e.ped;
                    int netId2 = e.netId;
                    float hp2 = e.hp, dist2 = e.dist;
                    Vec3 pos2 = e.pos;
                    {
                    std::string name2 = GetPlayerName(ped2);

                    float hpPct = hp2 / 200.f;
                    ImVec4 hpCol = {1.f - hpPct, hpPct, 0.f, 1.f};

                    ImGui::PushID((int)ped2);

                    // Row: clickable selectable with hover highlight
                    ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    char popupId[32]; snprintf(popupId, sizeof(popupId), "##ctx%d", (int)ped2);
                    bool rowClicked = ImGui::Selectable("##row", false,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                        {0, 20});
                    bool rowHovered = ImGui::IsItemHovered();
                    bool rowRClicked = ImGui::IsItemClicked(1);
                    bool rowLClicked = ImGui::IsItemClicked(0); // LEFT click also opens menu

                    if(rowHovered)
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            rowMin, {rowMin.x + ImGui::GetContentRegionAvail().x + 12, rowMin.y + 20},
                            IM_COL32(40,60,100,80));

                    // Open popup on left OR right click
                    if(rowRClicked || rowLClicked) ImGui::OpenPopup(popupId);

                    // Draw row content on top of the selectable
                    ImGui::SetCursorScreenPos(rowMin);
                    ImGui::SetCursorPosX(12);
                    ImGui::TextColored(Muted(), "%3d", row+1);  ImGui::SameLine(42);
                    ImGui::TextColored({.92f,.95f,1.f,1.f}, "%-18s", name2.c_str()); ImGui::SameLine(220);
                    ImGui::TextColored(hpCol, "%5.0f", hp2);    ImGui::SameLine(268);
                    ImGui::TextColored(Muted(), "%5.0fm", dist2);

                    // Context menu popup
                    if(ImGui::BeginPopup(popupId)) {
                        ImGui::TextColored(Accent(), "%s", name2.c_str());
                        ImGui::TextColored(Muted(), "ID %d  |  %.0f HP  |  %.0fm", netId2, hp2, dist2);
                        ImGui::Separator();
                        if(ImGui::MenuItem("  Teleport to")) {
                            if(localPed2) {
                                g_tpTarget = {pos2.x+1.f, pos2.y+1.f, pos2.z+0.5f};
                                g_tpFrames = 15;
                                SetPedPos(localPed2, g_tpTarget);
                            }
                        }
                        if(ImGui::MenuItem("  Spectate")) {
                            uintptr_t myPed = GetLocalPed();
                            g_frozenPedPos = myPed ? GetPos(myPed) : Vec3{};
                            g_spectateTarget = ped2; // track this ped every frame
                            g_fcPos   = {pos2.x, pos2.y, pos2.z + 2.5f};
                            g_fcInit  = true;
                            g_freecam = true;
                            if(g_current_offsets->FreecamByte)
                                Wr<uint8_t>(g_base + g_current_offsets->FreecamByte, 1);
                            Log("[Spectate] Following ped %llX at (%.1f, %.1f, %.1f)", (unsigned long long)ped2, pos2.x, pos2.y, pos2.z);
                        }
                        if(ImGui::MenuItem("  Ragdoll")) {
                            if(g_current_offsets->PedTaskOff)
                                Wr<uint8_t>(ped2 + g_current_offsets->PedTaskOff, 0);  // TASK_NONE
                            if(g_current_offsets->ConfigFlags) {
                                uint32_t cf = Rd<uint32_t>(ped2 + g_current_offsets->ConfigFlags);
                                cf &= ~(1u<<26); cf &= ~(1u<<19);
                                Wr<uint32_t>(ped2 + g_current_offsets->ConfigFlags, cf);
                            }
                            Log("[Ragdoll] Task cleared + ConfigFlags cleared");
                        }
                        if(ImGui::MenuItem("  Kill")) {
                            // Set health to 0 for instant death
                            Wr<float>(ped2 + g_current_offsets->Health, 0.f);
                            Log("[Kill] Health set to 0");
                        }
                        if(ImGui::MenuItem("  God Mode (toggle)")) {
                            float mhp = Rd<float>(ped2 + g_current_offsets->MaxHealth);
                            Wr<float>(ped2 + g_current_offsets->Health, mhp > 0.f ? mhp : 200.f);
                            if(g_current_offsets->GodMode) Wr<uint8_t>(ped2 + g_current_offsets->GodMode, 1);
                        }
                        if(ImGui::MenuItem("  Bucket on head (Lua)")) {
                            Log("\n==================== USER: BUCKET CLICKED ====================");
                            uintptr_t pi3 = RdPtr(ped2 + g_current_offsets->PlayerInfo);
                            int nid3 = AddrOk(pi3) ? Rd<int>(pi3 + g_current_offsets->PlayerID) : -1;
                            Log("[Bucket] Player ID: %d", nid3);
                            if(nid3 >= 0) {
                                char lua[768];
                                snprintf(lua, sizeof(lua),
                                    "local modelName='prop_bucket_01a'\n"
                                    "local h=joaat(modelName)\n"
                                    "if h==0 then h=GetHashKey(modelName) end\n"
                                    "RequestModel(h)\n"
                                    "local wait=0\n"
                                    "while not HasModelLoaded(h) and wait<100 do Wait(10) wait=wait+1 end\n"
                                    "local targetPed=GetPlayerPed(GetPlayerFromServerId(%d))\n"
                                    "if DoesEntityExist(targetPed) then\n"
                                    "  local px,py,pz=GetEntityCoords(targetPed)\n"
                                    "  local obj=CreateObject(h,px,py,pz+1.0,false,false,false)\n"
                                    "  if obj~=0 then\n"
                                    "    local boneIdx=GetPedBoneIndex(targetPed,0x796E)\n"
                                    "    AttachEntityToEntity(obj,targetPed,boneIdx,0,0.2,0.05,0,0,0,false,false,false,false,2,true)\n"
                                    "    TriggerEvent('chat:addMessage',{args={'QWAK','Bucket attached!'}})\n"
                                    "  end\n"
                                    "end\n"
                                    "SetModelAsNoLongerNeeded(h)\n", nid3);
                                Log("[Bucket] *** CALLING ExecLua() ***");
                                ExecLua(lua);
                                Log("[Bucket] ========== COMPLETE ==========\n");
                            } else {
                                Log("[Bucket] FAILED: No player ID");
                            }
                        }
                        ImGui::EndPopup();
                    }

                    // Double-click to teleport
                    if(rowHovered && ImGui::IsMouseDoubleClicked(0)) {
                        if(localPed2) {
                            g_tpTarget = {pos2.x+1.f, pos2.y+1.f, pos2.z+0.5f};
                            g_tpFrames = 15;
                            SetPedPos(localPed2, g_tpTarget);
                        }
                    }

                    ImGui::PopID();
                    row++;
                    } // inner block
                } // s_plist loop
                if(row == 0) {
                    ImGui::Spacing();
                    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 180) * 0.5f);
                    ImGui::TextColored(Muted(), g_pedIfaceKnown ? "No players detected" : "Scanning ped interface...");
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            //  Aimbot Tab 
            if(ImGui::BeginTabItem(" Aimbot ")){
                ImGui::Spacing();
                SectionLabel("TARGET LOCK"); ImGui::Indent(8);
                Toggle("##aim2","Enable Aimbot",&g_aimbot);
                if(g_aimbot) {
                    ImGui::TextColored(Muted(),"  Hold RMB to lock");
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##fov2",&g_aimbotFov,1.f,60.f,"FOV  %.0f%%");
                    Toggle("##fovcircle","Show FOV Circle",&g_aimbotFovCircle);
                    ImGui::Spacing();
                    // Bone target selection
                    const char* boneNames[] = {"Head (7)","Neck (6)","Chest (5)","Pelvis (1)","R_Hand (10)","L_Hand (13)"};
                    const int   boneIds[]   = {7, 6, 5, 1, 10, 13};
                    static int  s_boneIdx   = 0;
                    ImGui::TextColored(Muted(),"  Target bone:");
                    ImGui::SetNextItemWidth(-1);
                    if(ImGui::BeginCombo("##bone", boneNames[s_boneIdx])) {
                        for(int b = 0; b < 6; b++) {
                            if(ImGui::Selectable(boneNames[b], s_boneIdx==b))
                                { s_boneIdx = b; g_aimbotBone = boneIds[b]; }
                        }
                        ImGui::EndCombo();
                    }
                    // Smoothing — g_aimbotSmooth is a global used by TickAimbot
                    ImGui::TextColored(Muted(),"  Smooth (0=instant, 1=slow):");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##smooth",&g_aimbotSmooth,0.05f,1.f,"%.2f");
                    ImGui::Spacing();
                    // Target mode
                    const char* targetModes[] = {"Closest to Crosshair","Closest Distance"};
                    ImGui::TextColored(Muted(),"  Target mode:");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##tgtmode",&g_aimbotTargetMode,targetModes,2);
                    Toggle("##aliveonly","Alive Only",&g_aimbotAliveOnly);
                    Toggle("##playersonly","Players Only",&g_aimbotPlayersOnly);
                }
                ImGui::Unindent(8);

                SectionLabel("MAGIC BULLET"); ImGui::Indent(8);
                Toggle("##mb","Enable Magic Bullet",&g_magicBullet);
                ImGui::TextColored(Muted(),"  Hold RMB + fire to redirect bullets");
                ImGui::Unindent(8);

                SectionLabel("WEAPON"); ImGui::Indent(8);
                Toggle("##norecoil","No Recoil",    &g_wepNoRecoil);
                Toggle("##nospread","No Spread",    &g_wepNoSpread);
                Toggle("##infammo","Inf Ammo",      &g_wepInfAmmo);
                ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##dmg",&g_wepDmgMult,1.f,10.f,"Damage %.1fx");
                ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##rng",&g_wepRangeMult,1.f,5.f,"Range  %.1fx");
                ImGui::TextColored(Muted(),"  Applied to current held weapon");
                ImGui::Unindent(8);
                ImGui::EndTabItem();
            }

            if(ImGui::BeginTabItem(" Debug ")){
                //  live status â”€
                uintptr_t ped   = GetLocalPed();
                Vec3       pos  = ped ? GetPos(ped) : Vec3{};
                uintptr_t world = (g_base&&g_current_offsets) ? RdPtr(g_base+(g_worldOverride?g_worldOverride:g_current_offsets->World)) : 0;
                uintptr_t rep   = (g_base&&g_current_offsets) ? RdPtr(g_base+(g_replayIfaceOverride?g_replayIfaceOverride:g_current_offsets->ReplayInterface)) : 0;
                uintptr_t vpOff2 = g_viewportOverride ? g_viewportOverride : (g_current_offsets ? g_current_offsets->Viewport : 0);
                uintptr_t vport = (g_base&&vpOff2) ? RdPtr(g_base+vpOff2) : 0;

                auto StatusRow = [&](const char* label, bool ok, const char* fmt, ...) {
                    char buf[128];
                    va_list a; va_start(a,fmt); vsnprintf(buf,sizeof(buf),fmt,a); va_end(a);
                    ImVec4 dot = ok ? ImVec4(.2f,.9f,.5f,1.f) : ImVec4(1.f,.3f,.3f,1.f);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled({p.x+6,p.y+7},4.f,
                        IM_COL32((int)(dot.x*255),(int)(dot.y*255),(int)(dot.z*255),255));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+16);
                    ImGui::TextColored(Muted(),"%-12s",label);
                    ImGui::SameLine();
                    ImGui::TextColored(ok?Good():ImVec4(1,.35f,.35f,1),"%s",buf);
                };

                ImGui::Spacing();
                ImGui::BeginChild("##status",{0,128},true);
                StatusRow("Hook",    true,  "OK  |  %.0f fps", ImGui::GetIO().Framerate);
                StatusRow("Build",   g_current_offsets!=nullptr, "%s", g_current_offsets?g_current_offsets->name:"?");
                StatusRow("GameReady",g_gameReady, "%s", g_gameReady?"YES":"NO");
                StatusRow("World",   AddrOk(world), "0x%llX",(unsigned long long)world);
                StatusRow("LocalPed",ped!=0,        "0x%llX",(unsigned long long)ped);
                StatusRow("Pos",     ped!=0,        "%.1f  %.1f  %.1f", pos.x, pos.y, pos.z);
                StatusRow("Viewport",AddrOk(vport), "0x%llX",(unsigned long long)vport);
                StatusRow("Bones",   g_boneMode>=0, g_boneMode>=0?"mode=%d (0=inline,1=ptr)":"not detected", g_boneMode);
                ImGui::EndChild();

                //  log filter bar 
                ImGui::Spacing();
                static bool fESP=true, fBone=true, fNames=true, fW2S=true, fOther=true;
                static char fSearch[64]={};

                auto FilterBtn = [&](const char* lbl, bool* flag, ImVec4 col) {
                    ImVec4 bg = *flag ? ImVec4(col.x*.4f,col.y*.4f,col.z*.4f,.9f)
                                      : ImVec4(.08f,.09f,.14f,1.f);
                    ImGui::PushStyleColor(ImGuiCol_Button, bg);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x*.5f,col.y*.5f,col.z*.5f,1.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, *flag ? col : Muted());
                    if(ImGui::Button(lbl,{44,18})) *flag=!*flag;
                    ImGui::PopStyleColor(3);
                };

                FilterBtn("ESP",   &fESP,   {.2f,.6f,1.f,1.f});   ImGui::SameLine(0,4);
                FilterBtn("Bone",  &fBone,  {1.f,.8f,.2f,1.f});   ImGui::SameLine(0,4);
                FilterBtn("Names", &fNames, {.2f,.9f,.5f,1.f});   ImGui::SameLine(0,4);
                FilterBtn("W2S",   &fW2S,   {.4f,.8f,1.f,1.f});   ImGui::SameLine(0,4);
                FilterBtn("Other", &fOther, {.6f,.6f,.7f,1.f});   ImGui::SameLine(0,8);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##search", "filter...", fSearch, sizeof(fSearch));

                //  action buttons 
                ImGui::Spacing();
                if(ImGui::Button("Copy",{52,20})){
                    std::string full="--- DEBUG INFO ---\n";
                    for(const auto& l:g_logLines) full+=l+"\n";
                    CopyToClipboard(full);
                }
                ImGui::SameLine(0,4);
                if(ImGui::Button("Clear",{52,20})) g_logLines.clear();
                ImGui::SameLine(0,4);
                ImGui::Checkbox("Scroll",&g_logScroll);

                //  log window 
                ImGui::Spacing();
                float logH = ImGui::GetContentRegionAvail().y - 2.f;
                if(logH < 40.f) logH = 40.f;
                ImGui::BeginChild("##log",{0,logH},true,ImGuiWindowFlags_HorizontalScrollbar);

                for(const auto& line : g_logLines) {
                    const char* s = line.c_str();

                    // Category detect
                    bool isESP   = (strncmp(s,"ESP:",4)==0);
                    bool isBone  = (strncmp(s,"[Bone]",6)==0);
                    bool isNames = (strncmp(s,"[Names]",7)==0);
                    bool isW2S   = (strncmp(s,"[W2S]",5)==0);
                    bool isOther = !isESP && !isBone && !isNames && !isW2S;

                    // Filter pass
                    if(isESP   && !fESP)   continue;
                    if(isBone  && !fBone)  continue;
                    if(isNames && !fNames) continue;
                    if(isW2S   && !fW2S)   continue;
                    if(isOther && !fOther) continue;
                    if(fSearch[0] && !strstr(s, fSearch)) continue;

                    // Color by category
                    ImVec4 col;
                    if(isESP)        col = {.45f,.65f,1.f,1.f};
                    else if(isBone)  col = {1.f,.82f,.3f,1.f};
                    else if(isNames) col = {.3f,.95f,.55f,1.f};
                    else if(isW2S)   col = {.4f,.88f,1.f,1.f};
                    else             col = {.7f,.72f,.82f,1.f};

                    // Dim the category tag, bright the rest
                    const char* space = strchr(s, ' ');
                    if(space && (isESP||isBone||isNames||isW2S)) {
                        ImGui::TextColored({col.x*.6f,col.y*.6f,col.z*.6f,1.f},
                            "%.*s", (int)(space-s), s);
                        ImGui::SameLine(0,4);
                        ImGui::TextColored(col, "%s", space+1);
                    } else {
                        ImGui::TextColored(col, "%s", s);
                    }
                }

                if(g_logScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()-4.f)
                    ImGui::SetScrollHereY(1.f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar();
        float fh=ImGui::GetWindowHeight(); ImGui::SetCursorPosY(fh-22.f); HRule();
        ImGui::SetCursorPosX(8.f); ImGui::TextColored(Muted(),"HOME  toggle   |   qwak");
        ImGui::End();
    }

    ImGui::Render();
    if(g_rtv){
        ID3D11RenderTargetView* sr=nullptr; ID3D11DepthStencilView* sd=nullptr;
        g_ctx->OMGetRenderTargets(1,&sr,&sd);
        g_ctx->OMSetRenderTargets(1,&g_rtv,nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_ctx->OMSetRenderTargets(1,&sr,sd);
        if(sr)sr->Release(); if(sd)sd->Release();
    }
    return oSteamPresent(sc,sync,flags);
}

void HookVTable(void** vt,int idx,void* hook,void** orig){
    DWORD old; VirtualProtect(&vt[idx],8,PAGE_EXECUTE_READWRITE,&old);
    *orig=vt[idx]; vt[idx]=hook; VirtualProtect(&vt[idx],8,old,&old);
}
DWORD WINAPI InitThread(LPVOID){
    Sleep(4000);

    // Check if Steam overlay is loaded
    HMODULE hOverlay = GetModuleHandleA("GameOverlayRenderer64.dll");
    if(hOverlay){
        Log("Steam overlay detected (GameOverlayRenderer64.dll)");
    } else {
        Log("Steam overlay not found, using dummy window vtable hook");
    }

    // Create a dummy window + swapchain to grab the Present vtable
    WNDCLASSEXA wc{sizeof(wc),CS_CLASSDC,DefWindowProcA};
    wc.hInstance=GetModuleHandleA(nullptr); wc.lpszClassName="qwak";
    RegisterClassExA(&wc);
    HWND hw=CreateWindowExA(0,"qwak","",WS_POPUP|WS_DISABLED,-10,-10,1,1,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hw) return 1;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=1; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    IDXGISwapChain* sc=nullptr; ID3D11Device* dev=nullptr;
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
        nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,nullptr,nullptr))){
        DestroyWindow(hw); return 1;
    }
    void** vt=*(void***)sc;
    HookVTable(vt,8,(void*)hkSteamPresent,(void**)&oSteamPresent);
    sc->Release(); dev->Release();
    DestroyWindow(hw);
    return 0;
}
BOOL APIENTRY DllMain(HMODULE hMod,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        DisableThreadLibraryCalls(hMod);
        HANDLE h=CreateThread(nullptr,0,InitThread,nullptr,0,nullptr);
        if(h) CloseHandle(h);
    }
    return TRUE;
}
