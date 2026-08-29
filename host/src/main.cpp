// Camera Replace — entry point.
// WinMain is the Windows SUBSYSTEM:WINDOWS entry point so no console pops up
// when the user double-clicks the exe. If you need a console for a debug run,
// launch from PowerShell/cmd — stderr from util/Log is forwarded there.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app/App.h"
// MinGW's default C runtime dispatches to ANSI WinMain. We don't need Unicode
// argv here (no CLI args processed), so using the narrow variant keeps us off
// the -municode crt path and out of trouble.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd)
{
  cr::app::App app;
  return app.run(hInstance, nShowCmd);
}
