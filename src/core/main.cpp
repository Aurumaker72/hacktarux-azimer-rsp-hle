#include <cstdio>
#include <memory>
#include <string>
#include <shared/Config.h>
#include <shared/spec/RSP.h>

#include "hle.h"
#include "shared/services/FrontendService.h"

RSP_INFO rsp;

// Whether RSP has been called since last ROM close 
bool rsp_alive = false;

void* audio_plugin = nullptr;
extern void (*g_processAList)();

#define UCODE_MARIO (1)
#define UCODE_BANJO (2)
#define UCODE_ZELDA (3)

extern void (*ABI1[0x20])();
extern void (*ABI2[0x20])();
extern void (*ABI3[0x20])();

void (*ABI[0x20])();

u32 inst1, inst2;

/**
 * \brief Loads the audio plugin's globals
 * \param path Path to an audio plugin dll
 * \returns Handle to the audio plugin, or nullptr.
 */
void* plugin_load(const std::string& path);

/**
 * \brief Handles unknown RSP tasks.
 */
void handle_unknown_task(const OSTask_t* task, uint32_t sum);

__declspec(dllexport) void CloseDLL(void)
{
}

__declspec(dllexport) void DllAbout(void* hParent)
{
    auto message =
        "Made using Azimer's code by Hacktarux.\r\nMaintained by Aurumaker72\r\nhttps://github.com/Aurumaker72/hacktarux-azimer-rsp-hle";
    FrontendService::show_info(message, PLUGIN_NAME, hParent);
}

__declspec(dllexport) void DllConfig(void* hParent)
{
    if (rsp_alive)
    {
        FrontendService::show_error("Close the ROM before configuring the RSP plugin.", PLUGIN_NAME, hParent);
        return;
    }

    FrontendService::show_config_dialog(hParent);
}

__declspec(dllexport) void DllTest(void* hParent)
{
}

static int audio_ucode_detect(OSTask_t* task)
{
    if (*(unsigned long*)(rsp.RDRAM + task->ucode_data + 0) != 0x1)
    {
        if (*(rsp.RDRAM + task->ucode_data + (0 ^ (3 - S8))) == 0xF)
            return 4;
        else
            return 3;
    }
    else
    {
        if (*(unsigned long*)(rsp.RDRAM + task->ucode_data + 0x30) == 0xF0000F00)
            return 1;
        else
            return 2;
    }
}

static int audio_ucode(OSTask_t* task)
{
    auto ucode_kind = audio_ucode_detect(task);
    printf("[RSP] ucode kind: %d", ucode_kind);

    switch (ucode_kind)
    {
    case UCODE_MARIO:
        memcpy(ABI, ABI1, sizeof(ABI[0]) * 0x20);
        break;
    case UCODE_BANJO:
        memcpy(ABI, ABI2, sizeof(ABI[0]) * 0x20);
        break;
    case UCODE_ZELDA:
        memcpy(ABI, ABI3, sizeof(ABI[0]) * 0x20);
        break;
    default:
        return -1;
    }

    const auto p_alist = (unsigned long*)(rsp.RDRAM + task->data_ptr);

    for (unsigned int i = 0; i < (task->data_size / 4); i += 2)
    {
        inst1 = p_alist[i];
        inst2 = p_alist[i + 1];
        ABI[inst1 >> 24]();
    }

    return 0;
}

__declspec(dllexport) uint32_t DoRspCycles(uint32_t Cycles)
{
    OSTask_t* task = (OSTask_t*)(rsp.DMEM + 0xFC0);
    unsigned int i, sum = 0;

    rsp_alive = true;

    // For first-time initialization of audio plugin
    // I think it's safe to keep the plugin loaded across emulation starts...
    if (config.audio_external && !audio_plugin)
    {
        audio_plugin = plugin_load(config.audio_path);
    }


    if (task->type == 1 && task->data_ptr != 0 && config.graphics_hle)
    {
        if (rsp.ProcessDlistList != NULL)
        {
            rsp.ProcessDlistList();
        }
        *rsp.SP_STATUS_REG |= 0x0203;
        if ((*rsp.SP_STATUS_REG & 0x40) != 0)
        {
            *rsp.MI_INTR_REG |= 0x1;
            rsp.CheckInterrupts();
        }

        *rsp.DPC_STATUS_REG &= ~0x0002;
        return Cycles;
    }
    else if (task->type == 2 && config.audio_hle)
    {
        if (config.audio_external)
            g_processAList();
        else if (rsp.ProcessAlistList != NULL)
        {
            rsp.ProcessAlistList();
        }
        *rsp.SP_STATUS_REG |= 0x0203;
        if ((*rsp.SP_STATUS_REG & 0x40) != 0)
        {
            *rsp.MI_INTR_REG |= 0x1;
            rsp.CheckInterrupts();
        }
        return Cycles;
    }
    else if (task->type == 7)
    {
        rsp.ShowCFB();
    }

    *rsp.SP_STATUS_REG |= 0x203;
    if ((*rsp.SP_STATUS_REG & 0x40) != 0)
    {
        *rsp.MI_INTR_REG |= 0x1;
        rsp.CheckInterrupts();
    }

    if (task->ucode_size <= 0x1000)
        for (i = 0; i < (task->ucode_size / 2); i++)
            sum += *(rsp.RDRAM + task->ucode + i);
    else
        for (i = 0; i < (0x1000 / 2); i++)
            sum += *(rsp.IMEM + i);


    if (task->ucode_size > 0x1000)
    {
        switch (sum)
        {
        case 0x9E2: // banjo tooie (U) boot code
            {
                int i, j;
                memcpy(rsp.IMEM + 0x120, rsp.RDRAM + 0x1e8, 0x1e8);
                for (j = 0; j < 0xfc; j++)
                    for (i = 0; i < 8; i++)
                        *(rsp.RDRAM + ((0x2fb1f0 + j * 0xff0 + i) ^ S8)) = *(rsp.IMEM + ((0x120 + j * 8 + i) ^ S8));
            }
            return Cycles;
            break;
        case 0x9F2: // banjo tooie (E) + zelda oot (E) boot code
            {
                int i, j;
                memcpy(rsp.IMEM + 0x120, rsp.RDRAM + 0x1e8, 0x1e8);
                for (j = 0; j < 0xfc; j++)
                    for (i = 0; i < 8; i++)
                        *(rsp.RDRAM + ((0x2fb1f0 + j * 0xff0 + i) ^ S8)) = *(rsp.IMEM + ((0x120 + j * 8 + i) ^ S8));
            }
            return Cycles;
            break;
        }
    }
    else
    {
        switch (task->type)
        {
        case 2: // audio
            if (audio_ucode(task) == 0)
                return Cycles;
            break;
        case 4: // jpeg
            switch (sum)
            {
            case 0x278: // used by zelda during boot
                *rsp.SP_STATUS_REG |= 0x200;
                return Cycles;
                break;
            case 0x2e4fc: // uncompress
                jpg_uncompress(task);
                return Cycles;
                break;
            default:
                {
                    char s[1024];
                    sprintf(s, "unknown jpeg:\n\tsum:%x", sum);
                    FrontendService::show_error(s);
                }
            }
            break;
        }
    }

    handle_unknown_task(task, sum);

    return Cycles;
}

__declspec(dllexport) void GetDllInfo(PLUGIN_INFO* PluginInfo)
{
    PluginInfo->Version = 0x0101;
    PluginInfo->Type = PLUGIN_TYPE_RSP;
    strcpy(PluginInfo->Name, PLUGIN_NAME);
    PluginInfo->NormalMemory = 1;
    PluginInfo->MemoryBswaped = 1;
}

__declspec(dllexport) void InitiateRSP(RSP_INFO Rsp_Info, uint32_t* CycleCount)
{
    rsp = Rsp_Info;
}

__declspec(dllexport) void RomClosed(void)
{
    int i;
    for (i = 0; i < 0x1000; i++)
    {
        rsp.DMEM[i] = rsp.IMEM[i] = 0;
    }
    /*   init_ucode1();
       init_ucode2();*/
    rsp_alive = false;
}
