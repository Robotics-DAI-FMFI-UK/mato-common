#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>

#include "../../mato.h"
#include "AB.h"

#define NUMBER_OF_HELLOs_TO_WAIT_FOR 2
#define NUMBER_OF_DONEs_TO_WAIT_FOR (2 * 4)

typedef struct {
            char type;
            int module_id;
            int msg_queue[2];
            int starting_pipe[2];
            int hello_count;
            int done_count;
            int sending_msgs_to;
            pthread_mutex_t lock;
        } module_AB_instance_data;

static time_t tm0;

void *AB_create_instance(int module_id, char type)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)malloc(sizeof(module_AB_instance_data));
    data->module_id = module_id;
    data->type = type;
    data->hello_count = 0;
    data->done_count = 0;
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
    mato_inc_thread_count();
    module_AB_instance_data *data = (module_AB_instance_data *)arg;

    printf("%u module_%c_thread (%d) enters barrier...\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    // all modules barrier
    wait_for_hello_messages(data);

    printf("%u module_%c_thread (%d) leaves barrier...\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    sleep(1 + 2 * (data->module_id % 5));
    int val = 2; // send out first prime
    mato_send_message(data->module_id, data->sending_msgs_to, MESSAGE_PRIME, sizeof(int), &val);

    time(&tm);
    printf("%u module_%c_thread terminates (%d)\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    mato_dec_thread_count();
}

int is_prime(int x)
{
    int y = (int)sqrt(x);
    for (int i = 2; i <= y; i++)
        if (x % i == 0) return 0;
    return 1;
}

void check_job_done(module_AB_instance_data *my_data)
{
    if (my_data->done_count == NUMBER_OF_DONEs_TO_WAIT_FOR)
    {
        time_t tm;
        time(&tm);
        printf("%u %c(%d) all reached end of list, terminating my thread\n", (unsigned int)(tm - tm0), my_data->type, my_data->module_id);
        close(my_data->msg_queue[1]);
        close(my_data->msg_queue[0]);
    }
}

void *module_AB_msg_eating_thread(void *arg)
{
    time_t tm;
    mato_inc_thread_count();
    module_AB_instance_data *data = (module_AB_instance_data *)arg;
    int val2 = 1;
    while (program_runs)
    {
        int val;
        int rv = read(data->msg_queue[0], &val, sizeof(int));
        if (rv <= 0) break;
        time(&tm);
        printf("%u %c(%d) retrieves message %d from queue\n", (unsigned int)(tm - tm0), data->type, data->module_id, val);

        val2 = 0;
        for (int pr = val + 1; pr < 100; pr++)
           if (is_prime(pr)) 
           {
              val2 = pr;
              break;
           }
        if (val2)
        {
            usleep(100000 * (data->module_id % 7));
            printf("%u %c(%d) forwards message %d as %d\n", (unsigned int)(tm - tm0), data->type, data->module_id, val, val2);
            mato_send_message(data->module_id, data->sending_msgs_to, MESSAGE_PRIME, sizeof(int), &val2);
        }
        else
        {
            printf("%u %c(%d) announces it has reached the end of prime list\n", (unsigned int)(tm - tm0), data->type, data->module_id);
            mato_send_global_message(data->module_id, MESSAGE_DONE, 0, 0);
            AB_lock(data);
                data->done_count++;
                check_job_done(data);
            AB_unlock(data);
        }
    }
    time(&tm);
    printf("%u %c(%d) all primes generated, msg eating thread terminates\n", (unsigned int)(tm - tm0), data->type, data->module_id);

    mato_dec_thread_count();
}

void AB_start(void *instance_data)
{
    time_t tm;
    module_AB_instance_data *data = (module_AB_instance_data *)instance_data;
    int module_ids[2][2][2];  // [node_id][A=0/B=1][1-2]
    static char module_name[6];

    // there 8 nodes alltogether, forwarding messages as follows:
    // node 0    node 1
    // A1->A2    A1->A1
    // A2->B1    A2->A2
    // B1->B2    B1->B1
    // B2->A1    B2->B2

    int module_id = data->module_id;
    char *my_name = mato_get_module_name(module_id);

    for (int node_id = 0; node_id < 2; node_id++)
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

    if (my_node == 1)
      data->sending_msgs_to = module_ids[(my_node + 1) % 2][my_type - 'A'][my_ord];
    else if (my_ord == 0)
      data->sending_msgs_to = module_ids[(my_node + 1) % 2][my_type - 'A'][1];
    else
      data->sending_msgs_to = module_ids[(my_node + 1) % 2]['B' - my_type][0];

    if (pipe(data->msg_queue) < 0)
    {
        perror("could not create pipe for msg queue");
        return;
    }
    printf("module %s(%d) sending messages to module (%d)...\n", my_name, module_id, data->sending_msgs_to);
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
    else if (message_id == MESSAGE_PRIME)
    {
        time_t tm;
        int val = *((int *)message_data);
        time(&tm);
        printf("%u %c(%d) receives message from (%d): %d, pushed to queue\n", (unsigned int)(tm - tm0), my_data->type, my_data->module_id, module_id_sender, val);
        write(my_data->msg_queue[1], &val, sizeof(int));
    }
    else if (message_id == MESSAGE_DONE)
    {
        AB_lock(my_data);
            my_data->done_count++;
            printf("%u %c(%d) receives end of list announcment from (%d), count: %d\n", (unsigned int)(tm - tm0), my_data->type, my_data->module_id, module_id_sender, my_data->done_count);
            check_job_done(my_data);
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

