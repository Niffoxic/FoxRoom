#include <windows.h>
#include "base.h"
#include "imgui_hook.h"

#include "fox/chat_room.h"
#include "fox/settings_store.h"
#include "fox/winsock_transport.h"
#include "fox/text_chat.h"
#include "fox/theme_manager.h"
#include "fox/font_manager.h"
#include "fox/voice_chat.h"
#include "fox/music_playlist.h"

int WINAPI wWinMain(
    HINSTANCE   hInstance,
    HINSTANCE   hPrevInstance,
    PWSTR       pCmdLine,
    int         nCmdShow)
{
    GamesEngineeringBase::Window window;
    window.create(1280, 720, "ChatRoom");

    fox::winsock_transport_options net_opts{};
    net_opts.display_name   = "LocalUser";
    net_opts.room_password  = "foxchat1212";
    net_opts.host_port      = 48000;
    auto transport          = std::make_shared<fox::winsock_transport>(net_opts);
    auto text_chat          = std::make_shared<fox::text_chat>();
    auto settings           = std::make_shared<fox::settings_store>();
    auto themes             = std::make_shared<fox::theme_manager>();
    auto fonts              = std::make_shared<fox::font_manager>();
    auto voice              = std::make_shared<fox::voice_chat>();
    auto playlist           = std::make_shared<fox::music_playlist>();

    fox::chat_room_params params{};
    params.config.display_name  = "LocalUser";
    params.transport            = transport;
    params.text_chat            = text_chat;
    params.voice_chat           = voice;
    params.settings_store       = settings;
    params.theme_manager        = themes;
    params.font_manager         = fonts;
    params.playlist             = playlist;

    fox::chat_room room(std::move(params));

    fox::imgui_hook::instance().set_view_title("Chat");
    fox::imgui_hook::instance().set_view([&room]()
    {
        room.imgui_render();
    });

    fox::imgui_hook::instance().add_main_menu("chatroom_menu", [&room]()
    {
        room.imgui_main_menu();
    });


    while (true)
    {
        if (window.keyPressed(VK_ESCAPE))
        {
            std::exit(0);
        }

        window.clear();
        window.present();
    }
}
