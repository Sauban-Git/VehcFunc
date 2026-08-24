#include <mod/amlmod.h>
#include <mod/logger.h>
#include <fstream>
#include <string>
#include <set>

// Mod Metadata & Dependencies
MYMOD(net.retro.vehcfunc, VehcFunc, 1.0, Retro)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

// RenderWare 32-bit Data Structures
struct RwObject {
    uint8_t type;
    uint8_t subType;
    uint8_t flags;
    uint8_t privateFlags;
    void*   parent; // Points to parent RwFrame
};

struct RwLLLink {
    RwLLLink* next;
    RwLLLink* prev;
};

struct RwLinkList {
    RwLLLink link;
};

struct RwFrame {
    RwObject   object;
    RwLLLink   inDirtyList;
    void*      modelling;
    void*      ltm;
    RwLinkList objectList;
    RwFrame*   child;
    RwFrame*   next;
    RwFrame*   root;
};

typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);
GetFrameNodeName_t GetFrameNodeName = nullptr;

// Track logged vehicle memory pointers so each entity is only logged once
static std::set<uintptr_t> g_loggedVehicles;

// Validate pointers to prevent SIGSEGV crashes on corrupted memory
inline bool IsValidPointer(uintptr_t ptr) {
    return (ptr >= 0x10000000 && ptr <= 0xF0000000 && (ptr % 4 == 0));
}

// Dynamically locate root RwFrame across ARM Android CEntity offset variations
RwFrame* GetVehicleRootFrame(void* pVehicle) {
    if (!pVehicle || !IsValidPointer((uintptr_t)pVehicle)) return nullptr;

    // Offsets for m_pRwObject (RpClump*) in Android GTASA 32-bit
    static const size_t candidateOffsets[] = { 0x14, 0x18, 0x1C, 0x20, 0x04 };

    for (size_t offset : candidateOffsets) {
        uintptr_t clumpAddr = *(uintptr_t*)((uintptr_t)pVehicle + offset);
        if (IsValidPointer(clumpAddr)) {
            // RpClump.object.parent is at offset 0x04
            uintptr_t frameAddr = *(uintptr_t*)(clumpAddr + 0x4);
            if (IsValidPointer(frameAddr)) {
                return (RwFrame*)frameAddr;
            }
        }
    }
    return nullptr;
}

// Recursively traverse vehicle RwFrame hierarchy safely
void DumpFrameHierarchy(RwFrame* frame, std::ofstream& logFile, int depth = 0) {
    if (!frame || !IsValidPointer((uintptr_t)frame) || depth > 50) return;

    if (GetFrameNodeName) {
        const char* nodeName = GetFrameNodeName(frame);
        if (nodeName && IsValidPointer((uintptr_t)nodeName) && nodeName[0] != '\0') {
            std::string indent(depth * 2, ' ');
            logFile << indent << "[Node] " << nodeName << "\n";
        }
    }

    if (frame->child && IsValidPointer((uintptr_t)frame->child)) {
        DumpFrameHierarchy(frame->child, logFile, depth + 1);
    }

    if (frame->next && IsValidPointer((uintptr_t)frame->next)) {
        DumpFrameHierarchy(frame->next, logFile, depth);
    }
}

// Log any newly spawned vehicle
void LogVehicle(void* pVehicle) {
    if (!pVehicle || !IsValidPointer((uintptr_t)pVehicle)) return;

    uintptr_t vehAddr = (uintptr_t)pVehicle;
    if (g_loggedVehicles.count(vehAddr)) return; // Already logged

    RwFrame* rootFrame = GetVehicleRootFrame(pVehicle);
    if (!rootFrame) return;

    g_loggedVehicles.insert(vehAddr);

    std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
    std::ofstream logFile(logPath, std::ios::out | std::ios::app);

    if (logFile.is_open()) {
        logFile << "===========================================\n";
        logFile << "  SPAWNED VEHICLE DUMMY NODE DUMP\n";
        logFile << "  Vehicle Object Pointer: 0x" << std::hex << vehAddr << "\n";
        logFile << "===========================================\n";

        DumpFrameHierarchy(rootFrame, logFile, 0);
        logFile << "\n";
        logFile.close();

        logger->Info("Logged vehicle node hierarchy: 0x%X", vehAddr);
    }
}

// Hooks for all vehicle subclass ProcessControl methods (spawns/traffic/player)
DECL_HOOKv(CAutomobile_ProcessControl, void* self) {
    CAutomobile_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CBike_ProcessControl, void* self) {
    CBike_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CBoat_ProcessControl, void* self) {
    CBoat_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CHeli_ProcessControl, void* self) {
    CHeli_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CPlane_ProcessControl, void* self) {
    CPlane_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CMonsterTruck_ProcessControl, void* self) {
    CMonsterTruck_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CQuadBike_ProcessControl, void* self) {
    CQuadBike_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CTrailer_ProcessControl, void* self) {
    CTrailer_ProcessControl(self);
    LogVehicle(self);
}
DECL_HOOKv(CBmx_ProcessControl, void* self) {
    CBmx_ProcessControl(self);
    LogVehicle(self);
}

ON_MOD_PRELOAD() {
    logger->SetTag("VehcFunc");
}

ON_MOD_LOAD() {
    uintptr_t pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) {
        logger->Error("Failed to locate libGTASA.so!");
        return;
    }

    // Reset properties.log on startup
    std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
    std::ofstream clearLog(logPath, std::ios::out | std::ios::trunc);
    if (clearLog.is_open()) clearLog.close();

    SET_TO(GetFrameNodeName, pGTASA + BYBIT(0x0048241C + 1, 0x0));

    // Hook ProcessControl across all vehicle types to catch every spawned entity
    HOOK(CAutomobile_ProcessControl,   pGTASA + BYBIT(0x00553DD4 + 1, 0x0));
    HOOK(CBike_ProcessControl,         pGTASA + BYBIT(0x00561A20 + 1, 0x0));
    HOOK(CBoat_ProcessControl,         pGTASA + BYBIT(0x0056BE50 + 1, 0x0));
    HOOK(CHeli_ProcessControl,         pGTASA + BYBIT(0x00571238 + 1, 0x0));
    HOOK(CPlane_ProcessControl,        pGTASA + BYBIT(0x00575C88 + 1, 0x0));
    HOOK(CMonsterTruck_ProcessControl, pGTASA + BYBIT(0x005747F4 + 1, 0x0));
    HOOK(CQuadBike_ProcessControl,     pGTASA + BYBIT(0x0057A280 + 1, 0x0));
    HOOK(CTrailer_ProcessControl,      pGTASA + BYBIT(0x0057B304 + 1, 0x0));
    HOOK(CBmx_ProcessControl,          pGTASA + BYBIT(0x00568B14 + 1, 0x0));

    logger->Info("VehcFunc loaded. Auto-logging all spawned vehicle nodes safely.");
}
