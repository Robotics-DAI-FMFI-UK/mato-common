#ifndef __MATO_CORE_H__
#define __MATO_CORE_H__

#define _GNU_SOURCE

/// \file mato_core.h
/// Mato control framework - internal core implementation function protypes and types.

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "mato.h"

#define NODE_MULTIPLIER 100000L

#define CONFIG_FILENAME "mato_nodes.conf"

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
channel_data *new_channel_data(int node_id, int module_id, int channel_id, int length, void *data);

extern volatile int program_runs;

/// Contains the number of threads that are running. Use the functions mato_inc_thread_count() and mato_dec_thread_count().
extern volatile int threads_started;

/// Contains the number of internal framework threads that are running. Use the functions mato_inc_system_thread_count() and mato_dec_system_thread_count().
extern volatile int system_threads_started;

//global framework data

/// Contains list of names of all module instances.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
extern GArray *module_names;   // [node_id][module_id]

/// Contains list of types of all module instances.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
extern GArray *module_types;    // [node_id][module_id]

/// Contains pointers to instance_data of all module instances as returned by their create_instance_callback.
/// The GArray is indexed by module_id. After a particular module is deleted,
/// its module_id and the location in this array will remain unused.
extern GArray *instance_data;   // [module_id]

/// Contains module_specification structures for all type names. The keys in this hashtable are the
/// module types. The value is a pointer to module_specification structure as provided by the
/// argument to mato_register_new_type_of_module() function that is typically called from the init
/// function of a module type.
extern GHashTable *module_specifications;  // [type_name]

/// Data of all messages that are maintained by the framework at any point of time are kept in this
/// GArray. It is indexed by module_id and contains GArrays indexed by channel number of the particular
/// module instance. Finally, the elements of the nested GArray are GLists - the list of all messages
/// of the particular module in its particular channel that are still needed by the framework or
/// any other module and thus have not been deallocated. Each time a module posts a new message
/// to its channel, it is added to that list. The message is removed from the list when it has
/// have been forwarded to all the subscribers, no subscriber or another module has borrowed a pointer
/// to it and a new message from the module on the same channel has already arrived.
extern GArray *buffers;  // [node_id][module_id][channel_id] -> g_list (the most recent data buffer is at the beginning)

/// Contains all descriptions of subscriptions, instances of subscription structures.
/// The GArray is indexed by the module_id and contains GArrays indexed by channel number
/// Finally, the nested GArray elements are again GArrays containing all subscriptions
/// to that particular channel of that particular module.
extern GArray *subscriptions;  // [node_id][module_id][channel_id][subscription_index] - contains "subcription"s

/// New messages that are posted by the modules are allocated in dynamic memory. Pointers to that memory enter this pipe
/// and are picked up by a message redistribution loop that takes care of them in a serial manner. Handling of each message
/// is supposed to be done very quickly - assuming the subscriber callbacks return quickly. In the future release, we expect
/// each subscriber callback to be called in a separate thread taken from a thread pool.
extern int post_data_pipe[2];

/// Initialize data structures maintained by the core.
void core_mato_init();

/// Release resources used by the core.
void core_mato_shutdown();

/// Enter mutually-exclusive area accessing internal framework data structures.
void lock_framework();

/// Leave mutually-exclusive area with a protected acces to internal framework data structures.
void unlock_framework();

/// Returns the next available module_id.
int get_free_module_id();

/// Returns the next available subscription_id.
int get_free_subscription_id(); // is not thread-safe

void remove_subscription(int subscribed_node_id, int subscribed_module_id, int channel, int subscription_id);

/// Decrements the number of references to a particular buffer and deallocates the buffer itself as well
/// as the structure describing the buffer. It also removes it from the list of the framework-maintained messages.
GList *decrement_references(GList *data_buffers, channel_data *to_be_decremented);

/// A constructor for the module_info structure.
module_info *new_module_info(int node_id, int module_id, char *module_name, char *module_type);

/// The number of mato system threads that are currently running (not terminated yet).
int mato_system_threads_running();

void remove_node_buffers(int node_id);
void remove_node_from_subscriptions(int node_id);
void remove_names_types(int node_id);

/// Update internal data structures as necessary when a new module announcement arrives from another node
void store_new_remote_module(int node_id, int module_id, char *module_name, char *module_type, int number_of_channels);

/// Increment the number of internal framework threads running. This should be called by each framework thread that has been started.
void mato_inc_system_thread_count();

/// Decrement the number of internal framework threads running. It should be called by each framework thread that terminates.
void mato_dec_system_thread_count();

/// Update internal subscriptions: a remote node subscribing to our module's channel
void subscribe_channel_from_remote_node(int remote_node_id, int subscribed_module_id, int channel);

/// Update internal subscriptions: a remote node unsubscribing to our module's channel
void unsubscribe_channel_from_remote_node(int remote_node_id, int subscribed_module_id, int channel);

/// Remote module is requesting data from our channel, retrieve them and send back
void pack_and_send_data_to_remote(int remote_node_id, int module_id, int channel, int get_data_id);

/// Remote node is sending data that some module of this node requested and waits for.
/// Notify it through pipe so that the data is sent down to the module.
void return_data_to_waiting_module(int get_data_id, int data_length, uint8_t *data);

/// From buffers, retrieve the most recent message from a specified module/channel, make a copy of it
/// and return the pointer to and size of the message in the *data_length and *data variables.
/// If there is no data posted by that module yet, both *data_length and *data will be 0.
void copy_of_last_data_of_channel(int node_id, int module_id, int channel, int *data_length, uint8_t **data);

/// From buffers, retrieve the most recent message from a specified module/channel, increase its
/// reference count and return the pointer to and size of the message in the *data_length and *data variables.
/// If there is no data posted by that module yet, both *data_length and *data will be 0.
void borrow_last_data_of_channel(int node_id, int module_id, int channel, int *data_length, uint8_t **data);

#endif
