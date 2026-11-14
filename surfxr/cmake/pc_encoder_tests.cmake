# Opt-in GPU fixture: links the actual VR encoder, without driver/network owners.
set(SURFXR_ENCODER_CPP "${SURFXR_SURFXR_ROOT}/surfxr/server_openvr/cpp")
find_library(SURFXR_DESKTOP_swscale NAMES swscale HINTS "${SURFXR_DESKTOP_FFMPEG_ROOT}/lib" REQUIRED)
add_executable(surfxr-nvenc-color-test
  tests/pc/nvenc_color_checks.cpp
  tests/pc/nvenc_quality_probe.cpp
  tests/pc/frame_render_color_probe.cpp
  tests/pc/frame_render_pose_probe.cpp
  "${SURFXR_ENCODER_CPP}/surfxr_server/PoseHistory.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/FrameRender.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/FrameRenderStartup.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/FFR.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/GazeAtlas.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/d3d-render-utils/RenderPipeline.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/d3d-render-utils/RenderPipelineYUV.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/d3d-render-utils/RenderUtils.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/FrameRenderInput.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/VideoEncoderNVENC.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/VideoEncoderSW.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/NvEncoder.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/NvEncoderD3D11.cpp"
  "${SURFXR_ENCODER_CPP}/platform/win32/shared/d3drender.cpp")
target_compile_features(surfxr-nvenc-color-test PRIVATE cxx_std_17)
target_compile_definitions(surfxr-nvenc-color-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN SURFXR_GPL
  SURFXR_COLOR_OUTPUT="${PROJECT_BINARY_DIR}/color-validation"
  SURFXR_RENDER_SHADER_DIR="${SURFXR_ENCODER_CPP}/platform/win32")
target_include_directories(surfxr-nvenc-color-test SYSTEM PRIVATE
  "${SURFXR_ENCODER_CPP}" "${SURFXR_ENCODER_CPP}/platform/win32"
  "${SURFXR_SURFXR_ROOT}/openvr/headers" "${SURFXR_DESKTOP_FFMPEG_INCLUDE}")
target_compile_options(surfxr-nvenc-color-test PRIVATE /W3 /utf-8)
target_link_libraries(surfxr-nvenc-color-test PRIVATE d3d11 dxgi rpcrt4 advapi32
  ${SURFXR_DESKTOP_avcodec} ${SURFXR_DESKTOP_avutil} ${SURFXR_DESKTOP_swscale})
foreach(runtime_dll IN LISTS SURFXR_DESKTOP_RUNTIME_DLLS)
  add_custom_command(TARGET surfxr-nvenc-color-test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${runtime_dll}" "$<TARGET_FILE_DIR:surfxr-nvenc-color-test>")
endforeach()
# Hardware execution is deliberately not part of ordinary offline CTest.
