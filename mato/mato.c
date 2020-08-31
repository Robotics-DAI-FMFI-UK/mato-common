#define _GNU_SOURCE

#include "mato.h"
#include "mato_core.h"
#include "mato_net.h"

/// \file mato.c
/// Implementation of the Mato control framework: public functions.

void mato_init(int this_node_identifier)
{
    net_mato_init(this_node_identifier);
    core_mato_init();

    start_networking();
}

void mato_register_new_type_of_module(char *type, module_specification *specification)
{
    lock_framework();
        g_hash_table_insert (module_specifications, type, specification);
    unlock_framework();
}

int mato_create_new_module_instance(const char *module_type, const char *module_name)
{
    lock_framework();

        int module_id = get_free_module_id();
        char *name = malloc(strlen(module_name + 1));
        strcpy(name, module_name);
        g_array_append_val(g_array_index(module_names, GArray *, this_node_id), name);
        char *type = malloc(strlen(module_name + 1));
        strcpy(type, module_type);
        g_array_append_val(g_array_index(module_types, GArray *, this_node_id), type);

        GArray *channels_subscriptions = g_array_new(0, 0, sizeof(GArray *));
        g_array_append_val(g_array_index(subscriptions,GArray *,this_node_id), channels_subscriptions);

        GArray *module_buffers = g_array_new(0, 0, sizeof(GArray *));
        g_array_append_val(g_array_index(buffers,GArray *,this_node_id), module_buffers);

        module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
        int num_channels = spec->number_of_channels;

        for (int channel_id = 0; channel_id < num_channels; channel_id++)
        {
           GArray *subs_for_channel = g_array_new(0, 0, sizeof(GArray *));
           g_array_append_val(channels_subscriptions, subs_for_channel);

           GList *channel_buffers = 0;
           g_array_append_val(module_buffers, channel_buffers);
        }

        create_instance_callback create_instance = spec->create_instance;
    unlock_framework();

    int public_module_id = module_id + this_node_id * NODE_MULTIPLIER;
    void *module_instance_data = create_instance(public_module_id);

    lock_framework();
        g_array_append_val(instance_data, module_instance_data);
//      printf("appended instance data %" PRIuPTR "\n", (uintptr_t)module_instance_data);

        net_broadcast_new_module(module_id);
    unlock_framework();

    return public_module_id;
}

void mato_start_module(int module_id)
{
    lock_framework();
        char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
        if (module_type == 0) return;
        module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
        if (spec != 0)
        {
            void *data = g_array_index(instance_data, void *, module_id);
    unlock_framework();
            spec->start_instance(data);
        }
        else
    unlock_framework();
}

void mato_start()
{
    int n = g_array_index(module_names, GArray *, this_node_id)->len;
    for (int module_id = 0; module_id < n; module_id++)
        mato_start_module(module_id);
}

void mato_delete_module_instance(int module_id)
{
    int node_id=module_id / NODE_MULTIPLIER;
    module_id %= NODE_MULTIPLIER;
    if (node_id != this_node_id) return;

    lock_framework();

//    printf("%d: instance_data->len=%d\n", module_id, instance_data->len);
        char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
        char *module_name = g_array_index(g_array_index(module_names, GArray *, this_node_id), char *, module_id);
        void *data = g_array_index(instance_data, void *, module_id);

        module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
    unlock_framework();

    spec->delete_instance(data);
    lock_framework();

        GArray *channels_subscriptions = g_array_index(g_array_index(subscriptions, GArray *, this_node_id), GArray *, module_id);
        int number_of_channels = channels_subscriptions->len;

        for (int i = 0; i < number_of_channels; i++)
        {
            GArray *subscriptions_in_channel = g_array_index(channels_subscriptions, GArray *, i);
            int number_of_subscriptions_in_this_channel = subscriptions_in_channel->len;
            for (int j = 0; j < number_of_subscriptions_in_this_channel; j++)
            {
                subscription *s = g_array_index(subscriptions_in_channel, subscription *, j);
                free(s);
            }
            g_array_free(g_array_index(channels_subscriptions, GArray *, i), 1);
        }

        g_array_free(g_array_index(g_array_index(subscriptions, GArray *, this_node_id), GArray *, module_id), 1);
        void *zero = 0;
        g_array_index(g_array_index(subscriptions, GArray *, this_node_id), subscription *, module_id) = 0;
        g_array_index(g_array_index(module_names, GArray *, this_node_id), char *, module_id) = 0;
        free(module_name);
        g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id) = 0;
        free(module_type);
        g_array_index(instance_data, void *, module_id) = 0;

        net_send_delete_module(module_id);

    unlock_framework();
}

int mato_get_module_id(const char *module_name)
{
    for(int node_id = 0; node_id < nodes->len; node_id++)
    {
        int module_count = g_array_index(module_names, GArray *, node_id)->len;
        for (int i = 0; i < module_count; i++)
        {
            char *name = g_array_index(g_array_index(module_names, GArray *, node_id), char *, i);
            if ((name != 0) && (strcmp(name, module_name) == 0))
                return i + node_id * NODE_MULTIPLIER;
        }
    }
    return -1;
}

char *mato_get_module_name(int module_id)
{
    int node_id = module_id / NODE_MULTIPLIER;
    module_id %= NODE_MULTIPLIER;
    return g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id);
}

char *mato_get_module_type(int module_id)
{
    int node_id = module_id / NODE_MULTIPLIER;
    module_id %= NODE_MULTIPLIER;
    return g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id);
}

int mato_subscribe(int subscriber_module_id, int subscribed_module_id, int channel, subscriber_callback callback, int subscription_type)
{
    int subscriber_node_id = subscriber_module_id / NODE_MULTIPLIER;
    subscriber_module_id %= NODE_MULTIPLIER;
    if (subscriber_node_id != this_node_id)
        return -1;
    int subscribed_node_id = subscribed_module_id / NODE_MULTIPLIER;
    subscribed_module_id %= NODE_MULTIPLIER;
    subscription *new_subscription = (subscription *)malloc(sizeof(subscription));
    new_subscription->type = subscription_type;
    new_subscription->callback = callback;
    new_subscription->subscriber_module_id = subscriber_module_id;
    new_subscription->subscriber_node_id = subscriber_node_id;
    lock_framework();
        new_subscription->subscription_id = get_free_subscription_id();
        GArray *channel_subscriptions = g_array_index(g_array_index(g_array_index(subscriptions, GArray *, subscribed_node_id), GArray *, subscribed_module_id), GArray *, channel);
        g_array_append_val(channel_subscriptions, new_subscription);
        if ((channel_subscriptions->len == 1) && (subscribed_node_id != this_node_id))
            net_send_subscribe(subscribed_node_id, subscribed_module_id, channel);
    unlock_framework();
    return new_subscription->subscription_id;
}

void mato_unsubscribe(int module_id, int channel, int subscription_id)
{
    int subscribed_node_id = module_id / NODE_MULTIPLIER;
    module_id %= NODE_MULTIPLIER;

    lock_framework();
        remove_subscription(subscribed_node_id, module_id, channel, subscription_id);
    unlock_framework();
}

void *mato_get_data_buffer(int size)
{
    return malloc(size);
}

void mato_post_data(int id_of_posting_module, int channel, int data_length, void *data)
{
    int node_id = id_of_posting_module / NODE_MULTIPLIER;
    id_of_posting_module %= NODE_MULTIPLIER;

    //writing data of size <= PIPE_BUF to pipe are atomic, therefore no framework locking is needed
    channel_data *cd = new_channel_data(node_id, id_of_posting_module, channel, data_length, data);
//    printf("%d sending channel data to pipe: %" PRIuPTR "\n", id_of_posting_module, (uintptr_t)cd);

    write(post_data_pipe[1], &cd, sizeof(channel_data *));
}

int mato_send_global_message(int module_id_sender, int message_id, int msg_length, void *message_data)
{
    int local_module_id_sender = module_id_sender % NODE_MULTIPLIER;
    int sending_node_id = module_id_sender / NODE_MULTIPLIER;

    // a message from this node is broadcasted to other nodes
    if (sending_node_id == this_node_id)
        net_send_global_message(module_id_sender, message_id, (uint8_t *)message_data, msg_length);

    // all messages are delivered to all our modules
    int our_modules_count = g_array_index(module_names, GArray *, this_node_id)->len;
    for (int module_id = 0; module_id < our_modules_count; module_id++)
    {
        char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
        if (module_type != 0)
        {
            if (module_id + this_node_id * NODE_MULTIPLIER != module_id_sender)  // not delivering to the msg. originator
            {
                module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
                if (spec != 0)
                {
                    void *modules_instance_data = g_array_index(instance_data, void *, module_id);
                    if (modules_instance_data != 0)
                        spec->global_message(modules_instance_data, module_id_sender, message_id, msg_length, message_data);
                }
            }
        }
    }
}

void mato_get_data(int id_module, int channel, int *data_length, void **data)
{
    lock_framework();
        int node_id = id_module / NODE_MULTIPLIER;
        id_module %= NODE_MULTIPLIER;
        if (node_id == this_node_id)
            copy_of_last_data_of_channel(node_id, id_module, channel, data_length, (uint8_t **)data);
        else
        {
            GArray *node_subscriptions = g_array_index(subscriptions, GArray *, node_id);
            GArray *module_subscriptions = g_array_index(node_subscriptions, GArray *, id_module);
            GArray *channel_subscriptions = g_array_index(module_subscriptions, GArray *, channel);
            if (channel_subscriptions->len > 0)
            {  // there is at least 1 subscription on that channel from us
                copy_of_last_data_of_channel(node_id, id_module, channel, data_length, (uint8_t **)data);
            }
            else
            {  // otherwise request the data from another node
                int fd[2];
                pipe(fd);
                net_send_get_data(node_id, id_module, channel, fd[1]);
                if (read(fd[0], data_length, sizeof(int32_t)) < 0)
                    perror("could not retrieve data size from pipe");
                else if (read(fd[0], data, sizeof(uint8_t *)) < 0)
                    perror("could not retrieve data size from pipe");
                close(fd[0]);
                close(fd[1]);
            }
        }
    unlock_framework();
    return;
}

void mato_borrow_data(int id_module, int channel, int *data_length, void **data)
{
    lock_framework();
        int node_id = id_module / NODE_MULTIPLIER;
        id_module %= NODE_MULTIPLIER;
        if (node_id == this_node_id)
            borrow_last_data_of_channel(node_id, id_module, channel, data_length, (uint8_t **)data);
        else
        {
            GArray *node_subscriptions = g_array_index(subscriptions, GArray *, node_id);
            GArray *module_subscriptions = g_array_index(node_subscriptions, GArray *, id_module);
            GArray *channel_subscriptions = g_array_index(module_subscriptions, GArray *, channel);
            if (channel_subscriptions->len > 0)
            {  // there is at least 1 subscription on that channel from us
                borrow_last_data_of_channel(node_id, id_module, channel, data_length, (uint8_t **)data);
            }
            else
            {  // otherwise request the data from another node, store it to buffers and return borrowed pointer
                int fd[2];
                pipe(fd);
    unlock_framework();
                net_send_get_data(node_id, id_module, channel, fd[1]);
                if (read(fd[0], data_length, sizeof(int32_t)) < 0)
                    perror("could not retrieve data size from pipe");
                else if (read(fd[0], data, sizeof(uint8_t *)) < 0)
                    perror("could not retrieve data size from pipe");
                close(fd[0]);
                close(fd[1]);
                if (*data)
                {
                    channel_data *cd = new_channel_data(node_id, id_module, channel, *data_length, *data);
                    cd->references++;
        lock_framework();
                    GArray *module_buffers = g_array_index(g_array_index(buffers,GArray *, node_id), GArray *, id_module);
                    GList *channel_list = g_array_index(module_buffers, GList *, channel);
                    channel_list = g_list_prepend(channel_list, cd);
                    g_array_index(module_buffers, GList *, channel) = channel_list;
                }
            }
        }
    unlock_framework();
    return;
}

void mato_release_data(int id_module, int channel, void *data)
{
    lock_framework();
        int node_id = id_module / NODE_MULTIPLIER;
        id_module %= NODE_MULTIPLIER;

        GArray *buffers_for_module = g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, id_module);
        GList *waiting_buffers = g_array_index(buffers_for_module, GList *, channel);
        if (waiting_buffers == 0)
        {
    unlock_framework();
            return;
        }
        GList *lookup = waiting_buffers;
        while (lookup)
        {
            channel_data *buffer = (channel_data *)waiting_buffers->data;
            if (buffer->data == data)
            {
                lookup = decrement_references(waiting_buffers, buffer);
                if (lookup != waiting_buffers)
                    g_array_index(buffers_for_module, GList *, channel) = lookup;
                break;
            }
            lookup = lookup->next;
        }
    unlock_framework();
}

int mato_get_number_of_modules()
{
    int count = 0;
    for (int node_id = 0; node_id < nodes->len; node_id++)
      count += g_array_index(module_names, GArray *, node_id)->len;
    return count;
}

GArray* mato_get_list_of_all_modules()
{
    return mato_get_list_of_modules(0);
}

GArray* mato_get_list_of_modules(char *type)
{
    lock_framework();
        GArray *modules = g_array_new(0, 0, sizeof(module_info *));
        for (int node_id = 0; node_id < nodes->len; node_id++)
        {
            int module_count = g_array_index(module_names, GArray *, node_id)->len;
            for (int module_id = 0; module_id < module_count; module_id++)
            {
                char *module_name = g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id);
                if (module_name == 0) continue;
                char *module_type = g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id);
                if ((type == 0) || (strcmp(module_type, type) == 0))
                {
                    module_info *info = new_module_info(node_id, module_id + node_id * NODE_MULTIPLIER, module_name, module_type);
                    g_array_append_val(modules, info);
                }
            }
        }
    unlock_framework();
    return modules;
}

void mato_free_list_of_modules(GArray* a)
{
    int n = a->len;
    for (int i = 0; i < n; i++)
    {
        module_info *info = g_array_index(a, module_info *, i);
        free(info);
    }
    g_array_free(a, 1);
}

void mato_data_buffer_usage(int module_id, int channel, int *number_of_allocated_buffers, int *total_sum_of_ref_count)
{
    *number_of_allocated_buffers = 0;
    *total_sum_of_ref_count = 0;
    lock_framework();
        int node_id = module_id / NODE_MULTIPLIER;
        module_id %= NODE_MULTIPLIER;

        if(node_id != this_node_id)
            return;

        GArray *module_buffers = (GArray *)g_array_index(g_array_index(buffers, GArray *,this_node_id), GArray *, module_id);
        GList *channel_buffers = (GList *)g_array_index(module_buffers, GList *, channel);
        while (channel_buffers != 0)
        {
            (*number_of_allocated_buffers)++;
            (*total_sum_of_ref_count) += ((channel_data *)(channel_buffers->data))->references;
            channel_buffers = channel_buffers->next;
        }
    unlock_framework();
}

void mato_inc_thread_count()
{
    lock_framework();
        threads_started++;
    unlock_framework();
}

void mato_dec_thread_count()
{
    lock_framework();
        threads_started--;
    unlock_framework();
}

int mato_threads_running()
{
    return threads_started;
}

void mato_shutdown()
{
    net_mato_shutdown();
}

int mato_main_program_module_id()
{
    return this_node_id * NODE_MULTIPLIER + MATO_MAIN_PROGRAM_MODULE;
}

