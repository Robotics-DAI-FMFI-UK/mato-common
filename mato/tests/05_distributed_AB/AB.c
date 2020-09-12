#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>

#include "../../mato.h"
#include "AB.h"

#define NUMBER_OF_HELLOs_TO_WAIT_FOR 3

typedef struct {
            char type;
            int module_id;
            int my_subscription_id;
            int subscribed_to_module_id;
            int msg_queue[2];
            int starting_pipe[2];
            int hello_count;
            pthread_mutex_t lock;
            int forwarder;
        } module_AB_instance_data;

static time_t tm0;

void *AB_create_instance(int module_id, char type)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)malloc(sizeof(module_AB_instance_data));
    data->module_id = module_id;
    data->type = type;
    data->hello_count = 0;
    data->forwarder = 1;
    if (pipe(data->starting_pipe) < 0)
        perror("could not create starting pipe");
    pthread_mutex_init(&data->lock, 0);
    time(&tm);
    printf("%u created a new instance of module %c (%d) at %" PRIuPTR "\n", (unsigned int)(tm - tm0), type, module_id, (uintptr_t)data);
    return data;
}

void AB_lock(module_AB_instance_data *data)
{
    pthread_mutex_lock(&data->lock);
}

void AB_unlock(module_AB_instance_data *data)
{
    pthread_mutex_unlock(&data->lock);
}

void *A_create_instance(int module_id)
{
    return AB_create_instance(module_id, 'A');
}

void *B_create_instance(int module_id)
{
    return AB_create_instance(module_id, 'B');
}

static void wait_for_hello_messages(module_AB_instance_data *data)
{
    uint8_t b;
    if (read(data->starting_pipe[0], &b, 1) < 0)
        printf("could not write to starting pipe");
}

static void notify_about_hello_messages(module_AB_instance_data *data)
{
    uint8_t b = 123;
    if (write(data->starting_pipe[1], &b, 1) < 0)
        perror("could not write to starting pipe");
}

void *module_AB_thread(void *arg)
{
    time_t tm;
    time(&tm);

    char myname[13];
    module_AB_instance_data *data = (module_AB_instance_data *)arg;
    sprintf(myname, "%c%d", data->type, data->module_id);
    mato_inc_thread_count(myname);

    printf("%u module_%c_thread (%d) enters barrier...\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    // all modules barrier
    wait_for_hello_messages(data);

    printf("%u module_%c_thread (%d) leaves barrier...\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    sleep(1 + 2 * (data->module_id % 5));
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
    return 0;
}

void *module_AB_msg_eating_thread(void *arg)
{
    time_t tm;

    char myname[13];
    module_AB_instance_data *data = (module_AB_instance_data *)arg;
    sprintf(myname, "%c%d_eat", data->type, data->module_id);
    mato_inc_thread_count(myname);

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

        if (data->forwarder)
        {
            printf("%u %c(%d) post-forwards message %d as %d\n", (unsigned int)(tm - tm0), data->type, data->module_id, val, *fwd_val);
            mato_post_data(data->module_id, 0, sizeof(int), fwd_val);
        }
    }
    time(&tm);
    printf("%u %c(%d) msg queue closed, msg eating thread terminates\n", (unsigned int)(tm - tm0), data->type, data->module_id);
    mato_dec_thread_count();
    return 0;
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
    int module_ids[3][2][2];  // [node_id][A=0/B=1][1-2]
    static char module_name[6];

    // nx_yz wants to subscribe to n((x + 1) % 3 )_('A' + 'B' - y)(1 - z)
    // this gives 2 loops with 6 modules, always hoping to another node

    int module_id = data->module_id;
    char *my_name = mato_get_module_name(module_id);

    for (int node_id = 0; node_id < 3; node_id++)
        for (char type = 'A'; type <= 'B'; type++)
            for (int ord = 0; ord < 2; ord++)
            {
               sprintf(module_name, "n%d_%c%d", node_id, type, ord);
               module_ids[node_id][type - 'A'][ord] = mato_get_module_id(module_name);
               printf("# %s is %d\n", module_name, module_ids[node_id][type - 'A'][ord]);
            }
    int my_node = my_name[1] - '0';
    char my_type = my_name[3];
    int my_ord = my_name[4] - '0';

    data->subscribed_to_module_id = module_ids[(my_node + 1) % 3][('A' + 'B' - my_type) - 'A'][1 - my_ord];

    // n2_B{01} is not forwarding received messages further
    if ((my_node == 2) && (my_type == 'B')) data->forwarder = 0;

    if (pipe(data->msg_queue) < 0)
    {
        perror("could not create pipe for msg queue");
        return;
    }

    printf("module %s(%d) subscribing to module (%d)...\n", my_name, module_id, data->subscribed_to_module_id);
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
    time(&tm);
    close(data->starting_pipe[0]);
    close(data->starting_pipe[1]);
    pthread_mutex_destroy(&data->lock);
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
        printf("%u module %c(%d) received global HELLO messsage: '%s' from %d\n", (unsigned int)(tm - tm0), my_data->type, my_data->module_id, data, module_id_sender);
        AB_lock(my_data);
            my_data->hello_count++;
            if (my_data->hello_count == NUMBER_OF_HELLOs_TO_WAIT_FOR)
                notify_about_hello_messages(my_data);
        AB_unlock(my_data);
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

