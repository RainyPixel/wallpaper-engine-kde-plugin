set(LIB_NAME wpVulkanRender)

add_library(${LIB_NAME}
STATIC
CopyPass.cpp
CustomShaderPass.cpp
FinPass.cpp
PrePass.cpp
SceneToRenderGraph.cpp
VideoTexture.cpp
VulkanRender.cpp
)

target_link_libraries(${LIB_NAME}
PRIVATE
	wpVulkan
    wpScene
    wpRGraph
    wpFs
)
target_include_directories(${LIB_NAME} PUBLIC include PRIVATE include/VulkanRender)
if(TARGET wpVideo)
    target_link_libraries(${LIB_NAME} PRIVATE wpVideo)
    target_compile_definitions(${LIB_NAME} PRIVATE WEKDE_HAVE_FFMPEG=1)
endif()
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts} -Wno-missing-field-initializers)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
