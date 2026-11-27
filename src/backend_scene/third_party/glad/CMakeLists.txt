project(glad)

add_library(${PROJECT_NAME} STATIC include/glad/gl.h src/gl.c)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(${PROJECT_NAME} PUBLIC include)
