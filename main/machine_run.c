#include <string.h>
#include "machine_run.h"
#include "decode_powermeter.h"
#include "config.h"

static powermeter_data_t s_last = {0};
static bool s_have_reading = false;

static bool s_override_active = false;
static bool s_override_value = false;

void machine_run_capture(const void *data)
{
    const powermeter_data_t *p = (const powermeter_data_t *)data;

    /* on_data is only called by main.c after a successful Modbus read,
     * but check p->ok too since the decode step could still mark it
     * invalid. A bad reading is simply dropped — s_last keeps its
     * previous (good) value. */
    if (!p->ok)
        return;

    s_last = *p;
    s_have_reading = true;
}

bool machine_is_running(void)
{
    if (s_override_active)
        return s_override_value;

    if (!s_have_reading)
        return true;

    return s_last.i_rms > MACHINE_RUN_CURRENT_A;
}

void machine_run_set_override(bool active, bool running_state)
{
    s_override_active = active;
    s_override_value = running_state;
}