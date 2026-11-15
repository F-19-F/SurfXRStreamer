# Optional native diagnostic tools; protocol parser stays dependency-free.
if(UNIX AND NOT APPLE)
  add_executable(surfxr-viewer-status-test tests/frame/app/viewer_status_test.cpp)
  target_include_directories(surfxr-viewer-status-test PRIVATE ${SURFXR_PUBLIC_INCLUDE_DIRS})
  target_compile_features(surfxr-viewer-status-test PRIVATE cxx_std_17)
  target_link_libraries(surfxr-viewer-status-test PRIVATE Threads::Threads)
  target_compile_options(surfxr-viewer-status-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME viewer-status COMMAND surfxr-viewer-status-test --self-test)
  find_package(Vulkan QUIET)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(DASHBOARD_UI QUIET IMPORTED_TARGET pangocairo fontconfig json-c)
    find_path(FRAME_OPENVR_INCLUDE_DIR openvr.h PATHS /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers)
    find_file(FRAME_OPENVR_LIBRARY libopenvr_api.so PATHS /opt/steamvr/bin/linuxarm64 NO_DEFAULT_PATH)
    if(TARGET PkgConfig::DASHBOARD_UI AND FRAME_OPENVR_INCLUDE_DIR AND FRAME_OPENVR_LIBRARY AND Vulkan_FOUND)
      add_executable(
  surfxr-dashboard
  frame/dashboard/dashboard_checks.cpp
  frame/dashboard/dashboard_main.cpp
  frame/dashboard/dashboard_overlay.cpp
  frame/dashboard/dashboard_render.cpp
  frame/dashboard/launcher_ipc.cpp
  frame/dashboard/status_parser.cpp
)
      target_compile_features(surfxr-dashboard PRIVATE cxx_std_17)
      target_include_directories(surfxr-dashboard PRIVATE ${FRAME_OPENVR_INCLUDE_DIR})
      target_link_libraries(surfxr-dashboard PRIVATE surfxr-battery surfxr-preferences surfxr-localization PkgConfig::DASHBOARD_UI ${FRAME_OPENVR_LIBRARY} Threads::Threads Vulkan::Vulkan)
      target_compile_options(surfxr-dashboard PRIVATE -Wall -Wextra -Wpedantic)
      set_target_properties(surfxr-dashboard PROPERTIES BUILD_RPATH /opt/steamvr/bin/linuxarm64 INSTALL_RPATH /opt/steamvr/bin/linuxarm64)
      add_test(NAME dashboard-offline COMMAND surfxr-dashboard --self-test)
    endif()
    pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
    if(TARGET PkgConfig::PULSE)
      find_package(Threads REQUIRED)
      add_library(surfxr-audio-output frame/media/audio_output.cpp frame/media/audio_buffered.cpp)
      target_include_directories(surfxr-audio-output PUBLIC ${SURFXR_PUBLIC_INCLUDE_DIRS})
      target_compile_features(surfxr-audio-output PUBLIC cxx_std_17)
      target_link_libraries(surfxr-audio-output PUBLIC PkgConfig::PULSE)
      target_compile_options(surfxr-audio-output PRIVATE -Wall -Wextra -Wpedantic)
      add_executable(surfxr-audio-test tests/frame/media/audio_output_test.cpp)
      target_link_libraries(surfxr-audio-test PRIVATE surfxr-audio-output)
      target_compile_options(surfxr-audio-test PRIVATE -Wall -Wextra -Wpedantic)
      add_test(NAME audio-ring COMMAND surfxr-audio-test --self-test)
      add_library(surfxr-microphone frame/media/microphone_capture.cpp)
      target_include_directories(surfxr-microphone PUBLIC ${SURFXR_PUBLIC_INCLUDE_DIRS})
      target_compile_features(surfxr-microphone PUBLIC cxx_std_17)
      target_link_libraries(surfxr-microphone PUBLIC PkgConfig::PULSE Threads::Threads)
      target_compile_options(surfxr-microphone PRIVATE -Wall -Wextra -Wpedantic)
      add_executable(surfxr-microphone-test tests/frame/media/microphone_capture_test.cpp)
      target_link_libraries(surfxr-microphone-test PRIVATE surfxr-microphone)
      target_compile_options(surfxr-microphone-test PRIVATE -Wall -Wextra -Wpedantic)
      add_test(NAME microphone-ring COMMAND surfxr-microphone-test --self-test)
    endif()
    if(SURFXR_FFMPEG_ROOT)
      add_library(PkgConfig::SURFXR_DECODE INTERFACE IMPORTED)
      set(_media_libraries)
      foreach(_media avcodec avutil avformat swscale)
        find_library(_media_${_media} NAMES ${_media} PATHS "${SURFXR_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        list(APPEND _media_libraries "${_media_${_media}}")
      endforeach()
      set_target_properties(PkgConfig::SURFXR_DECODE PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SURFXR_FFMPEG_ROOT}/include"
        INTERFACE_LINK_LIBRARIES "${_media_libraries}")
      # DT_RPATH covers transitive media dependencies in build and installation.
      add_link_options("LINKER:--disable-new-dtags")
      set(CMAKE_BUILD_RPATH "$ORIGIN/runtime/lib;$ORIGIN/../lib;${SURFXR_FFMPEG_ROOT}/lib")
    else()
      pkg_check_modules(SURFXR_DECODE QUIET IMPORTED_TARGET libavcodec libavutil libavformat libswscale)
    endif()
    if(TARGET PkgConfig::SURFXR_DECODE)
      add_executable(surfxr-decode-probe frame/diagnostics/decode_probe.cpp)
      target_compile_features(surfxr-decode-probe PRIVATE cxx_std_17)
      target_link_libraries(surfxr-decode-probe PRIVATE PkgConfig::SURFXR_DECODE)
      target_compile_options(surfxr-decode-probe PRIVATE -Wall -Wextra -Wpedantic)

      add_library(surfxr-video-decoder frame/media/video_decoder.cpp frame/media/decoder_checks.cpp frame/media/nv12_output_checks.cpp frame/media/async_video_decoder.cpp frame/media/async_decoder_checks.cpp frame/media/v4l2_hevc.cpp)
      target_include_directories(surfxr-video-decoder PUBLIC ${SURFXR_PUBLIC_INCLUDE_DIRS})
      target_compile_features(surfxr-video-decoder PUBLIC cxx_std_17)
      target_link_libraries(surfxr-video-decoder PUBLIC PkgConfig::SURFXR_DECODE Threads::Threads)
      target_compile_options(surfxr-video-decoder PRIVATE -Wall -Wextra -Wpedantic)
      add_executable(surfxr-video-test tests/frame/media/video_decoder_test.cpp)
      target_link_libraries(surfxr-video-test PRIVATE surfxr-video-decoder)
      target_compile_options(surfxr-video-test PRIVATE -Wall -Wextra -Wpedantic)
      add_test(NAME video-decoder COMMAND surfxr-video-test --self-test)
      add_executable(surfxr-async-video-test tests/frame/media/async_video_decoder_test.cpp)
      target_link_libraries(surfxr-async-video-test PRIVATE surfxr-video-decoder)
      target_compile_options(surfxr-async-video-test PRIVATE -Wall -Wextra -Wpedantic)
      add_test(NAME async-video-decoder COMMAND surfxr-async-video-test --self-test)
    endif()
  endif()
  if(Vulkan_FOUND)
    if(TARGET surfxr-video-test)
      target_sources(surfxr-video-test PRIVATE tests/frame/media/nv12_copy_benchmark.cpp frame/xr/foveation_renderer.cpp)
      target_compile_definitions(surfxr-video-test PRIVATE SURFXR_NV12_COPY_BENCHMARK=1)
      target_link_libraries(surfxr-video-test PRIVATE Vulkan::Vulkan)
    endif()
    add_executable(surfxr-image-enhancement-gpu-test tests/frame/xr/image_enhancement_gpu_test.cpp frame/xr/image_enhancement_renderer.cpp)
    target_include_directories(surfxr-image-enhancement-gpu-test PRIVATE ${SURFXR_PUBLIC_INCLUDE_DIRS})
    target_compile_features(surfxr-image-enhancement-gpu-test PRIVATE cxx_std_17)
    target_link_libraries(surfxr-image-enhancement-gpu-test PRIVATE Vulkan::Vulkan)
    target_compile_options(surfxr-image-enhancement-gpu-test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(surfxr-rgba-upload-test PRIVATE SURFXR_UPLOAD_VULKAN_TEST=1)
    target_link_libraries(surfxr-rgba-upload-test PRIVATE Vulkan::Vulkan)
    target_compile_options(surfxr-rgba-upload-test PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(
  surfxr-foveation-gpu-test
  tests/frame/xr/foveation_gpu_test.cpp
  tests/frame/xr/foveation_reconstruction_checks.cpp
  tests/frame/xr/foveation_timewarp_checks.cpp
  tests/frame/xr/foveation_nv12_checks.cpp
  tests/frame/xr/foveation_atlas_checks.cpp
  tests/frame/xr/foveation_focus_checks.cpp
  tests/frame/xr/foveation_sharpen_checks.cpp
  tests/frame/xr/foveation_gamma2_checks.cpp
  frame/xr/foveation_renderer.cpp
  frame/xr/image_enhancement_renderer.cpp
)
    target_include_directories(surfxr-foveation-gpu-test PRIVATE ${SURFXR_PUBLIC_INCLUDE_DIRS})
    target_compile_features(surfxr-foveation-gpu-test PRIVATE cxx_std_17)
    target_link_libraries(surfxr-foveation-gpu-test PRIVATE Vulkan::Vulkan)
    target_compile_options(surfxr-foveation-gpu-test PRIVATE -Wall -Wextra -Wpedantic)
    # Explicit invocation only: the normal offline suite never opens the GPU.
  endif()
  find_path(OPENXR_INCLUDE_DIR openxr/openxr.h)
  if(Vulkan_FOUND AND OPENXR_INCLUDE_DIR)
    add_executable(surfxr-current-tracking-test tests/frame/xr/current_tracking_test.cpp)
    target_include_directories(surfxr-current-tracking-test PRIVATE frame/xr ${OPENXR_INCLUDE_DIR})
    target_link_libraries(surfxr-current-tracking-test PRIVATE surfxr-tracking)
    add_test(NAME current-tracking COMMAND surfxr-current-tracking-test)
    add_executable(surfxr-late-timewarp-test tests/frame/xr/late_timewarp_test.cpp)
    target_include_directories(surfxr-late-timewarp-test PRIVATE frame/xr ${OPENXR_INCLUDE_DIR})
    target_compile_features(surfxr-late-timewarp-test PRIVATE cxx_std_17)
    target_compile_options(surfxr-late-timewarp-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME late-timewarp COMMAND surfxr-late-timewarp-test)
    add_executable(surfxr-platform-probe frame/diagnostics/platform_probe.cpp)
    target_compile_features(surfxr-platform-probe PRIVATE cxx_std_17)
    target_include_directories(surfxr-platform-probe PRIVATE ${OPENXR_INCLUDE_DIR})
    target_link_libraries(surfxr-platform-probe PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})
    target_compile_options(surfxr-platform-probe PRIVATE -Wall -Wextra -Wpedantic)

    # The Frame runtime ships the shared loader here; avoid the system static .a.
    find_file(FRAME_OPENXR_LOADER NAMES libopenxr_loader.so
              PATHS /opt/steamvr/bin/linuxarm64 NO_DEFAULT_PATH)
    if(FRAME_OPENXR_LOADER)
      add_executable(frame_xr_presenter frame/diagnostics/xr_presenter.cpp frame/diagnostics/presenter_setup.cpp frame/diagnostics/presenter_render.cpp)
      target_compile_features(frame_xr_presenter PRIVATE cxx_std_17)
      target_include_directories(frame_xr_presenter PRIVATE ${OPENXR_INCLUDE_DIR})
      target_link_libraries(frame_xr_presenter PRIVATE Vulkan::Vulkan ${FRAME_OPENXR_LOADER})
      target_compile_options(frame_xr_presenter PRIVATE -Wall -Wextra -Wpedantic)
      set_target_properties(frame_xr_presenter PROPERTIES BUILD_RPATH /opt/steamvr/bin/linuxarm64)

      add_library(
  surfxr-xr-display
  frame/xr/actions.cpp
  frame/xr/desktop_projection.cpp
  frame/xr/desktop_nv12.cpp
  frame/xr/eye_gaze.cpp
  frame/xr/mini_hud.cpp
  frame/xr/foveation_renderer.cpp
  frame/xr/frame_loop.cpp
  frame/xr/image_enhancement_renderer.cpp
  frame/xr/lifecycle.cpp
  frame/xr/presentation_pacing.cpp
  frame/xr/projection_renderer.cpp
  frame/xr/projection_cache.cpp
  frame/xr/runtime_setup.cpp
  frame/xr/steamvr_refresh.cpp
  frame/xr/steamvr_boundary.cpp
  frame/xr/surface_staging.cpp
  frame/xr/surface_upload.cpp
  frame/xr/tracking_input.cpp
  frame/xr/surfxr_timing.cpp
  frame/xr/ui_layers.cpp
  frame/xr/vr_rendering.cpp
  frame/xr/timewarp.cpp
  frame/xr/xr_display.cpp
)
      target_compile_features(surfxr-xr-display PUBLIC cxx_std_17)
      target_include_directories(surfxr-xr-display PUBLIC ${SURFXR_PUBLIC_INCLUDE_DIRS} PRIVATE ${OPENXR_INCLUDE_DIR})
      target_link_libraries(surfxr-xr-display PUBLIC surfxr-vr-ui surfxr-tracking surfxr-frame-input Vulkan::Vulkan ${FRAME_OPENXR_LOADER})
      if(FRAME_OPENVR_INCLUDE_DIR AND FRAME_OPENVR_LIBRARY)
        target_include_directories(surfxr-xr-display PRIVATE ${FRAME_OPENVR_INCLUDE_DIR})
        target_compile_definitions(surfxr-xr-display PRIVATE SURFXR_HAVE_OPENVR_REFRESH=1)
        target_link_libraries(surfxr-xr-display PUBLIC ${FRAME_OPENVR_LIBRARY})
      endif()
      target_compile_options(surfxr-xr-display PRIVATE -Wall -Wextra -Wpedantic)
      add_executable(surfxr-runtime-bridge frame/xr/steamvr_bridge_main.cpp frame/xr/steamvr_boundary_worker.cpp frame/xr/steamvr_settings.cpp frame/xr/steamvr_controllers.cpp frame/xr/steamvr_headset_battery.cpp)
      target_compile_features(surfxr-runtime-bridge PRIVATE cxx_std_17)
      target_include_directories(surfxr-runtime-bridge PRIVATE ${SURFXR_PUBLIC_INCLUDE_DIRS})
      target_compile_options(surfxr-runtime-bridge PRIVATE -Wall -Wextra -Wpedantic)
      if(FRAME_OPENVR_INCLUDE_DIR AND FRAME_OPENVR_LIBRARY)
        target_include_directories(surfxr-runtime-bridge PRIVATE ${FRAME_OPENVR_INCLUDE_DIR})
        target_compile_definitions(surfxr-runtime-bridge PRIVATE SURFXR_HAVE_OPENVR_REFRESH=1)
        target_link_libraries(surfxr-runtime-bridge PRIVATE ${FRAME_OPENVR_LIBRARY})
        set_target_properties(surfxr-runtime-bridge PROPERTIES BUILD_RPATH /opt/steamvr/bin/linuxarm64)
      endif()
      add_executable(surfxr-projection-test tests/frame/xr/projection_renderer_test.cpp
  tests/frame/xr/projection_gpu_checks.cpp tests/frame/xr/projection_nv12_checks.cpp)
      target_link_libraries(surfxr-projection-test PRIVATE surfxr-xr-display)
      target_compile_options(surfxr-projection-test PRIVATE -Wall -Wextra -Wpedantic)
      set_target_properties(surfxr-projection-test PROPERTIES BUILD_RPATH /opt/steamvr/bin/linuxarm64)
      add_test(NAME projection-math COMMAND surfxr-projection-test --self-test)
    endif()
  endif()

  find_package(OpenSSL 3.0 QUIET COMPONENTS Crypto)
  include(cmake/surfxr.cmake)
  find_package(LibXml2 QUIET)
  include(cmake/launcher.cmake)
  if(TARGET OpenSSL::Crypto)
    add_executable(surfxr-discover frame/discovery/discovery_cli.cpp frame/discovery/discovery_scan.cpp)
    target_compile_features(surfxr-discover PRIVATE cxx_std_17)
    target_link_libraries(surfxr-discover PRIVATE OpenSSL::Crypto)
  endif()
  if(TARGET surfxr-session AND TARGET surfxr-video-decoder AND TARGET surfxr-xr-display)
    find_package(Threads REQUIRED)
    add_executable(
  surfxr-viewer
  frame/app/receiver_worker.cpp
  frame/app/viewer_checks.cpp
  frame/app/viewer_json.cpp
  frame/app/viewer_main.cpp
  frame/app/viewer_options.cpp
  frame/app/viewer_report.cpp
  frame/app/viewer_ui.cpp
  frame/ui/ui_input.cpp
  shared/protocol/input_wire.cpp
  frame/tracking/vr_client_state.cpp
  frame/tracking/vr_presentation.cpp
  tests/frame/tracking/vr_surfxr_checks.cpp
)
    target_link_libraries(surfxr-viewer PRIVATE surfxr-session surfxr-video-decoder surfxr-xr-display surfxr-stereo-splitter Threads::Threads)
    add_dependencies(surfxr-viewer surfxr-runtime-bridge)
    if(UNIX AND NOT APPLE)
      target_link_options(surfxr-viewer PRIVATE -Wl,--export-dynamic)
    endif()
    if(TARGET surfxr-audio-output)
      target_link_libraries(surfxr-viewer PRIVATE surfxr-audio-output)
      target_compile_definitions(surfxr-viewer PRIVATE SURFXR_HAVE_AUDIO_OUTPUT=1)
      target_link_libraries(surfxr-viewer PRIVATE surfxr-microphone)
      target_compile_definitions(surfxr-viewer PRIVATE SURFXR_HAVE_MICROPHONE=1)
    endif()
    target_compile_options(surfxr-viewer PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(surfxr-viewer PROPERTIES BUILD_RPATH "$ORIGIN/runtime/lib;$ORIGIN/../lib;/opt/steamvr/bin/linuxarm64")
    add_test(NAME viewer-offline COMMAND surfxr-viewer --self-test)
  endif()
endif()

# Packaged target-SDK executables must not retain SDK/build-tree lookup paths.
if(SURFXR_FFMPEG_ROOT AND UNIX AND NOT APPLE)
  foreach(_runtime surfxr-viewer surfxr-dashboard surfxr-discover surfxr-client surfxr-runtime-bridge)
    if(TARGET ${_runtime})
      set_target_properties(${_runtime} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/runtime/lib;$ORIGIN/../lib;/opt/steamvr/bin/linuxarm64")
    endif()
  endforeach()
  # Native CI has the SDK but no SteamVR installation. Keep its lookup path
  # in the offline test environment, never in shipped executable RPATHs.
  if(NOT CMAKE_CROSSCOMPILING AND FRAME_OPENVR_LIBRARY)
    get_filename_component(_openvr_test_library_dir "${FRAME_OPENVR_LIBRARY}" DIRECTORY)
    get_filename_component(_openxr_test_library_dir "${FRAME_OPENXR_LOADER}" DIRECTORY)
    foreach(_test dashboard-offline viewer-offline)
      if(TEST ${_test})
        set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
          "LD_LIBRARY_PATH=${_openvr_test_library_dir}:${_openxr_test_library_dir}:$ENV{LD_LIBRARY_PATH}")
      endif()
    endforeach()
  endif()
endif()
