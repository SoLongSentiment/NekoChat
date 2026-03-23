cmake_minimum_required(VERSION 3.16)
project(NekoChat CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX)
endif()

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive")
endif()

file(GLOB RNNOISE_SOURCES "libs/rnnoise/src/*.c")
set(RNNOISE_X86_SOURCES
    "libs/rnnoise/src/x86/x86_dnn_map.c"
    "libs/rnnoise/src/x86/x86cpu.c"
    "libs/rnnoise/src/x86/nnet_sse4_1.c"
    "libs/rnnoise/src/x86/nnet_avx2.c"
)

if(MSVC)
    set_source_files_properties(
        "libs/rnnoise/src/x86/nnet_sse4_1.c"
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
    )
    set_source_files_properties(
        "libs/rnnoise/src/x86/nnet_avx2.c"
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
    )
else()
    set_source_files_properties(
        "libs/rnnoise/src/x86/nnet_sse4_1.c"
        PROPERTIES COMPILE_OPTIONS "-msse4.1"
    )
    set_source_files_properties(
        "libs/rnnoise/src/x86/nnet_avx2.c"
        PROPERTIES COMPILE_OPTIONS "-mavx2"
    )
endif()
add_library(rnnoise_static STATIC ${RNNOISE_SOURCES} ${RNNOISE_X86_SOURCES})
target_compile_definitions(rnnoise_static PRIVATE
    RNN_ENABLE_X86_RTCD
    OPUS_X86_MAY_HAVE_SSE2
    OPUS_X86_MAY_HAVE_SSE4_1
    OPUS_X86_MAY_HAVE_AVX2
)
target_include_directories(rnnoise_static PUBLIC 
    "libs/rnnoise"         
    "libs/rnnoise/include" 
    "libs/rnnoise/src"     
)

set(WIN_LIBS ws2_32 iphlpapi winmm gdiplus ole32 rnnoise_static)

function(nekochat_add_target target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "libs/rnnoise/include")
    target_link_libraries(${target} PRIVATE ${WIN_LIBS})
endfunction()

nekochat_add_target(NekoChat_cmd_client P2PChat.cpp IceAgent.cpp)
nekochat_add_target(NekoChat_client P2PChatOpenGL.cpp IceAgent.cpp)
target_link_libraries(NekoChat_client PRIVATE opengl32)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/signalling_server_tmp.cpp "#include \"SignallingServer.hpp\"")
nekochat_add_target(NekoChat_server ${CMAKE_CURRENT_BINARY_DIR}/signalling_server_tmp.cpp)
