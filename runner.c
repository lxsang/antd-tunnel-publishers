
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <antd/list.h>
#include <antd/ini.h>
#include "log.h"

#define MODULE_NAME     "runner"

static volatile int running = 1;
static list_t pids;
static char socket_file[BUFFLEN];
static void int_handler(int dummy) {
    (void) dummy;
    running = 0;
}

static int ini_handle(void *user_data, const char *section, const char *name,
                      const char *value)
{
    pid_t pid;
    char* argv[3];
    UNUSED(user_data);
    if (EQU(section, "RUNNER"))
    {
        if(EQU(name, "socket"))
        {
            M_LOG(MODULE_NAME, "Configuration file is %s", value);
            (void)strncpy(socket_file,value,BUFFLEN);
        }
        else if(EQU(name, "service"))
        {
            M_LOG(MODULE_NAME, "Running service %s...", value);
            pid = fork();
            if(pid  == -1)
            {
                M_ERROR(MODULE_NAME, "Unable to fork: %s", strerror(errno));
                return 0;
            }
            if(pid == 0)
            {
                // child
                argv[0] = (char*)value;
                argv[1] = (char*)socket_file;
                argv[2] = NULL;
                execve(argv[0], &argv[0], NULL);
                // Nothing below this line should be executed by child process. If so,
                // it means that the execl function wasn't successfull, so lets exit:
                _exit(1);
            }
            // parent
            list_put_i(&pids, pid);
        }
        else
        {
            M_ERROR(MODULE_NAME, "Ignore unknown configuration %s = %s", name, value);
            return 0;
        }
    }
    else
    {
        return 0;
    }
    return 1;
}

int main(int argc, char const *argv[])
{
    const char* conf_file;
    item_t item;
    pid_t w_pid;
    int pid_count;
    signal(SIGPIPE, SIG_IGN);
	signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    if(argc != 2)
    {
        printf("Usage: %s /path/to/conf.ini", argv[0]);
        return -1;
    }
    conf_file = argv[1];
    
    pids = list_init();

    if (ini_parse(conf_file, ini_handle, NULL) < 0)
    {
        M_ERROR(MODULE_NAME, "Can't load '%s'", conf_file);
        return -1;
    }
    pid_count = list_size(pids);
    // monitoring the process
    while(running != 0 && pid_count != 0)
    {
        pid_count = 0;
        list_for_each(item, pids)
        {
            w_pid = waitpid((pid_t) item->value.i, NULL, WNOHANG);
            if(w_pid == -1 || w_pid > 0)
            {
                // child exits
                M_LOG(MODULE_NAME, "Process %d exits\n", item->value.i);
                item->value.i = -1;
            }
            else
            {
                pid_count++;
            }
        }
        // 500 ms
        usleep(500000);
    }
    // kill all the remaining processes
    list_for_each(item, pids)
    {
        if(item->value.i != -1)
        {
            if(kill(item->value.i, SIGKILL) == - 1)
            {
                M_ERROR(MODULE_NAME, "Unable to kill process %d: %s", item->value.i, strerror(errno));
            }
            else
            {
                (void)waitpid(item->value.i, NULL, 0);
            }
        }
    }
    list_free(&pids);
    return 0;
}
