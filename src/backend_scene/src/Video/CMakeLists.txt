# Video textures: decodes the MP4/WebM payload of Wallpaper Engine video
# textures with FFmpeg. Optional; without it those layers keep the transparent
# placeholder.
set(LIB_NAME wpVideo)

add_library(${LIB_NAME} STATIC VideoDecoder.cpp)
target_link_libraries(${LIB_NAME} PUBLIC PkgConfig::WEKDE_FFMPEG PRIVATE wpUtils wpFs)
target_include_directories(${LIB_NAME} PUBLIC include PRIVATE include/Video)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
