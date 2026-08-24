LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_CPP_EXTENSION := .cpp .cc

ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
    LOCAL_MODULE := NPCBikeHLFix
else
    LOCAL_MODULE := NPCBikeHLFix64
endif

LOCAL_SRC_FILES := main.cpp mod/logger.cpp mod/config.cpp

# -mfloat-abi=softfp removed to allow clean 64-bit (arm64-v8a) compilation
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH) $(LOCAL_PATH)/mod
LOCAL_LDLIBS += -llog

include $(BUILD_SHARED_LIBRARY)
