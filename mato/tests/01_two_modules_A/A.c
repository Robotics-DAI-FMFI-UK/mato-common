#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "../../mato.h"
#include "A.h"


typedef struct { 
           int module_id;
        } module_A_instance_data;

void *A_create_instance(int module_id)
{
    module_A_instance_data *data = (module_A_instance_data *)malloc(sizeof(module_A_instance_data));
    data->module_id = module_id;    
    printf("created a new instance of module A (%d) at %" PRIuPTR "\n", module_id, (uintptr_t)data);
    return data;
}

void *module_A_thread(void *arg)
{
    mato_inc_thread_count();
    module_A_instance_data *data = (module_A_instance_data *)arg;
    sleep(2 * (data->module_id % 5));
    for (int i = 0; program_runs && (i < 10); i++)
    {
        int *val = (int *)mato_get_data_buffer(sizeof(int));
        *val = i;
        printf("module A(%d) posting message %d\n", data->module_id, *val);
        mato_post_data(data->module_id, 0, sizeof(int), val);
        sleep(1);
    }
    mato_dec_thread_count();
    printf("module_A_thread done (%d)\n", data->module_id);
}

void A_start(void *instance_data)
{
    module_A_instance_data *data = (module_A_instance_data *)instance_data;
    pthread_t t;
    printf("starting module A(%d)..\n", data->module_id);
    if (pthread_create(&t, 0, module_A_thread, data) != 0)
          perror("could not create thread for module A");
}

void A_delete(void *instance_data)
{
    module_A_instance_data *data = (module_A_instance_data *)instance_data;
    printf("deleting module instance (%d) = %" PRIuPTR "\n", data->module_id, (uintptr_t)data);
    free(data);
}

void A_global_message(void *instance_data, int module_id_sender, int message_id, int msg_length, void *message_data)
{
    int module_id = ((module_A_instance_data *)instance_data)->module_id;
    char *data = (char *)message_data;
    if (message_id == MESSAGE_HELLO) 
    {
        printf("module %d received global HELLO messsage: '%s'\n", module_id, data);
    }
}

static module_specification A_specification = { A_create_instance, A_start, A_delete, A_global_message, 1 };

void A_init()
{
    printf("initializing module A...\n");
    mato_register_new_type_of_module("A", &A_specification);
}
