#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include "../../mato.h"
#include "A.h"


typedef struct { 
           int module_id;
           int my_subscription_id;
	   int subscribed_to_module_id;
        } module_A_instance_data;

void *A_create_instance(int module_id)
{
    module_A_instance_data *data = (module_A_instance_data *)malloc(sizeof(module_A_instance_data));
    data->module_id = module_id;    
    printf("created a new instance of module A (%d) at %lu\n", module_id, (uint64_t)data);
    return data;
}

void *module_A_thread(void *arg)
{
    mato_inc_thread_count();
    module_A_instance_data *data = (module_A_instance_data *)arg;
    sleep(2 * data->module_id);
    for (int i = 0; i < 5; i++)
    {
        int *val = mato_get_data_buffer(sizeof(int));
        *val = data->module_id * 10 + i;
        mato_post_data(data->module_id, 0, sizeof(int), val);
        sleep(2);
    }
    printf("module_A_thread done (%d), unsubscribing...\n", data->module_id);
    mato_unsubscribe(data->subscribed_to_module_id, 0, data->my_subscription_id); 
    printf("module_A_thread terminates (%d)\n", data->module_id);

    mato_dec_thread_count();
}

void message_from_B(void *instance_data, int sender_module_id, int data_length, void *new_data_ptr)
{
    module_A_instance_data *data = (module_A_instance_data *)instance_data;
    int val = *((int *)new_data_ptr);
    printf("A(%d) receives message from B(%d): %d\n", data->module_id, sender_module_id, val);
    int *fwd_val = mato_get_data_buffer(sizeof(int));
    *fwd_val = val + 100;
    mato_post_data(data->module_id, 0, sizeof(int), fwd_val);
}

void A_start(void *instance_data)
{
    module_A_instance_data *data = (module_A_instance_data *)instance_data;

    //a1 wants to subscribe to b2
    //a2 wants to subscribe to b1
    
    int module_id = data->module_id;
    char *my_name = mato_get_module_name(module_id);

    int b1 = mato_get_module_id("B1");
    int b2 = mato_get_module_id("B2");

    if (strcmp(my_name, "A1") == 0) 
	data->subscribed_to_module_id = b2;
    else if (strcmp(my_name, "A2") == 0)
	data->subscribed_to_module_id = b1;

    data->my_subscription_id = mato_subscribe(module_id, data->subscribed_to_module_id, 0, message_from_B, direct_data_ptr);

    pthread_t t;
    printf("starting module A(%d)..\n", data->module_id);
    if (pthread_create(&t, 0, module_A_thread, data) != 0)
          perror("could not create thread for module A");
}

void A_delete(void *instance_data)
{
    module_A_instance_data *data = (module_A_instance_data *)instance_data;
    printf("deleting module instance (%d) = %lu\n", data->module_id, (uint64_t)data);
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