enable_testing()

get_filename_component(SCENE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DOCTEST_INCLUDE_DIR
    "${SCENE_DIR}/third_party/nlohmann/tests/thirdparty/doctest")
set(SRC_DIR "${SCENE_DIR}/src")

add_executable(backend_scene_tests
    test_main.cpp
    test_WPTexImageParser.cpp
    test_SpecTexs.cpp
    test_SpriteAnimation.cpp
    test_PassCache.cpp
    test_OrthoCamera.cpp
    test_helpers.cpp
    test_PpmWriter.cpp
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
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

add_executable(backend_scene_parser_tests
    test_main.cpp
    test_WPShaderParser.cpp
    test_WPMdlParser.cpp
    test_WPMaterial.cpp
    test_WPSceneParser.cpp
    test_SceneLoader.cpp
    test_SceneWallpaper.cpp
)
target_include_directories(backend_scene_parser_tests PRIVATE ${DOCTEST_INCLUDE_DIR})
target_link_libraries(backend_scene_parser_tests PRIVATE
    wescene-renderer wpFs wpScene wpAudio wpParticle wpVulkan wpLooper wpTimer wpRGraph
    wpVulkanRender nlohmann_json)
add_test(NAME backend_scene_parser_tests COMMAND backend_scene_parser_tests)
set_tests_properties(backend_scene_parser_tests PROPERTIES TIMEOUT 30)

add_executable(backend_scene_shader_tests test_main.cpp test_ShaderCompiler.cpp)
target_include_directories(backend_scene_shader_tests PRIVATE
    ${SRC_DIR} ${DOCTEST_INCLUDE_DIR})
target_link_libraries(backend_scene_shader_tests PRIVATE wpVulkan)
add_test(NAME backend_scene_shader_tests COMMAND backend_scene_shader_tests)

# Video texture decoding (only when built with FFmpeg)
if(TARGET wpVideo)
    add_executable(backend_scene_video_tests test_main.cpp test_VideoDecoder.cpp)
    target_include_directories(backend_scene_video_tests PRIVATE ${SRC_DIR} ${DOCTEST_INCLUDE_DIR})
    target_link_libraries(backend_scene_video_tests PRIVATE wpVideo wpFs wpUtils)
    target_compile_definitions(backend_scene_video_tests PRIVATE
        WEKDE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME backend_scene_video_tests COMMAND backend_scene_video_tests)
    set_tests_properties(backend_scene_video_tests PROPERTIES TIMEOUT 60)
endif()
