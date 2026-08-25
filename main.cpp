#include <mod/amlmod.h>
#include <mod/logger.h>
#include <string>
#include <algorithm>
#include <cstdlib>

MYMOD(net.retro.vehcfunc, VehcFunc, 1.0, Retro)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

// RenderWare 32-bit ARM Structures
struct RwV3d {
    float x, y, z;
};

struct RwMatrix {
    RwV3d    right;
    uint32_t flags;
    RwV3d    up;
    uint32_t pad1;
    RwV3d    at;
    uint32_t pad2;
    RwV3d    pos;
    uint32_t pad3;
}; // 64 bytes (0x40)

struct RwObject {
    uint8_t  type;
    uint8_t  subType;
    uint8_t  flags;
    uint8_t  privateFlags;
    void*    parent;
};

struct RwLLLink {
    RwLLLink* next;
    RwLLLink* prev;
};

struct RwLinkList {
    RwLLLink link;
};

struct RwFrame {
    RwObject   object;        // 0x00 - 0x07
    RwLLLink   inDirtyList;   // 0x08 - 0x0F
    RwMatrix   modelling;     // 0x10 - 0x4F
    RwMatrix   ltm;           // 0x50 - 0x8F
    RwLinkList objectList;    // 0x90 - 0x97
    RwFrame*   child;         // 0x98
    RwFrame*   next;          // 0x9C
    RwFrame*   root;          // 0xA0
};

struct RwRGBA {
    uint8_t r, g, b, a;
};

struct RpMaterial {
    void*   texture;
    RwRGBA  color;
    void*   pipeline;
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
    RwLLLink    inFrameList; // Offset 0x08 inside atomic
    void*       info;
    RpGeometry* geometry;
};

typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);
GetFrameNodeName_t GetFrameNodeName = nullptr;

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
            if (IsValidPointer(frameAddr)) {
                return (RwFrame*)frameAddr;
            }
        }
    }
    return nullptr;
}

// Parses node names like "breakl_lr_prmFF0000150" for color (FF0000) and alpha (150)
bool ParsePrmColor(const std::string& name, RwRGBA& outColor) {
    size_t pos = name.find("prm");
    if (pos == std::string::npos || pos + 9 > name.length()) return false;

    std::string hexStr = name.substr(pos + 3, 6);
    uint32_t hexVal = std::strtoul(hexStr.c_str(), nullptr, 16);

    outColor.r = (hexVal >> 16) & 0xFF;
    outColor.g = (hexVal >> 8) & 0xFF;
    outColor.b = hexVal & 0xFF;
    outColor.a = 255;

    if (pos + 12 <= name.length()) {
        std::string alphaStr = name.substr(pos + 9, 3);
        int alphaVal = std::atoi(alphaStr.c_str());
        if (alphaVal > 0) outColor.a = (uint8_t)std::min(alphaVal, 255);
    }
    return true;
}

void SetAtomicColor(RwAtomic* atomic, RwRGBA color) {
    if (!atomic || !atomic->geometry) return;
    RpMaterialList* matList = &atomic->geometry->matList;
    if (!matList || !matList->materials) return;

    for (int i = 0; i < matList->numMaterials; ++i) {
        if (matList->materials[i]) {
            matList->materials[i]->color = color;
        }
    }
}

void ProcessFrameAtomics(RwFrame* frame, RwRGBA color) {
    RwLLLink* cur = frame->objectList.link.next;
    RwLLLink* end = &frame->objectList.link;

    while (cur && cur != end) {
        RwAtomic* atomic = (RwAtomic*)((uintptr_t)cur - offsetof(RwAtomic, inFrameList));
        if (atomic && atomic->object.type == 1) { // 1 = rpATOMIC
            SetAtomicColor(atomic, color);
        }
        cur = cur->next;
    }
}

// Traverses hierarchy and modulates any frame with "break" or "brake" in its name
void UpdateBrakeNodes(RwFrame* frame, bool isBraking, bool isLightsOn) {
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
                    // State 1: Braking active -> read prm color tag or fallback to full red glow
                    if (!ParsePrmColor(nameLower, targetColor)) {
                        targetColor = { 255, 0, 0, 255 };
                    }
                } else if (isLightsOn) {
                    // State 2: Lights on, brake off -> dimmed light state
                    if (ParsePrmColor(nameLower, targetColor)) {
                        targetColor.r /= 2;
                        targetColor.g /= 2;
                        targetColor.b /= 2;
                        targetColor.a = 120;
                    } else {
                        targetColor = { 100, 0, 0, 120 };
                    }
                } else {
                    // State 3: Lights off, brake off -> unlit
                    targetColor = { 30, 30, 30, 0 };
                }

                ProcessFrameAtomics(frame, targetColor);
            }
        }
    }

    if (frame->child && IsValidPointer((uintptr_t)frame->child)) {
        UpdateBrakeNodes(frame->child, isBraking, isLightsOn);
    }
    if (frame->next && IsValidPointer((uintptr_t)frame->next)) {
        UpdateBrakeNodes(frame->next, isBraking, isLightsOn);
    }
}

void ProcessVehicleBrakeLights(void* pVehicle) {
    if (!pVehicle || !IsValidPointer((uintptr_t)pVehicle)) return;

    // Read brake pedal state (float at 0x4A4 or fallback 0x49C)
    float brakePedal = *(float*)((uintptr_t)pVehicle + 0x4A4);
    if (brakePedal <= 0.05f) {
        brakePedal = *(float*)((uintptr_t)pVehicle + 0x49C);
    }

    // Read vehicle light override status (0x5A0)
    uint8_t lightMode = *(uint8_t*)((uintptr_t)pVehicle + 0x5A0);
    bool isBraking = (brakePedal > 0.05f);
    bool isLightsOn = (lightMode == 2);

    RwFrame* rootFrame = GetVehicleRootFrame(pVehicle);
    if (rootFrame) {
        UpdateBrakeNodes(rootFrame, isBraking, isLightsOn);
    }
}

DECL_HOOKv(CAutomobile_ProcessControl, void* self)   { CAutomobile_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CBike_ProcessControl, void* self)         { CBike_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CBoat_ProcessControl, void* self)         { CBoat_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CHeli_ProcessControl, void* self)         { CHeli_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CPlane_ProcessControl, void* self)        { CPlane_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CMonsterTruck_ProcessControl, void* self) { CMonsterTruck_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CQuadBike_ProcessControl, void* self)     { CQuadBike_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CTrailer_ProcessControl, void* self)      { CTrailer_ProcessControl(self); ProcessVehicleBrakeLights(self); }
DECL_HOOKv(CBmx_ProcessControl, void* self)          { CBmx_ProcessControl(self); ProcessVehicleBrakeLights(self); }

ON_MOD_PRELOAD() {
    logger->SetTag("VehcFunc");
}

ON_MOD_LOAD() {
    uintptr_t pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) {
        logger->Error("Failed to locate libGTASA.so!");
        return;
    }

    SET_TO(GetFrameNodeName, pGTASA + BYBIT(0x0048241C + 1, 0x0));

    HOOK(CAutomobile_ProcessControl,   pGTASA + BYBIT(0x00553DD4 + 1, 0x0));
    HOOK(CBike_ProcessControl,         pGTASA + BYBIT(0x00561A20 + 1, 0x0));
    HOOK(CBoat_ProcessControl,         pGTASA + BYBIT(0x0056BE50 + 1, 0x0));
    HOOK(CHeli_ProcessControl,         pGTASA + BYBIT(0x00571238 + 1, 0x0));
    HOOK(CPlane_ProcessControl,        pGTASA + BYBIT(0x00575C88 + 1, 0x0));
    HOOK(CMonsterTruck_ProcessControl, pGTASA + BYBIT(0x005747F4 + 1, 0x0));
    HOOK(CQuadBike_ProcessControl,     pGTASA + BYBIT(0x0057A280 + 1, 0x0));
    HOOK(CTrailer_ProcessControl,      pGTASA + BYBIT(0x0057B304 + 1, 0x0));
    HOOK(CBmx_ProcessControl,          pGTASA + BYBIT(0x00568B14 + 1, 0x0));

    logger->Info("VehcFunc loaded with brake light control.");
}
