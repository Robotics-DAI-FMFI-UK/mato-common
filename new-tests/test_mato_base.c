#include <stdio.h>
#include <unistd.h>

#include "../mato/mato.h"
#include "../modules/live/mato_base_module.h"
#include "core/config_mato.h"

int main()
{
   printf("starting up...\n");
   mato_init(0, 0);
   load_config();
   init_mato_base_module();
   int base = mato_create_new_module_instance("mato_base", "stepperduino");
   mato_start();

   sleep(5);
   printf("forward speed 30...\n");

   int8_t spds[] = {30, 30};
   mato_send_message(mato_main_program_module_id(), base, MATO_BASE_MSG_SET_SPEED, 2, spds);
   sleep(5);
   printf("backward speed 30...\n");
   spds[0] = spds[1] = -30;
   mato_send_message(mato_main_program_module_id(), base, MATO_BASE_MSG_SET_SPEED, 2, spds);
   sleep(5);
   printf("speed 0...\n");
   spds[0] = spds[1] = 0;
   mato_send_message(mato_main_program_module_id(), base, MATO_BASE_MSG_SET_SPEED, 2, spds);
   sleep(5);
   printf("shutdown...\n");
   
   program_runs = 0;
   sleep(1);

   mato_delete_module_instance(base);
   sleep(1);
   mato_shutdown();
   return 0;
}

