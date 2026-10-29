cmake_minimum_required(VERSION 3.13)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project("sceneviewer")

set(DEFAULT_BUILD_TYPE "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
endif()

option(BUILD_QML "Build qml scene viewer" ${BUILD_QML})

find_package(PkgConfig REQUIRED)

add_compile_options(
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-fsanitize=address>
)

add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
)

include_directories(../third_party)
add_subdirectory(.. scenebackend)

pkg_check_modules(GLFW REQUIRED glfw3)
add_executable(${PROJECT_NAME}
    glfwviewer.cpp
)
target_link_libraries(${PROJECT_NAME}
    PUBLIC 
        ${GLFW_LIBRARIES}
        wescene-renderer
)

# End-to-end screenshot test. It needs a display, a Vulkan device and a scene,
# so it reports SKIP unless WEKDE_E2E_ASSETS and WEKDE_E2E_SCENE are set.
if(BUILD_TESTS)
    enable_testing()
    add_test(NAME viewer_screenshot_e2e
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_screenshot.sh $<TARGET_FILE:${PROJECT_NAME}>)
    set_tests_properties(viewer_screenshot_e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

if(BUILD_QML)
    find_package(Qt6 COMPONENTS Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_executable(${PROJECT_NAME}-qml
        qmlviewer.cpp
        main.qml
        pkg.qrc
    )
    target_link_libraries(${PROJECT_NAME}-qml
        PUBLIC 
        wescene-renderer-qml
    )
    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF)
endif()
