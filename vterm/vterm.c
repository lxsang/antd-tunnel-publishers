#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include "tunnel.h"

#define MODULE_NAME     "vterm"

static volatile int running = 1;

void int_handler(int dummy) {
    (void) dummy;
    running = 0;
}


int main(int argc, char** argv)
{
    int fd;
    tunnel_msg_t msg;
    fd_set fd_in;
    int status;
    struct timeval timeout;
    char buff[MAX_CHANNEL_NAME+1];
    LOG_INIT(MODULE_NAME);
    if(argc != 2)
    {
        printf("Usage: %s path/to/hotline/socket\n", argv[0]);
        return -1;
    }
    signal(SIGINT, int_handler);
    LOG(MODULE_NAME, "Hotline is: %s", argv[1]);
    // now try to request new channel from hotline
    fd = open_unix_socket(argv[1]);
    if(fd == -1)
    {
        ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
        return -1;
    }
    msg.header.type = CHANNEL_OPEN;
    msg.header.channel_id = 0;
    msg.header.client_id = 0;
    LOG(MODULE_NAME, "Request to open the channel %s", MODULE_NAME);
    (void)strncpy(buff, MODULE_NAME,MAX_CHANNEL_NAME);
    msg.header.size = strlen(buff);
    msg.data = (uint8_t*) buff;
    if(msg_write(fd, &msg) == -1)
    {
        ERROR(MODULE_NAME, "Unable to write message to hotline");
        (void) close(fd);
        return -1;
    }
    LOG(MODULE_NAME, "Wait for comfirm creation of %s", MODULE_NAME);
    // now wait for message
    if(msg_read(fd, &msg) == -1)
    {
        ERROR(MODULE_NAME, "Unable to read message from hotline");
        (void) close(fd);
        return -1;
    }
    if(msg.header.type == CHANNEL_OK)
    {
        LOG(MODULE_NAME, "Channel created: %s", MODULE_NAME);
        if(msg.data)
            free(msg.data);
    }

    // now read data
    while(running)
    {
        timeout.tv_sec = 0;
	    timeout.tv_usec = 500;
        FD_ZERO(&fd_in);
        FD_SET(fd, &fd_in);
        status = select(fd + 1, &fd_in, NULL, NULL, &timeout);
        
        switch (status)
	    {
            case -1:
                LOG(MODULE_NAME, "Error %d on select()\n", errno);
                running = 0;
                break;
            case 0:
                timeout.tv_sec = 0;
                timeout.tv_usec = 10000; // 5 ms
                select(0, NULL, NULL, NULL, &timeout);
                break;
            // we have data
            default:
                if(msg_read(fd, &msg) == -1)
                {
                    ERROR(MODULE_NAME, "Unable to read message from channel. quit");
                    (void) close(fd);
                    running = 0;
                }
                else
                {
                    switch (msg.header.type)
                    {
                    case CHANNEL_SUBSCRIBE:
                        LOG(MODULE_NAME, "Client %d subscribes to the chanel", msg.header.client_id);
                        break;
                    
                    case CHANNEL_UNSUBSCRIBE:
                        LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", msg.header.client_id);
                        break;
                    case CHANNEL_DATA:
                        LOG(MODULE_NAME, "Got data");
                        if(msg_write(fd, &msg) == -1)
                        {
                            LOG(MODULE_NAME,"Unable to write data back");
                        }
                        break;
                    default:
                        LOG(MODULE_NAME, "Client %d send message of type %d", msg.header.client_id, msg.header.type);
                        break;
                    }
                }
        }
    }

    // close the channel
    LOG(MODULE_NAME, "Close the channel %s (%d)", MODULE_NAME, fd);
    msg.header.type = CHANNEL_CLOSE;
    msg.header.size = 0;
    msg.data = NULL;
    if( msg_write(fd, &msg) == -1)
    {
        ERROR(MODULE_NAME, "Unable to request channel close");
    }
    (void)msg_read(fd, &msg);
    (void) close(fd);
    return 0;
}