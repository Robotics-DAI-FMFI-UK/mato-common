#ifndef _CONFIG_MATO_H_
#define _CONFIG_MATO_H_

#include "mato/mato.h"

#define MATO_CONFIG "mato-common-tests.cfg"

typedef struct {
    int autostart;
    int with_gui;
    int use_ncurses_control;
    char *base_device;
    char *base_serial_config;
} mato_config_t;

extern mato_config_t mato_config;

void load_config();

#endif
