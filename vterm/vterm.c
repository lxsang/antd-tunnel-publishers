#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "tunnel.h"

#define MODULE_NAME     "vterm"

int main(int argc, char** argv)
{
    int fd;
    tunnel_msg_t msg;
    int status;
    char buff[MAX_CHANNEL_NAME+1];
    LOG_INIT(MODULE_NAME);
    if(argc != 2)
    {
        printf("Usage: %s path/to/hotline/socket\n", argv[0]);
        return -1;
    }
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
    // close the channel
    LOG(MODULE_NAME, "Close the channel %s (%d)", MODULE_NAME, fd);
    msg.header.type = CHANNEL_CLOSE;
    msg.header.size = 0;
    msg.data = NULL;
    sleep(5);
    if( msg_write(fd, &msg) == -1)
    {
        ERROR(MODULE_NAME, "Unable to request channel close");
    }
    (void)msg_read(fd, &msg);
    shutdown(fd, SHUT_WR);
    (void) close(fd);
    printf("Main application\n");
    return 0;
}