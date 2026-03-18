cmake_minimum_required(VERSION 3.16)
project(NekoChat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX)
endif()

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive")
endif()

set(WIN_LIBS ws2_32 iphlpapi winmm gdiplus ole32)

function(nekochat_add_target target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE ${WIN_LIBS})
endfunction()

nekochat_add_target(NekoChat_cmd_client P2PChat.cpp IceAgent.cpp)
nekochat_add_target(NekoChat_client P2PChatOpenGL.cpp IceAgent.cpp)
target_link_libraries(NekoChat_client PRIVATE opengl32)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/signalling_server_tmp.cpp "#include \"SignallingServer.hpp\"")
nekochat_add_target(NekoChat_server ${CMAKE_CURRENT_BINARY_DIR}/signalling_server_tmp.cpp)
