#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <antd/list.h>
#include <antd/bst.h>

#include "../tunnel.h"

#define MODULE_NAME     "vterm"

typedef struct{
	int     fdm;
	pid_t   pid;
	int     cid;
} vterm_proc_t;

static bst_node_t* processes = NULL;

static volatile int running = 1;

static void int_handler(int dummy) {
    (void) dummy;
    running = 0;
}

static vterm_proc_t* terminal_new(void)
{
    int fdm, fds, rc;
    pid_t pid;
    vterm_proc_t* proc = NULL;
    // Check arguments
    fdm = posix_openpt(O_RDWR);
    if (fdm < 0)
    {
        M_LOG(MODULE_NAME, "Error on posix_openpt(): %s\n", strerror(errno));
        return NULL;
    }
    
    rc = grantpt(fdm);
    if (rc != 0)
    {
        M_LOG(MODULE_NAME, "Error on grantpt(): %s\n", strerror(errno));
        return NULL;
    }
    rc = unlockpt(fdm);
    if (rc != 0)
    {
        M_LOG(MODULE_NAME, "Error on unlockpt(): %s\n", strerror(errno));
        return NULL;
    }
    
    // Open the slave side ot the PTY
    fds = open(ptsname(fdm), O_RDWR);
    
    // Create the child process
    pid = fork();
    if (pid)
    {
        // parent
        proc = (vterm_proc_t*)malloc(sizeof(vterm_proc_t));
        proc->fdm = fdm;
        proc->pid = pid;
        return proc;
    }
    else
    {
        //struct termios slave_orig_term_settings; // Saved terminal settings
        //struct termios new_term_settings; // Current terminal settings
        
        // CHILD
        
        // Close the master side of the PTY
        close(fdm);
        
        // Save the defaults parameters of the slave side of the PTY
        //rc = tcgetattr(fds, &slave_orig_term_settings);
        
        // Set RAW mode on slave side of PTY
        //new_term_settings = slave_orig_term_settings;
        //cfmakeraw (&new_term_settings);
        //tcsetattr (fds, TCSANOW, &new_term_settings);
        
        // The slave side of the PTY becomes the standard input and outputs of the child process
        // we use cook mode here
        close(0); // Close standard input (current terminal)
        close(1); // Close standard output (current terminal)
        close(2); // Close standard error (current terminal)
        
        rc = dup(fds); // PTY becomes standard input (0)
        rc = dup(fds); // PTY becomes standard output (1)
        rc = dup(fds); // PTY becomes standard error (2)
        
        // Now the original file descriptor is useless
        close(fds);
        
        // Make the current process a new session leader
        setsid();
        
        // As the child is a session leader, set the controlling terminal to be the slave side of the PTY
        // (Mandatory for programs like the shell to make them manage correctly their outputs)
        ioctl(0, TIOCSCTTY, 1);
        
        //system("/bin/bash");
        rc = system("TERM=linux login");
        //M_LOG("%s\n","Terminal exit");
        _exit(1);
    }
}

static void terminal_kill(int client_id, int should_delete)
{
    // find the proc
    bst_node_t* node = bst_find(processes, client_id);
    vterm_proc_t* proc;
    if(node != NULL)
    {
        proc = (vterm_proc_t*)node->data;
        if(proc != NULL)
        {
            (void) close(proc->fdm);
            M_LOG(MODULE_NAME, "Kill the process %d", proc->pid);
            if(kill(proc->pid, SIGKILL) == - 1)
            {
                M_ERROR(MODULE_NAME, "Unable to kill process %d: %s", proc->pid, strerror(errno));
            }
            else
            {
                (void)waitpid(proc->pid, NULL, 0);
            }
            free(node->data);
            if(should_delete)
                processes = bst_delete(processes, node->key);
            // wait child

        }
    }
}

static int terminal_write(tunnel_msg_t* msg)
{
    // TODO: control frame e.g. for window resize
     bst_node_t* node = bst_find(processes, msg->header.client_id);
    vterm_proc_t* proc;
    if(node != NULL)
    {
        proc = (vterm_proc_t*)node->data;
        if(proc != NULL)
        {
            if(write(proc->fdm, msg->data, msg->header.size) == -1)
            {
                M_ERROR(MODULE_NAME, "Unable to write data to the terminal corresponding to client %d", msg->header.client_id);
                return -1;
            }
        }
        else
        {
            M_ERROR(MODULE_NAME, "Unable to find the process linked to client %d", msg->header.client_id);
            return -1;
        }
    }
    else
    {
        M_ERROR(MODULE_NAME, "Unable to find the process from processes list for %d", msg->header.client_id);
        return -1;
    }
    return 0;
}

static void unsubscribe(bst_node_t* node, void** args, int argc)
{
    (void) argc;
    tunnel_msg_t msg;
    int* ufd = (int*) args[0];
    vterm_proc_t* proc = (vterm_proc_t*) node->data;
    if(proc != NULL)
    {
        msg.header.type = CHANNEL_UNSUBSCRIBE;
		msg.header.client_id = proc->cid;
		msg.header.size = 0;
		terminal_kill(proc->cid, 0);
		if(msg_write(*ufd, &msg) == -1)
		{
		    M_ERROR(MODULE_NAME, "Unable to request unsubscribe to client %d", proc->cid);
		}
    }
}

static void set_sock_fd(bst_node_t* node, void** args, int argc)
{
    (void) argc;
    tunnel_msg_t msg;
    pid_t wpid;
    fd_set* fd_in = (fd_set*) args[1];
    int* max_fd = (int*)args[2];
    list_t* list_p = (list_t*) args[3];
    int* ufd = (int*) args[0];
    
    vterm_proc_t* proc = (vterm_proc_t*) node->data;
     
    if(proc != NULL)
    {
        // monitor the pid
        wpid = waitpid(proc->pid, NULL, WNOHANG);
    	if(wpid == -1 || wpid > 0)
    	{
    		// child exits
    		M_LOG(MODULE_NAME, "Terminal linked to client %d exits\n", proc->cid);
    		unsubscribe(node, args, argc);
    		list_put_ptr(list_p, node);
    	}
    	else
    	{
    	    FD_SET(proc->fdm, fd_in);
            if(*max_fd < proc->fdm)
            {
                *max_fd = proc->fdm;
            }
    	}
    }
}

static void terminal_monitor(bst_node_t* node, void** args, int argc)
{
    (void) argc;
    int* ufd = (int*) args[0];
    fd_set* fd_in = (fd_set*) args[1];
    list_t* list = (list_t*) args[3];
    char buff[BUFFLEN];
    tunnel_msg_t msg;
    int rc;
    vterm_proc_t* proc = (vterm_proc_t*) node->data;
     
    if(proc != NULL && FD_ISSET(proc->fdm, fd_in))
    {
        if ((rc = read(proc->fdm, buff, BUFFLEN)) > 0)
        {
            // Send data to client
            msg.header.client_id = node->key;
            msg.header.type = CHANNEL_DATA;
            msg.header.size = rc;
            msg.data = buff;
            if(msg_write(*ufd, &msg) == -1)
            {
                terminal_kill(node->key, 0);
                M_ERROR(MODULE_NAME,"Unable to send data to client %d", msg.header.client_id);
                list_put_ptr(list, node);
            }
        }
        else
        {
            if (rc < 0)
            {
            	M_LOG(MODULE_NAME, "Error on read standard input: %s\n", strerror(errno));
            	terminal_kill(node->key, 0);
            	list_put_ptr(list, node);
            }
        }
    }
}

static void terminal_resize(int cid, int col, int row)
{
    struct winsize win = {0, 0, 0, 0};
    bst_node_t* node = bst_find(processes, cid);
    vterm_proc_t* proc;
    if(node != NULL)
    {
        proc = (vterm_proc_t*) node->data;
        if (ioctl(proc->fdm, TIOCGWINSZ, &win) != 0)
        {
            if (errno != EINVAL)
            {
                M_ERROR(MODULE_NAME, "Unable to get terminal winsize setting: %s", strerror(errno));
                return;
            }
            memset(&win, 0, sizeof(win));
        }
        //printf("Setting winsize\n");
        if (row >= 0)
            win.ws_row = (unsigned short)row;
        if (col >= 0)
            win.ws_col = (unsigned short)col;

        if (ioctl(proc->fdm, TIOCSWINSZ, (char *)&win) != 0)
            M_ERROR(MODULE_NAME, "Unable to set terminal window size process linked to client %d: %s", cid, strerror(errno));
        }
    else
    {
        M_ERROR(MODULE_NAME, "Unable to find the terminal process linked to client %d", cid);
    }
}

int main(int argc, char** argv)
{
    int fd;
    tunnel_msg_t msg;
    fd_set fd_in;
    int status, maxfd;
    struct timeval timeout;
    char buff[MAX_CHANNEL_NAME+1];
    void *args[4];
    list_t list;
    item_t item;
    int ncol, nrow;
    
    LOG_INIT(MODULE_NAME);
    if(argc != 2)
    {
        printf("Usage: %s path/to/hotline/socket\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
	signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    M_LOG(MODULE_NAME, "Hotline is: %s", argv[1]);
    // now try to request new channel from hotline
    fd = open_unix_socket(argv[1]);
    if(fd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to open the hotline: %s", argv[1]);
        return -1;
    }
    msg.header.type = CHANNEL_OPEN;
    msg.header.channel_id = 0;
    msg.header.client_id = 0;
    M_LOG(MODULE_NAME, "Request to open the channel %s", MODULE_NAME);
    (void)strncpy(buff, MODULE_NAME,MAX_CHANNEL_NAME);
    msg.header.size = strlen(buff);
    msg.data = (uint8_t*) buff;
    if(msg_write(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to write message to hotline");
        (void) close(fd);
        return -1;
    }
    M_LOG(MODULE_NAME, "Wait for comfirm creation of %s", MODULE_NAME);
    // now wait for message
    if(msg_read(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to read message from hotline");
        (void) close(fd);
        return -1;
    }
    if(msg.header.type == CHANNEL_OK)
    {
        M_LOG(MODULE_NAME, "Channel created: %s", MODULE_NAME);
        if(msg.data)
            free(msg.data);
    }
    else
    {
        M_ERROR(MODULE_NAME, "Channel is not created: %s. Tunnel service responds with msg of type %d", MODULE_NAME, msg.header.type);
        if(msg.data)
            free(msg.data);
        running = 0;
    }

    // now read data
    while(running)
    {
        timeout.tv_sec = 0;
	    timeout.tv_usec = 500;
        FD_ZERO(&fd_in);
        FD_SET(fd, &fd_in);
        maxfd = fd;
        
        // monitor processes
        list = list_init();
        args[1] = (void*) &fd_in;
        args[2] = (void*) &maxfd;
        args[3] = (void*) &list;
        args[0] = (void*) &fd;
        bst_for_each(processes, set_sock_fd, args, 4);
        list_for_each(item, list)
        {
            processes = bst_delete(processes, ((bst_node_t*)(item->value.ptr))->key);
            item->value.ptr = NULL;
        }
        list_free(&list);
        
        status = select(maxfd + 1, &fd_in, NULL, NULL, &timeout);
        
        switch (status)
	    {
            case -1:
                M_LOG(MODULE_NAME, "Error %d on select()\n", errno);
                running = 0;
                break;
            case 0:
                timeout.tv_sec = 0;
                timeout.tv_usec = 10000; // 5 ms
                select(0, NULL, NULL, NULL, &timeout);
                break;
            // we have data
            default:
                if (FD_ISSET(fd, &fd_in))
                {
                    if(msg_read(fd, &msg) == -1)
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
                            // create new process
                            vterm_proc_t* proc = terminal_new();
                            if(proc == NULL)
                            {
                                M_ERROR(MODULE_NAME, "Unable to create new terminal for client %d", msg.header.client_id);
                                // unsubscribe client
                                msg.header.type = CHANNEL_UNSUBSCRIBE;
                                msg.header.size = 0;
                                if(msg_write(fd, &msg) == -1)
                                {
                                    M_LOG(MODULE_NAME,"Unable to request unsubscribe client %d", msg.header.client_id);
                                }
                            }
                            else
                            {
                                proc->cid = msg.header.client_id;
                                // insert new terminal to the list
                                processes = bst_insert(processes, msg.header.client_id, proc);
                            }
                            break;
                        
                        case CHANNEL_UNSUBSCRIBE:
                            M_LOG(MODULE_NAME, "Client %d unsubscribes to the chanel", msg.header.client_id);
                            terminal_kill(msg.header.client_id, 1);
                            break;
                        
                        case CHANNEL_CTRL:
                            if(msg.header.size == 8)
                            {
                                (void)memcpy(&ncol, msg.data, sizeof(ncol));
                                (void)memcpy(&nrow, msg.data + sizeof(ncol), sizeof(nrow));
                                M_LOG(MODULE_NAME, "Client %d request terminal window resize of (%d,%d)", msg.header.client_id, ncol, nrow);
                                terminal_resize(msg.header.client_id, ncol, nrow);

                            }
                            else
                            {
                                M_ERROR(MODULE_NAME, "Invalid control message size: %d from client %d, expected 8", msg.header.size, msg.header.client_id);
                            }

                            break;

                        case CHANNEL_DATA:
                            if(terminal_write(&msg) == -1)
                            {
                                M_ERROR(MODULE_NAME, "Unable to write data to terminal corresponding to client %d", msg.header.client_id);
                                terminal_kill(msg.header.client_id, 1);
                                msg.header.type = CHANNEL_UNSUBSCRIBE;
                                msg.header.size = 0;
                                if(msg_write(fd, &msg) == -1)
                                {
                                    M_LOG(MODULE_NAME,"Unable to request unsubscribe client %d", msg.header.client_id);
                                }
                            }
                            break;
                        
                        default:
                            M_LOG(MODULE_NAME, "Client %d send message of type %d",
                                msg.header.client_id, msg.header.type);
                            break;
                        }
                        if(msg.data)
                        {
                            free(msg.data);
                        }
                    }
                }
                else
                {
                    // on the processes side
                    list = list_init();
                    bst_for_each(processes, terminal_monitor, args, 4);
                    list_for_each(item, list)
                    {
                        processes = bst_delete(processes, ((bst_node_t*)(item->value.ptr))->key);
                        item->value.ptr = NULL;
                    }
                    list_free(&list);
                }
        
        }
    }

    // unsubscribe all clients
    args[0] = (void*) &fd;
    bst_for_each(processes, unsubscribe, args, 1);
    (void)bst_free(processes);
    // close the channel
    M_LOG(MODULE_NAME, "Close the channel %s (%d)", MODULE_NAME, fd);
    msg.header.type = CHANNEL_CLOSE;
    msg.header.size = 0;
    msg.data = NULL;
    if( msg_write(fd, &msg) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to request channel close");
    }
    // close all opened terminal
    
    (void)msg_read(fd, &msg);
    (void) close(fd);
    return 0;
}