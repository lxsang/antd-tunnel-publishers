#ifndef TUNNEL_H
#define TUNNEL_H
#include <stdint.h>
#include "log.h"

#define MAX_CHANNEL_PATH            108
#define MAX_CHANNEL_NAME            64

#define MSG_MAGIC_BEGIN             0x414e5444 //ANTD
#define MSG_MAGIC_END               0x44544e41 //DTNA

#define    CHANNEL_OK               (uint8_t)0x0
#define    CHANNEL_ERROR            (uint8_t)0x1
#define    CHANNEL_OPEN             (uint8_t)0x4
#define    CHANNEL_CLOSE            (uint8_t)0x5
#define    CHANNEL_DATA             (uint8_t)0x6

typedef struct {
    uint8_t type;
    int channel_id;
    int client_id;
    int size;
} tunnel_msg_h_t;

typedef struct{
    tunnel_msg_h_t header;
    uint8_t* data;
} tunnel_msg_t;

int open_unix_socket(char* path);
int msg_write(int fd, tunnel_msg_t* msg);
int msg_read(int fd, tunnel_msg_t* msg);



#endif