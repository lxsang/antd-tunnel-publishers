#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include <antd/list.h>
#include <antd/bst.h>
#include <antd/utils.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <jpeglib.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/time.h>

#include "../tunnel.h"

#define MODULE_NAME "v4l2cam"
#define DEV_SIZE 32

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t jpeg_quality;
    uint8_t *raw_buffer;
    int fd;
    int timerfd;
    int raw_size;
    char dev_name[DEV_SIZE];
    uint8_t queued;
} cam_setting_t;

static bst_node_t *clients = NULL;
static cam_setting_t video_setting;
static volatile int running = 1;

static int cam_set_format(cam_setting_t *opts)
{
    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = (unsigned int)opts->width;
    format.fmt.pix.height = (unsigned int)opts->height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    int res = ioctl(opts->fd, VIDIOC_S_FMT, &format);
    if (res == -1)
    {
        M_ERROR(MODULE_NAME, "Could not set image format: %s", strerror(errno));
        return -1;
    }
    /*
   * set framerate
   */
    struct v4l2_streamparm *setfps;
    setfps =
        (struct v4l2_streamparm *)calloc(1, sizeof(struct v4l2_streamparm));
    memset(setfps, 0, sizeof(struct v4l2_streamparm));
    setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setfps->parm.capture.timeperframe.numerator = 1;
    setfps->parm.capture.timeperframe.denominator = (unsigned int)opts->fps;
    res = ioctl(opts->fd, VIDIOC_S_PARM, setfps);
    if (res == -1)
    {
        M_ERROR(MODULE_NAME, "Could not set image format: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int cam_request_buffer(int fd, int count)
{
    struct v4l2_requestbuffers req = {0};
    req.count = count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request cam buffer: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int cam_query_buffer(cam_setting_t *opts)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    int res = ioctl(opts->fd, VIDIOC_QUERYBUF, &buf);
    if (res == -1)
    {
        M_ERROR(MODULE_NAME, "Could not query buffer: %s", strerror(errno));
        return -1;
    }
    opts->raw_buffer = (u_int8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, opts->fd, buf.m.offset);
    opts->raw_size = buf.length;
    return buf.length;
}

static int cam_release_buffer(cam_setting_t *opts)
{
    if (opts->raw_buffer == NULL)
    {
        return 0;
    }
    if (munmap(opts->raw_buffer, opts->raw_size) == -1)
    {
        M_ERROR(MODULE_NAME, "Error munmap: %s", strerror(errno));
        return -1;
    }
    opts->raw_buffer = NULL;
    opts->raw_size = 0;
    return 0;
}

int cam_start_streaming(int fd)
{
    unsigned int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to start VIDIOC_STREAMON: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int cam_queue_buffer(int fd)
{
    struct v4l2_buffer bufd = {0};
    bufd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufd.memory = V4L2_MEMORY_MMAP;
    bufd.index = 0;
    if (-1 == ioctl(fd, VIDIOC_QBUF, &bufd))
    {
        M_ERROR(MODULE_NAME, "Unable to queue buffer on %d: %s", fd, strerror(errno));
        return -1;
    }
    return bufd.bytesused;
}

int cam_dequeue_buffer(int fd)
{
    struct v4l2_buffer bufd = {0};
    bufd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufd.memory = V4L2_MEMORY_MMAP;
    bufd.index = 0;
    if (-1 == ioctl(fd, VIDIOC_DQBUF, &bufd))
    {
        M_ERROR(MODULE_NAME, "Unable to dequeue buffer: %s", strerror(errno));
        return -1;
    }
    return bufd.bytesused;
}

int cam_jpeg_commpress(cam_setting_t *opts, uint8_t **out)
{
    uint8_t *tmp = opts->raw_buffer;
    struct jpeg_compress_struct cinfo = {0};
    struct jpeg_error_mgr jerror = {0};
    cinfo.err = jpeg_std_error(&jerror);
    jerror.trace_level = 10;
    cinfo.err->trace_level = 10;
    jpeg_create_compress(&cinfo);

    unsigned long outbuffer_size = 0;
    jpeg_mem_dest(&cinfo, out, &outbuffer_size);
    cinfo.image_width = opts->width;
    cinfo.image_height = opts->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, opts->jpeg_quality, true);
    jpeg_start_compress(&cinfo, true);
    //unsigned counter = 0;
    JSAMPROW row_pointer[1];
    row_pointer[0] = NULL;

    while (cinfo.next_scanline < cinfo.image_height)
    {
        row_pointer[0] = (JSAMPROW)(&tmp[cinfo.next_scanline * opts->width * 3]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return outbuffer_size;
}

int cam_grab_frame(cam_setting_t *opts)
{
    if (cam_queue_buffer(opts->fd) == -1)
        return -1;
    //Wait for io operation
    fd_set fds;
    uint8_t *jpeg_frame = NULL;
    FD_ZERO(&fds);
    FD_SET(opts->fd, &fds);
    struct timeval tv = {0};
    tv.tv_sec = 2; //set timeout to 2 second
    int r = select(opts->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1)
    {
        M_ERROR(MODULE_NAME, "Error on Waiting for Frame: %s", strerror(errno));
        return -1;
    }
    if (r == 0)
    {
        // time out
        return -1;
    }
    size_t size = cam_jpeg_commpress(opts, &jpeg_frame);
    // send to other endpoint
    int file = open("/home/root/output.jpeg", O_RDWR | O_CREAT, 0777);
    if (file == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open file for save: %s", strerror(errno));
    }
    else
    {
        if (write(file, jpeg_frame, size) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to write frame to file: %s", strerror(errno));
        }
        close(file);
        M_LOG(MODULE_NAME, "written %d bytes to file", (int)size);
    }

    free(jpeg_frame);
    //size is obtained from the query_buffer function
    return cam_dequeue_buffer(opts->fd);
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
static int cam_send_frame_client(cam_setting_t *opts, int sock, bst_node_t *client)
{
    if (opts->queued == 0)
    {
        return 0;
    }
    tunnel_msg_t msg;
    uint8_t *jpeg_frame = NULL;
    if (clients)
    {
        size_t size = cam_jpeg_commpress(opts, &jpeg_frame);
        // send to other endpoint
        //size is obtained from the query_buffer function

        msg.header.type = CHANNEL_DATA;
        msg.header.size = size;
        msg.data = (uint8_t *)jpeg_frame;
        void *args[2];
        args[0] = (void *)&msg;
        args[1] = (void *)&sock;
        bst_for_each(clients, send_data, args, 2);
        free(jpeg_frame);
    }
    if (cam_dequeue_buffer(opts->fd) == -1)
    {
        return -1;
    }
    opts->queued = 0;
    return 0;
}

static int cam_stop_streaming(int fd)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1)
    {
        M_ERROR(MODULE_NAME, "Error on VIDIOC_STREAMOFF: %s", strerror(errno));
        return -1;
    }
    return 0;
}
static int cam_cleanup(cam_setting_t *opts, int close_fd)
{
    if (video_setting.queued == 1)
    {
        if (cam_dequeue_buffer(video_setting.fd) == -1)
        {
            return -1;
        }
        else
        {
            video_setting.queued = 0;
        }
    }
    if (cam_stop_streaming(opts->fd) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to stop streaming");
        return -1;
    }
    if (cam_release_buffer(opts) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to release previously mapped buffer");
        return -1;
    }
    if (close_fd && opts->fd > 0)
    {
        (void)close(opts->fd);
    }
    if (close_fd && opts->timerfd > 0)
    {
        (void)close(opts->timerfd);
    }
    return 0;
}
static int cam_init_timer(cam_setting_t* opts)
{
    long period = 0;
    if(opts->timerfd != -1)
    {
        (void) close(opts->timerfd);
    }
    opts->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (opts->timerfd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to create timerfd: %s", strerror(errno));
        return -1;
    }
    else
    {
        period = (long) (1e9 / opts->fps);
        M_LOG(MODULE_NAME, "Frame period set to %lu", period);
        struct itimerspec ch_period =
            {
                .it_interval = {.tv_sec = 0, .tv_nsec = period},
                .it_value = {.tv_sec = 0, .tv_nsec = period}, /* first wake-up = interval */
            };

        if (timerfd_settime(opts->timerfd, 0 /* no flags */, &ch_period, NULL) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to set framerate period: %s", strerror(errno));
            (void)close(opts->timerfd);
            opts->timerfd = -1;
            return -1;
        }
    }
    return 0;
}
static int cam_apply_setting(cam_setting_t *opts)
{
    if (opts->raw_buffer != NULL)
    {
        if (cam_cleanup(opts, 1) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to cleanup setting");
            return -1;
        }
    }
    opts->fd = open(opts->dev_name, O_RDWR);
    if (opts->fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open device: %s", video_setting.dev_name);
        return -1;
    }
    if (cam_set_format(opts) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to set format");
        return -1;
    }
    // 2 request buffer
    if (cam_request_buffer(opts->fd, 1) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request buffer");
        return -1;
    }
    // 3 query buffer
    int buffer_size = cam_query_buffer(opts);
    if (buffer_size == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to query buffer");
        return -1;
    }
    (void) cam_init_timer(opts);

    return 0;
}

static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
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

int main(const int argc, const char **argv)
{
    int sock, maxfd = -1;
    char buff[BUFFLEN + 1];
    tunnel_msg_t msg;
    int status;
    fd_set fd_in;
    uint64_t expirations_count;
    void *fargv[2];
    unsigned int offset = 0;
    if (argc != 4)
    {
        printf("Usage: %s path/to/hotline/socket channel_name video_dev\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);

    strncpy(video_setting.dev_name, argv[3], DEV_SIZE - 1);

    // default setting
    video_setting.width = 640;
    video_setting.height = 480;
    video_setting.fps = 5;
    video_setting.jpeg_quality = 60;
    video_setting.raw_buffer = NULL;
    video_setting.queued = 0;
    // apply the default setting
    if (cam_apply_setting(&video_setting) == -1)
    {
        exit(1);
    }
    sock = open_unix_socket((char *)argv[1]);
    if (sock == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
        cam_cleanup(&video_setting, 1);
        return -1;
    }
    // create a video channel on the tunnel
    msg.header.type = CHANNEL_OPEN;
    msg.header.channel_id = 0;
    msg.header.client_id = 0;
    M_LOG(MODULE_NAME, "Request to open the channel %s", argv[2]);
    (void)strncpy(buff, argv[2], MAX_CHANNEL_NAME);
    msg.header.size = strlen(buff);
    msg.data = (uint8_t *)buff;
    if (msg_write(sock, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write message to hotline");
        cam_cleanup(&video_setting, 1);
        (void)close(sock);
        exit(1);
    }
    M_LOG(MODULE_NAME, "Wait for comfirm creation of %s", argv[2]);
    // now wait for message
    if (msg_read(sock, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read message from hotline");
        cam_cleanup(&video_setting, 1);
        (void)close(sock);
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
    // start streaming
    if (cam_start_streaming(video_setting.fd) == -1)
    {
        running = 0;
    }
    while (running)
    {
        FD_ZERO(&fd_in);
        FD_SET(sock, &fd_in);
        FD_SET(video_setting.fd, &fd_in);
        maxfd = sock > video_setting.fd ? sock : video_setting.fd;

        if (clients != NULL && video_setting.queued == 0)
        {
            if (cam_queue_buffer(video_setting.fd) == -1)
            {
                running = 0;
            }
            else
            {
                video_setting.queued = 1;
            }
        }
        if(clients == NULL && video_setting.timerfd != -1)
        {
            (void) close(video_setting.timerfd);
            video_setting.timerfd = -1;
        }

        status = select(maxfd + 1, &fd_in, NULL, NULL, NULL);
        switch (status)
        {
        case -1:
            M_LOG(MODULE_NAME, "Error %d on select()\n", errno);
            running = 0;
            break;
        case 0:
            break;
        default:
            if (FD_ISSET(sock, &fd_in))
            {
                if (msg_read(sock, &msg) == -1)
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
                        if(clients == NULL)
                        {
                            (void) cam_init_timer(&video_setting);
                        }
                        clients = bst_insert(clients, msg.header.client_id, NULL);
                        // send back the ctl message
                        msg.header.type = CHANNEL_CTRL;
                        msg.header.size = 6;
                        msg.data = (uint8_t *)buff;
                        (void)memcpy(buff, &video_setting.width, sizeof(video_setting.width));
                        (void)memcpy(buff + sizeof(video_setting.width), &video_setting.height, sizeof(video_setting.height));
                        buff[sizeof(video_setting.width) + sizeof(video_setting.height)] = video_setting.fps;
                        buff[sizeof(video_setting.width) + sizeof(video_setting.height) + 1] = video_setting.jpeg_quality;
                        if (msg_write(sock, &msg) == -1)
                        {
                            running = 0;
                        }
                        break;

                    case CHANNEL_UNSUBSCRIBE:
                        M_LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", msg.header.client_id);
                        clients = bst_delete(clients, msg.header.client_id);
                        break;
                    case CHANNEL_CTRL:
                        // apply setting here
                        // [w_16,h_16,fps_8,q_8]
                        if (msg.header.size == 6)
                        {
                            offset = 0;
                            (void)memcpy(&video_setting.width, msg.data, 2);
                            offset += 2;
                            (void)memcpy(&video_setting.height, msg.data + offset, 2);
                            offset += 2;
                            (void)memcpy(&video_setting.fps, msg.data + offset, 1);
                            offset++;
                            (void)memcpy(&video_setting.jpeg_quality, msg.data + offset, 1);
                            M_LOG(MODULE_NAME, "Client request width: %d, height: %d, FPS: %d, JPEG quality: %d",
                                  video_setting.width,
                                  video_setting.height,
                                  video_setting.fps,
                                  video_setting.jpeg_quality);
                            if (cam_apply_setting(&video_setting) == -1)
                            {
                                M_ERROR(MODULE_NAME, "Unable to apply video setting");
                                cam_cleanup(&video_setting, 0);
                                running = 0;
                            }
                            else
                            {
                                // restart the streaming
                                if (cam_start_streaming(video_setting.fd) == -1)
                                {
                                    running = 0;
                                }
                                else
                                {
                                    // send back the ctl message
                                    msg.header.type = CHANNEL_CTRL;
                                    msg.header.size = 6;
                                    msg.data = (uint8_t *)buff;
                                    (void)memcpy(buff, &video_setting.width, sizeof(video_setting.width));
                                    (void)memcpy(buff + sizeof(video_setting.width), &video_setting.height, sizeof(video_setting.height));
                                    buff[sizeof(video_setting.width) + sizeof(video_setting.height)] = video_setting.fps;
                                    buff[sizeof(video_setting.width) + sizeof(video_setting.height) + 1] = video_setting.jpeg_quality;
                                    fargv[0] = (void *)&msg;
                                    fargv[1] = (void *)&sock;
                                    bst_for_each(clients, send_data, fargv, 2);
                                }
                            }
                        }
                        else
                        {
                            M_ERROR(MODULE_NAME, "Invalid control message size: %d from client %d, expected 8", msg.header.size, msg.header.client_id);
                        }
                        break;

                    default:
                        M_LOG(MODULE_NAME, "Client %d send message of type %d",
                              msg.header.client_id, msg.header.type);
                        break;
                    }
                }
            }
            else if (FD_ISSET(video_setting.fd, &fd_in) && clients != NULL)
            {
                if (cam_send_frame_client(&video_setting, sock, clients) == -1)
                {
                    running = 0;
                }
                else
                {
                    // check timeout
                    if(video_setting.timerfd > 0)
                    {
                        if(read(video_setting.timerfd, &expirations_count, sizeof(expirations_count)) != (int)sizeof(expirations_count))
                        {
                            M_ERROR(MODULE_NAME, "Unable to read timer: %s", strerror(errno));
                        }
                        else if (expirations_count > 1u)
                        {
                            M_ERROR(MODULE_NAME, "LOOP OVERFLOW COUNT: %lu", (long unsigned int)expirations_count);
                        }
                    }
                }
            }
            else
            {
                // sleep to save CPU 100 ms
                usleep(100000);
            }
        }
    }

    (void)cam_cleanup(&video_setting, 1);
    // unsubscribe all client
    fargv[0] = (void *)&sock;
    bst_for_each(clients, unsubscribe, fargv, 1);
    // close the channel
    M_LOG(MODULE_NAME, "Close the channel %s (%d)", argv[2], sock);
    msg.header.type = CHANNEL_CLOSE;
    msg.header.size = 0;
    msg.data = NULL;
    if (msg_write(sock, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request channel close");
    }
    // close all opened terminal

    (void)msg_read(sock, &msg);
    (void)close(sock);
}
