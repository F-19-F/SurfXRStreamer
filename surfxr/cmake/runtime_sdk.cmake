# Prefer the installed runtime SDK; a standalone build uses the bundled OpenVR SDK.
set(SURFXR_EXO_OPENVR_SDK "${PROJECT_SOURCE_DIR}/pc/surfxr-server/openvr")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(SURFXR_EXO_OPENVR_ARCH linuxarm64)
else()
  set(SURFXR_EXO_OPENVR_ARCH linux64)
endif()
find_path(FRAME_OPENVR_INCLUDE_DIR openvr.h
  PATHS /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
        "${SURFXR_EXO_OPENVR_SDK}/headers")
find_path(FRAME_OPENVR_DRIVER_INCLUDE_DIR openvr_driver.h
  PATHS /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
        "${SURFXR_EXO_OPENVR_SDK}/headers")
find_file(FRAME_OPENVR_LIBRARY libopenvr_api.so
  PATHS "/opt/steamvr/bin/${SURFXR_EXO_OPENVR_ARCH}"
        "${SURFXR_EXO_OPENVR_SDK}/lib/${SURFXR_EXO_OPENVR_ARCH}" NO_DEFAULT_PATH)
# Use a shared loader on Frame and on Linux build hosts.
find_library(FRAME_OPENXR_LOADER NAMES openxr_loader
  PATHS "/opt/steamvr/bin/${SURFXR_EXO_OPENVR_ARCH}")
