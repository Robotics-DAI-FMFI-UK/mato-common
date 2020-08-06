#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include "mato.h"

typedef struct {
    int subscription_id;
    subscription_type type;
    subscriber_callback callback;
    int subscriber_module_id;
} subscription;

typedef struct {
    int module_id;
    int channel_id;
    int length;
    void* data;
    int references;
} channel_data;

channel_data *new_channel_data(int module_id, int channel_id, int length, void *data)
{
    channel_data *cd = (channel_data *)malloc(sizeof(channel_data));
    cd->module_id = module_id;
    cd->channel_id = channel_id;
    cd->length = length;
    cd->data = data;
    cd->references = 0;
    return cd;
}

volatile int program_runs;
volatile int threads_started;

//global framework data

GArray *module_names;   // [module_id]
GArray *module_types;    // [module_id]
GArray *instance_data;   // [module_id]
GHashTable *module_specifications;  // [type_name]
GArray *buffers;  // [module_id][channel_id] -> g_list (the most recent data buffer is at the beginning)

int next_free_module_id;
int next_free_subscription_id;
                        
GArray *subscriptions;  // [module_id][channel_id][subscription_index] - contains "subcription"s

pthread_mutex_t framework_mutex;
int post_data_pipe[2];

void lock_framework()
{
    pthread_mutex_lock(&framework_mutex);
}

void unlock_framework()
{
    pthread_mutex_unlock(&framework_mutex);    
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

void *mato_thread(void *arg)
{
    channel_data *cd;
    mato_inc_thread_count();
    while (program_runs)
    {
      int retval = read(post_data_pipe[0], &cd, sizeof(channel_data *));
      if (retval < 0)
      {
          perror("error reading from message pipe");
          return 0;
      }
//      printf("retrieved channel data from pipe: %lu\n", (uint64_t)cd);
      
      cd->references++; // last valid data from module channel
      cd->references++; // currently being sent out to subscribers
      
      lock_framework();
      GArray *module_buffers = g_array_index(buffers, GArray *, cd->module_id);
      GList *channel_list = g_array_index(module_buffers, GList *, cd->channel_id);
      
      channel_data *last_valid = (channel_data *)g_list_first(channel_list);
      if (last_valid)
          // previous last valid data is not last valid data anymore => ref--
          channel_list = decrement_references(channel_list, last_valid);
      channel_list = g_list_prepend(channel_list, cd);

      g_array_index(module_buffers, GList *, cd->channel_id) = channel_list;
            
      GArray *subscriptions_for_channel = g_array_index(g_array_index(subscriptions, GArray *, cd->module_id), GArray *, cd->channel_id);
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
        free(subscriber->data);
        subscriber = subscriber->next;
      }
      g_list_free(list_of_subscriptions_to_use);
      
      channel_list = decrement_references(channel_list, cd);  // done with this channel data, ref--
      // no need to check for channel_list changed, since it is still the last valid, ie. refcount > 0
      unlock_framework();
    }
    mato_dec_thread_count();
}

void mato_init()
{
    program_runs = 1;
    threads_started = 0;
    
    module_names = g_array_new(0, 0, sizeof(char *));
    module_types = g_array_new(0, 0, sizeof(char *));
    instance_data = g_array_new(0, 0, sizeof(void *));
    buffers = g_array_new(0, 0, sizeof(GArray *));

    next_free_module_id = 0;
    next_free_subscription_id = 0;
    module_specifications = g_hash_table_new(g_int64_hash, g_int64_equal); // assuming 64-bit arch

    subscriptions = g_array_new(0, 0, sizeof(GArray *));    
    pthread_mutex_init(&framework_mutex, 0);
    
    if (pipe(post_data_pipe) != 0)
    {
      perror("could not create pipe for framework");
      return;
    }
    
    pthread_t t;
    if (pthread_create(&t, 0, mato_thread, 0) != 0)
          perror("could not create thread for framework");
}

void mato_register_new_type_of_module(char *type, module_specification *specification)
{
    lock_framework();
      g_hash_table_insert (module_specifications, type, specification);    
    unlock_framework();
}

// is not thread-safe
int get_free_module_id()
{
    return next_free_module_id++;
}

// is not thread-safe
int get_free_subscription_id()
{
    return next_free_subscription_id++;
}

int mato_create_new_module_instance(const char *module_type, const char *module_name)
{    
    lock_framework();
    
    int module_id = get_free_module_id();
    g_array_append_val(module_names, module_name);
    g_array_append_val(module_types, module_type);
    
    GArray *channels_subscriptions = g_array_new(0, 0, sizeof(GArray *));
    g_array_append_val(subscriptions, channels_subscriptions);
    
    GArray *module_buffers = g_array_new(0, 0, sizeof(GArray *));
    g_array_append_val(buffers, module_buffers);

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

    void *module_instance_data = create_instance(module_id);
    
    lock_framework();
      g_array_append_val(instance_data, module_instance_data);    
//      printf("appended instance data %lu\n", (uint64_t)module_instance_data);
    unlock_framework();

    return module_id;
}

void mato_start_module(int module_id)
{
    lock_framework();
    char *module_type = g_array_index(module_types, char *, module_id); 
    if (module_type == 0) return;
    module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type); 
    if (spec != 0)
    {
        void *data = g_array_index(instance_data, void *, module_id);
        unlock_framework();
        spec->start_instance(data);
        lock_framework();
    }
    unlock_framework();
}

void mato_start()
{
    int n = module_names->len;
    for (int module_id = 0; module_id < n; module_id++)
        mato_start_module(module_id);
}

void mato_delete_module_instance(int module_id)
{    
    lock_framework();
    
//    printf("%d: instance_data->len=%d\n", module_id, instance_data->len);
    char *module_type = g_array_index(module_types, char *, module_id); 
    void *data = g_array_index(instance_data, void *, module_id);

       module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type); 
       unlock_framework();
     spec->delete_instance(data);
     lock_framework();
    
    GArray *channels_subscriptions = g_array_index(subscriptions, GArray *, module_id);
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
            
    g_array_free(g_array_index(subscriptions, GArray *, module_id), 1);
    void *zero = 0;
    g_array_index(subscriptions, subscription *, module_id) = 0;
    g_array_index(module_names, char *, module_id) = 0;
    g_array_index(module_types, char *, module_id) = 0;
    g_array_index(instance_data, void *, module_id) = 0;
    unlock_framework();
}

int mato_get_module_id(const char *module_name)
{
    for (int i = 0; i < module_names->len; i++)
    {
        char *name = g_array_index(module_names, char *, i);
        if ((name != 0) && (strcmp(name, module_name) == 0))
          return i;
    }
    return -1;
}

char *mato_get_module_name(int module_id)
{
    return g_array_index(module_names, char *, module_id);
}

char *mato_get_module_type(int module_id)
{
    return g_array_index(module_types, char *, module_id);
}

int mato_subscribe(int subscriber_module_id, int subscribed_module_id, int channel, subscriber_callback callback, int subscription_type)
{
    subscription *new_subscription = (subscription *)malloc(sizeof(subscription));
    new_subscription->type = subscription_type;
    new_subscription->callback = callback;
    new_subscription->subscriber_module_id = subscriber_module_id;
    lock_framework();
        new_subscription->subscription_id = get_free_subscription_id();
    
      g_array_append_val(g_array_index(g_array_index(subscriptions, GArray *, subscribed_module_id), GArray *, channel), new_subscription);
    unlock_framework();
    return new_subscription->subscription_id;
}

void mato_unsubscribe(int module_id, int channel, int subscription_id)
{
    lock_framework();
    GArray *subscriptions_for_channel = g_array_index(g_array_index(subscriptions, GArray *, module_id), GArray *, channel);
    int number_of_channel_subscriptions = subscriptions_for_channel->len;
    for (int i = 0; i < number_of_channel_subscriptions; i++)
    {
        subscription *s = (subscription *)g_array_index(subscriptions_for_channel, GArray *, i);
        if (s->subscription_id == subscription_id)
        {
            free(s);
            g_array_remove_index_fast(subscriptions_for_channel, i);
            break;
        }
    }
    unlock_framework();
}

void *mato_get_data_buffer(int size)
{
    return malloc(size);
}

void mato_post_data(int id_of_posting_module, int channel, int data_length, void *data)
{
    //writing data of size <= PIPE_BUF to pipe are atomic, therefore no framework locking is needed
    channel_data *cd = new_channel_data(id_of_posting_module, channel, data_length, data);
//    printf("%d sending channel data to pipe: %lu\n", id_of_posting_module, (uint64_t)cd);

    write(post_data_pipe[1], &cd, sizeof(channel_data *));
}    
    
int mato_send_global_message(int module_id_sender, int message_id, int msg_length, void *message_data)
{
    for(int module_id = 0; module_id < next_free_module_id; module_id++)
    {
        char *module_type = g_array_index(module_types, char *, module_id);
        if (module_type != 0)
            if (module_id != module_id_sender)
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

void mato_get_data(int id_module, int channel, int *data_length, void **data)
{
    lock_framework();
      GList *waiting_buffers = g_array_index(g_array_index(buffers, GArray *, id_module), GList *, channel);
      if (waiting_buffers == 0)
      {
          unlock_framework();
          *data_length = 0;
          *data = 0;
          return;
      }
      channel_data *cd = (channel_data *)(waiting_buffers->data);
      
      *data_length = cd->length;
      *data = malloc(cd->length);
      memcpy(*data, cd->data, cd->length);
    unlock_framework();
    return;
}

void mato_borrow_data(int id_module, int channel, int *data_length, void **data)//28.7 nove
{
    lock_framework();
      GList *waiting_buffers = g_array_index(g_array_index(buffers, GArray *, id_module), GList *, channel);
      if (waiting_buffers == 0)
      {
          unlock_framework();
          *data_length = 0;
          *data = 0;
          return;
      }
      channel_data *cd = (channel_data *)waiting_buffers->data;
      
      *data_length = cd->length;
      *data = cd->data;
      cd->references++;
      
    unlock_framework();
    return;
}


void mato_release_data(int id_module, int channel, void *data)
{
    lock_framework();
      GArray *buffers_for_module = g_array_index(buffers, GArray *, id_module);
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

module_info *new_module_info(int module_id, char *module_name, char *module_type)
{
    module_info *info = (module_info *)malloc(sizeof(module_info));
    info->module_id = module_id;
    info->name = module_name;
    info->type = module_type;
    return info;
}

GArray* mato_get_list_of_all_modules()
{
    return mato_get_list_of_modules(0);
}

GArray* mato_get_list_of_modules(char *type)
{
    lock_framework();
      GArray *modules = g_array_new(0, 0, sizeof(module_info *));
      for(int i = 0; i < module_names->len; i++)
      {
          char *module_name = g_array_index(module_names, char *, i);
          if (module_name == 0) continue;
          char *module_type = g_array_index(module_types, char *, i);
          if ((type == 0) || (strcmp(module_type, type) == 0))
          {  
            module_info *info = new_module_info(i, module_name, module_type);
            g_array_append_val(modules, info);
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
    lock_framework();
      GArray *module_buffers = (GArray *)g_array_index(buffers, GArray *, module_id);
      GList *channel_buffers = (GList *)g_array_index(module_buffers, GList *, channel);
      *number_of_allocated_buffers = 0;
      *total_sum_of_ref_count = 0;
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
