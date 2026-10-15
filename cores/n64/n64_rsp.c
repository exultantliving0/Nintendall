/*
 * Nintendall - Nintendo 64 RSP entry layer (mupen64plus-rsp-cxd4)
 *
 * angrylion is a software RDP only: it renders a raw RDP command list.  The
 * graphics microcode (F3DEX & friends) is what turns a display list into that
 * RDP command list, and it has to actually *run*.  mupen64plus-rsp-hle never
 * runs it - it is HLE, it pattern-matches known tasks and forwards the display
 * list straight to the graphics plugin - so with angrylion nothing ever
 * reaches the RDP and the screen stays blank.
 *
 * cxd4 is a full RSP interpreter.  It executes the microcode, writes the DPC
 * registers (DPC_START/DPC_END) and calls back into the graphics plugin's
 * ProcessRdpList(), which is exactly the path angrylion implements.
 *
 * cxd4 ships as a plugin whose module.c resolves the frontend's config and
 * core API through a dynamic-library handle at PluginStartup() time.  Nintendall
 * links it statically and never calls PluginStartup(), so the handful of
 * function pointers module.c dereferences on the InitiateRSP() path
 * (CoreDoCommand, ConfigGetParamBool) are installed here as minimal stubs.  The
 * pointers themselves are renamed at compile time (see cores/n64/CMakeLists.txt)
 * so they do not clash with mupen64plus-core's public API of the same name.
 *
 * Returning 0 ("false") for every boolean option selects cxd4's LLE behaviour:
 *   DisplayListToGraphicsPlugin = false -> run the graphics microcode (LLE)
 *   AudioListToAudioPlugin      = false -> run the audio microcode (LLE)
 * Both then write the correct RCP/DPC/AI registers, which the core and the
 * angrylion video plugin consume.
 */

#include <stdint.h>

#include "m64p_types.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_plugin.h"

/* cxd4 entry points (module.c). */
void CALL InitiateRSP(RSP_INFO rsp_info, unsigned int* cycle_count);
unsigned int CALL DoRspCycles(unsigned int cycles);
void CALL RomClosed(void);

/* module.c's config/core shims - renamed during compilation (see CMakeLists). */
extern ptr_CoreDoCommand      cxd4_CoreDoCommand;
extern ptr_ConfigGetParamBool cxd4_ConfigGetParamBool;

static m64p_error CALL rsp_stub_core_do_command(m64p_command command, int param_int,
                                                void* param_ptr)
{
    (void)command;
    (void)param_int;
    (void)param_ptr;
    return M64ERR_SUCCESS;
}

static int CALL rsp_stub_config_get_param_bool(m64p_handle section, const char* param_name)
{
    (void)section;
    (void)param_name;
    return 0;
}

void n64_rsp_init(RSP_INFO* info)
{
    if (info == NULL)
        return;

    cxd4_CoreDoCommand      = rsp_stub_core_do_command;
    cxd4_ConfigGetParamBool = rsp_stub_config_get_param_bool;

    InitiateRSP(*info, NULL);
}

void n64_rsp_execute(void)
{
    DoRspCycles(0);
}

void n64_rsp_rom_closed(void)
{
    RomClosed();
}
