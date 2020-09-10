#define _GNU_SOURCE

#include "mato.h"
#include "mato_net.h"
#include "mato_core.h"

/// \file mato_core.c
/// Implementation of the Mato control framework - internal data structures and algorithms.

volatile int program_runs;
volatile int threads_started;
volatile int system_threads_started;

//--- declared and commented in mato_core.h
GArray *module_names;
GArray *module_types;
GArray *instance_data;
GHashTable *module_specifications;
GArray *buffers;
GList *dangling_channel_data;
GArray *subscriptions;
int post_data_pipe[2];
//---

/// A counter for assigning a new module_id for newly created module instances.
static int next_free_module_id;

/// A counter for assining a new subscription_id for newly registered subscriptions.
static int next_free_subscription_id;

/// Used for mutual exclusion when accessing framework structures from functions that can be called from different threads.
static pthread_mutex_t framework_mutex;

void core_mato_shutdown()
{
    close(post_data_pipe[1]);
    while (mato_system_threads_running() > 0) { usleep(10000); }
    close(post_data_pipe[0]);
    pthread_mutex_destroy(&framework_mutex);
}

void lock_framework()
{
    pthread_mutex_lock(&framework_mutex);
}

void unlock_framework()
{
    pthread_mutex_unlock(&framework_mutex);
}

void mato_inc_system_thread_count()
{
    lock_framework();
        system_threads_started++;
    unlock_framework();
}

void mato_dec_system_thread_count()
{
    lock_framework();
        system_threads_started--;
    unlock_framework();
}

int mato_system_threads_running()
{
    return system_threads_started;
}

GList *decrement_references(GList *data_buffers, channel_data *to_be_decremented)
{
    to_be_decremented->references--;
    if (to_be_decremented->references == 0)
    {
        free(to_be_decremented->data);
        data_buffers = g_list_remove(data_buffers, to_be_decremented);
        free(to_be_decremented);
    }
    return data_buffers;
}

/// The main loop of the framework thread that takes care of redistributing all the messages posted by the modules.
static void *mato_core_thread(void *arg)
{
    channel_data *cd;
    mato_inc_system_thread_count();
    while (program_runs)
    {
        int retval = read(post_data_pipe[0], &cd, sizeof(channel_data *));
        if (retval < 0)
        {
          perror("error reading from message pipe");
          return 0;
        }
        //      printf("retrieved channel data from pipe: %" PRIuPTR "\n", (uintptr_t)cd);

        if (retval == 0) // pipe write has closed, framework terminates
          break;

        cd->references++; // last valid data from module channel
        cd->references++; // currently being sent out to subscribers

        lock_framework();
            if(g_array_index(nodes,node_info*,cd->node_id)->is_online == 0)
            {
                free(cd->data);
                free(cd);
        unlock_framework();
                continue;
            }

            char *check_module_exists = g_array_index(g_array_index(module_names, GArray *,cd->node_id), char *, cd->module_id);
            if (check_module_exists == 0)
            {
                free(cd->data);
                free(cd);
        unlock_framework();
                continue;
            }

            GArray *module_buffers = g_array_index(g_array_index(buffers,GArray *,cd->node_id), GArray *, cd->module_id);
            GList *channel_list = g_array_index(module_buffers, GList *, cd->channel_id);

            if (channel_list)
                // previous last valid data is not last valid data anymore => ref--
                channel_list = decrement_references(channel_list, (channel_data *)(channel_list->data));
            channel_list = g_list_prepend(channel_list, cd);

            g_array_index(module_buffers, GList *, cd->channel_id) = channel_list;

            GArray *subscriptions_for_channel = g_array_index(g_array_index(g_array_index(subscriptions,GArray *,cd->node_id), GArray *, cd->module_id), GArray *, cd->channel_id);
            int n = subscriptions_for_channel->len;

            // extract the list of all subscriptions to call before unlocking framework
            // as they could change under our hands meanwhile
            // and then before calling each, check that the subscription is still valid

            GList *list_of_subscriptions_to_use = 0;
            for (int i = 0; i < n; i++)
            {
                subscription *sub = g_array_index(subscriptions_for_channel, subscription *, i);
                int *sub_id = (int *)malloc(sizeof(int));
                *sub_id = sub->subscription_id;
                list_of_subscriptions_to_use = g_list_append(list_of_subscriptions_to_use, sub_id);
            }

            GList *subscriber = list_of_subscriptions_to_use;
            while (subscriber != 0)
            {
                int sub_id = *((int *)(subscriber->data));

                subscription *sub = 0;
                for (int i = 0; i < n; i++)
                {
                    subscription *try_sub = g_array_index(subscriptions_for_channel, subscription *, i);
                    if (try_sub->subscription_id == sub_id)
                    {
                        sub = try_sub;
                        break;
                    }
                }
                if (sub == 0)  // subscription removed meanwhile, skip
                {
                    subscriber = subscriber->next;
                    continue;
                }
                if(sub->subscriber_node_id==this_node_id)
                {
                    void *subscriber_instance_data = g_array_index(instance_data, void *, sub->subscriber_module_id);
                    if (sub->type == direct_data_ptr)
                    {
                        unlock_framework();
                            sub->callback(subscriber_instance_data, cd->module_id, cd->length, cd->data);
                        lock_framework();
                    }
                    else if (sub->type == data_copy)
                    {
                        void *copy_of_data = malloc(cd->length);
                        memcpy(copy_of_data, cd->data, cd->length);
                        unlock_framework();
                            sub->callback(subscriber_instance_data, cd->module_id, cd->length, copy_of_data);
                        lock_framework();
                    }
                    else if (sub->type == borrowed_pointer)
                    {
                        cd->references++;
                        unlock_framework();
                            sub->callback(subscriber_instance_data, cd->module_id, cd->length, cd->data);
                        lock_framework();
                    }
                }
                else
                {
                    unlock_framework();
                        net_send_subscribed_data(sub->subscriber_node_id, cd);
                    lock_framework();
                }
                free(subscriber->data);
                subscriber = subscriber->next;
            }
            g_list_free(list_of_subscriptions_to_use);

            channel_list = decrement_references(channel_list, cd);  // done with this channel data, ref--
            // no need to check for channel_list changed, since it is still the last valid, ie. refcount > 0
        unlock_framework();
    }
    mato_dec_system_thread_count();
}

/// Essentially removes traces about names and types of all modules of a particular node that has just disconnected.
/// The outcome is that the names[] and types[] arrays of that node will be empty, and the names and types strings are deallocated.
void remove_names_types(int node_id)
{
    GArray* names = g_array_index(module_names, GArray*, node_id);
    GArray* types = g_array_index(module_types, GArray*, node_id);
    int module_number = names->len;
    for(int module = 0; module < module_number; module++)
    {
        char* name = g_array_index(names, char*, 0);
        char* type = g_array_index(types, char*, 0);
        g_array_remove_index_fast(names, 0);
        g_array_remove_index_fast(types, 0);
        if (name) free(name);
        if (type) free(type);
    }
}

/// Removes all subscriptions to all channels of the specified module of some node, because
/// the module is not valid anymore, so all the subscriptions of our local modules to that module channels
/// should silently disappear.
static void remove_subscriptions_to_module_channels(int node_id, int module, GArray *node_subscriptions)
{
    GArray *module_subscriptions = g_array_index(node_subscriptions, GArray *, module);
    if (module_subscriptions == 0) return;  // no subscriptions array => module have been deleted already
    int channels_number = module_subscriptions->len;
    for (int channel = 0; channel < channels_number; channel++)
    {
        GArray *channel_subscriptions = g_array_index(module_subscriptions, GArray *, channel);
        int subscription_count = channel_subscriptions->len;
        for (int sub = 0; sub < subscription_count; sub++)
        {
            subscription *s = g_array_index(channel_subscriptions, subscription *, sub);
            g_array_index(channel_subscriptions, subscription *, sub) = 0;
            free(s);
        }
        g_array_free(channel_subscriptions, 1);
        g_array_index(module_subscriptions, GArray *, channel) = 0;
    }
    g_array_free(module_subscriptions, 1);
    g_array_index(node_subscriptions, GArray *, module) = 0;
}

/// Removes all subscriptions to channels of modules of the specified remote node - because it has just been disconnected.
/// Also removes all subscriptions of modules of that remote node to channels of local modules
void remove_node_from_subscriptions(int node_id)
{
    // First part: remove all subscriptions to channels of modules of the specified node
    GArray* node_subscriptions = g_array_index(subscriptions, GArray *, node_id);
    int modules_number = node_subscriptions->len;
    for (int module=0; module < modules_number; module++)
        remove_subscriptions_to_module_channels(node_id, module, node_subscriptions);

    g_array_remove_range(node_subscriptions, 0, modules_number);

    // Second part: remove all subscriptions of the remote node to any local channels
    node_subscriptions = g_array_index(subscriptions, GArray *, this_node_id);
    modules_number = node_subscriptions->len;
    for (int module=0; module < modules_number; module++)
    {
        GArray *module_subscriptions = g_array_index(node_subscriptions, GArray *, module);
        int channels_number = module_subscriptions->len;
        for (int channel = 0; channel < channels_number; channel++)
        {
            GArray *channel_subscriptions = g_array_index(module_subscriptions, GArray *, channel);
            int subscription_count = channel_subscriptions->len;
            for (int sub = 0; sub < subscription_count; sub++)
            {
                subscription *s = g_array_index(channel_subscriptions, subscription *, sub);
                if (s->subscriber_node_id == node_id)
                {
                    g_array_remove_index_fast(channel_subscriptions, sub);
                    free(s);
                }
            }
        }
    }
}

/// Pass through all the buffers of the specified module of some remote node, and if any of our local modules
/// is subscribed to the channel, decrement the reference count for the leading (latest) message,
/// because the module is not valid anymore, so we will not be providing a most recent message from
/// this time on.
static void decrement_references_of_the_last_channel_messages_for_module(int node_id, int module_id)
{
    GArray *module_channels_subscriptions = g_array_index(g_array_index(subscriptions, GArray *, node_id), GArray *, module_id);
    if (module_channels_subscriptions == 0) // no channels => node must have been removed already
        return;
    for (int channel = 0; channel < module_channels_subscriptions->len; channel++)
    {
        if ((node_id == this_node_id) || (g_array_index(module_channels_subscriptions, GArray *, channel)->len > 0))
        {
            GArray *channels = g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id);
            GList *bufs = g_array_index(channels, GList *, channel);
            if (bufs)
                bufs = decrement_references(bufs, (channel_data *)bufs->data);
            g_array_index(channels, GList *, channel) = bufs;
        }
    }
}

/// Takes all messages that are still existing in the buffers (i.e. some our module[s] must have borrowed their reference)
/// and moves them to dangling_channel_data list for later removal, the buffers of this module will be completely cleared and freed from memory.
static void move_remaining_channel_data_to_dangling(int node_id, int module_id)
{
    GArray *module_channels_subscriptions = g_array_index(g_array_index(subscriptions, GArray *, node_id), GArray *, module_id);
    if (module_channels_subscriptions == 0) // no channels => node must have been removed already
        return;
    for (int channel = 0; channel < module_channels_subscriptions->len; channel++)
    {
        GList *bufs = g_array_index(g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id), GList *, channel);
        while (bufs)
        {
            dangling_channel_data = g_list_prepend(dangling_channel_data, bufs->data);
            bufs = bufs->next;
        }
        g_list_free(bufs);
    }
    g_array_free(g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id), 1);
    g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id) = 0;
}

/// Removes all channel_data that are not used by some local module anymore. Those that are should
/// be moved to dangling channel data structure for later removal. The buffers must remain clean
/// i.e. the buffers[node_id] should be an empty array, and everything else should have been deallocated.
/// For those channels that are subscribed from us: the leading message can be decremented, if present.
/// Other channel_data in buffers are not touched, they will disappear eventually when modules stop using them.
void remove_node_buffers(int node_id)
{
    int supr_module_id = g_array_index(buffers, GArray*, node_id)->len;
    // first pass: decrement references: no more last received data (-1 on the first item in the list)
    // for the locally subscribed channels
    for(int module_id = 0; module_id < supr_module_id; module_id++)
        decrement_references_of_the_last_channel_messages_for_module(node_id, module_id);

    // second pass: everything that remains go to dangling channel data
    for(int module_id = 0; module_id < supr_module_id; module_id++)
        move_remaining_channel_data_to_dangling(node_id, module_id);

    g_array_remove_range(g_array_index(buffers, GArray *, node_id), 0, supr_module_id);
}

/// Deallocate and remove the name and type of a specified module from framework records.
static void free_name_and_type(int node_id, int module_id)
{
    char *module_type = g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id);
    char *module_name = g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id);
    g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id) = 0;
    free(module_name);
    g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id) = 0;
    free(module_type);
}

void delete_module_instance(int node_id, int module_id)
{
    decrement_references_of_the_last_channel_messages_for_module(node_id, module_id);
    move_remaining_channel_data_to_dangling(node_id, module_id);
    GArray* node_subscriptions = g_array_index(subscriptions, GArray *, node_id);
    remove_subscriptions_to_module_channels(node_id, module_id, node_subscriptions);
    free_name_and_type(node_id, module_id);
}

/// Another node has just announced its new module, update the structures: store the name, type, create and append
/// the arrays for buffers and subscriptions.
void store_new_remote_module(int node_id, int module_id, char *module_name, char *module_type, int number_of_channels)
{
    lock_framework();
        GArray *node_modules_names = g_array_index(module_names, GArray *, node_id);
        while (module_id >= node_modules_names->len)
        {
            GArray *zero = 0;
            g_array_append_val(node_modules_names, zero);
            g_array_append_val(g_array_index(module_types, GArray *, node_id), zero);
            g_array_append_val(g_array_index(subscriptions, GArray *, node_id), zero);
            g_array_append_val(g_array_index(buffers, GArray *, node_id), zero);
        }
        g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id) = module_name;
        g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id) = module_type;

        GArray *channels_subscriptions = g_array_new(0, 0, sizeof(GArray *));
        g_array_index(g_array_index(subscriptions, GArray *, node_id), GArray *, module_id) = channels_subscriptions;

        GArray *module_buffers = g_array_new(0, 0, sizeof(GList *));
        g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id) = module_buffers;

        for (int channel_id = 0; channel_id < number_of_channels; channel_id++)
        {
           GArray *subs_for_channel = g_array_new(0, 0, sizeof(GArray *));
           g_array_append_val(channels_subscriptions, subs_for_channel);

           GList *channel_buffers = 0;
           g_array_append_val(module_buffers, channel_buffers);
        }
    unlock_framework();

    printf("got info about new module %d from node %d (%s|%s) with %d channels\n", module_id, node_id, module_name, module_type, number_of_channels);
}

int get_free_module_id() // is not thread-safe
{
    return next_free_module_id++;
}

int get_free_subscription_id() // is not thread-safe
{
    return next_free_subscription_id++;
}

void remove_subscription(int subscribed_node_id, int subscribed_module_id, int channel, int subscription_id)
{
    GArray *subscriptions_for_channel = g_array_index(g_array_index(g_array_index(subscriptions, GArray *,subscribed_node_id), GArray *, subscribed_module_id), GArray *, channel);
    int number_of_channel_subscriptions = subscriptions_for_channel->len;
    for (int i = 0; i < number_of_channel_subscriptions; i++)
    {
        subscription *s = (subscription *)g_array_index(subscriptions_for_channel, GArray *, i);
        if (s->subscription_id == subscription_id)
        {
            free(s);
            g_array_remove_index_fast(subscriptions_for_channel, i);
            if (subscribed_node_id != this_node_id)
                if (subscriptions_for_channel->len == 0)
                    net_send_unsubscribe(subscribed_node_id, subscribed_module_id, channel);
            break;
        }
    }
}

module_info *new_module_info(int node_id, int module_id, char *module_name, char *module_type)
{
    module_info *info = (module_info *)malloc(sizeof(module_info));
    info->module_id = module_id;
    info->node_id = node_id;
    info->name = module_name;
    info->name = module_name;
    info->type = module_type;
    return info;
}

/// signal handler, intercept CTRL-C
static void intHandler(int signum)
{
    program_runs = 0;
    printf("...CTRL-C hit, terminating\n");
}

void core_mato_init()
{
    signal(SIGINT, intHandler);

    program_runs = 1;
    threads_started = 0;
    next_free_module_id = 0;
    next_free_subscription_id = 0;

    instance_data = g_array_new(0, 0, sizeof(void *));
    module_specifications = g_hash_table_new(g_str_hash, g_str_equal);

    module_names = g_array_new(0, 0, sizeof(GArray *));
    module_types = g_array_new(0, 0, sizeof(GArray *));
    buffers = g_array_new(0, 0, sizeof(GArray *));
    dangling_channel_data = 0;
    subscriptions = g_array_new(0, 0, sizeof(GArray *));

    pthread_mutex_init(&framework_mutex, 0);

    for(int i=0;i<nodes->len;i++)
    {
        GArray * names = g_array_new(0, 0, sizeof(char *));
        g_array_append_val(module_names, names);
        GArray * types = g_array_new(0, 0, sizeof(char *));
        g_array_append_val(module_types , types);
        GArray * buffs = g_array_new(0, 0, sizeof(GArray *));
        g_array_append_val(buffers , buffs);
        GArray * subsc = g_array_new(0, 0, sizeof(GArray *));
        g_array_append_val(subscriptions , subsc);
    }

    if (pipe(post_data_pipe) != 0)
    {
        perror("could not create pipe for framework");
        return;
    }

    pthread_t t;
    if (pthread_create(&t, 0, mato_core_thread, 0) != 0)
        perror("could not create thread for framework");
}

channel_data *new_channel_data(int node_id, int module_id, int channel_id, int length, void *data)
{
    channel_data *cd = (channel_data *)malloc(sizeof(channel_data));
    cd->node_id = node_id;
    cd->module_id = module_id;
    cd->channel_id = channel_id;
    cd->length = length;
    cd->data = data;
    cd->references = 0;
    return cd;
}

void subscribe_channel_from_remote_node(int remote_node_id, int subscribed_module_id, int channel)
{
    subscription *new_subscription = (subscription *)malloc(sizeof(subscription));
    new_subscription->type = data_copy;
    new_subscription->callback = 0;
    new_subscription->subscriber_module_id = 0;
    new_subscription->subscriber_node_id = remote_node_id;
    lock_framework();
        new_subscription->subscription_id = get_free_subscription_id();
        GArray *channel_subscriptions = g_array_index(g_array_index(g_array_index(subscriptions, GArray *, this_node_id), GArray *, subscribed_module_id), GArray *, channel);
        g_array_append_val(channel_subscriptions, new_subscription);
    unlock_framework();
}

void unsubscribe_channel_from_remote_node(int remote_node_id, int subscribed_module_id, int channel)
{
    lock_framework();
        GArray *subscriptions_for_channel = g_array_index(g_array_index(g_array_index(subscriptions, GArray *, this_node_id), GArray *, subscribed_module_id), GArray *, channel);
        int number_of_channel_subscriptions = subscriptions_for_channel->len;
        for (int i = 0; i < number_of_channel_subscriptions; i++)
        {
            subscription *s = (subscription *)g_array_index(subscriptions_for_channel, GArray *, i);
            if (s->subscriber_node_id == remote_node_id)
            {
                free(s);
                g_array_remove_index_fast(subscriptions_for_channel, i);
                break;
            }
        }
    unlock_framework();
}

void pack_and_send_data_to_remote(int remote_node_id, int module_id, int channel, int get_data_id)
{
    lock_framework();
        GArray *module_buffers = g_array_index(g_array_index(buffers, GArray *,this_node_id), GArray *, module_id);
        GList *waiting_buffers = g_array_index(module_buffers, GList *, channel);

        if (waiting_buffers == 0)  // module has not provided any data yet, send 0 response
        {
    unlock_framework();
            net_send_data(remote_node_id, get_data_id, 0, 0);
            return;
        }
        // retrieve the last valid data pointer (and increment reference count)
        channel_data *cd = (channel_data *)(waiting_buffers->data);
        cd->references++;
    unlock_framework();
        // sending outside of the lock, could take some time
        net_send_data(remote_node_id, get_data_id, cd->data, cd->length);
    lock_framework();
        waiting_buffers = g_array_index(module_buffers, GList *, channel);
        waiting_buffers = decrement_references(waiting_buffers, cd);
        g_array_index(module_buffers, GList *, channel) = waiting_buffers;
    unlock_framework();
}

void return_data_to_waiting_module(int get_data_id, int32_t data_length, uint8_t *data)
{
    if (write(get_data_id, &data_length, sizeof(int32_t)) < 0)
        perror("error writing data size to pipe");
    else if (write(get_data_id, &data, sizeof(uint8_t *)) < 0)
        perror("error writing data to pipe");
}

static channel_data *get_ptr_to_last_data_of_channel(int node_id, int module_id, int channel, int *data_length)
{
    GList *waiting_buffers = g_array_index(g_array_index(g_array_index(buffers, GArray *, node_id), GArray *, module_id), GList *, channel);
    if (waiting_buffers == 0)
    {
        *data_length = 0;
        return 0;
    }
    channel_data *cd = (channel_data *)(waiting_buffers->data);
    *data_length = cd->length;
    return cd;
}

void copy_of_last_data_of_channel(int node_id, int module_id, int channel, int *data_length, uint8_t **data)
{
    channel_data *cd = get_ptr_to_last_data_of_channel(node_id, module_id, channel, data_length);
    if (cd)
    {
        *data = malloc(cd->length);
        memcpy(*data, cd->data, cd->length);
    }
    else *data = 0;
}

void borrow_last_data_of_channel(int node_id, int module_id, int channel, int *data_length, uint8_t **data)
{
    channel_data *cd = get_ptr_to_last_data_of_channel(node_id, module_id, channel, data_length);
    if (cd)
    {
        *data = cd->data;
        cd->references++;
    }
    else *data = 0;
}

long long msec()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return 1000L * tv.tv_sec + tv.tv_usec / 1000L;
}

long long usec()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (1000000L * (long long)tv.tv_sec) + tv.tv_usec;
}


