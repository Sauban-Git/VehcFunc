#include <mod/amlmod.h>
#include <mod/logger.h>
#include <string>
#include <algorithm>
#include <cstdlib>

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

struct RwObject {
    uint8_t type;
    uint8_t subType;
    uint8_t flags;
    uint8_t privateFlags;
    void*   parent;
};

struct RwAtomic {
    RwObject    object;
    void*       info;
    RpGeometry* geometry;
};

typedef const char* (*GetFrameNodeName_t)(RwFrame* frame);
extern GetFrameNodeName_t GetFrameNodeName;

// Parse "prmRRGGBBAAA" tag inside node names (e.g. breakl_lr_prmFF0000150)
bool ParsePrmColor(const std::string& name, RwRGBA& outColor) {
    size_t pos = name.find("prm");
    if (pos == std::string::npos || pos + 9 > name.length()) return false;

    std::string hexStr = name.substr(pos + 3, 6);
    uint32_t hexVal = std::strtoul(hexStr.c_str(), nullptr, 16);

    outColor.r = (hexVal >> 16) & 0xFF;
    outColor.g = (hexVal >> 8) & 0xFF;
    outColor.b = hexVal & 0xFF;
    outColor.a = 255;

    // Optional 3-digit alpha/brightness modifier at the end
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
        RwAtomic* atomic = (RwAtomic*)((uintptr_t)cur - offsetof(RwObject, parent));
        if (atomic && atomic->object.type == 1) { // rpATOMIC
            SetAtomicColor(atomic, color);
        }
        cur = cur->next;
    }
}

// Recursive node traversal for brake nodes
void UpdateBrakeNodes(RwFrame* frame, bool isBraking) {
    if (!frame) return;

    if (GetFrameNodeName) {
        const char* nodeName = GetFrameNodeName(frame);
        if (nodeName && nodeName[0] != '\0') {
            std::string name(nodeName);
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

            // Match break / brake dummies
            if (nameLower.find("break") != std::string::npos || nameLower.find("brake") != std::string::npos) {
                RwRGBA targetColor;

                if (isBraking) {
                    // Try to extract color from _prm tag, fallback to full red
                    if (!ParsePrmColor(nameLower, targetColor)) {
                        targetColor = { 255, 0, 0, 255 };
                    }
                } else {
                    // Off state: dimmed/darkened
                    targetColor = { 30, 0, 0, 0 };
                }

                ProcessFrameAtomics(frame, targetColor);
            }
        }
    }

    if (frame->child) UpdateBrakeNodes(frame->child, isBraking);
    if (frame->next)  UpdateBrakeNodes(frame->next, isBraking);
}

// Called inside ProcessControl hooks
void ProcessVehicleBrakeLights(void* pVehicle) {
    if (!pVehicle) return;

    // Read brake pedal float at offset 0x4A4
    float brakePedal = *(float*)((uintptr_t)pVehicle + 0x4A4);
    
    // Fallback check if primary offset returns 0 while handbraking/reversing
    if (brakePedal <= 0.05f) {
        brakePedal = *(float*)((uintptr_t)pVehicle + 0x49C);
    }

    bool isBraking = (brakePedal > 0.05f);

    RwFrame* rootFrame = GetVehicleRootFrame(pVehicle);
    if (rootFrame) {
        UpdateBrakeNodes(rootFrame, isBraking);
    }
}
