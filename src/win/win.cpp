#include <windows.h>
#include "win.h"

#include <stdio.h>
#include <string>

#include <shared/Config.h>
#include <shared/spec/RSP.h>
#include <shared/spec/Audio.h>
#include <shared/helpers/disasm.h>
#include <win/resource.h>

#include "shared/services/FrontendService.h"

extern RSP_INFO rsp;
extern bool g_rsp_alive;

HINSTANCE g_instance;
std::string g_app_path;

// ProcessAList function from audio plugin, only populated when audio_external is true
void (*g_processAList)() = nullptr;

t_config prev_config = {};

std::string get_app_full_path()
{
    char ret[MAX_PATH] = {0};
    char drive[_MAX_DRIVE], dirn[_MAX_DIR];
    char path_buffer[_MAX_DIR];
    GetModuleFileName(nullptr, path_buffer, sizeof(path_buffer));
    _splitpath(path_buffer, drive, dirn, nullptr, nullptr);
    strcpy(ret, drive);
    strcat(ret, dirn);

    return ret;
}


char* getExtension(char* str)
{
    if (strlen(str) > 3) return str + strlen(str) - 3;
    else return NULL;
}


BOOL APIENTRY
DllMain(
    HINSTANCE hInst /* Library instance handle. */ ,
    DWORD reason /* Reason this function is being called. */ ,
    LPVOID reserved /* Not used. */)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
        g_instance = hInst;
        g_app_path = get_app_full_path();
        config_load();

    // FIXME: Are we really sure we want to load the audio plugin here, and not on RomOpen?
    // audiohandle = (HMODULE)get_handle(liste_plugins, config.audio_path);
        break;

    case DLL_PROCESS_DETACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    }

    /* Returns TRUE on success, FALSE on failure */
    return TRUE;
}

static uint8_t fake_header[0x1000];
static uint32_t fake_AI_DRAM_ADDR_REG;
static uint32_t fake_AI_LEN_REG;
static uint32_t fake_AI_CONTROL_REG;
static uint32_t fake_AI_STATUS_REG;
static uint32_t fake_AI_DACRATE_REG;
static uint32_t fake_AI_BITRATE_REG;

void* plugin_load(const std::string& path)
{
    const auto module = LoadLibrary(path.c_str());

    if (!module)
    {
        FrontendService::show_error("Failed to load the external audio plugin.\nEmulation will not behave as expected.");
        return nullptr;
    }

    AUDIO_INFO info;
    // FIXME: Do we have to provide hwnd?
    info.hwnd = NULL;
    info.hinst = (HINSTANCE)rsp.hInst;
    info.MemoryBswaped = TRUE;
    info.HEADER = fake_header;
    info.RDRAM = rsp.RDRAM;
    info.DMEM = rsp.DMEM;
    info.IMEM = rsp.IMEM;
    info.MI_INTR_REG = rsp.MI_INTR_REG;
    info.AI_DRAM_ADDR_REG = &fake_AI_DRAM_ADDR_REG;
    info.AI_LEN_REG = &fake_AI_LEN_REG;
    info.AI_CONTROL_REG = &fake_AI_CONTROL_REG;
    info.AI_STATUS_REG = &fake_AI_STATUS_REG;
    info.AI_DACRATE_REG = &fake_AI_DACRATE_REG;
    info.AI_BITRATE_REG = &fake_AI_BITRATE_REG;
    info.CheckInterrupts = rsp.CheckInterrupts;
    auto initiateAudio = (BOOL (__cdecl *)(AUDIO_INFO))GetProcAddress(module, "InitiateAudio");
    g_processAList = (void (__cdecl *)(void))GetProcAddress(module, "ProcessAList");
    initiateAudio(info);

    return module;
}


INT_PTR CALLBACK ConfigDlgProc(HWND hwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
    case WM_INITDIALOG:
        config_load();
        memcpy(&prev_config, &config, sizeof(t_config));

        if (!config.audio_hle && !config.audio_external)
        {
            CheckDlgButton(hwnd, IDC_ALISTS_INSIDE_RSP, BST_CHECKED);
        }
        if (config.audio_hle && !config.audio_external)
        {
            CheckDlgButton(hwnd, IDC_ALISTS_EMU_DEFINED_PLUGIN, BST_CHECKED);
        }
        if (config.audio_hle && config.audio_external)
        {
            CheckDlgButton(hwnd, IDC_ALISTS_RSP_DEFINED_PLUGIN, BST_CHECKED);
        }
        CheckDlgButton(hwnd, IDC_UCODE_CACHE_VERIFY, config.ucode_cache_verify ? BST_CHECKED : BST_UNCHECKED);
        goto refresh;
    case WM_CLOSE:
        config_save();
        EndDialog(hwnd, IDOK);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            config.ucode_cache_verify = IsDlgButtonChecked(hwnd, IDC_UCODE_CACHE_VERIFY);
            config_save();
            EndDialog(hwnd, IDOK);
            break;
        case IDCANCEL:
            memcpy(&config, &prev_config, sizeof(t_config));
            config_save();
            EndDialog(hwnd, IDCANCEL);
            break;
        case IDC_ALISTS_INSIDE_RSP:
            config.audio_hle = FALSE;
            config.audio_external = FALSE;
            goto refresh;
        case IDC_ALISTS_EMU_DEFINED_PLUGIN:
            config.audio_hle = TRUE;
            config.audio_external = FALSE;
            goto refresh;
        case IDC_ALISTS_RSP_DEFINED_PLUGIN:
            config.audio_hle = TRUE;
            config.audio_external = TRUE;
            goto refresh;
        case IDC_BROWSE_AUDIO_PLUGIN:
            MessageBox(hwnd,
                       "Warning: use this feature at your own risk\n"
                       "It allows you to use a second audio plugin to process alists\n"
                       "before they are sent\n"
                       "Example: jabo's sound plugin in emu config to output sound\n"
                       "        +azimer's sound plugin in rsp config to process alists\n"
                       "Do not choose the same plugin in both place, or it'll probably crash\n"
                       "For more informations, please read the README",
                       "Warning", MB_OK);

            char path[sizeof(config.audio_path)] = {0};
            OPENFILENAME ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = path;
            ofn.nMaxFile = sizeof(path);
            ofn.lpstrFilter = "DLL Files (*.dll)\0*.dll";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

            if (GetOpenFileName(&ofn))
            {
                strcpy(config.audio_path, path);
            }

            goto refresh;
        }
        break;
    default:
        return FALSE;
    }
    return TRUE;

refresh:
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_AUDIO_PLUGIN), config.audio_external);
    EnableWindow(GetDlgItem(hwnd, IDC_BROWSE_AUDIO_PLUGIN), config.audio_external);
    SetDlgItemText(hwnd, IDC_EDIT_AUDIO_PLUGIN, config.audio_path);

    return TRUE;
}

void handle_unknown_task(const OSTask_t* task, const uint32_t sum)
{
    char s[1024];
    FILE* f;
    sprintf(s, "unknown task:\n\ttype:%d\n\tsum:%x\n\tPC:%x", task->type, sum, rsp.SP_PC_REG);
    MessageBox(NULL, s, "unknown task", MB_OK);

    if (task->ucode_size <= 0x1000)
    {
        f = fopen("imem.dat", "wb");
        fwrite(rsp.RDRAM + task->ucode, task->ucode_size, 1, f);
        fclose(f);

        f = fopen("dmem.dat", "wb");
        fwrite(rsp.RDRAM + task->ucode_data, task->ucode_data_size, 1, f);
        fclose(f);

        f = fopen("disasm.txt", "wb");
        memcpy(rsp.DMEM, rsp.RDRAM + task->ucode_data, task->ucode_data_size);
        memcpy(rsp.IMEM + 0x80, rsp.RDRAM + task->ucode, 0xF7F);
        disasm(f, (unsigned long*)(rsp.IMEM));
        fclose(f);
    }
    else
    {
        f = fopen("imem.dat", "wb");
        fwrite(rsp.IMEM, 0x1000, 1, f);
        fclose(f);

        f = fopen("dmem.dat", "wb");
        fwrite(rsp.DMEM, 0x1000, 1, f);
        fclose(f);

        f = fopen("disasm.txt", "wb");
        disasm(f, (unsigned long*)(rsp.IMEM));
        fclose(f);
    }
}

__declspec(dllexport) void DllAbout(void* hParent)
{
    auto message =
        "Made using Azimer's code by Hacktarux.\r\nMaintained by Aurumaker72\r\nhttps://github.com/Aurumaker72/hacktarux-azimer-rsp-hle";
    FrontendService::show_info(message, PLUGIN_NAME, hParent);
}

__declspec(dllexport) void DllConfig(void* hParent)
{
    if (g_rsp_alive)
    {
        FrontendService::show_error("Close the ROM before configuring the RSP plugin.", PLUGIN_NAME, hParent);
        return;
    }

    DialogBox(g_instance, MAKEINTRESOURCE(IDD_RSPCONFIG), (HWND)hParent, ConfigDlgProc);
}

__declspec(dllexport) void DllTest(void* hParent)
{
}
