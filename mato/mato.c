#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include "mato.h"

#define MAX_PENDINGS_CONNECTIONS 10


/// \file mato.c
/// Implementation of the Mato control framework.


/// The structure describes a single individual subscription to some channel of some module.
/// Each subscription is identified by a computational node unique identifier (id_subscription)
/// has one of the supported subscription types, pointer to a callback function of the subscriber
/// and the module_id of the subscriber module.
typedef struct {
    int subscription_id;
    subscription_type type;
    subscriber_callback callback;
    int subscriber_module_id;
} subscription;

/// The structure holds one message that was posted by a module to one of its output channels.
/// It contains the module_id of the posting module, the channel number where the message was posted,
/// the length of the data and pointer to malloc-ed data buffer that holds the actual data,
/// and the number of references, i.e. how many users (typically modules) have received the data
/// pointer and must return it back.
typedef struct {
    int module_id;
    int channel_id;
    int length;
    void* data;
    int references;
} channel_data;

/// A constructor for the channel_data structure.
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

/// Set to one when the framework is initialized and cleared when any part of the program wants to terminate the whole program.
/// After it is set to zero, all threads are expected to terminated soon. When all threads terminate, the application terminates.
volatile int program_runs;

/// Contains the number of threads that are running. Use the functions mato_inc_thread_count() and mato_dec_thread_count().
volatile int threads_started;

/// Socket for accepting connections from other nodes.
int listening_socket;

/// Id of this computational node.
int32_t this_node_id;


//global framework data

/// Contains list of names of all module instances. 
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
GArray *module_names;   // [module_id]

/// Contains list of types of all module instances.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
GArray *module_types;    // [module_id]

/// Contains pointers to instance_data of all module instances as returned by their create_instance_callback.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
GArray *instance_data;   // [module_id]

///Contains node info for all computational nodes filled from config file in mato_init.
GArray *nodes;   // [node_id]

GArray *sockets;   // [node_id]

/// Contains module_specification structures for all type names. The keys in this hashtable are the 
/// module types. The value is a pointer to module_specification structure as provided by the 
/// argument to mato_register_new_type_of_module() function that is typically called from the init
/// function of a module type.
GHashTable *module_specifications;  // [type_name]

/// Data of all messages that are maintained by the framework at any point of time are kept in this
/// GArray. It is indexed by module_id and contains GArrays indexed by channel number of the particular
/// module instance. Finally, the elements of the nested GArray are GLists - the list of all messages
/// of the particular module in its particular channel that are still needed by the framework or
/// any other module and thus have not been deallocated. Each time a module posts a new message
/// to its channel, it is added to that list. The message is removed from the list when it has
/// have been forwarded to all the subscribers, no subscriber or another module has borrowed a pointer
/// to it and a new message from the module on the same channel has already arrived.
GArray *buffers;  // [module_id][channel_id] -> g_list (the most recent data buffer is at the beginning)

/// A counter for assigning a new module_id for newly created module instances.
int next_free_module_id;

/// A counter for assining a new subscription_id for newly registered subscriptions.
int next_free_subscription_id;
                        
/// Contains all descriptions of subscriptions, instances of subscription structures.
/// The GArray is indexed by the module_id and contains GArrays indexed by channel number
/// Finally, the nested GArray elements are again GArrays containing all subscriptions
/// to that particular channel of that particular module.
GArray *subscriptions;  // [module_id][channel_id][subscription_index] - contains "subcription"s

/// Used for mutual exclusion when accessing framework structures from functions that can be called from different threads.
pthread_mutex_t framework_mutex;

/// New messages that are posted by the modules are allocated in dynamic memory. Pointers to that memory enter this pipe
/// and are picked up by a message redistribution loop that takes care of them in a serial manner. Handling of each message
/// is supposed to be done very quickly - assuming the subscriber callbacks return quickly. In the future release, we expect
/// each subscriber callback to be called in a separate thread taken from a thread pool.
int post_data_pipe[2];

/// Enter mutually-exclusive area accessing internal framework data structures.
void lock_framework()
{
    pthread_mutex_lock(&framework_mutex);
}

/// Leave mutually-exclusive area with a protected acces to internal framework data structures.
void unlock_framework()
{
    pthread_mutex_unlock(&framework_mutex);    
}

/// Decrements the number of references to a particular buffer and deallocates the buffer itself as well
/// as the structure describing the buffer. It also removes it from the list of the framework-maintained messages.
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
//      printf("retrieved channel data from pipe: %" PRIuPTR "\n", (uintptr_t)cd);
      
      cd->references++; // last valid data from module channel
      cd->references++; // currently being sent out to subscribers
      
      lock_framework();
      GArray *module_buffers = g_array_index(buffers, GArray *, cd->module_id);
      GList *channel_list = g_array_index(module_buffers, GList *, cd->channel_id);
      
      if (channel_list)
          // previous last valid data is not last valid data anymore => ref--
          channel_list = decrement_references(channel_list, (channel_data *)(channel_list->data));
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


void node_disconected(int s, int i)
{
	g_array_index(nodes,node_info*,i)->is_online = 0;
	close(s);
	printf("node %d has disconnected\n", i);
	
}

void process_node_message(int s, int i)
{
	int32_t message_type;
	int retval = recv(s, &message_type, sizeof(int32_t), MSG_WAITALL);
	if(retval<0)
	{
		perror("reading from socket");
		return;
	}
	switch(message_type){
		case MSG_NEW_MODULE_INSTANCE:	
	}
}

void *reconnecting_thread(void *arg)
{
	while (program_runs){
		for(int i = 0; i < nodes->len; i++)
		{
			if (g_array_index(nodes,node_info*,i)->is_online == 0)
			{
				int s = socket(AF_INET, SOCK_STREAM, 0);
				if( s < 0)
				{
					perror("could not create socket");
					return;
				}
				memset(&my_addr, 0, sizeof(struct sockaddr_in));
				my_addr.sin_family = AF_INET;
				my_addr.sin_port = htons(g_array_index(nodes,node_info*,i)->port);
				char *IP = g_array_index(nodes,node_info*,i)->IP;
				if (inet_pton(AF_INET, IP, &my_addr.sin_addr)<=0)
				{
					printf("Invalid ip address (%s)\n",IP);
					continue;
				}
				if(connect(s, (struct sockaddr *)&my_addr, sizeof(struct sockaddr_in))<0)
					continue;
				int retval = send(s, &this_node_id, sizeof(int32_t), 0);
				if(retval<0)
				{
					perror("Could not send this node id");
					close(s);
					continue;
				}
				g_array_index(sockets, int, i) = s;
				g_array_index(nodes,node_info*,i)->is_online = 1;
				printf("connected to %d\n",i);
			}
		}
		sleep(10);
	}
}

void *communication_thread(void *arg)
{
	fd_set rfds;
	fd_set expfds;
	int nfd=0;
	while (program_runs){
		FD_ZERO(&rfds);
		FD_ZERO(&expfds);
		nfd=0;
		for(int i = 0; i < nodes->len; i++)
		{
			if (g_array_index(nodes,node_info*,i)->is_online == 1)
			{
				int s = g_array_index(sockets, int, i);
				FD_SET(s, &rfds);
				FD_SET(s, &expfds);
				nfd++;
			}
		}
		FD_SET(listening_socket,&rfds);
		nfd++;
		int retval = select(nfd, &rfds, 0, &expfds, 0);
		if(retval<0)
		{
			perror("Select error");
			break;			
		}
		if(FD_ISSET(listening_socket,&rfds))
		{
			printf("Connectiong ...")
			sockaddr_in incomming;
			int s = accept(listening_socket, &incomming, sizeof(struct sockaddr_in));
			int32_t new_node_id;
			retval = recv(s, &new_node_id, sizeof(int32_t), MSG_WAITALL);
			if(retval<0)
			{
				perror("reading from socket");
				continue;
			}
			g_array_index(sockets, int, new_node_id) = s;
			g_array_index(nodes, node_info*, new_node_id)->is_online = 1;
		}
		for(int i = 0; i < nodes->len; i++)
		{
			if (g_array_index(nodes,node_info*,i)->is_online == 1)
			{
				int s = g_array_index(sockets, int, i);
				if (FD_ISSET(s, &rfds))
				{
					process_node_message(s, i);
				}
				if (FD_ISSET(s, &expfds))
				{
					node_disconected(s, i);
				}
			}
		}
	}
}

void start_networking()
{
	int sfd, cfd;
	struct sockaddr_in my_addr, peer_addr;
	socklen_t peer_addr_size;

	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd == -1)
	   perror("socket");

	memset(&my_addr, 0, sizeof(struct sockaddr_un));
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	my_addr.sin_port = htons(g_array_index(nodes,node_info*,this_node_id)->port);
	
	if (bind(sfd, (struct sockaddr *) &my_addr, sizeof(struct sockaddr_in)) == -1)
	   perror("bind");
   
	if (listen(sfd, LISTEN_BACKLOG) == -1)
	   perror("listen");
   
    listening_socket = listen(SOCK_STREAM , MAX_PENDINGS_CONNECTIONS);
	pthread_t t;
    if (pthread_create(&t, 0, reconnecting_thread, 0) != 0)
          perror("could not create reconnecting thread for framework");
    if (pthread_create(&t, 0, communication_thread, 0) != 0)
          perror("could not create communication thread for framework");
}

node_info *new_node_info(int node_id, char *ip, int port, char *name, int is_online)
{
	node_info *n = (node_info *) malloc(sizeof(node_info));
	n->node_id = node_id;
	n->IP = ip;
	n->name = name;
	n->port = port;
	n->is_online = is_online;
	return n;
}


void read_config()
{
	node_info *node_0 = new_node_info(0,"127.0.0.1",9999,"Jetson",0);
	node_info *node_1 = new_node_info(1,"127.0.0.1",10000,"Raspbery",0);
	g_array_append_val(nodes, node_0);
	g_array_append_val(nodes, node_1);
	g_array_append_val(sockets, 0);
	g_array_append_val(sockets, 0);
	this_node_id = 0;
	g_array_index(nodes,node_info*,this_node_id)->is_online = 1;
}


/// Initializes the framework. It must be the first function of the framework to be called. It should be called only once. 
void mato_init()
{
    program_runs = 1;
    threads_started = 0;
    
    module_names = g_array_new(0, 0, sizeof(char *));
    module_types = g_array_new(0, 0, sizeof(char *));
	nodes = g_array_new(0, 0, sizeof(node_info *));
	sockets = g_array_new(0, 0, sizeof(int));
    instance_data = g_array_new(0, 0, sizeof(void *));
    buffers = g_array_new(0, 0, sizeof(GArray *));

    next_free_module_id = 0;
    next_free_subscription_id = 0;
    module_specifications = g_hash_table_new(g_str_hash, g_str_equal); 

    subscriptions = g_array_new(0, 0, sizeof(GArray *));    
    pthread_mutex_init(&framework_mutex, 0);
    read_config();
    if (pipe(post_data_pipe) != 0)
    {
      perror("could not create pipe for framework");
      return;
    }
	pthread_t t;
    if (pthread_create(&t, 0, mato_thread, 0) != 0)
          perror("could not create thread for framework");
	start_networking();
}

void mato_register_new_type_of_module(char *type, module_specification *specification)
{
    lock_framework();
      g_hash_table_insert (module_specifications, type, specification);    
    unlock_framework();
}

/// Returns the next available module_id.
int get_free_module_id() // is not thread-safe
{ 
    return next_free_module_id++;
}

/// Returns the next available subscription_id.
int get_free_subscription_id() // is not thread-safe
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
//      printf("appended instance data %" PRIuPTR "\n", (uintptr_t)module_instance_data);
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
//    printf("%d sending channel data to pipe: %" PRIuPTR "\n", id_of_posting_module, (uintptr_t)cd);

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

/// A constructor for the module_info structure.
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

/// Increment the number of threads running. This should be called by each thread that has been started.
void mato_inc_thread_count()
{
    lock_framework();
      threads_started++;
    unlock_framework();
}

/// Decrement the number of threads running. It should be called by each thread that terminates.
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
