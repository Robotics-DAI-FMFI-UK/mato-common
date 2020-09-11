#ifndef __MATO_H__
#define __MATO_H__

#include <glib.h>

/// \file mato.h
/// Mato control framework - main header file. Declarations of the public interface of the framework.
/// The framework manages the data flow between modules of the control architecture.
/// Modules have types and names. There can be multiple instances of the same type of module, having different names.
/// Module instances should only keep their local data inside of one structure that is created for each
/// instance (we call it "instance data").
/// Each module type contains its "init()" function that registers the module in the framework so that it can
/// be used by the main program and other modules.
/// Each module type is specified by the structure module_specification that lists the public interface of
/// the module (used by the framework):
///   * create_instance_callback - creates a new instance of the module, i.e. allocates and initializes
///     the module instance data, it is called by the framework when the main program or another module
///     requests to create a new module instance
///   * start_instance_callback  - all modules are started by the framework by calling this function
///     at once, typically after all instances have been created
///   * delete_instance_callback - it is called by the framework when the main program or another module
///     reqyests to delete the module instance
///   * global_message_callback - global messages can be sent by anybody and they are immediatelly forwarded
///     to the callback functions of all the modules
///  Modules communicate by posting data in their output channels. Other modules can request the most recent
///  data that have been posted by any module, or subscribe to receive all posted data immediatelly in their
///  subscription callback function. The framework gives the possiblity to list all module types and all modules
///  that are present in the system. Modules can be added and removed dynamically while the system runs.
///  When sending a message, modules should ask the framework for the memory buffer that will be filled by
///  the data of the message the module is sending. The framework deallocates the data buffers when they are
///  not needed anymore automatically, unless the copy_mode is specified.

typedef enum subscription_type_enum {direct_data_ptr = 1, data_copy = 2, borrowed_pointer = 3} subscription_type;

/// Create instance data of a module and initialize it. Each module should define this callback.
/// This instance will be from now on always referred by the module_id passed in the argument.
/// It is recommended that the module saves it to its instance_data. The function should return
/// a pointer to the newly allocated instance data.
typedef void * (* create_instance_callback)(int module_id);

/// Start the module instance. Typically, a module instance will create a new thread that will take care
/// of the functionality of the module (active module). Alternately, the module can remain passive - without a thread
/// and only responding to the subscribes... This is also the right place for the module to subscribe to the data
/// channels of other module instances, because they are expected to be instantiated at this point of time
typedef void (* start_instance_callback)(void *instance_data);

/// A request to delete the module instance has been made. This callback should deallocate the instance data.
typedef void (* delete_instance_callback)(void *instance_data);

/// When some other module or the main program has sent a global message, this callback of each module is called
/// immediatelly - from the thread of the sender.
typedef void (* global_message_callback)(void *instance_data, int module_id_sender, int message_id, int msg_length, void *message_data);

/// Modules that subscribe to the data on channels of other module instances provide this callback. It receives
/// the instance data of the subscriber, the id of the sender module, and the actual raw data of the message and its length
typedef void (* subscriber_callback)(void *instance_data, int sender_module_id, int data_length, void *new_data_ptr);

/// Each module type has to provide the callbacks for its module instances.
typedef struct {
        create_instance_callback create_instance;
        start_instance_callback start_instance;
        delete_instance_callback delete_instance;
        global_message_callback global_message;
        int number_of_channels;
} module_specification;

/// List of modules descriptions consists of structures describing the basic information about a module.
typedef struct {
        int module_id;
        char *name;
        char *type;
        int node_id;
        int number_of_channels;
} module_info;


// in config file:
// node_id,IP,port,name
typedef struct {
    int node_id;
    char *IP;
    int port;
    char *name;
    int is_online;
} node_info;

/// Initializes the framework. It must be the first function of the framework to be called.
/// It should be called only once. When in distributed environment, the user should assign integers to its nodes,
/// counting from 0, and each main program should pass its node id to the init function. In a single-machine mode, just pass 0 in the argument.
void mato_init(int this_node_identifier);

/// Register a new module type. This is typically called from the init() function of each module type, which
/// should be called directly from the main program.
void mato_register_new_type_of_module(char *type, module_specification *specification);

/// The main program (or other module) can call this function to request creation of a module instance.
/// The function returns the new module id.
int mato_create_new_module_instance(const char *module_type, const char *module_name);

/// Start the framework and all modules. This is typically called after all module instances have been created.
void mato_start();

/// Start a newly created instance of a particular module. This is typically used when the module instance
/// is created later than when the framework has already been started.
void mato_start_module(int module_id);

/// Allows to dynamically delete a module instance that was previously created.
void mato_delete_module_instance(int module_id);

/// Translate a module name of some existing module instance to its module id. Returns -1, if module name is not known.
int mato_get_module_id(const char *module_name);

/// Determine the name of a module with the specified module_id.
char *mato_get_module_name(int module_id);

/// Determine the type of a module with the specified module_id.
char *mato_get_module_type(int module_id);

/// Subscribe on a channel of some module instance. Returns a number that represents this subscription (a subscription_id).
int mato_subscribe(int subscriber_module_id, int subscribed_module_id, int channel, subscriber_callback callback, int subscription_type);

/// Given a subscription_id, cancel the ongoing subscription to a channel of some module instance.
void mato_unsubscribe(int module_id, int channel, int subscription_id);

/// Allocate a memory for a new message to be posted with post_data.
void *mato_get_data_buffer(int size);

/// A module instance posts a new message to its own output data channel by calling this function.
void mato_post_data(int id_of_posting_module, int channel, int data_length, void *data);

/// The main program or any module instance can post a global message to be posted to all modules immediatelly in the same
/// thread by calling this function. To send a global message from the main program, use mato_main_program_module_id().
int mato_send_global_message(int module_id_sender, int message_id, int msg_length, void *message_data);

/// The main program or any module instance can post a message targeted on a single module, it will be immediatelly
/// handled by the same thread in the receiving module's global message handler.
int mato_send_message(int module_id_sender, int module_id_receiver, int message_id, int msg_length, void *message_data);

/// Retrieve the most recently posted data of some channel of some module instance. Data is copied into new buffer allocated and its pointer is returned in data variable,
/// and the length of the data is set to data_length. The calling module should release the memory by calling free() when
/// it is not needed anymore.
void mato_get_data(int id_module, int channel, int *data_length, void **data);

/// Retrieve the most recently posted data of some channel of some module instance. In this case, the data is not copied,
/// but a pointer to read-only memory containing the data is provided. The module should return the borrowed pointer
/// back to the framework by calling mato_release_data() when the data is not needed anymore.
void mato_borrow_data(int id_module, int channel, int *data_length, void **data);

/// Return a borrowed pointer that was obtained either by mato_borrow_data() or by a callback in the borrowed_pointer mode.
/// The id_module and channel specify the origin of the message.
void mato_release_data(int id_module, int channel, void *data);

/// Retrieve the list of currently running modules.
/// The list should be freed by calling mato_free_list_of_modules() when
/// it is not needed anymore.
GArray* mato_get_list_of_all_modules();

/// Retrieve the list of currently running modules of the specified type.
/// The list should be freed by calling mato_free_list_of_modules() when
/// it is not needed anymore.
GArray* mato_get_list_of_modules(char *type);

/// Retrieve the number of currently running modules
int mato_get_number_of_modules();

/// Free the list of modules returned by either of the two functions mato_get_list_of_modules() or mato_get_list_of_modules().
void mato_free_list_of_modules(GArray* a);

/// Provide a diagnostic information on use of the buffers by a particular channel of a particular module. If this
/// starts growing, the callbacks take too much time to process data and should be refactored.
void mato_data_buffer_usage(int module_id, int channel, int *number_of_allocated_buffers, int *total_sum_of_ref_count);

/// Each module instance (or other part of the program) that creates a new thread should call this funciton for each newly
/// created thread. Increments the number of threads running. This should be called from each thread that has been started.
/// The short thread name provided is associated with the thread id and can later be retrieved by this_thread_name() function.
void mato_inc_thread_count(char *short_thread_name);

/// Each thread that terminates should call this function just before it quits.
/// Decrement the number of threads running. It should be called by each thread that terminates.
void mato_dec_thread_count();

/// Retrieve the name of the thread that is calling the function that was previously registered 
/// with mato_inc_thread_count() function. If the thread that is calling has not been registered, 
/// it will return the string "noname"
char *this_thread_name();

/// The number of threads currently running in the system
int mato_threads_running();

/// Releases all resources used by the framework, recommended to be called before the main program terminates
void mato_shutdown();

/// Returns a module id for the "main program module", which can be used in mato_send_global_message() function.
int mato_main_program_module_id();

/// This variable is set to non-zero when the program is about to terminate.
/// All threads should terminate at the earliest possbility
extern volatile int program_runs;

#endif
