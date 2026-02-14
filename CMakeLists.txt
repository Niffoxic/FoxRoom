cmake_minimum_required(VERSION 3.20)
project(FoxChatRoom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.0
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(imgui_lib PUBLIC
        IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_link_libraries(imgui_lib PUBLIC
        d3d11
        dxgi
)

add_executable(chat WIN32
        main.cpp
        src/imgui_hook.cpp
        src/fox/chat_room.cpp
        src/fox/text_chat.cpp
        src/fox/settings_store.cpp
        src/fox/theme_manager.cpp
        src/fox/font_manager.cpp
        src/fox/winsock_transport.cpp
        src/fox/voice_chat.cpp
        src/fox/music_playlist.cpp
)

target_include_directories(chat PRIVATE
        include
)

set(OPUS_ROOT ${CMAKE_SOURCE_DIR}/third_party/opus)

add_library(opus_prebuilt STATIC IMPORTED)
set_target_properties(opus_prebuilt PROPERTIES
        IMPORTED_LOCATION "${OPUS_ROOT}/lib/x64/opus.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${OPUS_ROOT}/include"
)

set(FMOD_ROOT ${CMAKE_SOURCE_DIR}/third_party/fmod/api/core)

target_include_directories(chat PRIVATE
        ${FMOD_ROOT}/inc
)

target_link_directories(chat PRIVATE
        ${FMOD_ROOT}/lib/x64
)

target_link_libraries(chat PRIVATE
        imgui_lib
        d3d11
        d3dcompiler
        dxgi
        windowscodecs
        xinput
        Ws2_32
        opus_prebuilt
        $<$<CONFIG:Debug>:fmodL_vc>
        $<$<CONFIG:Release>:fmod_vc>
)

add_custom_command(TARGET chat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/FoxChat
        $<TARGET_FILE_DIR:chat>/FoxChat
)

add_custom_command(TARGET chat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${FMOD_ROOT}/lib/x64/fmodL.dll>
        $<$<CONFIG:Release>:${FMOD_ROOT}/lib/x64/fmod.dll>
        $<TARGET_FILE_DIR:chat>
)

if(MSVC)
    target_compile_options(chat PRIVATE /W4)
    target_compile_definitions(chat PRIVATE UNICODE _UNICODE NOMINMAX)
endif()
