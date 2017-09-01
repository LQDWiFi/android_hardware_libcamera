ifeq ($(USE_CAMERA_STUB),false)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CFLAGS := -fno-short-enums -DHAVE_CONFIG_H

LOCAL_C_INCLUDES := \
	system/media/camera/include \
	external/jpeg

LOCAL_SRC_FILES := \
	CameraFactory.cpp \
	CameraHal.cpp \
	CameraHardware.cpp \
	CameraSpec.cpp \
	Converter.cpp \
	Metadata.cpp \
	SurfaceDesc.cpp \
	SurfaceSize.cpp \
	Utils.cpp \
	V4L2Camera.cpp \

LOCAL_SHARED_LIBRARIES := \
	libcamera_client \
	libcamera_metadata \
	libcutils \
	libjpeg \
	liblog \
	libui \
	libutils \

# This is not a qualcomm module!
# It's named "camera.qcom" because of how Android loads the HAL modules
# Android will load the first module it finds in the following order:
# /system/lib{,64}/camera.${ro.hardware}.so       = camera.qcom
# /system/lib{,64}/camera.${ro.product.board}.so  = camera.msm8996
# /system/lib{,64}/camera.${ro.board.platform}.so = camera.msm8996
# /system/lib{,64}/camera.${ro.arch}.so           = <none>
# So because there's already a module called camera.msm8996 which is the real Qualcomm camera module
# The only way we can be considered before that module is to name ourselves "camera.qcom"
LOCAL_MODULE := camera.qcom
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw

include $(BUILD_SHARED_LIBRARY)

endif
