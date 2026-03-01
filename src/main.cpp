import core;
import std;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "App/AppLifecycle.h"

int main(int argc, char** argv)
{
    try
    {
        appLifecycle::AppState app{};
        appLifecycle::InitializeApp(app, argc, argv);

        while (appLifecycle::TickApp(app))
        {
        }

        appLifecycle::ShutdownApp(app);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal: " << exception.what() << "\n";
        return 2;
    }
}