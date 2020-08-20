#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "../../mato.h"
#include "B.h"


typedef struct { 
           int module_id;
           int my_subscription_id; 
           int subscribed_to_module_id;
           int hello_starting_pipe[2];
        } module_B_instance_data;

void *B_create_instance(int module_id)
{
    module_B_instance_data *data = (module_B_instance_data *)malloc(sizeof(module_B_instance_data));
    data->module_id = module_id;    
    if (pipe(data->hello_starting_pipe) < 0)
        perror("could not create starting pipe");
    printf("created a new instance of module B (%d) at %" PRIuPTR "\n", module_id, (uintptr_t)data);
    return data;
}

static void wait_for_hello_starting_message(module_B_instance_data *data)
{
    uint8_t b;
    if (read(data->hello_starting_pipe[0], &b, 1) < 0)
        printf("could not write to starting pipe");
}

static void notify_about_hello_starting_message(module_B_instance_data *data)
{
    uint8_t b = 123;
    if (write(data->hello_starting_pipe[1], &b, 1) < 0)
        perror("could not write to starting pipe");
}

void *module_B_thread(void *arg)
{
    mato_inc_thread_count();
    module_B_instance_data *data = (module_B_instance_data *)arg;
    wait_for_hello_starting_message(data);
    sleep(2 * (data->module_id % 5));
    for (int i = 0; i < 5; i++)
    {
        int *val = mato_get_data_buffer(sizeof(int));
	*val = data->module_id * 10 + i;
        mato_post_data(data->module_id, 0, sizeof(int), val);
        sleep(2);
    }
    printf("module_B_thread (%d) done, unsubscribing...\n", data->module_id);
    mato_unsubscribe(data->subscribed_to_module_id, 0, data->my_subscription_id);
    printf("module_B_thread terminates (%d)\n", data->module_id);

    mato_dec_thread_count();
}

void message_from_A(void *instance_data, int sender_module_id, int data_length, void *new_data_ptr)
{
    module_B_instance_data *data = (module_B_instance_data *)instance_data;
    int val = *((int *)new_data_ptr);
    printf("B(%d) receives message from A(%d): %d\n", data->module_id, sender_module_id, val);
    
    int b1 = mato_get_module_id("B1");
    if (data->module_id == b1)
    {
        int *fwd_val = mato_get_data_buffer(sizeof(int));
	*fwd_val = val + 1000;
        mato_post_data(data->module_id, 0, sizeof(int), fwd_val);
    }
}

void B_start(void *instance_data)
{
    module_B_instance_data *data = (module_B_instance_data *)instance_data;

    //b1 wants to subscribe to a2
    //b2 wants to subscribe to a1
    
    int module_id = data->module_id;
    char *my_name = mato_get_module_name(module_id);

    int a1 = mato_get_module_id("A1");
    int a2 = mato_get_module_id("A2");

    if (strcmp(my_name, "B1") == 0) 
	data->subscribed_to_module_id = a1;
    else if (strcmp(my_name, "B2") == 0)
	data->subscribed_to_module_id = a2;

    data->my_subscription_id = mato_subscribe(module_id, data->subscribed_to_module_id, 0, message_from_A, direct_data_ptr);

    pthread_t t;
    printf("starting module B(%d)..\n", data->module_id);
    if (pthread_create(&t, 0, module_B_thread, data) != 0)
          perror("could not create thread for module B");
}

void B_delete(void *instance_data)
{
    module_B_instance_data *data = (module_B_instance_data *)instance_data;
    close(data->hello_starting_pipe[0]);
    close(data->hello_starting_pipe[1]);
    printf("deleting module instance (%d) = %" PRIuPTR "\n", data->module_id, (uintptr_t)data);
    free(data);
}

void B_global_message(void *instance_data, int module_id_sender, int message_id, int msg_length, void *message_data)
{
    module_B_instance_data *B_data = ((module_B_instance_data *)instance_data);
    int module_id = B_data->module_id;
    char *data = (char *)message_data;
    if (message_id == MESSAGE_HELLO) 
    {
        printf("module %d received global HELLO messsage: '%s'\n", module_id, data);
        notify_about_hello_starting_message(B_data);
    }
}

static module_specification B_specification = { B_create_instance, B_start, B_delete, B_global_message, 1 };

void B_init()
{
    printf("initializing module B...\n");
    mato_register_new_type_of_module("B", &B_specification);
}
