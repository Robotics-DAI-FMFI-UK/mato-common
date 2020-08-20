#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "mato.h"

#define MAX_PENDINGS_CONNECTIONS 10
#define NODE_MULTIPLIER 100000L

// messages of node to node communication protocol
#define MSG_NEW_MODULE_INSTANCE 1
#define MSG_DELETED_MODULE_INSTANCE 2
#define MSG_SUBSCRIBE 3
#define MSG_UNSUBSCRIBE 4
#define MSG_GET_DATA 5
#define MSG_SUBSCRIBED_DATA 6
#define MSG_GLOBAL_MESSAGE 7

#define LISTEN_BACKLOG  10

#define CONFIG_FILENAME "mato_nodes.conf"

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
    int subscriber_node_id;
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
    int node_id;
} channel_data;

/// A constructor for the channel_data structure.
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

/// Set to one when the framework is initialized and cleared when any part of the program wants to terminate the whole program.
/// After it is set to zero, all threads are expected to terminated soon. When all threads terminate, the application terminates.
volatile int program_runs;

/// Contains the number of threads that are running. Use the functions mato_inc_thread_count() and mato_dec_thread_count().
volatile int threads_started;

/// Contains the number of internal framework threads that are running. Use the functions mato_inc_system_thread_count() and mato_dec_system_thread_count().
volatile int system_threads_started;

/// Socket for accepting connections from other nodes.
static int listening_socket;

/// Id of this computational node.
static int32_t this_node_id;


//global framework data

/// Contains list of names of all module instances.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
static GArray *module_names;   // [node_id][module_id]

/// Contains list of types of all module instances.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
static GArray *module_types;    // [node_id][module_id]

/// Contains pointers to instance_data of all module instances as returned by their create_instance_callback.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
static GArray *instance_data;   // [module_id]

///Contains node info for all computational nodes filled from config file in mato_init.
static GArray *nodes;   // [node_id]

static GArray *sockets;   // [node_id]

/// Contains module_specification structures for all type names. The keys in this hashtable are the
/// module types. The value is a pointer to module_specification structure as provided by the
/// argument to mato_register_new_type_of_module() function that is typically called from the init
/// function of a module type.
static GHashTable *module_specifications;  // [type_name]

/// Data of all messages that are maintained by the framework at any point of time are kept in this
/// GArray. It is indexed by module_id and contains GArrays indexed by channel number of the particular
/// module instance. Finally, the elements of the nested GArray are GLists - the list of all messages
/// of the particular module in its particular channel that are still needed by the framework or
/// any other module and thus have not been deallocated. Each time a module posts a new message
/// to its channel, it is added to that list. The message is removed from the list when it has
/// have been forwarded to all the subscribers, no subscriber or another module has borrowed a pointer
/// to it and a new message from the module on the same channel has already arrived.
static GArray *buffers;  // [node_id][module_id][channel_id] -> g_list (the most recent data buffer is at the beginning)

/// A counter for assigning a new module_id for newly created module instances.
static int next_free_module_id;

/// A counter for assining a new subscription_id for newly registered subscriptions.
static int next_free_subscription_id;

/// Contains all descriptions of subscriptions, instances of subscription structures.
/// The GArray is indexed by the module_id and contains GArrays indexed by channel number
/// Finally, the nested GArray elements are again GArrays containing all subscriptions
/// to that particular channel of that particular module.
static GArray *subscriptions;  // [node_id][module_id][channel_id][subscription_index] - contains "subcription"s

/// Used for mutual exclusion when accessing framework structures from functions that can be called from different threads.
static pthread_mutex_t framework_mutex;

/// New messages that are posted by the modules are allocated in dynamic memory. Pointers to that memory enter this pipe
/// and are picked up by a message redistribution loop that takes care of them in a serial manner. Handling of each message
/// is supposed to be done very quickly - assuming the subscriber callbacks return quickly. In the future release, we expect
/// each subscriber callback to be called in a separate thread taken from a thread pool.
static int post_data_pipe[2];

/// pipe for sending a signal to select() waiting on msgs from nodes - it has to be interrupted when new node
/// connects (or similar events occur)
static int select_wakeup_pipe[2];

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

/// Increment the number of internal framework threads running. This should be called by each framework thread that has been started.
void mato_inc_system_thread_count()
{
    lock_framework();
      system_threads_started++;
    unlock_framework();
}

/// Decrement the number of internal framework threads running. It should be called by each framework thread that terminates.
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

      char *check_module_exists = g_array_index(g_array_index(module_names, GArray *,cd->node_id), char *, cd->module_id);
      if (check_module_exists == 0)
      {
          free(cd->data);
          free(cd);
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
              //TODO forwarding data to subscriber from another node
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

void node_disconnected(int s, int i)
{
    g_array_index(nodes,node_info*,i)->is_online = 0;
    close(s);
    printf("node %d has disconnected\n", i);
    // TODO delete all node traces from data structures
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
    else if (retval == 0) // node disconnected
    {
        node_disconnected(s, i);
    }
    switch(message_type){
        case MSG_NEW_MODULE_INSTANCE: 
               break;
        case MSG_DELETED_MODULE_INSTANCE:
               break;
        case MSG_SUBSCRIBE:
               break;
        case MSG_UNSUBSCRIBE:
               break;
        case MSG_GET_DATA:
               break;
        case MSG_SUBSCRIBED_DATA:
               break;
        case MSG_GLOBAL_MESSAGE:
               break;
    }
}

void *reconnecting_thread(void *arg)
{
    struct sockaddr_in my_addr;

    mato_inc_system_thread_count();

    while (program_runs){
        for(int i = this_node_id + 1; i < nodes->len; i++)
        {
            if (g_array_index(nodes,node_info*,i)->is_online == 0)
            {
                printf("trying to connect to node %d, constructing socket...\n", i);

                int s = socket(AF_INET, SOCK_STREAM, 0);
                if( s < 0)
                {
                    perror("could not create socket");
                    return 0;
                }
                char *IP = g_array_index(nodes,node_info*,i)->IP;
                int port = g_array_index(nodes,node_info*,i)->port;
                memset(&my_addr, 0, sizeof(struct sockaddr_in));
                my_addr.sin_family = AF_INET;
                my_addr.sin_port = htons(port);
                if (inet_pton(AF_INET, IP, &my_addr.sin_addr)<=0)
                {
                    printf("Invalid ip address (%s:%d)\n", IP, port);
                    continue;
                }
                printf("calling connect (%s:%d)...\n", IP, port);
                if(connect(s, (struct sockaddr *)&my_addr, sizeof(struct sockaddr_in))<0)
                    continue;
                    printf("sending my node_id\n");
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
                continue;
            }
        }
        for (int i = 0; i < 10; i++)
            if (!program_runs) break;
                else sleep(1);
    }
    mato_dec_system_thread_count();
}

void *communication_thread(void *arg)
{
    mato_inc_system_thread_count();
    fd_set rfds;
    int nfd = 0;
    while (program_runs){
        FD_ZERO(&rfds);
        for(int i = 0; i < nodes->len; i++)
        {
            if (i == this_node_id) continue;
            if (g_array_index(nodes,node_info*,i)->is_online == 1)
            {
                int s = g_array_index(sockets, int, i);
                FD_SET(s, &rfds);
                if (s > nfd) nfd = s;
                printf("selected fd(%d) of node %d\n", s, i);
            }
        }
        FD_SET(listening_socket, &rfds);
        if (listening_socket > nfd) nfd = listening_socket;
        FD_SET(select_wakeup_pipe[0], &rfds);
        if (select_wakeup_pipe[0] > nfd) nfd = select_wakeup_pipe[0];
        printf("calling select...\n");
        nfd++;
        int retval = select(nfd, &rfds, 0, 0, 0);
        printf("select() returns %d\n", retval);
        if (retval < 0)
        {
            perror("Select error");
            break;
        }
        if (FD_ISSET(select_wakeup_pipe[0], &rfds))
        {
            uint8_t b;
            read(select_wakeup_pipe[0], &b, 1);
            continue;
        }

        if (FD_ISSET(listening_socket,&rfds))
        {
            printf("Connection ...\n");
            struct sockaddr_in incomming;
            socklen_t size = sizeof(struct sockaddr_in);
            int s = accept(listening_socket, (struct sockaddr *)&incomming, &size);
            int32_t new_node_id;
            retval = recv(s, &new_node_id, sizeof(int32_t), MSG_WAITALL);
            if(retval<0)
            {
                perror("reading from socket");
                continue;
            }
            printf("...from node %d\n", new_node_id);
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
            }
        }
    }
    mato_dec_system_thread_count();
}

void start_networking()
{
    struct sockaddr_in my_addr, peer_addr;
    socklen_t peer_addr_size;

    listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_socket == -1)
       perror("socket");

    memset(&my_addr, 0, sizeof(struct sockaddr_in));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(g_array_index(nodes,node_info*,this_node_id)->port);

    int i = 0;
    do {
        if (bind(listening_socket, (struct sockaddr *) &my_addr, sizeof(struct sockaddr_in)) == -1)
            perror("bind, will retry...");
        else break;
        sleep(6);
    } while (program_runs && (++i < 20));
    if (i >= 20) exit(1);       

    int rv = listen(listening_socket, MAX_PENDINGS_CONNECTIONS);
    if (rv < 0)
    {
        perror("listen");
        return;
    }

    if (pipe(select_wakeup_pipe) < 0)
    {
        perror("could not create pipe for select");
        return;
    }

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
    n->IP = (char *)malloc(strlen(ip) + 1);
    strcpy(n->IP, ip);
    n->name = (char *)malloc(strlen(name) + 1);
    strcpy(n->name, name);
    n->port = port;
    n->is_online = is_online;
    return n;
}

int config_error(int ln)
{
    printf("Could not parse nodes config file, error at line %d\n", ln);
    return 0;
}

int read_config()
{
    char config_line[256];
    int ln = 0;

    int node_id;
    char *ip;
    int port;
    char *name;

    FILE *f = fopen(CONFIG_FILENAME, "r");
    while (fgets(config_line, 255, f))
    {
		char *comma = strchr(config_line, ',');
		if (comma == 0) return config_error(ln);
		*comma = 0;
		sscanf(config_line, "%d", &node_id);
		comma++;
		char *comma2 = strchr(comma, ',');
		if (comma2 == 0) return config_error(ln);
		*comma2 = 0;
		ip = comma;
		comma2++;
		comma = strchr(comma2, ',');
		if (comma == 0) return config_error(ln);
		*comma = 0;
		sscanf(comma2, "%d", &port);
		comma++;
		int ln = strlen(comma);
		while (ln > 0)
		{
			char c = comma[ln - 1];
			if ((c != '\n') && (c != '\r')) break;
			comma[ln - 1] = 0;
			ln--;
		}
		name = comma;
		node_info *node = new_node_info(node_id, ip, port, name, 0);
		g_array_append_val(nodes, node);
		int zero = 0;
		g_array_append_val(sockets, zero);
    }
    g_array_index(nodes,node_info*,this_node_id)->is_online = 1;
    return 1;
}

/// signal handler, intercept CTRL-C
void intHandler(int signum) 
{
    program_runs = 0;
    printf("...CTRL-C hit, terminating\n");
}

/// Initializes the framework. It must be the first function of the framework to be called. It should be called only once.
void mato_init(int this_node_identifier)
{
    signal(SIGINT, intHandler);

    this_node_id = this_node_identifier;
    program_runs = 1;
    threads_started = 0;
    next_free_module_id = 0;
    next_free_subscription_id = 0;

    instance_data = g_array_new(0, 0, sizeof(void *));
    module_specifications = g_hash_table_new(g_str_hash, g_str_equal);

    module_names = g_array_new(0, 0, sizeof(GArray *));
    module_types = g_array_new(0, 0, sizeof(GArray *));
    buffers = g_array_new(0, 0, sizeof(GArray *));
    subscriptions = g_array_new(0, 0, sizeof(GArray *));
    nodes = g_array_new(0, 0, sizeof(node_info *));
    sockets = g_array_new(0, 0, sizeof(int));

    pthread_mutex_init(&framework_mutex, 0);


    if (!read_config())
    {
        printf("Error loading nodes config file\n");
        return;
    }
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
    g_array_append_val(g_array_index(module_names, GArray *, this_node_id), module_name);
    g_array_append_val(g_array_index(module_types, GArray *, this_node_id), module_type);

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
    unlock_framework();

    // TODO: notify other nodes that the node was created
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
        lock_framework();
    }
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
		g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id) = 0;
		g_array_index(instance_data, void *, module_id) = 0;
		// TODO: notify other nodes that the module has been deleted
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
		{
			//TODO send subscription to another node
		}
    unlock_framework();
    return new_subscription->subscription_id;
}

void mato_unsubscribe(int module_id, int channel, int subscription_id)
{
    int subscribed_node_id = module_id / NODE_MULTIPLIER;
    module_id %= NODE_MULTIPLIER;

    lock_framework();
		GArray *subscriptions_for_channel = g_array_index(g_array_index(g_array_index(subscriptions, GArray *,subscribed_node_id), GArray *, module_id), GArray *, channel);
		int number_of_channel_subscriptions = subscriptions_for_channel->len;
		for (int i = 0; i < number_of_channel_subscriptions; i++)
		{
			subscription *s = (subscription *)g_array_index(subscriptions_for_channel, GArray *, i);
			if (s->subscription_id == subscription_id)
			{
				free(s);
				g_array_remove_index_fast(subscriptions_for_channel, i);
				if (subscribed_node_id != this_node_id)
				{
					if (subscriptions_for_channel->len == 0) 
					{
						// TODO: notify another node about unsubscription for this channel
					}
				}
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

    for (int node_id = 0; node_id < nodes->len; node_id++)
    {
        if (this_node_id == node_id) continue;
        // TODO: forward global message to other node
	}
	
	int our_modules_count = g_array_index(module_names, GArray *, this_node_id)->len;
	for (int module_id = 0; module_id < our_modules_count; module_id++)
	{
		char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
		if (module_type != 0)
		{
			if (module_id != local_module_id_sender)  // not delivering to the msg. originator
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
    {
        GList *waiting_buffers = g_array_index(g_array_index(g_array_index(buffers,GArray *,this_node_id), GArray *, id_module), GList *, channel);
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
    }
    else
    {
        //TODO get data from other node
        //if we are subscribed to that channel, just copy the last valid from the buffers,
        //otherwise request the last valid data from other node
    }
    unlock_framework();
    return;
}

void mato_borrow_data(int id_module, int channel, int *data_length, void **data)//28.7 nove
{
    lock_framework();
    int node_id = id_module / NODE_MULTIPLIER;
    id_module %= NODE_MULTIPLIER;
    if (node_id == this_node_id)
    {
        GList *waiting_buffers = g_array_index(g_array_index(g_array_index(buffers,GArray *,this_node_id), GArray *, id_module), GList *, channel);
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
    }
    else
    {
        //TODO borrow data from other node
        //if we are subscribed to that channel, just copy the last valid from the buffers,
        //otherwise request the last valid data from other node
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

/// A constructor for the module_info structure.
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
			for (int i = 0; i < g_array_index(module_names, GArray *, node_id)->len; i++)
			{
				char *module_name = g_array_index(g_array_index(module_names, GArray *, node_id), char *, i);
				if (module_name == 0) continue;
				char *module_type = g_array_index(g_array_index(module_types, GArray *, node_id), char *, i);
				if ((type == 0) || (strcmp(module_type, type) == 0))
				{
					module_info *info = new_module_info(node_id, i + node_id * NODE_MULTIPLIER, module_name, module_type);
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

void mato_shutdown()
{
    uint8_t wakeup_byte;
    program_runs = 0;

    if (write(select_wakeup_pipe[1], &wakeup_byte, 1) < 0)
        perror("could not wakeup networking thread");

    close(post_data_pipe[1]);

    while (mato_system_threads_running() > 0) { usleep(10000); }

    close(post_data_pipe[0]);
    close(select_wakeup_pipe[0]);
    close(select_wakeup_pipe[1]);
    close(listening_socket);

    pthread_mutex_destroy(&framework_mutex);
}
