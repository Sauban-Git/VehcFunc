#include <mod/amlmod.h>
#include <mod/logger.h>
#include <fstream>
#include <string>

// Mod Metadata (32-bit Target)
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

struct RpClump {
    RwObject object;
};

// Function Pointers (Using your verified 32-bit offsets)
typedef void* (*FindPlayerVehicle_t)(int playerId, bool bIncludeBicycles);
typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);
typedef void (*PlayerPed_ProcessControl_t)(void* ped);

FindPlayerVehicle_t     FindPlayerVehicle = nullptr;
GetFrameNodeName_t      GetFrameNodeName  = nullptr;
PlayerPed_ProcessControl_t old_CPlayerPed_ProcessControl = nullptr;

// Tracking state to ensure vehicle is only logged once per entry
static void* g_pLastLoggedVehicle = nullptr;

// Recursively traverse vehicle RwFrame hierarchy and write nodes to properties.log
void DumpFrameHierarchy(RwFrame* frame, std::ofstream& logFile, int depth = 0) {
    if (!frame) return;

    if (GetFrameNodeName) {
        const char* nodeName = GetFrameNodeName(frame);
        if (nodeName && nodeName[0] != '\0') {
            std::string indent(depth * 2, ' ');
            logFile << indent << "[Node] " << nodeName << "\n";
        }
    }

    // Traverse child sub-frames (nested dummies/components)
    if (frame->child) {
        DumpFrameHierarchy(frame->child, logFile, depth + 1);
    }

    // Traverse sibling frames (parallel dummies at same depth)
    if (frame->next) {
        DumpFrameHierarchy(frame->next, logFile, depth);
    }
}

// Hooked CPlayerPed::ProcessControl (0x004C4778)
void hook_CPlayerPed_ProcessControl(void* ped) {
    if (old_CPlayerPed_ProcessControl) {
        old_CPlayerPed_ProcessControl(ped);
    }

    if (!FindPlayerVehicle) return;

    // Retrieve active vehicle driven by player
    void* pVehicle = FindPlayerVehicle(-1, false);

    // If driving a vehicle and it's a new entry, dump all dummy nodes
    if (pVehicle && pVehicle != g_pLastLoggedVehicle) {
        g_pLastLoggedVehicle = pVehicle;

        std::ofstream logFile("/sdcard/Android/data/com.rockstargames.gtasa/files/properties.log", std::ios::out | std::ios::trunc);
        if (logFile.is_open()) {
            logFile << "===========================================\n";
            logFile << "  DRIVEN VEHICLE DUMMY NODE PROPERTY DUMP  \n";
            logFile << "  Vehicle Object Pointer: 0x" << std::hex << (uintptr_t)pVehicle << "\n";
            logFile << "===========================================\n\n";

            // Offset 0x18 in CEntity points to m_pRwObject (RpClump*) in 32-bit GTASA
            RpClump* pClump = *(RpClump**)((uintptr_t)pVehicle + 0x18);

            if (pClump && pClump->object.parent) {
                RwFrame* rootFrame = (RwFrame*)pClump->object.parent;
                DumpFrameHierarchy(rootFrame, logFile, 0);
                logger->Info("Successfully dumped vehicle dummy nodes to properties.log!");
            } else {
                logFile << "Error: Root RwFrame for vehicle could not be accessed.\n";
            }

            logFile.close();
        } else {
            logger->Error("Failed to open properties.log for writing!");
        }
    } else if (!pVehicle) {
        // Reset tracking pointer when player steps out of vehicle
        g_pLastLoggedVehicle = nullptr;
    }
}

ON_MOD_PRELOAD() {
    logger->SetTag("VehicleDummyDumper");
}

ON_MOD_LOAD() {
    uintptr_t pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) {
        logger->Error("Failed to locate libGTASA.so!");
        return;
    }

    // Resolve functions using your verified 32-bit addresses (+1 for Thumb mode)
    GetFrameNodeName = (GetFrameNodeName_t)(pGTASA + 0x0048241C + 1);
    FindPlayerVehicle = (FindPlayerVehicle_t)(pGTASA + 0x0040B530 + 1);

    // Hook CPlayerPed::ProcessControl (0x004C4778)
    uintptr_t pProcessControl = pGTASA + 0x004C4778 + 1;
    aml->Redirect(pProcessControl, (uintptr_t)hook_CPlayerPed_ProcessControl, (uintptr_t*)&old_CPlayerPed_ProcessControl);

    logger->Info("VehicleDummyDumper 32-bit loaded. Ready to dump player vehicle nodes.");
}

