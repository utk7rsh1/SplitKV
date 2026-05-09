# Collect all .cpp source files in src/
file(GLOB SPLITKV_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(splitkv STATIC ${SPLITKV_SOURCES})

target_include_directories(splitkv
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

# Platform-specific link libraries
if(UNIX)
    target_link_libraries(splitkv PUBLIC pthread)
endif()
if(WIN32)
    target_link_libraries(splitkv PUBLIC ws2_32)
endif()
