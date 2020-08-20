#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>

#include "../../mato.h"
#include "AB.h"

typedef struct { 
           char type;
           int module_id;
           int my_subscription_id;
           int subscribed_to_module_id;
           int msg_queue[2];
           int hello_starting_pipe[2];
        } module_AB_instance_data;

static time_t tm0;

void *AB_create_instance(int module_id, char type)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)malloc(sizeof(module_AB_instance_data));
    data->module_id = module_id;    
    data->type = type;
    if (pipe(data->hello_starting_pipe) < 0)
        perror("could not create starting pipe");
    time(&tm);
    printf("%u created a new instance of module %c (%d) at %" PRIuPTR "\n", (unsigned int)(tm - tm0), type, module_id, (uintptr_t)data);
    return data;
}

void *A_create_instance(int module_id)
{
    return AB_create_instance(module_id, 'A');
}

void *B_create_instance(int module_id)
{
    return AB_create_instance(module_id, 'B');
}

static void wait_for_hello_starting_message(module_AB_instance_data *data)
{
    uint8_t b;
    if (read(data->hello_starting_pipe[0], &b, 1) < 0)
        printf("could not write to starting pipe");
}

static void notify_about_hello_starting_message(module_AB_instance_data *data)
{
    uint8_t b = 123;
    if (write(data->hello_starting_pipe[1], &b, 1) < 0)
        perror("could not write to starting pipe");
}

void *module_AB_thread(void *arg)
{
    time_t tm;
    mato_inc_thread_count();
    module_AB_instance_data *data = (module_AB_instance_data *)arg;
    wait_for_hello_starting_message(data);
    sleep(2 * (data->module_id % 5));
    for (int i = 0; program_runs && (i < 5); i++)
    {
        int *val = mato_get_data_buffer(sizeof(int));
        *val = data->module_id * 10 + i;
        mato_post_data(data->module_id, 0, sizeof(int), val);
        sleep(2);
    }
    time(&tm);
    printf("%u module_%c_thread done (%d), unsubscribing...\n", (unsigned int)(tm - tm0), data->type, data->module_id);
    mato_unsubscribe(data->subscribed_to_module_id, 0, data->my_subscription_id); 
    printf("%u module_%c_thread terminates (%d)\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    close(data->msg_queue[1]);
    close(data->msg_queue[0]);

    mato_dec_thread_count();
}

void *module_AB_msg_eating_thread(void *arg)
{
    time_t tm;
    mato_inc_thread_count();
    module_AB_instance_data *data = (module_AB_instance_data *)arg;
    while (program_runs)
    {
        int *val_ptr;
        int rv = read(data->msg_queue[0], &val_ptr, sizeof(void *));
        if (rv == -1) break;
        int val = *val_ptr;
        time(&tm);
        printf("%u %c(%d) retrieves message %d from queue\n", (unsigned int)(tm - tm0), data->type, data->module_id, val);
        int *fwd_val = mato_get_data_buffer(sizeof(int));
        *fwd_val = val + 100;
        sleep(data->module_id % 5);
        time(&tm);
        printf("%u %c(%d) returns borrowed ptr to message %d\n", (unsigned int)(tm - tm0), data->type, data->module_id, val);
        mato_release_data(data->subscribed_to_module_id, 0, val_ptr);

        if (data->module_id < 3)
        {
            printf("%u %c(%d) post-forwards message %d as %d\n", (unsigned int)(tm - tm0), data->type, data->module_id, val, *fwd_val);
            mato_post_data(data->module_id, 0, sizeof(int), fwd_val);
        }
    }
    time(&tm);
    printf("%u %c(%d) msg queue closed, msg eating thread terminates\n", (unsigned int)(tm - tm0), data->type, data->module_id);
    mato_dec_thread_count();
}

void message_from_other(void *instance_data, int sender_module_id, int data_length, void *new_data_ptr)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)instance_data;
    int val = *((int *)new_data_ptr);
    char other_type = 'B' - (data->type - 'A');
    time(&tm);
    printf("%u %c(%d) receives message from %c(%d): %d, pushed to queue\n", (unsigned int)(tm - tm0), data->type, data->module_id, other_type, sender_module_id, val);
    write(data->msg_queue[1], &new_data_ptr, sizeof(void *));
}

void AB_start(void *instance_data)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)instance_data;

    //a1 wants to subscribe to b2
    //a2 wants to subscribe to b1
    //b1 wants to subscribe to a1
    
    int module_id = data->module_id;
    char *my_name = mato_get_module_name(module_id);

    int a1 = mato_get_module_id("A1");
    int a2 = mato_get_module_id("A2");
    int b1 = mato_get_module_id("B1");
    int b2 = mato_get_module_id("B2");

    if (strcmp(my_name, "A1") == 0) 
	data->subscribed_to_module_id = b2;
    else if (strcmp(my_name, "A2") == 0)
	data->subscribed_to_module_id = b1;
    else if (strcmp(my_name, "B1") == 0)
	data->subscribed_to_module_id = a1;
    else data->subscribed_to_module_id = a2;

    if (pipe(data->msg_queue) < 0)
    {
        perror("could not create pipe for msg queue");
        return;
    }

    data->my_subscription_id = mato_subscribe(module_id, data->subscribed_to_module_id, 0, message_from_other, borrowed_pointer);

    pthread_t t;
    time(&tm);
    printf("%u starting module %c(%d)..\n", (unsigned int)(tm - tm0), data->type, data->module_id);
    if (pthread_create(&t, 0, module_AB_thread, data) != 0)
        perror("could not create thread for module");
    if (pthread_create(&t, 0, module_AB_msg_eating_thread, data) != 0)
        perror("could not create msg eating thread for module");
}

void AB_delete(void *instance_data)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)instance_data;
    close(data->hello_starting_pipe[0]);
    close(data->hello_starting_pipe[1]);
    time(&tm);
    printf("%u deleting module instance (%d) = %" PRIuPTR "\n", (unsigned int)(tm - tm0), data->module_id, (uintptr_t)data);
    free(data);
}

void AB_global_message(void *instance_data, int module_id_sender, int message_id, int msg_length, void *message_data)
{
    time_t tm;
    module_AB_instance_data *my_data = (module_AB_instance_data *)instance_data;
    char *data = (char *)message_data;
    time(&tm);
    if (message_id == MESSAGE_HELLO) 
    {
        printf("%u module %c(%d) received global HELLO messsage: '%s'\n", (unsigned int)(tm - tm0), my_data->type, my_data->module_id, data);
        notify_about_hello_starting_message(my_data);
    }
}

static module_specification A_specification = { A_create_instance, AB_start, AB_delete, AB_global_message, 1 };
static module_specification B_specification = { B_create_instance, AB_start, AB_delete, AB_global_message, 1 };

void A_init()
{
    time(&tm0);
    printf("initializing module type A...\n");
    mato_register_new_type_of_module("A", &A_specification);
}

void B_init()
{
    printf("initializing module type B...\n");
    mato_register_new_type_of_module("B", &B_specification);
}

