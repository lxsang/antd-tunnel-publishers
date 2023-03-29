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
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <antd/list.h>
#include <antd/bst.h>
#include <antd/utils.h>

#include "../tunnel.h"

#define MODULE_NAME "syslogb"

static bst_node_t *clients = NULL;

static volatile int running = 1;

static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
}
static void send_data(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    tunnel_msg_t *msg = (tunnel_msg_t *)argv[0];
    int *fd = (int *)argv[1];
    msg->header.client_id = node->key;
    if (msg_write(*fd, msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write data message to client %d", node->key);
    }
}
static void unsubscribe(bst_node_t *node, void **args, int argc)
{
    (void)argc;
    tunnel_msg_t msg;
    int *ufd = (int *)args[0];
    msg.header.type = CHANNEL_UNSUBSCRIBE;
    msg.header.client_id = node->key;
    msg.header.size = 0;
    if (msg_write(*ufd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request unsubscribe to client %d", node->key);
    }
}
int main(int argc, char **argv)
{
    int fd, sock_fd;
    tunnel_msg_t msg;
    fd_set fd_in;
    int status, maxfd, length;
    char buff[BUFFLEN + 1];
    void *fargv[2];
    struct sockaddr_un saddr, caddr;
    uint8_t *tmp;
    LOG_INIT(MODULE_NAME);

    if (argc != 4)
    {
        printf("Usage: %s path/to/hotline/socket channel_name input_file\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    // create the unix domain socket
    (void)unlink(argv[3]);
    if ((sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
        M_ERROR(MODULE_NAME, "Unable to create socket %s: %s", argv[3], strerror(errno));
        return -1;
    }

    /* bind to the socket path */
    saddr.sun_family = AF_UNIX;
    snprintf(saddr.sun_path, (strlen(argv[3])+1), "%s", argv[3]);

    if (0 != (bind(sock_fd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_un)))) {
        M_ERROR(MODULE_NAME, "Unable to bind socket %s: %s", argv[3], strerror(errno));
        (void) close(sock_fd);
        return -1;
    }
    (void)chmod(argv[3], 0777);

    M_LOG(MODULE_NAME, "Unix domain socket: %s created", argv[3]);
    M_LOG(MODULE_NAME, "Hotline is: %s", argv[1]);
    // now try to request new channel from hotline
    fd = open_socket(argv[1]);
    if (fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
        (void)close(sock_fd);
        return -1;
    }
    msg.header.type = CHANNEL_OPEN;
    msg.header.channel_id = 0;
    msg.header.client_id = 0;
    M_LOG(MODULE_NAME, "Request to open the channel %s", argv[2]);
    (void)strncpy(buff, argv[2], MAX_CHANNEL_NAME);
    msg.header.size = strlen(buff);
    msg.data = (uint8_t *)buff;
    if (msg_write(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write message to hotline");
        (void)close(fd);
        (void)close(sock_fd);
        return -1;
    }
    M_LOG(MODULE_NAME, "Wait for comfirm creation of %s", argv[2]);
    // now wait for message
    if (msg_read(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read message from hotline");
        (void)close(fd);
        (void)close(sock_fd);
        return -1;
    }
    if (msg.header.type == CHANNEL_OK)
    {
        M_LOG(MODULE_NAME, "Channel created: %s", argv[2]);
        if (msg.data)
            free(msg.data);
    }
    else
    {
        M_ERROR(MODULE_NAME, "Channel is not created: %s. Tunnel service responds with msg of type %d", argv[2], msg.header.type);
        if (msg.data)
            free(msg.data);
        running = 0;
    }

    length = sizeof(struct sockaddr_un);
    // now read data
    while (running)
    {
        FD_ZERO(&fd_in);
        FD_SET(fd, &fd_in);
        FD_SET(sock_fd, &fd_in);
        maxfd = fd > sock_fd ? fd : sock_fd;

        status = select(maxfd + 1, &fd_in, NULL, NULL, NULL);

        switch (status)
        {
        case -1:
            M_LOG(MODULE_NAME, "Error %d on select()\n", errno);
            running = 0;
            break;
        case 0:
            break;
        // we have data
        default:
            if (FD_ISSET(fd, &fd_in))
            {
                if (msg_read(fd, &msg) == -1)
                {
                    M_ERROR(MODULE_NAME, "Unable to read message from channel. quit");
                    running = 0;
                }
                else
                {
                    switch (msg.header.type)
                    {
                    case CHANNEL_SUBSCRIBE:
                        M_LOG(MODULE_NAME, "Client %d subscribes to the chanel", msg.header.client_id);
                        clients = bst_insert(clients, msg.header.client_id, NULL);
                        break;

                    case CHANNEL_UNSUBSCRIBE:
                        M_LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", msg.header.client_id);
                        clients = bst_delete(clients, msg.header.client_id);
                        break;

                    default:
                        M_LOG(MODULE_NAME, "Client %d send message of type %d",
                              msg.header.client_id, msg.header.type);
                        break;
                    }
                    if (msg.data)
                    {
                        free(msg.data);
                    }
                }
            }
            else if (FD_ISSET(sock_fd, &fd_in))
            {
                // read data to buffer
                memset(buff, 0, sizeof(buff));
                status = recvfrom(sock_fd, buff, sizeof(buff) -1 , 0, (struct sockaddr *)&caddr, &length);
                // on the fifo side
                if (status == -1)
                {
                    M_ERROR(MODULE_NAME, "Unable to read data from the UDP socket %s: %s", argv[3], strerror(errno));
                    running = 0;
                }
                else
                {
                    msg.header.type = CHANNEL_DATA;
                    msg.header.size = status;
                    msg.data = (uint8_t *)buff;
                    fargv[0] = (void *)&msg;
                    fargv[1] = (void *)&fd;
                    bst_for_each(clients, send_data, fargv, 2);
                }
            }
        }
    }
    // unsubscribe all client
    fargv[0] = (void *)&fd;
    bst_for_each(clients, unsubscribe, fargv, 1);
    bst_free(clients);
    // close the channel
    M_LOG(MODULE_NAME, "Close the channel %s (%d)", argv[2], fd);
    msg.header.type = CHANNEL_CLOSE;
    msg.header.size = 0;
    msg.data = NULL;
    if (msg_write(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request channel close");
    }
    // close all opened terminal

    (void)msg_read(fd, &msg);
    (void)close(fd);
    (void)close(sock_fd);
    return 0;
}