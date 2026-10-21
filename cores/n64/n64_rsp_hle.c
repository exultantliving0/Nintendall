/*
 * Nintendall - mupen64plus-rsp-hle fast RSP backend
 *
 * This is deliberately not the upstream plugin.c wrapper.  plugin.c resolves
 * the core's config API and RSP fallback through a dynamic-library handle and
 * owns a fair amount of frontend policy we do not need.  The HLE engine
 * itself (hle.c and the alist/audio/mp3/... helpers) is self-contained: it
 * needs the RSP register pointers and four graphics/audio callbacks, all of
 * which the core already passes in RSP_INFO.
 *
 * We therefore drive hle_init()/hle_execute() directly and provide the small
 * set of Hle* callbacks that upstream normally defines in plugin.c.  Unknown
 * microcodes are not forwarded anywhere: HleForwardTask() reports "not
 * handled", hle.c logs the ucode and signals task-done so the game can keep
 * running.
 *
 * The glue layer routes M_AUDTASK to cxd4's LLE interpreter so fast mode
 * keeps the accurate audio path; everything else (graphics and support tasks)
 * runs through this HLE engine.  hle_aud stays false so that an audio task
 * reaching HLE would render into RDRAM rather than forward to a plugin.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "m64p_plugin.h"
#include "hle.h"

static struct hle_t s_hle;
static RSP_INFO s_info;
static int s_active;

static void hle_log(const char* level, const char* fmt, va_list ap)
{
    fprintf(stderr, "[n64/rsp-hle] %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void HleVerboseMessage(void* user_defined, const char* message, ...)
{
    (void)user_defined;
    (void)message;
    /* Verbose task logging is far too noisy for a shipped build. */
}

void HleInfoMessage(void* user_defined, const char* message, ...)
{
    va_list ap;
    (void)user_defined;
    va_start(ap, message);
    hle_log("info", message, ap);
    va_end(ap);
}

void HleWarnMessage(void* user_defined, const char* message, ...)
{
    va_list ap;
    (void)user_defined;
    va_start(ap, message);
    hle_log("warning", message, ap);
    va_end(ap);
}

void HleErrorMessage(void* user_defined, const char* message, ...)
{
    va_list ap;
    (void)user_defined;
    va_start(ap, message);
    hle_log("error", message, ap);
    va_end(ap);
}

void HleCheckInterrupts(void* user_defined)
{
    (void)user_defined;
    if (s_info.CheckInterrupts != NULL)
        s_info.CheckInterrupts();
}

void HleProcessDlistList(void* user_defined)
{
    (void)user_defined;
    if (s_info.ProcessDlistList != NULL)
        s_info.ProcessDlistList();
}

void HleProcessAlistList(void* user_defined)
{
    (void)user_defined;
    if (s_info.ProcessAlistList != NULL)
        s_info.ProcessAlistList();
}

void HleProcessRdpList(void* user_defined)
{
    (void)user_defined;
    if (s_info.ProcessRdpList != NULL)
        s_info.ProcessRdpList();
}

void HleShowCFB(void* user_defined)
{
    (void)user_defined;
    if (s_info.ShowCFB != NULL)
        s_info.ShowCFB();
}

int HleForwardTask(void* user_defined)
{
    (void)user_defined;
    return -1;   /* no LLE fallback in the fast stack */
}

int n64_rsp_hle_init(RSP_INFO* info)
{
    if (info == NULL)
        return -1;

    s_info = *info;
    memset(&s_hle, 0, sizeof(s_hle));

    hle_init(&s_hle,
             info->RDRAM, info->DMEM, info->IMEM,
             info->MI_INTR_REG,
             info->SP_MEM_ADDR_REG, info->SP_DRAM_ADDR_REG,
             info->SP_RD_LEN_REG, info->SP_WR_LEN_REG,
             info->SP_STATUS_REG, info->SP_DMA_FULL_REG,
             info->SP_DMA_BUSY_REG, info->SP_PC_REG,
             info->SP_SEMAPHORE_REG,
             info->DPC_START_REG, info->DPC_END_REG,
             info->DPC_CURRENT_REG, info->DPC_STATUS_REG,
             info->DPC_CLOCK_REG, info->DPC_BUFBUSY_REG,
             info->DPC_PIPEBUSY_REG, info->DPC_TMEM_REG,
             NULL);

    s_hle.hle_gfx = 1;
    s_hle.hle_aud = 0;
    s_active = 1;
    return 0;
}

void n64_rsp_hle_execute(void)
{
    if (!s_active)
        return;
    hle_execute(&s_hle);
}

void n64_rsp_hle_rom_closed(void)
{
    if (!s_active)
        return;
    s_hle.cached_ucodes.count = 0;
    s_active = 0;
}
