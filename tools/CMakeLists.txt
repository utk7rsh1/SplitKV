# CLI tool
add_executable(splitkv-cli cli.cpp)
target_link_libraries(splitkv-cli PRIVATE splitkv)

# Server tool
add_executable(splitkv-server server.cpp)
target_link_libraries(splitkv-server PRIVATE splitkv)

# On Windows, the server needs Winsock for networking
if(WIN32)
    target_link_libraries(splitkv-server PRIVATE ws2_32)
endif()
