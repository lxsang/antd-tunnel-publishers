#ifndef TUNNEL_H
#define TUNNEL_H
#include <stdint.h>
#include <netinet/in.h>
#include <regex.h>
#include "log.h"

#define MAX_CHANNEL_PATH            108
#define MAX_CHANNEL_NAME            64

#define MSG_MAGIC_BEGIN             (uint16_t)0x414e //AN
#define MSG_MAGIC_END               (uint16_t)0x5444 //TD

#define    CHANNEL_OK               (uint8_t)0x0
#define    CHANNEL_ERROR            (uint8_t)0x1
#define    CHANNEL_OPEN             (uint8_t)0x4
#define    CHANNEL_CLOSE            (uint8_t)0x5
#define    CHANNEL_DATA             (uint8_t)0x6
#define    CHANNEL_UNSUBSCRIBE      (uint8_t)0x3
#define    CHANNEL_SUBSCRIBE        (uint8_t)0x2
#define    CHANNEL_CTRL             (uint8_t)0x7

typedef struct {
    uint8_t type;
    uint16_t channel_id;
    uint16_t client_id;
    uint32_t size;
} tunnel_msg_h_t;

typedef struct{
    tunnel_msg_h_t header;
    uint8_t* data;
} tunnel_msg_t;

int open_socket(char* path);
int msg_write(int fd, tunnel_msg_t* msg);
int msg_read(int fd, tunnel_msg_t* msg);
int regex_match(const char* expr,const char* search, int msize, regmatch_t* matches);

#endif