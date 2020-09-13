#include <stdio.h>

#include "config_mato.h"
#include "mato/mato.h"

mato_config_t mato_config;

mato_config_t default_mato_config = { 0, 1, 1, "/dev/steppers", "115200,N,n,8,1" };
mato_config_t mato_config;

void load_config()
{
    mato_log(ML_INFO, "load_config()");

    void *cfg = mato_config_read(MATO_CONFIG);
    if (cfg == 0)
    {
       mato_log_str(ML_ERR, "Could not open config file", MATO_CONFIG);
       program_runs = 0;
    }
    mato_config = default_mato_config;
    mato_config.autostart = mato_config_get_intval(cfg, "autostart", mato_config.autostart);
    mato_config.with_gui = mato_config_get_intval(cfg, "show_gui", mato_config.with_gui);
    mato_config.use_ncurses_control = mato_config_get_intval(cfg, "use_ncurses_control", mato_config.use_ncurses_control);
    mato_config.base_device = mato_config_get_alloc_strval(cfg, "base_device", mato_config.base_device);
    mato_config.base_serial_config = mato_config_get_alloc_strval(cfg, "base_serial_config", mato_config.base_serial_config);

    mato_config_dispose(cfg);
}
