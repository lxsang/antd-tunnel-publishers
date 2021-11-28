
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "tunnel.h"

#define MODULE_NAME "api"

static int guard_read(int fd, void* buffer, size_t size)
{
    int n = 0;
    int read_len;
    int st;
    while(n != (int)size)
    {
        read_len = (int)size - n;
        st = read(fd,buffer + n,read_len);
        if(st == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to read from #%d: %s", fd, strerror(errno));
            return -1;
        }
        if(st == 0)
        {
            M_ERROR(MODULE_NAME,"Endpoint %d is closed", fd);
            return -1;
        }
        n += st;
    }
    return n;
}

static int guard_write(int fd, void* buffer, size_t size)
{
    int n = 0;
    int write_len;
    int st;
    while(n != (int)size)
    {
        write_len = (int)size - n;
        st = write(fd,buffer + n,write_len);
        if(st == -1)
        {
            M_ERROR(MODULE_NAME,"Unable to write to #%d: %s", fd, strerror(errno));
            return -1;
        }
        if(st == 0)
        {
            M_ERROR(MODULE_NAME,"Endpoint %d is closed", fd);
            return -1;
        }
        n += st;
    }
    return n;
}

static int msg_check_number(int fd, uint16_t number)
{
    uint16_t value;
    if(guard_read(fd,&value,sizeof(value)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read integer value: %s", strerror(errno));
        return -1;
    }
    value = ntohs(value);
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
    if(guard_read(fd,&size,sizeof(size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read string size: %s", strerror(errno));
        return -1;
    }
    if(size > max_length)
    {
        M_ERROR(MODULE_NAME, "String length exceeds the maximal value of %d", max_length);
        return -1;
    }
    if(guard_read(fd,buffer,size) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read string to buffer: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static uint8_t* msg_read_payload(int fd, uint32_t* size)
{
    uint8_t* data;
    if(guard_read(fd,size,sizeof(*size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read payload data size: %s", strerror(errno));
        return NULL;
    }
    *size = ntohl(*size);
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
    if(guard_read(fd,data,*size) == -1)
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
    if(msg_check_number(fd, MSG_MAGIC_BEGIN) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to check begin magic number");
        return -1;
    }
    if(guard_read(fd,&msg->header.type,sizeof(msg->header.type)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg type: %s", strerror(errno));
        return -1;
    }
    if(msg->header.type > 0x7)
    {
        M_ERROR(MODULE_NAME, "Unknown msg type: %d", msg->header.type);
        return -1;
    }
    if(guard_read(fd, &msg->header.channel_id, sizeof(msg->header.channel_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg channel id");
        return -1;
    }
    msg->header.channel_id = ntohs(msg->header.channel_id);
    if(guard_read(fd, &msg->header.client_id, sizeof(msg->header.client_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg client id");
        return -1;
    }
    msg->header.client_id = ntohs(msg->header.client_id);
    if((msg->data = msg_read_payload(fd, &msg->header.size)) == NULL && msg->header.size != 0)
    {
        M_ERROR(MODULE_NAME, "Unable to read msg payload data");
        return -1;
    }
    if(msg_check_number(fd, MSG_MAGIC_END) == -1)
    {
        if(msg->data)
        {
            free(msg->data);
        }
        M_ERROR(MODULE_NAME, "Unable to check end magic number");
        return -1;
    }
    return 0;
}

int msg_write(int fd, tunnel_msg_t* msg)
{
    // write begin magic number
    uint16_t net16;
    uint32_t net32;
    net16 = htons(MSG_MAGIC_BEGIN);
    if(guard_write(fd,&net16, sizeof(net16)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write begin magic number: %s", strerror(errno));
        return -1;
    }
    // write type
    if(guard_write(fd,&msg->header.type, sizeof(msg->header.type)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg type: %s", strerror(errno));
        return -1;
    }
    // write channel id
    net16 = htons(msg->header.channel_id);
    if(guard_write(fd,&net16, sizeof(msg->header.channel_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg channel id: %s", strerror(errno));
        return -1;
    }
    //write client id
    net16 = htons(msg->header.client_id);
    if(guard_write(fd,&net16, sizeof(msg->header.client_id)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg client id: %s", strerror(errno));
        return -1;
    }
    // write payload len
    net32 = htonl(msg->header.size);
    if(guard_write(fd,&net32, sizeof(msg->header.size)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write msg payload length: %s", strerror(errno));
        return -1;
    }
    // write payload data
    if(msg->header.size > 0)
    {
        if(guard_write(fd,msg->data, msg->header.size) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to write msg payload: %s", strerror(errno));
            return -1;
        }
    }
    net16 = htons(MSG_MAGIC_END);
    if(guard_write(fd,&net16, sizeof(net16)) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write end magic number: %s", strerror(errno));
        return -1;
    }
    return 0;
}