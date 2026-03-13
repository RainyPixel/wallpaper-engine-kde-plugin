enable_testing()

set(DOCTEST_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann/tests/thirdparty/doctest")
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")

add_executable(backend_scene_tests
    test_main.cpp
    test_WPTexImageParser.cpp
    test_SpecTexs.cpp
    test_SpriteAnimation.cpp
    test_helpers.cpp
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${DOCTEST_INCLUDE_DIR}
)

target_link_libraries(backend_scene_tests PRIVATE
    wpUtils
    wpFs
    ${LZ4_LIBRARIES}
    Eigen3::Eigen
)

target_compile_options(backend_scene_tests PRIVATE ${warn_opts})

add_test(NAME backend_scene_tests COMMAND backend_scene_tests)
