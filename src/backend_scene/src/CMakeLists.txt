find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

include_directories(.)

add_subdirectory(Fs)
add_subdirectory(Utils)
add_subdirectory(Timer)
add_subdirectory(Audio)
add_subdirectory(Scene)
add_subdirectory(Looper)
add_subdirectory(Vulkan)
add_subdirectory(Particle)

# Video textures (MP4/WebM stored inside a .tex) are decoded with FFmpeg when
# its development files are available; otherwise those layers stay transparent.
option(WEKDE_VIDEO_TEXTURES "Decode Wallpaper Engine video textures with FFmpeg" ON)
if(WEKDE_VIDEO_TEXTURES)
  pkg_check_modules(WEKDE_FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec
                    libswscale libavutil)
endif()
if(WEKDE_VIDEO_TEXTURES AND WEKDE_FFMPEG_FOUND)
  message(STATUS "Video textures: enabled (libavcodec ${WEKDE_FFMPEG_libavcodec_VERSION})")
  add_subdirectory(Video)
else()
  message(STATUS "Video textures: disabled (WEKDE_VIDEO_TEXTURES=OFF or FFmpeg development "
                 "files not found); such layers stay transparent")
endif()

add_subdirectory(VulkanRender)
add_subdirectory(RenderGraph)

add_library(
  ${PROJECT_NAME} STATIC
  WPJson.cpp
  WPPkgFs.cpp
  Type.cpp
  wpscene/WPImageObject.cpp
  wpscene/WPParticleObject.cpp
  wpscene/WPMaterial.cpp
  wpscene/WPScene.cpp
  wpscene/WPLightObject.cpp
  WPParticleParser.cpp
  WPShaderParser.cpp
  WPSceneParser.cpp
  WPShaderValueUpdater.cpp
  WPTexImageParser.cpp
  WPSoundParser.cpp
  WPMdlParser.cpp
  WPPuppet.cpp
  SceneLoader.cpp
  SceneWallpaper.cpp)

set(InteralLib
    wpAudio
    wpLooper
    wpVulkanRender
    wpFs
    wpScene
    wpVulkan
    wpRGraph
    wpParticle
    wpTimer)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib} nlohmann_json ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

target_include_directories(${PROJECT_NAME} PUBLIC . Swapchain)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
