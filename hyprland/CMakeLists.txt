# Standalone build of the Hyprland host:
#   cmake -B build/hyprland -S hyprland
# The KDE Plasma plugin keeps its own build in the repository root.
cmake_minimum_required(VERSION 3.21)

project(WallpaperEngineHyprland VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "Choose the type of build." FORCE)
endif()

option(WEHYPR_BUILD_TESTS "Build unit tests (no display needed)" ON)
option(WEHYPR_INTEGRATION_TESTS
       "Register the Wayland smoke test with CTest (opens a real surface)" OFF)

include(GNUInstallDirs)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network Gui Qml Quick QuickControls2)
find_package(Qt6 6.7 QUIET COMPONENTS WebEngineQuick)
if(NOT TARGET Qt6::WebEngineQuick)
  message(FATAL_ERROR
    "Qt WebEngine Quick 6.7 or newer was not found.\n"
    "  Arch:          pacman -S qt6-webengine\n"
    "  Fedora:        dnf install qt6-qtwebengine-devel\n"
    "  Debian/Ubuntu: apt install qt6-webengine-dev")
endif()

find_package(LayerShellQt CONFIG QUIET)
if(NOT TARGET LayerShellQt::Interface)
  message(FATAL_ERROR
    "LayerShellQt (Qt 6 build) was not found.\n"
    "  Arch:          pacman -S layer-shell-qt\n"
    "  Fedora:        dnf install layer-shell-qt-devel\n"
    "  Debian/Ubuntu: apt install liblayershellqtinterface-dev")
endif()

# setActivateOnShow() is missing in older LayerShellQt (Debian trixie ships 6.3.4).
include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES LayerShellQt::Interface Qt6::Gui)
check_cxx_source_compiles(
  "#include <LayerShellQt/Window>
   int main() { LayerShellQt::Window* w = nullptr; w->setActivateOnShow(false); return 0; }"
  WEHYPR_HAVE_ACTIVATE_ON_SHOW)
cmake_pop_check_state()

qt_standard_project_setup(REQUIRES 6.7)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(WEHYPR_WARNINGS -Wall -Wextra)
endif()

# The Steam detector is shared with the KDE plugin and compiled in directly:
# src/ exports nothing (hidden visibility plus --exclude-libs,ALL).
add_library(wehypr-core STATIC
  src/Ipc.cpp
  src/Ipc.hpp
  src/Library.cpp
  src/Library.hpp
  src/Options.cpp
  src/Options.hpp
  src/ProjectConfig.cpp
  src/ProjectConfig.hpp
  src/State.cpp
  src/State.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/SteamPaths.cpp
)
target_include_directories(wehypr-core PUBLIC src)
target_include_directories(wehypr-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(wehypr-core PUBLIC Qt6::Core Qt6::Network)
target_compile_options(wehypr-core PRIVATE ${WEHYPR_WARNINGS})

qt_add_executable(wallpaper-engine-hyprland
  src/main.cpp
  src/WallpaperHost.cpp
  src/WallpaperHost.hpp
)
set_source_files_properties(qml/WallpaperSurface.qml
  PROPERTIES QT_RESOURCE_ALIAS WallpaperSurface.qml)
set_source_files_properties(qml/Browser.qml
  PROPERTIES QT_RESOURCE_ALIAS Browser.qml)
qt_add_qml_module(wallpaper-engine-hyprland
  URI WallpaperEngineHyprland
  VERSION 1.0
  QML_FILES qml/WallpaperSurface.qml qml/Browser.qml
)
target_compile_definitions(wallpaper-engine-hyprland
  PRIVATE WEHYPR_VERSION="${PROJECT_VERSION}")
if(WEHYPR_HAVE_ACTIVATE_ON_SHOW)
  target_compile_definitions(wallpaper-engine-hyprland PRIVATE WEHYPR_HAVE_ACTIVATE_ON_SHOW)
endif()
target_compile_options(wallpaper-engine-hyprland PRIVATE ${WEHYPR_WARNINGS})
target_link_libraries(wallpaper-engine-hyprland
  PRIVATE
  wehypr-core
  Qt6::Gui
  Qt6::Qml
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::WebEngineQuick
  LayerShellQt::Interface
)

install(TARGETS wallpaper-engine-hyprland RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WEHYPR_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 6.7 REQUIRED COMPONENTS Test)

  foreach(test_name tst_options tst_project tst_ipc tst_library)
    qt_add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE wehypr-core Qt6::Test)
    target_compile_options(${test_name} PRIVATE ${WEHYPR_WARNINGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # Runs the built binary for commands that never open a window.
  qt_add_executable(tst_cli tests/tst_cli.cpp)
  target_link_libraries(tst_cli PRIVATE Qt6::Core Qt6::Test)
  target_compile_options(tst_cli PRIVATE ${WEHYPR_WARNINGS})
  target_compile_definitions(tst_cli PRIVATE
    WEHYPR_BINARY="$<TARGET_FILE:wallpaper-engine-hyprland>"
    WEHYPR_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/web-basic")
  add_dependencies(tst_cli wallpaper-engine-hyprland)
  add_test(NAME tst_cli COMMAND tst_cli)

  set_tests_properties(tst_options tst_project tst_ipc tst_library tst_cli
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS unit)
endif()

if(WEHYPR_INTEGRATION_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME hyprland_smoke
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/smoke_test.py
            --binary $<TARGET_FILE:wallpaper-engine-hyprland>)
  set_tests_properties(hyprland_smoke
    PROPERTIES LABELS integration TIMEOUT 300 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
