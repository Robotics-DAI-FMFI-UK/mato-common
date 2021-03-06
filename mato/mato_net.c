#define _GNU_SOURCE

#include "mato.h"
#include "mato_net.h"
#include "mato_core.h"
#include "mato_logs.h"

/// \file mato_net.c
/// Implementation of the Mato control framework - networking with other nodes.

//--- declared and commented in mato_net.h
int32_t this_node_id;
GArray *nodes;
//---

/// Socket for accepting connections from other nodes.
static int listening_socket;

/// Communication sockets with all the other nodes.
static GArray *sockets;   // [node_id]

/// pipe for sending a signal to select() waiting on msgs from nodes - it has to be interrupted when new node
/// connects (or similar events occur)
static int select_wakeup_pipe[2];

//-------------- reading network config file ----------------------

/// Print error about reading config at the specified line
static int config_error(int ln)
{
    mato_log_val(ML_ERR, "Could not parse nodes config file, error at line", ln);
    return 0;
}

/// Node_info structure constructor, just fills newly allocated copies of data.
static node_info *new_node_info(int node_id, char *ip, int port, char *name, int is_online)
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

/// Reads mato networking config file.
static int read_mato_config()
{
    char config_line[256];
    int ln = 0;

    int node_id;
    char *ip;
    int port;
    char *name;

    FILE *f = fopen(NODES_CONFIG_FILENAME, "r");
    while (fgets(config_line, 255, f))
    {
        if (config_line[0] == '#') continue;
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

//-------------- init and shutdown ----------------------

void net_mato_init(int this_node_identifier)
{
    nodes = g_array_new(0, 0, sizeof(node_info *));
    sockets = g_array_new(0, 0, sizeof(int));
    this_node_id = this_node_identifier;
    if (!read_mato_config())
    {
        mato_log_val(ML_ERR, "Error loading nodes config file", errno);
        exit(1);
    }
}

void net_mato_shutdown()
{
    uint8_t wakeup_byte = 123;
    program_runs = 0;

    if (write(select_wakeup_pipe[1], &wakeup_byte, 1) < 0)
        mato_log_val(ML_ERR, "could not wakeup networking thread", errno);

    core_mato_shutdown();

    close(select_wakeup_pipe[0]);
    close(select_wakeup_pipe[1]);
    close(listening_socket);
}

/// Clean up all traces of a node (and its modules) after it got disconnected.
static void node_disconnected(int s, int node_id)
{
    g_array_index(nodes, node_info *, node_id)->is_online = 0;
    close(s);
    mato_log_val(ML_WARN, "node has disconnected", node_id);
    lock_framework();
        remove_node_buffers(node_id);
        remove_node_from_subscriptions(node_id);
        remove_names_types(node_id);
    unlock_framework();
}

//-------------- low-level incoming data receiving ---------------------

/// Receive one 32-bit integer from a socket. Returns 0 on failure (and the sending node is updated to be off-line), otherwise returns 1.
/// See net_send_int32_t() function.
static int net_recv_int32t(int s, int32_t *num, int sending_node_id)
{
    int retval = recv(s, num, sizeof(int32_t), MSG_WAITALL);
    if(retval<0)
    {
        mato_log_val(ML_ERR, "reading from socket", errno);
        return 0;
    }
    else if (retval == 0) // node disconnected
    {
        node_disconnected(s, sending_node_id);
        return 0;
    }
    return 1;
}

/// Receive string of bytes from a socket. This could be a zero-terminated string,
/// or any other bunch of bytes. String is sent as int32_t (length+1) and then data.
/// Returns 0 on failure (and the sending node is updated to be off-line), otherwise returns 1.
/// See net_send_string() function.
static int net_recv_bytes(int s, uint8_t **str, int32_t *str_len, int sending_node_id)
{
    if (!net_recv_int32t(s, str_len, sending_node_id))
        return 0;
    if (*str_len == 0)
    {
        *str = 0;
        return 1;
    }
    *str = (uint8_t *)malloc(*str_len);
    int retval = recv(s, *str, *str_len, MSG_WAITALL);
    if (retval < 0)
    {
        mato_log_val(ML_ERR, "reading from socket", errno);
        return 0;
    }
    else if (retval == 0) // node disconnected
    {
        node_disconnected(s, sending_node_id);
        return 0;
    }
    return 1;
}

//-------------- handling incoming messages ----------------------

/// Receive and process new module instance message from another node. For the packet format, see net_broadcast_new_module() function.
static void net_process_new_module(int s, int sending_node_id)
{
    int32_t module_id, number_of_channels, ignore;
    char *module_name, *module_type;
    if (
        !net_recv_int32t(s, &module_id, sending_node_id) ||
        !net_recv_bytes(s, (uint8_t **)&module_name, &ignore, sending_node_id)  ||
        !net_recv_bytes(s, (uint8_t **)&module_type, &ignore, sending_node_id)  ||
        !net_recv_int32t(s, &number_of_channels, sending_node_id)
    )
        return;
    store_new_remote_module(sending_node_id, module_id, module_name, module_type, number_of_channels);
}

/// Receive and process a delete module message from another node. For the packet format see net_send_delete_module() function.
static void net_process_delete_module(int s, int sending_node_id)
{
    int32_t module_id;
    if (
        !net_recv_int32t(s, &module_id, sending_node_id)
    )
        return;
    lock_framework();
        delete_module_instance(sending_node_id, module_id);
    unlock_framework();
}

/// Receive and process a subscribe channel message from another node. For the packet format see net_send_subscribe() function.
static void net_process_subscribe_module(int s, int sending_node_id)
{
    int32_t subscribed_module_id, channel;
    if (
        !net_recv_int32t(s, &subscribed_module_id, sending_node_id) ||
        !net_recv_int32t(s, &channel, sending_node_id)
    )
        return;
    subscribe_channel_from_remote_node(sending_node_id, subscribed_module_id, channel);
}

/// Receive and process a unsubscribe channel message from another node. For the packet format see net_send_subscribe() function.
static void net_process_unsubscribe_module(int s, int sending_node_id)
{
    int32_t subscribed_module_id, channel;
    if (
        !net_recv_int32t(s, &subscribed_module_id, sending_node_id) ||
        !net_recv_int32t(s, &channel, sending_node_id)
    )
        return;
    unsubscribe_channel_from_remote_node(sending_node_id, subscribed_module_id, channel);
}

/// Receive and process a get_data message from another node. For the packet format see net_send_get_data() function.
static void net_process_get_data(int s, int sending_node_id)
{
    int32_t module_id, channel, get_data_id;

    if (
        !net_recv_int32t(s, &module_id, sending_node_id) ||
        !net_recv_int32t(s, &channel, sending_node_id) ||
        !net_recv_int32t(s, &get_data_id, sending_node_id)
    )
        return;
    pack_and_send_data_to_remote(sending_node_id, module_id, channel, get_data_id);
}

/// Receive and process a data message that this node have requested by get_data message earlier. For the packet format see net_send_data() function.
static void net_process_data(int s, int sending_node_id)
{
    int32_t get_data_id;
    int32_t data_length;
    uint8_t *data;

    if (
       !net_recv_int32t(s, &get_data_id, sending_node_id) ||
       !net_recv_bytes(s, &data, &data_length, sending_node_id)
    )
       return;

    return_data_to_waiting_module(get_data_id, data_length, data);
}

/// Receive and process a subscribed data message from another node. For the packet format see net_send_subscribed_data() function.
static void net_process_subscribed_data(int s, int sending_node_id)
{
    int32_t sending_module_id, channel, data_length;
    uint8_t *data;
    if (
        !net_recv_int32t(s, &sending_module_id, sending_node_id) ||
        !net_recv_int32t(s, &channel, sending_node_id) ||
        !net_recv_bytes(s, &data, &data_length, sending_node_id)
    )
        return;
    mato_post_data(sending_module_id + sending_node_id * NODE_MULTIPLIER, channel, data_length, data);
}

/// Receive and process a global message from another node. For the packet format see net_send_global_message() function.
static void net_process_global_message(int s, int sending_node_id)
{
    int32_t sending_module_id, receiving_module_id, message_id, msg_length;
    uint8_t *message_data;
    if (
        !net_recv_int32t(s, &sending_module_id, sending_node_id) ||
        !net_recv_int32t(s, &receiving_module_id, sending_node_id) ||
        !net_recv_int32t(s, &message_id, sending_node_id) ||
        !net_recv_bytes(s, &message_data, &msg_length, sending_node_id)
    )
        return;
    if (receiving_module_id == MATO_BROADCAST)
        mato_send_global_message(sending_module_id, message_id, msg_length, message_data);
    else
        mato_send_message(sending_module_id, receiving_module_id, message_id, msg_length, message_data);
}

/// Receive, unpack, and process a new message arriving from another node from socket s.
static void process_node_message(int s, int sending_node_id)
{
    int32_t message_type;
    if (!net_recv_int32t(s, &message_type, sending_node_id))
        return;

    switch(message_type){
        case MSG_NEW_MODULE_INSTANCE:
            net_process_new_module(s, sending_node_id);
            break;
        case MSG_DELETED_MODULE_INSTANCE:
            net_process_delete_module(s, sending_node_id);
            break;
        case MSG_SUBSCRIBE:
            net_process_subscribe_module(s, sending_node_id);
            break;
        case MSG_UNSUBSCRIBE:
            net_process_unsubscribe_module(s, sending_node_id);
            break;
        case MSG_GET_DATA:
            net_process_get_data(s, sending_node_id);
            break;
        case MSG_DATA:
            net_process_data(s, sending_node_id);
            break;
        case MSG_SUBSCRIBED_DATA:
            net_process_subscribed_data(s, sending_node_id);
            break;
        case MSG_GLOBAL_MESSAGE:
            net_process_global_message(s, sending_node_id);
            break;
    }
}

/// Send information about all our modules to a specific node after it has been connected.
static void inform_about_our_modules(int node_id)
{
    lock_framework();
        int module_count = g_array_index(module_names, GArray *, this_node_id)->len;
        for (int module_id = 0; module_id < module_count; module_id++)
        {
            // TODO: this sometimes probably gets announced "double", if the node connects in a wrong sync
            net_send_new_module(node_id, module_id);
        }
    unlock_framework();
}

//-------------- the core of the networking ----------------------

/// This thread monitors connections to all nodes according to config file and tries to connect/reconnect
/// those that are not currently connected. It runs in the background from the start till the framework shutdown.
static void *reconnecting_thread(void *arg)
{
    struct sockaddr_in my_addr;

    mato_inc_system_thread_count("connect");

    while (program_runs)
    {
        for(int node_id = this_node_id + 1; node_id < nodes->len; node_id++)
        {
            if (g_array_index(nodes, node_info*, node_id)->is_online == 0)
            {
                int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s < 0)
                {
                    mato_log_val(ML_ERR, "could not create socket", errno);
                    return 0;
                }
                char *IP = g_array_index(nodes, node_info*, node_id)->IP;
                int port = g_array_index(nodes, node_info*, node_id)->port;
                memset(&my_addr, 0, sizeof(struct sockaddr_in));
                my_addr.sin_family = AF_INET;
                my_addr.sin_port = htons(port);
                if (inet_pton(AF_INET, IP, &my_addr.sin_addr) <= 0)
                {
                    mato_log_str_val(ML_ERR, "Invalid ip address", IP, port);
                    continue;
                }
                if (connect(s, (struct sockaddr *)&my_addr, sizeof(struct sockaddr_in)) < 0)
                    continue;
                int retval = send(s, &this_node_id, sizeof(int32_t), 0);
                if (retval < 0)
                {
                    mato_log_val(ML_ERR, "Could not send this node id", errno);
                    close(s);
                    continue;
                }
                g_array_index(sockets, int, node_id) = s;
                g_array_index(nodes, node_info*, node_id)->is_online = 1;
                mato_log_val(ML_INFO, "connected to ", node_id);

                uint8_t wakeup_byte = 123;
                if (write(select_wakeup_pipe[1], &wakeup_byte, 1) < 0)
                    mato_log_val(ML_ERR, "could not wakeup networking thread", errno);

                inform_about_our_modules(node_id);
                continue;
            }
        }
        //for (int i = 0; i < 10; i++)
        //    if (!program_runs) break;
                sleep(1);
    }
    mato_dec_system_thread_count();
    return 0;
}

/// Fills the set of "read" file descriptor sets for the communication_thread() function.
/// Includes the listening_socket, read end of the select_wakeup_pipe, and all open sockets with the other connected nodes.
static void fill_select_fd_set(fd_set *rfds, int *nfd)
{
    FD_ZERO(rfds);
    for(int i = 0; i < nodes->len; i++)
    {
        if (i == this_node_id) continue;
        if (g_array_index(nodes,node_info*, i)->is_online == 1)
        {
            int s = g_array_index(sockets, int, i);
            FD_SET(s, rfds);
            if (s > *nfd) *nfd = s;
        }
    }
    FD_SET(listening_socket, rfds);
    if (listening_socket > *nfd) *nfd = listening_socket;
    FD_SET(select_wakeup_pipe[0], rfds);
    if (select_wakeup_pipe[0] > *nfd) *nfd = select_wakeup_pipe[0];
    (*nfd)++;
}

/// In case the listening socket is selected in rfds, accept the new connection,
/// retrieve the login packet from the other node, store its socket, and mark it online.
static void handle_incomming_connections(fd_set *rfds)
{
    if (FD_ISSET(listening_socket, rfds))
    {
        struct sockaddr_in incomming;
        socklen_t size = sizeof(struct sockaddr_in);
        int s = accept(listening_socket, (struct sockaddr *)&incomming, &size);
        int32_t new_node_id;
        int retval = recv(s, &new_node_id, sizeof(int32_t), MSG_WAITALL);
        if(retval < 0)
        {
            mato_log_val(ML_ERR, "reading from socket", errno);
            return;
        }
        mato_log_val(ML_INFO, "connection from node", new_node_id);
        g_array_index(sockets, int, new_node_id) = s;
        g_array_index(nodes, node_info*, new_node_id)->is_online = 1;
        inform_about_our_modules(new_node_id);
    }
}

/// In case a notifications from other threads through wakeup pipe arrives, pick it up.
static void handle_wakeup_signal(fd_set *rfds)
{
    if (FD_ISSET(select_wakeup_pipe[0], rfds))
    {
        uint8_t b;
        read(select_wakeup_pipe[0], &b, 1);
    }
}

/// For the messages received from other nodes: process them.
static void handle_messages_from_other_nodes(fd_set *rfds)
{
    for(int i = 0; i < nodes->len; i++)
        if (g_array_index(nodes,node_info*,i)->is_online == 1)
        {
            int s = g_array_index(sockets, int, i);
            if (FD_ISSET(s, rfds))
                process_node_message(s, i);
        }
}

/// Call the select() OS system call to wait for any communication arriving.
static int wait_on_select(fd_set *rfds, int nfd)
{
    int retval = select(nfd, rfds, 0, 0, 0);
    if (retval < 0)
    {
        mato_log_val(ML_ERR, "Select error", errno);
        return 0;
    }
    return 1;
}

/// The network receiving thread: uses select() to wait for data arriving from all nodes including new nodes connecting.
/// Dispatches the arriving messages to the process_node_message() function.
static void *communication_thread(void *arg)
{
    mato_inc_system_thread_count("comm");
    fd_set rfds;
    int nfd = 0;
    while (program_runs){

        fill_select_fd_set(&rfds, &nfd);
        if (
            !wait_on_select(&rfds, nfd)
        ) break;
        handle_wakeup_signal(&rfds);
        if (!program_runs) break;
        handle_incomming_connections(&rfds);
        handle_messages_from_other_nodes(&rfds);
    }
    mato_dec_system_thread_count();
    return 0;
}

void start_networking()
{
    struct sockaddr_in my_addr;

    listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_socket == -1)
       mato_log_val(ML_ERR, "socket", errno);

    memset(&my_addr, 0, sizeof(struct sockaddr_in));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(g_array_index(nodes,node_info*,this_node_id)->port);

    int i = 0;
    do {
        if (bind(listening_socket, (struct sockaddr *) &my_addr, sizeof(struct sockaddr_in)) == -1)
            mato_log_val(ML_WARN, "bind, will retry...", errno);
        else break;
        sleep(6);
    } while (program_runs && (++i < 20));
    if (i >= 20) exit(1);

    int rv = listen(listening_socket, MAX_PENDINGS_CONNECTIONS);
    if (rv < 0)
    {
        mato_log_val(ML_ERR, "listen", errno);
        return;
    }

    if (pipe(select_wakeup_pipe) < 0)
    {
        mato_log_val(ML_ERR, "could not create pipe for select", errno);
        return;
    }

    pthread_t t;
    if (pthread_create(&t, 0, reconnecting_thread, 0) != 0)
          mato_log_val(ML_ERR, "could not create reconnecting thread for framework", errno);
    if (pthread_create(&t, 0, communication_thread, 0) != 0)
          mato_log_val(ML_ERR, "could not create communication thread for framework", errno);
}

//-------------- low-level outgoing data sending ----------------------

/// Send a zero-terminated character string to socket.
/// First send its length+1 as int32_t and then data.
/// See net_send_bytes() and net_recv_bytes() functions.
static int net_send_string(int socket, char *str)
{
    int32_t len = strlen(str) + 1;
    if (write(socket, &len, sizeof(int32_t)) < 0)
        return 0;
    if (write(socket, str, len) < 0)
        return 0;
    return 1;
}

/// Send an array of bytes to socket.
/// First send its length as int32_t and then data.
/// See net_send_string() and net_recv_bytes() functions.
static int net_send_bytes(int socket, uint8_t *data, int32_t length)
{
    if (write(socket, &length, sizeof(int32_t)) < 0)
        return 0;
    if (length > 0)
        if (write(socket, data, length) < 0)
            return 0;
    return 1;
}

/// Send 32-bit signed integer to a socket.
/// See net_recv_int32_t() function.
static int net_send_int32t(int socket, int32_t num)
{
    if (write(socket, &num, sizeof(int32_t)) < 0)
        return 0;
    else return 1;
}

//-------------- outgoing messages -------------------------

void net_send_data(int node_id, int get_data_id, uint8_t *data, int32_t data_length)
{
    int socket = g_array_index(sockets, int, node_id);

    if (
      !net_send_int32t(socket, MSG_DATA)  ||
      !net_send_int32t(socket, get_data_id) ||
      !net_send_bytes(socket, data, data_length)
    )
    {
        node_disconnected(socket, node_id);
    }
}

void net_send_subscribed_data(int subscribed_node_id, channel_data *cd)
{
    int socket = g_array_index(sockets, int, subscribed_node_id);

    if (
        !net_send_int32t(socket, MSG_SUBSCRIBED_DATA)  ||
        !net_send_int32t(socket, cd->module_id) ||
        !net_send_int32t(socket, cd->channel_id) ||
        !net_send_bytes(socket, cd->data, cd->length)
    )
    {
        node_disconnected(socket, subscribed_node_id);
    }
}

void net_broadcast_new_module(int module_id)
{
    for (int node_id = 0; node_id < nodes->len; node_id++)
    {
        if (node_id == this_node_id) continue;
        node_info *ni = g_array_index(nodes, node_info *, node_id);
        if (ni->is_online == 0) continue;
        net_send_new_module(node_id, module_id);
    }
}

void net_send_new_module(int node_id, int module_id)
{
    int s = g_array_index(sockets, int, node_id);
    char *module_name = g_array_index(g_array_index(module_names, GArray *, this_node_id), char *, module_id);
    char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
    module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
    int32_t number_of_channels = spec->number_of_channels;

    if (
        !net_send_int32t(s, MSG_NEW_MODULE_INSTANCE)  ||
        !net_send_int32t(s, module_id) ||
        !net_send_string(s, module_name) ||
        !net_send_string(s, module_type) ||
        !net_send_int32t(s, number_of_channels)
    )
    {
        node_disconnected(s, node_id);
    }
}

void net_send_get_data(int node_id, int module_id, int channel, int get_data_id)
{
    int s = g_array_index(sockets, int, node_id);
    if (
      !net_send_int32t(s, MSG_GET_DATA)  ||
      !net_send_int32t(s, module_id) ||
      !net_send_int32t(s, channel) ||
      !net_send_int32t(s, get_data_id)
    )
    {
        node_disconnected(s, node_id);
    }
}

void net_send_delete_module(int module_id)
{
    for (int node_id = 0; node_id < nodes->len; node_id++)
    {
        if (node_id == this_node_id) continue;
        node_info *ni = g_array_index(nodes, node_info *, node_id);
        if (ni->is_online == 0) continue;

        int s = g_array_index(sockets, int, node_id);

        if (
            !net_send_int32t(s, MSG_DELETED_MODULE_INSTANCE)  ||
            !net_send_int32t(s, module_id)
        )
        {
            node_disconnected(s, node_id);
        }
    }
}

void net_send_subscribe(int node_id, int module_id, int channel)
{
    int s = g_array_index(sockets, int, node_id);
    if (
      !net_send_int32t(s, MSG_SUBSCRIBE)  ||
      !net_send_int32t(s, module_id) ||
      !net_send_int32t(s, channel)
    )
    {
        node_disconnected(s, node_id);
    }
}

void net_send_unsubscribe(int node_id, int module_id, int channel)
{
    int s = g_array_index(sockets, int, node_id);
    if (
      !net_send_int32t(s, MSG_UNSUBSCRIBE)  ||
      !net_send_int32t(s, module_id) ||
      !net_send_int32t(s, channel)
    )
    {
        node_disconnected(s, node_id);
    }
}

void net_send_global_message(int sending_module_id, int message_id, uint8_t *message_data, int message_length)
{
    lock_framework();
        for (int node_id = 0; node_id < nodes->len; node_id++)
        {
            if (node_id == this_node_id) continue;
            node_info *ni = g_array_index(nodes, node_info *, node_id);
            if (ni->is_online == 0) continue;
            int s = g_array_index(sockets, int, node_id);

            if (
                !net_send_int32t(s, MSG_GLOBAL_MESSAGE) ||
                !net_send_int32t(s, sending_module_id) ||
                !net_send_int32t(s, MATO_BROADCAST) ||
                !net_send_int32t(s, message_id) ||
                !net_send_bytes(s, message_data, message_length)
            )
            {
                node_disconnected(s, node_id);
            }
        }
    unlock_framework();
}

void net_send_message(int sending_module_id, int receiving_node_id, int module_id_receiver, int message_id, uint8_t *message_data, int message_length)
{
    lock_framework();
        node_info *ni = g_array_index(nodes, node_info *, receiving_node_id);
        if (ni->is_online == 0) return;

        int s = g_array_index(sockets, int, receiving_node_id);

        if (
            !net_send_int32t(s, MSG_GLOBAL_MESSAGE) ||
            !net_send_int32t(s, sending_module_id) ||
            !net_send_int32t(s, module_id_receiver) ||
            !net_send_int32t(s, message_id) ||
            !net_send_bytes(s, message_data, message_length)
        )
        {
            node_disconnected(s, receiving_node_id);
        }
    unlock_framework();
}
