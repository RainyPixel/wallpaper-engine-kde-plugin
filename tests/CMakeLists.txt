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

find_package(Qt6 REQUIRED COMPONENTS Core Test)
set(CMAKE_AUTOMOC ON)

# ── FileHelper unit tests ──────────────────────────────────────────────────────
add_executable(tst_filehelper
    tst_filehelper.cpp
    ${WEKDE_SRC_DIR}/FileHelper.cpp
)
target_include_directories(tst_filehelper PRIVATE ${WEKDE_SRC_DIR})
target_link_libraries(tst_filehelper PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_filehelper COMMAND tst_filehelper -v2)
