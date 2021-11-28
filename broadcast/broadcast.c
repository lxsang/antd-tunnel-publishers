#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <antd/bst.h>
#include <antd/utils.h>
#include <antd/list.h>
#include "../tunnel.h"

#define BC_ERROR(r, fd, c, ...)                         \
    do                                                  \
    {                                                   \
        r.header.client_id = c;                         \
        r.header.type = CHANNEL_ERROR;                  \
        (void)snprintf(r.data, BUFFLEN, ##__VA_ARGS__); \
        r.header.size = strlen(r.data);                 \
        M_ERROR(MODULE_NAME, "%s", r.data);             \
        (void)msg_write(fd, &r);                        \
    } while (0)

#define MODULE_NAME "broadcast"

#define BC_SUBSCRIPTION 0x0A
#define BC_UNSUBSCRIPTION 0x0B
#define BC_QUERY_USER 0x0C
#define BC_QUERY_GROUP 0x0D

#define MAX_STR_LEN 255

typedef struct
{
    int ref;
    char name[MAX_STR_LEN];
    bst_node_t *groups;
} bc_client_t;

/**
 * @brief Send notified to client.
 * 
 * @param type 
 * @param bc_client broadcast client handle
 * @param groupname group name
 */
static void bc_notify(bst_node_t *node, void **argv, int argc);
static void int_handler(int dummy);
static void bc_get_handle(bst_node_t *node, void **argv, int argc);
static void bc_unsubscription(bst_node_t *node, void **args, int argc);
static void unsubscribe(bst_node_t *node, void **args, int argc);
static void bc_send_query_user(bst_node_t *node, void **argv, int argc);
static void bc_send_query_group(bst_node_t *node, void **argv, int argc);

static bst_node_t *clients = NULL;
static uint8_t msg_buffer[BUFFLEN];
static volatile int running = 1;

static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
}

static void bc_get_handle(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    bc_client_t **bc_client = (bc_client_t **)argv[1];
    char *name = (char *)argv[2];
    bc_client_t *node_client = (bc_client_t *)node->data;
    if (bc_client == NULL || name == NULL || node->data == NULL)
    {
        return;
    }
    M_DEBUG(MODULE_NAME, "comparing %s vs %s", name, node_client->name);
    if (strcmp(name, node_client->name) == 0)
    {
        M_LOG(MODULE_NAME, "Handle for user %s exits (ref %d)", name, node_client->ref);
        *bc_client = node_client;
    }
}
static void bc_unsubscription(bst_node_t *node, void **args, int argc)
{
    (void)argc;
    tunnel_msg_t *msg = (tunnel_msg_t *)args[1];
    int len = *((int *)args[3]) + 2;
    if (!node)
        return;
    // write hash value to data
    int hash = node->key;
    uint32_t net32 = htonl(hash);
    (void)memcpy(&msg->data[len], &net32, sizeof(net32));
    msg->header.size = len + sizeof(net32);
    args[2] = &hash;
    M_DEBUG(MODULE_NAME, "All clients subscribed to the groupe %d is notified that user is leaving", hash);
    bst_for_each(clients, bc_notify, args, 3);
    if(node->data)
    {
        free(node->data);
        node->data = NULL;
    }
}

static void bc_free_groupname(void* data)
{
    if(data)
    {
        free(data);
    }
}
static void unsubscribe(bst_node_t *node, void **args, int argc)
{
    (void)argc;
    tunnel_msg_t msg;
    int *ufd = (int *)args[0];
    int len;
    bc_client_t *bc_client = (bc_client_t *)node->data;
    void *bc_argv[] = {ufd, &msg, 0, &len};
    if (!node || !node->data)
    {
        return;
    }
    bc_client->ref--;
    // notify all clients in our groups that we're done
    msg.header.type = CHANNEL_CTRL;
    msg.data = msg_buffer;
    msg.data[0] = BC_UNSUBSCRIPTION;
    len = strlen(bc_client->name);
    msg.data[1] = (uint8_t)len;
    (void)memcpy(&msg.data[2], bc_client->name, len);
    // group name
    // unsubscribe
    if (bc_client->ref <= 0)
    {
        M_DEBUG(MODULE_NAME, "User %s is leaving all its subscribed groups (%d)", bc_client->name, bc_client->groups == NULL);
        bst_for_each(bc_client->groups, bc_unsubscription, bc_argv, 3);
        M_DEBUG(MODULE_NAME, "Handle for user %s ref is %d, free handle data", bc_client->name, bc_client->ref);
        bst_free(bc_client->groups);
        free(node->data);
    }
    msg.header.type = CHANNEL_UNSUBSCRIBE;
    msg.header.client_id = node->key;
    msg.header.size = 0;
    msg.data = NULL;
    if (msg_write(*ufd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request unsubscribe to client %d", node->key);
    }
}
static void bc_send_query_group(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    int *fd = (int *)argv[0];
    tunnel_msg_t *msg = (tunnel_msg_t *)argv[1];
    char *gname = NULL;
    if (!node->data)
    {
        return;
    }
    int net32 = htonl(node->key);
    (void)memcpy(&msg->data[1], &net32, sizeof(net32));
    gname = (char*)node->data;
    (void)memcpy(&msg->data[sizeof(net32) + 1u], gname, strlen(gname));
    msg->header.size = sizeof(net32) + 1u + strlen(gname);
    M_DEBUG(MODULE_NAME, "Sent group query to client %d: group %s (%d)", msg->header.client_id, gname, node->key);
    if (msg_write(*fd, msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write query message to client %d", node->key);
    }
}
static void bc_send_query_user(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    int *fd = (int *)argv[0];
    tunnel_msg_t *msg = (tunnel_msg_t *)argv[1];
    bc_client_t *bc_client = NULL;
    int *group = (int *)argv[2];
    int len = *(int *)argv[3];
    if (!node->data)
    {
        return;
    }
    bc_client = (bc_client_t *)node->data;
    if (bst_find(bc_client->groups, *group) == NULL)
    {
        return;
    }
    (void)memcpy(&msg->data[len], bc_client->name, strlen(bc_client->name));
    msg->header.size = len + strlen(bc_client->name);
    M_DEBUG(MODULE_NAME, "Sent user query to client %d: User %s is in group %d", msg->header.client_id, bc_client->name, *group);
    if (msg_write(*fd, msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write query message to client %d", node->key);
    }
}
static void bc_notify(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    int *fd = (int *)argv[0];
    tunnel_msg_t *msg = (tunnel_msg_t *)argv[1];
    bc_client_t *bc_client = NULL;
    int *group = (int *)argv[2];
    if (!node->data)
    {
        return;
    }
    bc_client = (bc_client_t *)node->data;
    if (bst_find(bc_client->groups, *group) == NULL)
    {
        return;
    }
    msg->header.client_id = node->key;
    if (msg_write(*fd, msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write notify message to client %d", node->key);
    }
    M_DEBUG(MODULE_NAME, "Notify message sent to client %d", node->key);
}
int main(int argc, char **argv)
{
    int fd, hash;
    size_t len;
    tunnel_msg_t request, response;
    fd_set fd_in;
    int status, length;
    char name[MAX_STR_LEN + 1];
    void *fargv[4];
    bst_node_t *node_p;
    uint32_t net32;
    bc_client_t *bc_client;
    LOG_INIT(MODULE_NAME);
    if (argc != 3)
    {
        printf("Usage: %s path/to/hotline/socket channel_name\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    // now try to request new channel from hotline
    fd = open_unix_socket(argv[1]);
    if (fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
        return -1;
    }
    request.header.type = CHANNEL_OPEN;
    request.header.channel_id = 0;
    request.header.client_id = 0;
    M_LOG(MODULE_NAME, "Request to open the channel %s", argv[2]);
    (void)strncpy(name, argv[2], sizeof(name));
    request.header.size = strlen(name);
    request.data = (uint8_t *)name;
    if (msg_write(fd, &request) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write message to hotline");
        (void)close(fd);
        return -1;
    }
    M_DEBUG(MODULE_NAME, "Wait for comfirm creation of %s", argv[2]);
    // now wait for message
    if (msg_read(fd, &response) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read message from hotline");
        (void)close(fd);
        return -1;
    }
    if (response.header.type == CHANNEL_OK)
    {
        M_LOG(MODULE_NAME, "Channel created: %s", argv[2]);
    }
    else
    {
        M_ERROR(MODULE_NAME, "Channel is not created: %s. Tunnel service responds with msg of type %d", argv[2], response.header.type);
        running = 0;
    }
    if (response.data)
        free(response.data);
    // now read data
    fargv[0] = (void *)&fd;
    while (running)
    {
        FD_ZERO(&fd_in);
        FD_SET(fd, &fd_in);
        status = select(fd + 1, &fd_in, NULL, NULL, NULL);

        switch (status)
        {
        case -1:
            M_ERROR(MODULE_NAME, "Error %d on select()\n", errno);
            running = 0;
            break;
        case 0:
            break;
        // we have data
        default:
            if (msg_read(fd, &request) == -1)
            {
                M_ERROR(MODULE_NAME, "Unable to read message from channel. quit");
                running = 0;
            }
            else
            {
                switch (request.header.type)
                {
                case CHANNEL_SUBSCRIBE:
                    if (request.header.size > MAX_STR_LEN - 1)
                    {
                        M_ERROR(MODULE_NAME, "User name string overflow");
                    }
                    else
                    {
                        node_p = bst_find(clients, request.header.client_id);
                        if (node_p)
                        {
                            M_LOG(MODULE_NAME, "Client %d is already subscript to this channel", request.header.client_id);
                        }
                        {
                            // store user name
                            bc_client = NULL;
                            (void)memcpy(name, request.data, request.header.size);
                            name[request.header.size] = '\0';
                            fargv[1] = &bc_client;
                            fargv[2] = name;
                            bst_for_each(clients, bc_get_handle, fargv, 3);
                            if (bc_client)
                            {
                                bc_client->ref++;
                            }
                            else
                            {
                                bc_client = (bc_client_t *)malloc(sizeof(bc_client_t));
                                (void)strncpy(bc_client->name, name, MAX_STR_LEN);
                                bc_client->groups = NULL;
                                bc_client->ref = 1;
                            }
                            clients = bst_insert(clients, request.header.client_id, bc_client);
                            M_LOG(MODULE_NAME, "Client %s (%d) subscribes to the chanel (ref %d)", bc_client->name, request.header.client_id, bc_client->ref);
                        }
                    }
                    break;
                case CHANNEL_CTRL:
                    /**
                     * @brief message in format [code][group name]
                     * 
                     */
                    if (request.header.size > 0u && request.header.size <= MAX_STR_LEN)
                    {
                        node_p = bst_find(clients, request.header.client_id);
                        if (node_p && node_p->data)
                        {
                            bc_client = (bc_client_t *)node_p->data;
                            fargv[1] = &response;
                            fargv[2] = &hash;
                            response.header.channel_id = request.header.channel_id;
                            response.header.type = CHANNEL_CTRL;
                            response.data = msg_buffer;
                            response.data[0] = request.data[0];
                            switch (request.data[0])
                            {
                            case BC_SUBSCRIPTION:
                            case BC_UNSUBSCRIPTION:
                                /**
                                 * @brief * The notify message is in the following format
                                    * [1 byte type][1 byte user name size][user name][4byte gid][groupname (optional)]
                                * 
                                */

                                if (request.data[0] == BC_SUBSCRIPTION)
                                {
                                    memcpy(name, &request.data[1], request.header.size - 1u);
                                    name[request.header.size - 1u] = '\0';
                                    hash = simple_hash(name);
                                    if(bst_find(bc_client->groups, hash) == NULL)
                                    {
                                        bc_client->groups = (void *)bst_insert(bc_client->groups, hash, strdup(name));
                                        M_LOG(MODULE_NAME, "Client %d subscription to broadcast group: %s (%d)", request.header.client_id, name, hash);
                                    }
                                    else
                                    {
                                        M_LOG(MODULE_NAME, "Client %d is already subscribed to the group %s", request.header.client_id, name);
                                    }
                                }
                                else
                                {
                                    name[0] = '\0';
                                    (void)memcpy(&hash, &request.data[1], sizeof(hash));
                                    hash = ntohl(hash);
                                }
                                len = strlen(bc_client->name);
                                response.data[1] = (uint8_t)len;
                                (void)memcpy(&response.data[2], bc_client->name, len);
                                len += 2u;
                                // group name
                                net32 = htonl(hash);
                                (void)memcpy(&response.data[len], &net32, sizeof(net32));
                                len += sizeof(net32);
                                (void)memcpy(&response.data[len], name, strlen(name));
                                response.header.size = len + strlen(name);
                                bst_for_each(clients, bc_notify, fargv, 3);
                                if (request.data[0] == BC_UNSUBSCRIPTION)
                                {
                                    bc_client->groups = (void *)bst_delete(bc_client->groups, hash);
                                    M_LOG(MODULE_NAME, "Client %d leaves broadcast group: %d", request.header.client_id, hash);
                                }
                                break;

                            case BC_QUERY_USER:
                                (void)memcpy(&hash, &request.data[1], sizeof(hash));
                                hash = ntohl(hash);
                                net32 = htonl(hash);
                                (void)memcpy(&response.data[1], &net32, sizeof(net32));
                                len = len = sizeof(net32) + 1u;
                                fargv[3] = &len;
                                response.header.client_id = request.header.client_id;
                                if (bst_find(bc_client->groups, hash) == NULL)
                                {
                                    BC_ERROR(response, fd, request.header.client_id, "Client %d query a group that it does not belong to", request.header.client_id);
                                }
                                else
                                {
                                    // data format [type][4bytes group][user]
                                    M_LOG(MODULE_NAME, "Client %d query  user from group %d", request.header.client_id, hash);
                                    bst_for_each(clients, bc_send_query_user, fargv, 4);
                                }
                                break;
                            case BC_QUERY_GROUP:
                                if(bc_client->groups)
                                {
                                    /** send back group to client one by one inform of [type][gid 4][group name]*/
                                    response.header.client_id = request.header.client_id;
                                    M_LOG(MODULE_NAME, "Client %d query  all user subscribed group", request.header.client_id);
                                    bst_for_each(bc_client->groups, bc_send_query_group, fargv, 2);
                                }
                                break;
                            default:
                                BC_ERROR(response, fd, request.header.client_id, "Invalid client control message: 0x%.2X", request.data[0]);
                                break;
                            }
                        }
                        else
                        {
                            BC_ERROR(response, fd, request.header.client_id, "Client %d does not previously subscribe to the channel", request.header.client_id);
                        }
                    }
                    else
                    {
                        BC_ERROR(response, fd, request.header.client_id, "Invalid CTRL message size: %d", request.header.size);
                    }
                    break;
                case CHANNEL_DATA:
                    /**
                     * @brief Send message to a group of client
                     * message is in the following format
                     * [4bytes group id][data]
                     */
                    (void)memcpy(&hash, &request.data[0], sizeof(hash));
                    hash = ntohl(hash);
                    fargv[1] = &request;
                    fargv[2] = &hash;
                    bst_for_each(clients, bc_notify, fargv, 3);
                    break;
                case CHANNEL_UNSUBSCRIBE:
                    M_LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", request.header.client_id);

                    node_p = bst_find(clients, request.header.client_id);
                    unsubscribe(node_p, fargv, 1);
                    clients = bst_delete(clients, request.header.client_id);
                    break;

                default:
                    M_LOG(MODULE_NAME, "Client %d send message of type %d",
                          request.header.client_id, request.header.type);
                    break;
                }
                if (request.data)
                {
                    free(request.data);
                }
            }
        }
    }
    // unsubscribe all client
    bst_for_each(clients, unsubscribe, fargv, 1);
    bst_free(clients);
    // close the channel
    M_LOG(MODULE_NAME, "Close the channel %s (%d)", argv[2], fd);
    request.header.type = CHANNEL_CLOSE;
    request.header.size = 0;
    request.data = NULL;
    if (msg_write(fd, &request) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request channel close");
    }

    if (msg_read(fd, &response) == 0)
    {
        if (response.data)
        {
            free(response.data);
        }
    }
    (void)close(fd);
    return 0;
}