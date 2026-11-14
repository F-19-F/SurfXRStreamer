# Native SurfXR connection manager and embedded installation assets.
if(UNIX AND NOT APPLE AND PKG_CONFIG_FOUND)
  pkg_check_modules(LAUNCHER_UI QUIET IMPORTED_TARGET gtk+-3.0 json-c)
  pkg_check_modules(LAUNCHER_CRYPTO QUIET IMPORTED_TARGET libcrypto)
  if(TARGET PkgConfig::LAUNCHER_UI AND TARGET PkgConfig::LAUNCHER_CRYPTO)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame/launcher/surfxr-client.service" SURFXR_CLIENT_SERVICE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame/launcher/surfxr-dashboard.service" SURFXR_DASHBOARD_SERVICE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame/xr/shaders/image_enhancement.LICENSE" SURFXR_IMAGE_LICENSE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame/ui/fonts/NotoSans.LICENSE" SURFXR_FONT_LICENSE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame/input/driver_manifest.in" SURFXR_INPUT_DRIVER_MANIFEST)
    configure_file(frame/launcher/native/runtime_files.hpp.in launcher-generated/runtime_files.hpp @ONLY)
    add_library(surfxr-launcher-core
      frame/launcher/native/platform.cpp
      frame/launcher/native/connection.cpp
      frame/launcher/native/network.cpp
      frame/launcher/native/controller.cpp
      frame/launcher/native/lifecycle.cpp
      frame/launcher/native/session_failure.cpp
      frame/launcher/native/discovery.cpp
      frame/launcher/native/preferences.cpp
      frame/launcher/native/ipc.cpp
      frame/launcher/native/install.cpp
      frame/launcher/native/deploy.cpp
      frame/launcher/native/desktop.cpp)
    target_compile_features(surfxr-launcher-core PUBLIC cxx_std_17)
    target_include_directories(surfxr-launcher-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/launcher-generated")
    target_link_libraries(surfxr-launcher-core PUBLIC surfxr-preferences surfxr-localization PkgConfig::LAUNCHER_UI PkgConfig::LAUNCHER_CRYPTO Threads::Threads)
    target_compile_options(surfxr-launcher-core PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(surfxr-client frame/launcher/native/main.cpp)
    target_link_libraries(surfxr-client PRIVATE surfxr-launcher-core)
    target_compile_options(surfxr-client PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(surfxr-launcher-test tests/frame/launcher/native_test.cpp tests/frame/launcher/native_fixture.cpp
      tests/frame/launcher/native_deployment_fixture.cpp)
    add_dependencies(surfxr-launcher-test surfxr-client)
    target_compile_definitions(surfxr-launcher-test PRIVATE
      SURFXR_RESIDUAL_REMOVE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/frame/launcher/native/residual_remove.sh.in")
    target_include_directories(surfxr-launcher-test PRIVATE frame/launcher/native)
    target_link_libraries(surfxr-launcher-test PRIVATE surfxr-launcher-core)
    target_compile_options(surfxr-launcher-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME launcher-native COMMAND surfxr-launcher-test)
  endif()
endif()
