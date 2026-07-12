#include <Windows.h>
#include "mod.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        Mod::Init(hModule);
        break;

    case DLL_PROCESS_DETACH:
        Mod::Shutdown();
        break;

    default:
        break;
    }
    return TRUE;
}
