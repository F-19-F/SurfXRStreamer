cmake_minimum_required(VERSION 3.16)
project(SurfXR_OpenXR_Link LANGUAGES C CXX)

# A build/test-only reference for the public loader supplied by SteamVR on Frame.
# Configure the upstream target, without changing upstream source or ELF bytes.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_OpenGL ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
add_subdirectory("${SURFXR_OPENXR_SOURCE}" sdk)
set_property(TARGET openxr_loader PROPERTY VERSION)
set_property(TARGET openxr_loader PROPERTY SOVERSION)
set_target_properties(openxr_loader PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
