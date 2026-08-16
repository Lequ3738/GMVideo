// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include "Gmapi.h"

gm::CGMAPI* gmapi = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        {
            DWORD result = 0;
            gmapi = gm::CGMAPI::Create(&result);
            // Check the initialization
            if (result == gm::GMAPI_INITIALIZATION_FAILED)
            {
                MessageBox(NULL, L"Unable to initialize GMAPI.", NULL, MB_SYSTEMMODAL | MB_ICONERROR);
                return FALSE;
            }
        }
        break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;

        case DLL_PROCESS_DETACH:
            gm::CGMAPI::Destroy();
            break;
    }

    return TRUE;
}
