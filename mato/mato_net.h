#ifndef __MATO_NET_H__
#define __MATO_NET_H__

/// \file mato_net.h
/// Mato control framework - internal networking definitions.

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mato_core.h"

#define MAX_PENDINGS_CONNECTIONS 10

// messages of node to node communication protocol
#define MSG_NEW_MODULE_INSTANCE 1
#define MSG_DELETED_MODULE_INSTANCE 2
#define MSG_SUBSCRIBE 3
#define MSG_UNSUBSCRIBE 4
#define MSG_GET_DATA 5
#define MSG_DATA 6
#define MSG_SUBSCRIBED_DATA 7
#define MSG_GLOBAL_MESSAGE 8

/// Id of this computational node.
extern int32_t this_node_id;

///Contains node info for all computational nodes filled from config file in mato_init.
extern GArray *nodes;   // [node_id]

/// Initialize data structures maintained by the networking.
void net_mato_init();

/// Close and release resources used by the networking.
void net_mato_shutdown();

/// Create listening socket for other nodes, start reconnecting and communication threads.
void start_networking();

/// Broadcast information about new local module instance to all other nodes.
///
/// Packet format:
/// -------------------------------------
/// MSG_NEW_MODULE_INSTANCE   int32
/// this_node_id              int32
/// [local]module_id          int32
/// len(module_name)+1        int32
/// module_name               variable
/// len(module_type)+1        int32
/// module_type               variable
/// number_of_channels        int32
/// -------------------------------------
void net_announce_new_module(int module_id);

/// Broadcast information about new local module instance to all other nodes.
///
/// Packet format:
/// -------------------------------------
/// MSG_SUBSCRIBED_DATA       int32
/// sending_node_id           int32
/// module_id                 int32
/// channel                   int32
/// length                    int32
/// data                      variable
/// -------------------------------------
void net_send_subscribed_data(int subscribed_node_id, channel_data *cd);

/// Broadcast information about new local module instance to all other nodes.
///
/// Packet format:
/// -------------------------------------
/// MSG_DATA                  int32
/// this_node_id              int32
/// get_data_id               int32
/// len(data)                 int32
/// data                      variable
/// -------------------------------------
void net_send_data(int node_id, int get_data_id, uint8_t *data, int32_t data_length);

void net_delete_module_instance(int node_id, int module_id);

#endif
