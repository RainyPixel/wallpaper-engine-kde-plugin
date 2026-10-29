# Can be built standalone:  cmake -B build/tests -S tests
# Or included from root with: -DBUILD_TESTS=ON
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(WallpaperEngineKdeTests CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
    set(WEKDE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")
else()
    set(WEKDE_SRC_DIR "${CMAKE_SOURCE_DIR}/src")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Test DBus)
set(CMAKE_AUTOMOC ON)

# ── FileHelper unit tests ──────────────────────────────────────────────────────
add_executable(tst_filehelper
    tst_filehelper.cpp
    ${WEKDE_SRC_DIR}/FileHelper.cpp
    ${WEKDE_SRC_DIR}/SteamPaths.cpp
)
target_include_directories(tst_filehelper PRIVATE ${WEKDE_SRC_DIR})
target_link_libraries(tst_filehelper PRIVATE Qt6::Core Qt6::Test Qt6::DBus)
add_test(NAME tst_filehelper COMMAND tst_filehelper -v2)

# Use a private session so this test never contacts the desktop notification daemon.
add_executable(tst_savenotification
    tst_savenotification.cpp
    ${WEKDE_SRC_DIR}/FileHelper.cpp
    ${WEKDE_SRC_DIR}/SteamPaths.cpp
)
target_include_directories(tst_savenotification PRIVATE ${WEKDE_SRC_DIR})
target_link_libraries(tst_savenotification PRIVATE Qt6::Core Qt6::Test Qt6::DBus)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME tst_savenotification
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_savenotification>)
    set_tests_properties(tst_savenotification PROPERTIES
        ENVIRONMENT "WEKDE_NOTIFICATION_TEST_BUS=1" TIMEOUT 15)
endif()

# ── Global configuration persistence tests ────────────────────────────────────
add_executable(tst_globalconfig
    tst_globalconfig.cpp
    ${WEKDE_SRC_DIR}/GlobalConfigStore.cpp
)
target_include_directories(tst_globalconfig PRIVATE ${WEKDE_SRC_DIR})
target_link_libraries(tst_globalconfig PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_globalconfig COMMAND tst_globalconfig -v2)

# ── Steam path detection unit tests ────────────────────────────────────────────
add_executable(tst_steampaths
    tst_steampaths.cpp
    ${WEKDE_SRC_DIR}/SteamPaths.cpp
)
target_include_directories(tst_steampaths PRIVATE ${WEKDE_SRC_DIR})
target_link_libraries(tst_steampaths PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_steampaths COMMAND tst_steampaths -v2)

find_package(Qt6 6.7 QUIET COMPONENTS QuickTest)
if(Qt6_FOUND AND TARGET Qt6::qmltestrunner)
    add_test(NAME qml_tests COMMAND Qt6::qmltestrunner
        -input ${CMAKE_CURRENT_SOURCE_DIR}/qml
        -import ${CMAKE_CURRENT_SOURCE_DIR}/qml/imports)
    set_tests_properties(qml_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_XHR_ALLOW_FILE_READ=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu;QTWEBENGINE_DISABLE_SANDBOX=1")
endif()

option(WEKDE_GPU_TESTS "Run native scene tests on an OpenGL/Vulkan desktop" OFF)
if(WEKDE_GPU_TESTS)
    if(NOT TARGET WallpaperEngineKde OR NOT TARGET Qt6::qmltestrunner)
        message(FATAL_ERROR "WEKDE_GPU_TESTS requires the native plugin and Qt QuickTest")
    endif()
    set(gpu_imports "${CMAKE_CURRENT_BINARY_DIR}/gpu-imports")
    file(GENERATE
        OUTPUT "${gpu_imports}/com/github/catsout/wallpaperEngineKde/qmldir"
        CONTENT "module com.github.catsout.wallpaperEngineKde\nplugin WallpaperEngineKde $<TARGET_FILE_DIR:WallpaperEngineKde>\n")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gpu-config" "${CMAKE_CURRENT_BINARY_DIR}/gpu-cache")
    add_test(NAME scene_gpu_tests COMMAND Qt6::qmltestrunner
        -input ${CMAKE_CURRENT_SOURCE_DIR}/gpu -import ${gpu_imports})
    set_tests_properties(scene_gpu_tests PROPERTIES LABELS gpu TIMEOUT 30
        ENVIRONMENT "QSG_RHI_BACKEND=opengl;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/gpu-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/gpu-cache")
endif()
