#define _GNU_SOURCE

#include "mato.h"
#include "mato_net.h"
#include "mato_core.h"

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

static int config_error(int ln)
{
    printf("Could not parse nodes config file, error at line %d\n", ln);
    return 0;
}

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

static int read_mato_config()
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

void net_mato_init(int this_node_identifier)
{
    nodes = g_array_new(0, 0, sizeof(node_info *));
    sockets = g_array_new(0, 0, sizeof(int));
    this_node_id = this_node_identifier;
    if (!read_mato_config())
    {
        printf("Error loading nodes config file\n");
        exit(1);
    }
}

void net_mato_shutdown()
{
    uint8_t wakeup_byte;
    program_runs = 0;

    if (write(select_wakeup_pipe[1], &wakeup_byte, 1) < 0)
        perror("could not wakeup networking thread");

    core_mato_shutdown();

    close(select_wakeup_pipe[0]);
    close(select_wakeup_pipe[1]);
    close(listening_socket);
}

void node_disconnected(int s, int node_id)
{
    g_array_index(nodes,node_info*,node_id)->is_online = 0;
    close(s);
    printf("node %d has disconnected\n", node_id);
    lock_framework();
        remove_node_buffers(node_id);
        remove_node_from_subscriptions(node_id);
        remove_names_types(node_id);
    unlock_framework();
}

/// Receive one 32-bit integer from a socket. Returns 0 on failure (and the sending node is updated to be off-line), otherwise returns 1.
/// See net_send_int32_t() function.
int net_recv_int32t(int s, int32_t *num, int sending_node_id)
{
    int retval = recv(s, num, sizeof(int32_t), MSG_WAITALL);
    if(retval<0)
    {
        perror("reading from socket");
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
int net_recv_bytes(int s, uint8_t **str, int32_t *str_len, int sending_node_id)
{

    if (!net_recv_int32t(s, str_len, sending_node_id))
        return 0;
    *str = (uint8_t *)malloc(*str_len);
    int retval = recv(s, *str, *str_len, MSG_WAITALL);
    if(retval<0)
    {
        perror("reading from socket");
        return 0;
    }
    else if (retval == 0) // node disconnected
    {
        node_disconnected(s, sending_node_id);
        return 0;
    }
    return 1;
}

/// Receive and process new module instance message from another node. For the packet format, see net_announce_new_module() function.
void net_process_new_module(int s, int sending_node_id)
{
    int32_t node_id, module_id, number_of_channels, ignore;
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

void net_process_delete_module(int s, int sending_node_id)
{
    int32_t node_id, module_id;
    if (
      !net_recv_int32t(s, &module_id, sending_node_id)
    )
        return;
    lock_framework();
        net_delete_module_instance(sending_node_id, module_id);
    unlock_framework();
}

void net_delete_module_buffers(int node_id, int module_id)
{
    GArray *module_buffers = g_array_index(g_array_index(buffers,GArray *,node_id), GArray *, module_id);
    int number_of_channels = module_buffers -> len;
    for (int i = 0; i < number_of_channels; i++)
    {
        GList *channel_list = g_array_index(module_buffers, GList *, i);
        if (channel_list)
        {
            channel_list = decrement_references(channel_list, (channel_data *)(channel_list->data));
            if (channel_list==0)
            g_array_index(module_buffers, GList *, i) = 0;
        }
    }

    g_array_index(g_array_index(module_names, GArray *, node_id), char *, module_id) = 0;
    g_array_index(g_array_index(module_types, GArray *, node_id), char *, module_id) = 0;
}

void net_delete_module_instance(int node_id, int module_id)
{
    // refactorization lossess...
}

void net_subscribe(int sending_node_id, int subscribed_module_id, int channel)
{
    subscription *new_subscription = (subscription *)malloc(sizeof(subscription));
    new_subscription->type = data_copy;
    new_subscription->callback = 0;
    new_subscription->subscriber_module_id = 0;
    new_subscription->subscriber_node_id = sending_node_id;
    lock_framework();
        new_subscription->subscription_id = get_free_subscription_id();
        GArray *channel_subscriptions = g_array_index(g_array_index(g_array_index(subscriptions, GArray *, this_node_id), GArray *, subscribed_module_id), GArray *, channel);
        g_array_append_val(channel_subscriptions, new_subscription);
    unlock_framework();
}

void net_process_subscribe_module(int s, int sending_node_id)
{
    int32_t subscribed_module_id, channel;
    char *module_name, *module_type;
    subscriber_callback callback;
    if (
      !net_recv_int32t(s, &subscribed_module_id, sending_node_id) ||
      !net_recv_int32t(s, &channel, sending_node_id)
    )
        return;
    net_subscribe(sending_node_id, subscribed_module_id, channel);
}

void net_unsubscribe(int sending_node_id, int subscribed_module_id, int channel)
{
    lock_framework();
        GArray *subscriptions_for_channel = g_array_index(g_array_index(g_array_index(subscriptions, GArray *,this_node_id), GArray *, subscribed_module_id), GArray *, channel);
        int number_of_channel_subscriptions = subscriptions_for_channel->len;
        for (int i = 0; i < number_of_channel_subscriptions; i++)
        {
            subscription *s = (subscription *)g_array_index(subscriptions_for_channel, GArray *, i);
            if (s->subscriber_node_id == sending_node_id)
            {
                free(s);
                g_array_remove_index_fast(subscriptions_for_channel, i);
                break;
            }
        }
    unlock_framework();
}

void net_process_unsubscribe_module(int s, int sending_node_id)
{
    int32_t subscribed_module_id, channel;
    if (
      !net_recv_int32t(s, &subscribed_module_id, sending_node_id) ||
      !net_recv_int32t(s, &channel, sending_node_id)
    )
        return;
    net_unsubscribe(sending_node_id, subscribed_module_id, channel);
}

void net_get_data(int sending_node_id, int module_id, int channel, int get_data_id)
{
    lock_framework();
        GList *waiting_buffers = g_array_index(g_array_index(g_array_index(buffers, GArray *,this_node_id), GArray *, module_id), GList *, channel);
        if (waiting_buffers == 0)
        {
    unlock_framework();
          //  TODO
          //  *data_length = 0;
          //  *data = 0;
            return;
        }
        channel_data *cd = (channel_data *)(waiting_buffers->data);

        // TODO
        //*data_length = cd->length;
        //*data = malloc(cd->length);
        //memcpy(*data, cd->data, cd->length);
    unlock_framework();
    return;
}

void net_process_get_data(int s, int sending_node_id)
{
    int32_t module_id, channel, get_data_id;

    if (
      !net_recv_int32t(s, &module_id, sending_node_id) ||
      !net_recv_int32t(s, &channel, sending_node_id) ||
      !net_recv_int32t(s, &get_data_id, sending_node_id)
    )
        return;
    net_get_data(sending_node_id, module_id, channel, get_data_id);
}

void net_process_subscribed_data(int s, int sending_node_id)
{
    int32_t sending_module_id, channel, data_length;
    uint8_t *data;
    if (
      !net_recv_int32t(s, &sending_module_id, sending_node_id) ||
      !net_recv_int32t(s, &channel, sending_node_id) ||
      !net_recv_int32t(s, &data_length, sending_node_id) ||
      !net_recv_bytes(s, &data, &data_length, sending_node_id)
    )
        return;
    mato_post_data(sending_module_id + sending_node_id * NODE_MULTIPLIER, channel, data_length, data);
}

void net_process_global_message(int s, int sending_node_id)
{
    int32_t sending_module_id, message_id, msg_length;
    uint8_t *message_data;
    if (
      !net_recv_int32t(s, &sending_module_id, sending_node_id) ||
      !net_recv_int32t(s, &message_id, sending_node_id) ||
      !net_recv_bytes(s, &message_data, &msg_length, sending_node_id)
    )
        return;
    mato_send_global_message(sending_module_id+sending_node_id*NODE_MULTIPLIER, message_id, msg_length, message_data);
}

/// Receive, unpack, and process a new message arriving from another node from socket s.
void process_node_message(int s, int sending_node_id)
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
        case MSG_SUBSCRIBED_DATA:
            net_process_subscribed_data(s, sending_node_id);
            break;
        case MSG_GLOBAL_MESSAGE:
            net_process_global_message(s, sending_node_id);
            break;
    }
}

/// This thread monitors connections to all nodes according to config file and tries to connect/reconnect
/// those that are not currently connected. It runs in the background from the start till the framework shutdown.
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

/// Send a zero-terminated character string to socket.
/// First send its length+1 as int32_t and then data.
/// See net_recv_bytes() function.
int net_send_string(int socket, char *str)
{
    int32_t len = strlen(str) + 1;
    if (write(socket, &len, sizeof(int32_t)) < 0)
        return 0;
    if (write(socket, str, len) < 0)
        return 0;
    return 1;
}

int net_send_bytes(int socket, uint8_t *data, int32_t length)
{
    if (write(socket, &length, sizeof(int32_t)) < 0)
        return 0;
    if (write(socket, data, length) < 0)
        return 0;
    return 1;
}

/// Send 32-bit signed integer to a socket.
/// See net_recv_int32_t() function.
int net_send_int32t(int socket, int32_t num)
{
    if (write(socket, &num, sizeof(int32_t)) < 0)
        return 0;
    else return 1;
}

void net_send_data(int node_id, int get_data_id, uint8_t *data, int32_t data_length)
{
    int socket = g_array_index(sockets, int, node_id);

    if (
      !net_send_int32t(socket, MSG_DATA)  ||
      !net_send_int32t(socket, this_node_id) ||
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
      !net_send_int32t(socket, this_node_id) ||
      !net_send_int32t(socket, cd->module_id) ||
      !net_send_int32t(socket, cd->channel_id) ||
      !net_send_bytes(socket, cd->data, cd->length)
    )
    {
        node_disconnected(socket, subscribed_node_id);
    }
}

void net_announce_new_module(int module_id)
{
    for (int node_id = 0; node_id < nodes->len; node_id++)
    {
        if (node_id == this_node_id) continue;
        node_info *ni = g_array_index(nodes, node_info *, node_id);
        if (ni->is_online == 0) continue;

        int s = g_array_index(sockets, int, node_id);
        char *module_name = g_array_index(g_array_index(module_names, GArray *, this_node_id), char *, module_id);
        char *module_type = g_array_index(g_array_index(module_types, GArray *, this_node_id), char *, module_id);
        module_specification *spec = (module_specification *)g_hash_table_lookup(module_specifications, module_type);
        int32_t number_of_channels = spec->number_of_channels;

        if (
          !net_send_int32t(s, MSG_NEW_MODULE_INSTANCE)  ||
          !net_send_int32t(s, this_node_id) ||
          !net_send_int32t(s, module_id) ||
          !net_send_string(s, module_name) ||
          !net_send_string(s, module_type) ||
          !net_send_int32t(s, number_of_channels)
        )
        {
            node_disconnected(s, node_id);
        }
    }
    printf("announced new module %d to other nodes\n", module_id);
}
