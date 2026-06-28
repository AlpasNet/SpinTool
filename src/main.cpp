#include <iostream>
#include <memory>

#define SDL_ENABLE_OLD_NAMES
#include "SDL3/SDL.h"
#include "backends/imgui_impl_sdl3.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "render.h"
#include "ui/ui_editor.h"

namespace spintool
{
    namespace
    {
        struct ApplicationState
        {
            bool quitting = false;
            std::unique_ptr<EditorUI> editor_ui;
        };

#if defined(__EMSCRIPTEN__)
        std::unique_ptr<ApplicationState> g_web_application;
#endif

        void RunFrame(ApplicationState& application)
        {
            SDL_Event event{};
            Renderer::NewFrame();

            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT)
                {
                    application.quitting = true;
                    break;
                }
            }

            if (!application.quitting && application.editor_ui)
            {
                application.editor_ui->Update();
                Renderer::Render();
            }
        }

        void ShutdownApplication(ApplicationState& application)
        {
            if (application.editor_ui)
            {
                application.editor_ui->Shutdown();
                application.editor_ui.reset();
            }
            Renderer::Shutdown();
            SDL_Quit();
        }

#if defined(__EMSCRIPTEN__)
        void WebMainLoop()
        {
            if (!g_web_application)
            {
                emscripten_cancel_main_loop();
                return;
            }

            RunFrame(*g_web_application);
            if (g_web_application->quitting)
            {
                ShutdownApplication(*g_web_application);
                g_web_application.reset();
                emscripten_cancel_main_loop();
            }
        }
#endif
    }

    int SSEMain()
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
            return 1;
        }

        if (!Renderer::Initialise())
        {
            std::cerr << "Renderer initialisation failed: " << SDL_GetError() << '\n';
            SDL_Quit();
            return 1;
        }

        auto application = std::make_unique<ApplicationState>();
        std::cerr << "[startup] Constructing editor UI\n";
        application->editor_ui = std::make_unique<EditorUI>();
        std::cerr << "[startup] Initialising editor UI\n";
        application->editor_ui->Initialise();

#if defined(__EMSCRIPTEN__)
        std::cerr << "[startup] Entering browser main loop\n";
        g_web_application = std::move(application);
        emscripten_set_main_loop(WebMainLoop, 0, false);
        return 0;
#else
        std::cerr << "[startup] Entering native main loop\n";
        while (!application->quitting)
        {
            RunFrame(*application);
            SDL_Delay(16);
        }

        ShutdownApplication(*application);
        return 0;
#endif
    }
}

int main(int, char**)
{
    return spintool::SSEMain();
}
