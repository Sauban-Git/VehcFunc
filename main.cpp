#include <mod/amlmod.h>
#include <mod/logger.h>
#include <fstream>
#include <string>

// Mod Metadata and Dependencies
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

// Function Pointers
typedef void* (*FindPlayerVehicle_t)(int playerId, bool bIncludeBicycles);
typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);

FindPlayerVehicle_t FindPlayerVehicle = nullptr;
GetFrameNodeName_t  GetFrameNodeName  = nullptr;

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

    if (frame->child) {
        DumpFrameHierarchy(frame->child, logFile, depth + 1);
    }

    if (frame->next) {
        DumpFrameHierarchy(frame->next, logFile, depth);
    }
}

// Hook CPlayerPed::ProcessControl
DECL_HOOKv(CPlayerPed_ProcessControl, void* self)
{
    CPlayerPed_ProcessControl(self);

    if (!FindPlayerVehicle) return;

    void* pVehicle = FindPlayerVehicle(-1, false);

    if (pVehicle && pVehicle != g_pLastLoggedVehicle) {
        g_pLastLoggedVehicle = pVehicle;

        std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
        std::ofstream logFile(logPath, std::ios::out | std::ios::trunc);

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
        g_pLastLoggedVehicle = nullptr;
    }
}

ON_MOD_PRELOAD()
{
    logger->SetTag("VehcFunc");
}

ON_MOD_LOAD()
{
    uintptr_t pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) {
        logger->Error("Failed to locate libGTASA.so!");
        return;
    }

    SET_TO(GetFrameNodeName, pGTASA + BYBIT(0x0048241C + 1, 0x0));
    SET_TO(FindPlayerVehicle, pGTASA + BYBIT(0x0040B530 + 1, 0x0));

    HOOK(CPlayerPed_ProcessControl, pGTASA + BYBIT(0x004C4778 + 1, 0x0));

    logger->Info("VehcFunc loaded successfully.");
}
