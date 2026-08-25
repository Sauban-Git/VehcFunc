#include <mod/amlmod.h>
#include <mod/logger.h>
#include <fstream>
#include <string>
#include <set>
#include <algorithm>
#include <cstdlib>

MYMOD(net.retro.vehcfunc, VehcFunc, 1.0, Retro)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

// ==========================================
// RENDERWARE STRUCTURES
// ==========================================
struct RwV3d { float x, y, z; };

struct RwMatrix {
    RwV3d right; uint32_t flags;
    RwV3d up;    uint32_t pad1;
    RwV3d at;    uint32_t pad2;
    RwV3d pos;   uint32_t pad3;
};

struct RwObject {
    uint8_t type, subType, flags, privateFlags;
    void* parent;
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
    RwMatrix   modelling; // Local offset matrix
    RwMatrix   ltm;       // World position matrix
    RwLinkList objectList;
    RwFrame*   child;
    RwFrame*   next;
    RwFrame*   root;
};

struct RwRGBA { uint8_t r, g, b, a; };

struct RpMaterial {
    void*  texture;
    RwRGBA color;
    void*  pipeline;
};

struct RpMaterialList {
    RpMaterial** materials;
    int32_t      numMaterials;
    int32_t      space;
};

struct RpGeometry {
    uint8_t        pad[16];
    int16_t        numTriangles;
    int16_t        numVertices;
    int16_t        numMorphTargets;
    int16_t        numTexCoordSets;
    RpMaterialList matList;
};

struct RwAtomic {
    RwObject    object;
    RwLLLink    inFrameList;
    void*       info;
    RpGeometry* geometry;
};

// ==========================================
// GLOBALS, HELPERS & ENGINE FUNCTIONS
// ==========================================
typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);
GetFrameNodeName_t GetFrameNodeName = nullptr;

// Signature for CCoronas::RegisterCorona (Matches 0x005A39B0 overload)
typedef void (*RegisterCorona_t)(
    uint32_t id, void* attachTo, 
    uint8_t r, uint8_t g, uint8_t b, uint8_t a, 
    RwV3d* pos, float radius, float farClip, 
    uint8_t coronaType, uint8_t flareType, 
    bool enableReflection, bool checkLOS, 
    int unused, float angle, bool longDistance, 
    float nearClip, bool bFadeInOut
);
RegisterCorona_t RegisterCorona = nullptr;

static std::set<uintptr_t> g_loggedVehicles;
static std::set<RwFrame*>  g_litLoggedNodes;

inline bool IsValidPointer(uintptr_t ptr) {
    return (ptr >= 0x10000000 && ptr <= 0xF0000000 && (ptr % 4 == 0));
}

RwFrame* GetVehicleRootFrame(void* pVehicle) {
    if (!pVehicle || !IsValidPointer((uintptr_t)pVehicle)) return nullptr;
    static const size_t candidateOffsets[] = { 0x18, 0x14, 0x1C, 0x20 };
    for (size_t offset : candidateOffsets) {
        uintptr_t clumpAddr = *(uintptr_t*)((uintptr_t)pVehicle + offset);
        if (IsValidPointer(clumpAddr)) {
            uintptr_t frameAddr = *(uintptr_t*)(clumpAddr + 0x4);
            if (IsValidPointer(frameAddr)) return (RwFrame*)frameAddr;
        }
    }
    return nullptr;
}

// ==========================================
// FEATURE 1: TARGETED DETECTION LOGGER
// ==========================================
void DumpBrakeNodesOnly(RwFrame* frame, std::ofstream& logFile) {
    if (!frame || !IsValidPointer((uintptr_t)frame)) return;

    if (GetFrameNodeName) {
        const char* nodeName = GetFrameNodeName(frame);
        if (nodeName && IsValidPointer((uintptr_t)nodeName) && nodeName[0] != '\0') {
            std::string name(nodeName);
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

            if (nameLower.find("break") != std::string::npos || nameLower.find("brake") != std::string::npos) {
                logFile << "[Detected Brake Node] " << name << "\n";
            }
        }
    }

    if (frame->child) DumpBrakeNodesOnly(frame->child, logFile);
    if (frame->next)  DumpBrakeNodesOnly(frame->next, logFile);
}

void LogVehicle(void* pVehicle) {
    if (!pVehicle) return;
    uintptr_t vehAddr = (uintptr_t)pVehicle;
    if (g_loggedVehicles.count(vehAddr)) return; 

    RwFrame* rootFrame = GetVehicleRootFrame(pVehicle);
    if (!rootFrame) return;

    g_loggedVehicles.insert(vehAddr);

    std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
    std::ofstream logFile(logPath, std::ios::out | std::ios::app);
    if (logFile.is_open()) {
        logFile << "===========================================\n";
        logFile << "  Scanning Vehicle: 0x" << std::hex << vehAddr << "\n";
        logFile << "===========================================\n";
        DumpBrakeNodesOnly(rootFrame, logFile);
        logFile << "\n";
        logFile.close();
    }
}

// ==========================================
// FEATURE 2: DYNAMIC BRAKE LIGHTS & CORONAS
// ==========================================
bool ParsePrmColor(const std::string& name, RwRGBA& outColor) {
    size_t pos = name.find("prm");
    if (pos == std::string::npos || pos + 9 > name.length()) return false;

    std::string hexStr = name.substr(pos + 3, 6);
    uint32_t hexVal = std::strtoul(hexStr.c_str(), nullptr, 16);

    outColor.r = (hexVal >> 16) & 0xFF;
    outColor.g = (hexVal >> 8) & 0xFF;
    outColor.b = hexVal & 0xFF;
    outColor.a = 255;
    return true;
}

void ProcessFrameAtomics(RwFrame* frame, RwRGBA color) {
    RwLLLink* cur = frame->objectList.link.next;
    RwLLLink* end = &frame->objectList.link;

    while (cur && cur != end) {
        RwAtomic* atomic = (RwAtomic*)((uintptr_t)cur - offsetof(RwAtomic, inFrameList));
        if (atomic && atomic->object.type == 1 && atomic->geometry && atomic->geometry->matList.materials) {
            for (int i = 0; i < atomic->geometry->matList.numMaterials; ++i) {
                if (atomic->geometry->matList.materials[i]) {
                    atomic->geometry->matList.materials[i]->color = color;
                }
            }
        }
        cur = cur->next;
    }
}

void UpdateBrakeNodes(RwFrame* frame, void* pVehicle, bool isBraking, bool isLightsOn) {
    if (!frame || !IsValidPointer((uintptr_t)frame)) return;

    if (GetFrameNodeName) {
        const char* nodeName = GetFrameNodeName(frame);
        if (nodeName && IsValidPointer((uintptr_t)nodeName) && nodeName[0] != '\0') {
            std::string name(nodeName);
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

            if (nameLower.find("break") != std::string::npos || nameLower.find("brake") != std::string::npos) {
                RwRGBA targetColor;

                if (isBraking) {
                    if (!ParsePrmColor(nameLower, targetColor)) {
                        targetColor = { 255, 60, 60, 255 }; 
                    }

                    ProcessFrameAtomics(frame, targetColor);

                    if (RegisterCorona) {
                        uint32_t coronaID = (uint32_t)frame + 0x1000; 
                        RegisterCorona(
                            coronaID, pVehicle, 
                            targetColor.r, targetColor.g, targetColor.b, 255, 
                            &frame->ltm.pos, 
                            0.8f, 50.0f, 
                            1, 0, false, false, 0, 0.0f, false, 0.5f, false 
                        );
                    }

                    if (g_litLoggedNodes.find(frame) == g_litLoggedNodes.end()) {
                        g_litLoggedNodes.insert(frame);
                        std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
                        std::ofstream logFile(logPath, std::ios::out | std::ios::app);
                        if (logFile.is_open()) {
                            logFile << "[Event] Lit node: " << name 
                                    << " | RGBA: (" << (int)targetColor.r << "," 
                                    << (int)targetColor.g << "," << (int)targetColor.b << ")\n";
                            logFile.close();
                        }
                    }
                } else if (isLightsOn) {
                    targetColor = { 80, 0, 0, 100 };
                    ProcessFrameAtomics(frame, targetColor);
                    g_litLoggedNodes.erase(frame);
                } else {
                    targetColor = { 30, 30, 30, 0 };
                    ProcessFrameAtomics(frame, targetColor);
                    g_litLoggedNodes.erase(frame);
                }
            }
        }
    }

    if (frame->child) UpdateBrakeNodes(frame->child, pVehicle, isBraking, isLightsOn);
    if (frame->next)  UpdateBrakeNodes(frame->next, pVehicle, isBraking, isLightsOn);
}

void ProcessVehicleBrakeLights(void* pVehicle) {
    if (!pVehicle) return;

    float brakePedal = *(float*)((uintptr_t)pVehicle + 0x4A4);
    if (brakePedal <= 0.05f) brakePedal = *(float*)((uintptr_t)pVehicle + 0x49C);

    uint8_t lightMode = *(uint8_t*)((uintptr_t)pVehicle + 0x5A0);
    bool isBraking = (brakePedal > 0.05f);
    bool isLightsOn = (lightMode == 2);

    RwFrame* rootFrame = GetVehicleRootFrame(pVehicle);
    if (rootFrame) UpdateBrakeNodes(rootFrame, pVehicle, isBraking, isLightsOn);
}

// ==========================================
// PROCESS CONTROL HOOKS
// ==========================================
DECL_HOOKv(CAutomobile_ProcessControl, void* self) { 
    CAutomobile_ProcessControl(self); 
    LogVehicle(self); 
    ProcessVehicleBrakeLights(self); 
}
DECL_HOOKv(CBike_ProcessControl, void* self) { CBike_ProcessControl(self); LogVehicle(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CQuadBike_ProcessControl, void* self) { CQuadBike_ProcessControl(self); LogVehicle(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CMonsterTruck_ProcessControl, void* self) { CMonsterTruck_ProcessControl(self); LogVehicle(self); ProcessVehicleBrakeLights(self); }

ON_MOD_PRELOAD() {
    logger->SetTag("VehcFunc");
}

ON_MOD_LOAD() {
    uintptr_t pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) {
        logger->Error("Failed to locate libGTASA.so!");
        return;
    }

    std::string logPath = std::string(aml->GetAndroidDataPath()) + "/properties.log";
    std::ofstream clearLog(logPath, std::ios::out | std::ios::trunc);
    if (clearLog.is_open()) clearLog.close();

    SET_TO(GetFrameNodeName, pGTASA + BYBIT(0x0048241C + 1, 0x0));
    
    // Connected offset for RegisterCorona
    SET_TO(RegisterCorona, pGTASA + BYBIT(0x005A39B0 + 1, 0x0));

    HOOK(CAutomobile_ProcessControl,   pGTASA + BYBIT(0x00553DD4 + 1, 0x0));
    HOOK(CBike_ProcessControl,         pGTASA + BYBIT(0x00561A20 + 1, 0x0));
    HOOK(CQuadBike_ProcessControl,     pGTASA + BYBIT(0x0057A280 + 1, 0x0));
    HOOK(CMonsterTruck_ProcessControl, pGTASA + BYBIT(0x005747F4 + 1, 0x0));

    logger->Info("VehcFunc loaded with targeted brake log and coronas.");
}
