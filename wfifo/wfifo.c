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
#include <pwd.h>

#include <antd/list.h>
#include <antd/bst.h>
#include <antd/utils.h>

#include "../tunnel.h"

#define MODULE_NAME "wfifo"

static bst_node_t *clients = NULL;
static bst_node_t *fifo_handles = NULL;

static volatile int running = 1;

static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
}
static void send_data(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    tunnel_msg_t *msg = (tunnel_msg_t *)argv[2];
    int *fd = (int *)argv[0];
    int *ffd = (int*)argv[1];
    if(!node || !node->data || (int)node->data != *ffd)
    {
        return;
    }

    msg->header.client_id = node->key;
    if (msg_write(*fd, msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write data message to client %d", node->key);
    }
    M_DEBUG(MODULE_NAME, "Message sent to client %d", node->key);
}

static void prepare_fd_set(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    fd_set* fd_in = (fd_set *)argv[1];
    int* max_fd = (int*) argv[2];
    if(!node || ! node->data)
    {
        return;
    }
    FD_SET((int)node->data, fd_in);
    *max_fd = (int)node->data > *max_fd? (int)node->data: *max_fd;
}

static void monitor_fifo_handles(bst_node_t *node, void **argv, int argc)
{
    (void)argc;
    int ffd, status;
    int* fd;
    fd_set* fd_in = (fd_set *)argv[1];
    tunnel_msg_t* msg = (tunnel_msg_t*) argv[2];
    void * fargv[3];
    fd = (int*) argv[0];
    if(!node || ! node->data)
    {
        return;
    }
    ffd = (int) node->data;
    if(FD_ISSET(ffd, fd_in))
    {
        if ((status = read(ffd, msg->data, BUFFLEN)) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to read data from the FIFO %d: %s", ffd, strerror(errno));
            return;
        }
        else
        {
            msg->header.size = status;
            fargv[0] = argv[0];
            fargv[1] = &ffd;
            fargv[2] = argv[2];
            bst_for_each(clients, send_data, fargv, 2);
        }
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
static void close_fifo_handles(bst_node_t *node, void **args, int argc)
{
    (void)argc;
    (void) args;
    if(!node || !node->data)
    {
        return;
    }
    if((int) node->data > 0)
    {
        M_DEBUG(MODULE_NAME, "Close fifo handle %d", (int)node->data);
        (void) close((int) node->data);
    }
}

int init_fifo(char *buff, const char *base, const char *user)
{
    int fd, hash;
    int generic_fd = 1;
    struct stat path_stat;
    uid_t uid;
    struct passwd *pwd = NULL;
    bst_node_t * node;
    (void)memset(buff, 0, BUFFLEN);
    if (stat(base, &path_stat) == 0)
    {
        if (S_ISREG(path_stat.st_mode))
        {
            // is directory
            if (user == NULL)
            {
                M_ERROR(MODULE_NAME, "Cannot init fifo for null user");
                return -1;
            }
            (void)snprintf(buff, BUFFLEN, "%s/%s.fifo", base, user);
            generic_fd = 0;
            pwd = getpwnam(user); /* Try getting UID for username */
            if (pwd == NULL)
            {
                M_ERROR(MODULE_NAME, "Unable to get userid from user %s: %s", user, strerror(errno));
                return -1;
            }
            uid = pwd->pw_uid;
        }
    }
    if (generic_fd)
    {
        (void)snprintf(buff, BUFFLEN, "%s", base);
    }
    // check if file exists
    hash = simple_hash(buff);
    node = bst_find(fifo_handles,hash);
    if(node && node->data)
    {
        M_DEBUG(MODULE_NAME, "handle for file %s exists (%d)", buff, (int)node->data);
        return (int) node->data;
    }
    (void)unlink(buff);
    if (mkfifo(buff, 0666) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to create FIFO %s: %s", buff, strerror(errno));
        return -1;
    }
    fd = open(buff, O_RDWR);
    if (fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open FIFO %s: %s", buff, strerror(errno));
        return -1;
    }
    if (!generic_fd)
    {
        // chown file by user
        if (chown(buff, uid, -1) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to change ownerfor file %s to %s: %s", buff, user, strerror(errno));
            (void) close(fd);
            return -1;
        }
    }
    fifo_handles = bst_insert(fifo_handles, hash, (void*)fd);
    M_LOG(MODULE_NAME, "FIFO: %s created", buff);
    return fd;
}
int main(int argc, char **argv)
{
    int fd, ffd;
    tunnel_msg_t msg;
    fd_set fd_in;
    int status, maxfd;
    char buff[BUFFLEN + 1];
    void *fargv[3];
    bst_node_t* node = NULL;
    uint8_t *tmp;
    LOG_INIT(MODULE_NAME);

    if (argc != 5)
    {
        printf("Usage: %s path/to/hotline/socket channel_name input_file r/w\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);

    // create the fifo first
    (void)unlink(argv[3]);
    if (mkfifo(argv[3], 0666) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to create FIFO %s: %s", argv[3], strerror(errno));
        return -1;
    }
    // now try to request new channel from hotline
    fd = open_unix_socket(argv[1]);
    if (fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
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
        return -1;
    }
    M_LOG(MODULE_NAME, "Wait for comfirm creation of %s", argv[2]);
    // now wait for message
    if (msg_read(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read message from hotline");
        (void)close(fd);
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
    /**
     * @brief init global fifo handle
     * 
     * If the publisher is configured to be user base fifo,
     * a error LOG will be shown
     */
    (void)init_fifo(buff, argv[3], NULL);

    fargv[0] = (void *)&fd;
    // now read data
    while (running)
    {
        FD_ZERO(&fd_in);
        FD_SET(fd, &fd_in);
        fargv[1] = &fd_in;
        maxfd = fd;
        fargv[2] = &maxfd;
        bst_for_each(fifo_handles, prepare_fd_set, fargv, 3);
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
                        ffd = init_fifo(buff, argv[3], (char*)msg.data);
                        if(ffd != -1)
                        {
                            clients = bst_insert(clients, msg.header.client_id, (void*)ffd);
                            M_LOG(MODULE_NAME, "Client %d subscribes to the chanel", msg.header.client_id);
                        }
                        break;

                    case CHANNEL_UNSUBSCRIBE:
                        M_LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", msg.header.client_id);
                        clients = bst_delete(clients, msg.header.client_id);
                        break;

                    case CHANNEL_DATA:
                        if (argv[4][0] == 'w')
                        {
                            node = bst_find(clients, msg.header.client_id);
                            if(node && node->data)
                            {
                                // write data to the FIFO
                                if (msg.header.size > 0)
                                {
                                    if (write((int)node->data, msg.data, msg.header.size) == -1)
                                    {
                                        M_ERROR(MODULE_NAME, "Unable to write data to the FIFO %s from client %d: %s", argv[3], msg.header.client_id, strerror(errno));
                                        running = 0;
                                    }
                                }
                            }
                        }
                        else
                        {
                            (void)snprintf(buff, BUFFLEN, "Channel is read only");
                            msg.header.type = CHANNEL_ERROR;
                            msg.header.size = strlen(buff);
                            tmp = msg.data;
                            msg.data = (uint8_t *)buff;
                            if (msg_write(fd, &msg) == -1)
                            {
                                M_ERROR(MODULE_NAME, "Unable to write message to hotline");
                                running = 0;
                            }
                            msg.data = tmp;
                            M_ERROR(MODULE_NAME, "Channel is read only %s(%d)", argv[3], msg.header.client_id);
                        }
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
            else if (argv[4][0] == 'r')
            {
                // on the fifo side
                fargv[1] = &fd_in;
                fargv[2] = &msg;
                msg.data = (uint8_t *)buff;
                msg.header.type = CHANNEL_DATA;
                bst_for_each(fifo_handles, monitor_fifo_handles, fargv, 3);
            }
        }
    }
    // unsubscribe all client
    bst_for_each(clients, unsubscribe, fargv, 1);
    bst_for_each(fifo_handles, close_fifo_handles, NULL, 0);
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
    bst_free(clients);
    bst_free(fifo_handles);
    return 0;
}