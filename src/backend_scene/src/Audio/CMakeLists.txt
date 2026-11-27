set(LIB_NAME wpAudio)

add_library(${LIB_NAME}
    STATIC
    AudioSpectrum.cpp
    SoundManager.cpp
)

pkg_check_modules(WEKDE_PULSE QUIET IMPORTED_TARGET libpulse)
if(WEKDE_PULSE_FOUND)
    target_compile_definitions(${LIB_NAME} PRIVATE WEKDE_AUDIO_MONITOR)
    target_link_libraries(${LIB_NAME} PRIVATE PkgConfig::WEKDE_PULSE Eigen3::Eigen)
endif()

target_link_libraries(${LIB_NAME}
PUBLIC
    wpUtils
PRIVATE
    wpFs
)
target_include_directories(${LIB_NAME} PUBLIC include PRIVATE include/Audio)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
