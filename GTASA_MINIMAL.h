#pragma once
#include <stdint.h>

// Basic 3D Vector used by GTA:SA
struct CVector {
  float x, y, z;

  CVector() : x(0.0f), y(0.0f), z(0.0f) {}
  CVector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

// Forward declarations (opaque pointers) for engine types
class CEntity;
struct RwTexture;
