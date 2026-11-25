cmake_minimum_required(VERSION 3.22.1)
project(sdvx_nabla LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDVX_DEPS}/SDL-release-3.4.18" sdl EXCLUDE_FROM_ALL)

foreach(option FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
    set(${option} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${SDVX_DEPS}/freetype-VER-2-14-3" freetype EXCLUDE_FROM_ALL)
add_library(Freetype::Freetype ALIAS freetype)

foreach(option WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP
        WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX
        WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${SDVX_DEPS}/libwebp-1.6.0" webp EXCLUDE_FROM_ALL)
foreach(target webpdecode webpdsp webputils sharpyuv)
    target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:-O2>")
endforeach()

set(RML_ROOT "${SDVX_DEPS}/RmlUi-bc6ea0ec9968600d0b6e9dc379c5b9ba20f2806d")
set(RMLUI_PRECOMPILED_HEADERS ON CACHE BOOL "" FORCE)
add_subdirectory("${RML_ROOT}" rmlui EXCLUDE_FROM_ALL)

add_library(bass SHARED IMPORTED)
set_target_properties(bass PROPERTIES IMPORTED_LOCATION "${SDVX_DEPS}/bass/libs/${ANDROID_ABI}/libbass.so")
add_library(bass_fx SHARED IMPORTED)
set_target_properties(bass_fx PROPERTIES IMPORTED_LOCATION "${SDVX_DEPS}/bass-fx/libs/${ANDROID_ABI}/libbass_fx.so")

set(FFMPEG_ROOT "${SDVX_DEPS}/ffmpeg-9.0.2-android/${ANDROID_ABI}/install")
foreach(library avformat avcodec swresample avutil)
    add_library(${library} SHARED IMPORTED)
    set_target_properties(${library} PROPERTIES IMPORTED_LOCATION "${FFMPEG_ROOT}/lib/lib${library}.so")
endforeach()

set(CUBISM_ROOT "${SDVX_DEPS}/CubismSdkForNative-5-r.5")
set(FRAMEWORK_SOURCE OpenGL)
set(FRAMEWORK_DEFINITIOINS CSM_TARGET_ANDROID_ES2)
add_subdirectory("${CUBISM_ROOT}/Framework" cubism EXCLUDE_FROM_ALL)
target_compile_definitions(Framework PUBLIC CSM_TARGET_ANDROID_ES2)
target_compile_options(Framework PRIVATE -O2)
target_include_directories(Framework SYSTEM PUBLIC "${CUBISM_ROOT}/Core/include")
set_target_properties(Framework PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${CUBISM_ROOT}/Framework/src;${CUBISM_ROOT}/Core/include")
add_library(CubismCore STATIC IMPORTED)
set_target_properties(CubismCore PROPERTIES IMPORTED_LOCATION "${CUBISM_ROOT}/Core/lib/android/${ANDROID_ABI}/libLive2DCubismCore.a")
target_link_libraries(Framework PUBLIC CubismCore GLESv3)

file(GLOB STAGE_FILES CONFIGURE_DEPENDS "stage*.cpp")
file(GLOB CABINET_FILES CONFIGURE_DEPENDS "cabinet*.cpp")
add_library(main SHARED main.cpp audio.cpp s3v_audio.cpp render.cpp afp.cpp afp_movie.cpp ${STAGE_FILES} ${CABINET_FILES})
target_compile_features(main PRIVATE cxx_std_20)
target_compile_options(main PRIVATE -Wall -Wextra -O2)
target_include_directories(main SYSTEM PRIVATE
    "${SDVX_DEPS}/json-3.12.0/single_include" "${SDVX_DEPS}/stb-4e9b368bfc071e97206ea5c6adba68dd0cbf8897"
    "${SDVX_DEPS}/bass/c" "${SDVX_DEPS}/bass-fx/C" "${RML_ROOT}/Backends" "${FFMPEG_ROOT}/include")
target_link_libraries(main PRIVATE SDL3::SDL3 EGL GLESv3 android log z bass bass_fx avformat avcodec swresample avutil
    freetype webp RmlUi::RmlUi Framework)

# Composite the upstream renderer into the same logical cabinet framebuffer as AFP.
file(READ "${RML_ROOT}/Backends/RmlUi_Renderer_GL3.cpp" rml_renderer)
string(REPLACE "// Draw to backbuffer\n\tglBindFramebuffer(GL_FRAMEBUFFER, 0);"
    "// Preserve the overlay and the caller's cabinet clipping rectangle.\n\textern unsigned int sdvxRmlDrawTarget, sdvxRmlOverlayTexture;\n\tsdvxRmlOverlayTexture = fb_postprocess.color_tex_buffer;\n\tglBindFramebuffer(GL_FRAMEBUFFER, sdvxRmlDrawTarget);\n\tif (glstate_backup.enable_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);\n\tglScissor(glstate_backup.scissor[0], glstate_backup.scissor[1], glstate_backup.scissor[2], glstate_backup.scissor[3]);"
    rml_renderer "${rml_renderer}")
if(SDVX_EMULATOR_GLES31)
    string(REPLACE "#version 320 es" "#version 310 es" rml_renderer "${rml_renderer}")
    target_compile_definitions(main PRIVATE SDVX_EMULATOR_GLES31=1)
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RmlUi_Renderer_GL3.cpp" "${rml_renderer}")
target_sources(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/RmlUi_Renderer_GL3.cpp")
