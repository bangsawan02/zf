LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

XDL_FILES := $(wildcard $(LOCAL_PATH)/xdl/*.c)

APP_STL=none
LOCAL_MODULE := zygiskfrida
LOCAL_C_INCLUDES := $(LOCAL_PATH)/xdl/include $(LOCAL_PATH)/include
LOCAL_SRC_FILES := inject.cpp config.cpp child_gating.cpp main_zygisk.cpp $(XDL_FILES:$(LOCAL_PATH)/%=%)
LOCAL_LDLIBS := -llog -ldl

include $(BUILD_SHARED_LIBRARY)
