
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "tunnel.h"

#define MODULE_NAME "api"


static int msg_check_number(int fd, int number)
{
    int value;
    if(read(fd,&value,sizeof(value)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read integer value: %s", strerror(errno));
        return -1;
    }
    if(number != value)
    {
        M_ERROR(MODULE_NAME, "Value mismatches: %04X, expected %04X", value, number);
        return -1;
    }
    return 0;
}
static int msg_read_string(int fd, char* buffer, uint8_t max_length)
{
    uint8_t size;
    if(read(fd,&size,sizeof(size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read string size: %s", strerror(errno));
        return -1;
    }
    if(size > max_length)
    {
        M_ERROR(MODULE_NAME, "String length exceeds the maximal value of %d", max_length);
        return -1;
    }
    if(read(fd,buffer,size) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read string to buffer: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static uint8_t* msg_read_payload(int fd, int* size)
{
    uint8_t* data;
    if(read(fd,size,sizeof(*size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read payload data size: %s", strerror(errno));
        return NULL;
    }
    if(*size <= 0)
    {
        return NULL;
    }

    data = (uint8_t*) malloc(*size);
    if(data == NULL)
    {
        M_ERROR(MODULE_NAME, "Unable to allocate memory for payload data: %s", strerror(errno));
        return NULL;
    }
    if(read(fd,data,*size) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read payload data to buffer: %s", strerror(errno));
        free(data);
        return NULL;
    }
    return data;
}


int open_unix_socket(char* path)
{
    struct sockaddr_un address;
    address.sun_family = AF_UNIX;

    (void) strncpy(address.sun_path, path, sizeof(address.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to create Unix domain socket: %s", strerror(errno));
        return -1;
    }
    if(connect(fd, (struct sockaddr*)(&address), sizeof(address)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to connect to socket '%s': %s", address.sun_path, strerror(errno));
        return -1;
    }
    M_LOG(MODULE_NAME, "Socket %s is created successfully", path);
    return fd;
}


int msg_read(int fd, tunnel_msg_t* msg)
{
#ifdef VERIFY_HEADER
    if(msg_check_number(fd, MSG_MAGIC_BEGIN) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to check begin magic number");
        return -1;
    }
#endif
    if(read(fd,&msg->header.type,sizeof(msg->header.type)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg type: %s", strerror(errno));
        return -1;
    }
    if(msg->header.type > 0x7)
    {
        M_ERROR(MODULE_NAME, "Unknown msg type: %d", msg->header.type);
        return -1;
    }
    if(read(fd, &msg->header.channel_id, sizeof(msg->header.channel_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg channel id");
        return -1;
    }
    if(read(fd, &msg->header.client_id, sizeof(msg->header.client_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg client id");
        return -1;
    }
    if((msg->data = msg_read_payload(fd, &msg->header.size)) == NULL && msg->header.size != 0)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg payload data");
        return -1;
    }
#ifdef VERIFY_HEADER
    if(msg_check_number(fd, MSG_MAGIC_END) == -1)
    {
        if(msg->data)
        {
            free(msg->data);
        }
        M_ERROR(MODULE_NAME, "Unable to check end magic number");
        return -1;
    }
#endif
    return 0;
}

int msg_write(int fd, tunnel_msg_t* msg)
{
#ifdef VERIFY_HEADER
    // write begin magic number
    int number = MSG_MAGIC_BEGIN;
    if(write(fd,&number, sizeof(number)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write begin magic number: %s", strerror(errno));
        return -1;
    }
#endif
    // write type
    if(write(fd,&msg->header.type, sizeof(msg->header.type)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg type: %s", strerror(errno));
        return -1;
    }
    // write channel id
    if(write(fd,&msg->header.channel_id, sizeof(msg->header.channel_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg channel id: %s", strerror(errno));
        return -1;
    }
    //write client id
    if(write(fd,&msg->header.client_id, sizeof(msg->header.client_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg client id: %s", strerror(errno));
        return -1;
    }
    // write payload len
    
    if(write(fd,&msg->header.size, sizeof(msg->header.size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg payload length: %s", strerror(errno));
        return -1;
    }
    // write payload data
    if(msg->header.size > 0)
    {
        if(write(fd,msg->data, msg->header.size) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to write msg payload: %s", strerror(errno));
            return -1;
        }
    }
#ifdef VERIFY_HEADER
    number = MSG_MAGIC_END;
    if(write(fd,&number, sizeof(number)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write end magic number: %s", strerror(errno));
        return -1;
    }
#endif
    return 0;
}