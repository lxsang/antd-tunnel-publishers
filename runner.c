
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <antd/ini.h>
#include <antd/list.h>
#include <antd/utils.h>
#include "log.h"

#define MODULE_NAME "runner"
#define MAX_STR_LEN 255u
/** up to 20 arguments*/
#define MAX_ARGC 10u

typedef struct
{
    char *name;
    char strings[MAX_ARGC * 2u][MAX_STR_LEN];
    char *envs[MAX_ARGC + 1];
    char *params[MAX_ARGC + 1];
    size_t n_params;
    size_t n_envs;
    size_t s_p;
} runner_cmd_t;

static volatile int running = 1;
static runner_cmd_t cmd;
static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
}
static void execute_command(list_t *plist)
{
    pid_t pid;
    ASSERT(cmd.name != NULL, "Invalid service handler (NULL)");
    pid = fork();
    ASSERT(pid != -1, "Unable to fork: %s", strerror(errno));
    if (pid == 0)
    {
        execve(cmd.params[0], &cmd.params[0], &cmd.envs[0]);
        // Nothing below this line should be executed by child process. If so,
        // it means that the execl function wasn't successfull, so lets exit:
        _exit(1);
    }
    else
    {
        M_LOG(MODULE_NAME, "Running service %s (%d)...", cmd.name, pid);
        (void)memset(&cmd, 0, sizeof(cmd));
        list_put_i(plist, pid);
    }
}

static int ini_handle(void *user_data, const char *section, const char *name,
                      const char *value)
{
    list_t *plist = (list_t *)user_data;
    ASSERT(cmd.s_p < MAX_ARGC * 2u, "String buffer overflow: %ld", cmd.s_p);
    ASSERT(cmd.n_params <= MAX_ARGC, "Max arguments reached %ld", cmd.n_params);
    ASSERT(cmd.n_envs <= MAX_ARGC, "Max environment variables reached %ld", cmd.n_envs);
    if ((cmd.name == NULL) || ! EQU(section, cmd.name))
    {
        if (cmd.params[0])
        {
            execute_command(plist);
        }
        cmd.n_params = 1u;
        (void)strncpy(cmd.strings[cmd.s_p], section, MAX_STR_LEN);
        cmd.name = cmd.strings[cmd.s_p];
        cmd.s_p++;
    }
    if (EQU(name, "exec"))
    {
        (void)strncpy(cmd.strings[cmd.s_p], value, MAX_STR_LEN);
        cmd.params[0] = cmd.strings[cmd.s_p];
        cmd.s_p++;
    }
    if (EQU(name, "param"))
    {
        (void)strncpy(cmd.strings[cmd.s_p], value, MAX_STR_LEN);
        cmd.params[cmd.n_params] = cmd.strings[cmd.s_p];
        cmd.s_p++;
        cmd.n_params++;
    }
    else
    {
        (void)snprintf(cmd.strings[cmd.s_p], MAX_STR_LEN, "%s=%s", name, value);
        cmd.envs[cmd.n_envs] = cmd.strings[cmd.s_p];
        cmd.s_p++;
        cmd.n_envs++;
    }
    return 1;
}

int main(int argc, char const *argv[])
{
    const char *conf_file;
    item_t item;
    pid_t w_pid;
    int pid_count;
    list_t pids;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    if (argc != 2)
    {
        printf("Usage: %s /path/to/conf.ini\n", argv[0]);
        return -1;
    }
    conf_file = argv[1];
    LOG_INIT(MODULE_NAME);
    M_LOG(MODULE_NAME,"config file is %s", conf_file);
    pids = list_init();
    (void)memset(&cmd, 0, sizeof(cmd));
    ASSERT(ini_parse(conf_file, ini_handle, &pids) == 0, "Can't load service from '%s'", conf_file);
    if (cmd.params[0] != NULL)
    {
        execute_command(&pids);
    }
    pid_count = list_size(pids);
    // monitoring the process
    while (running != 0 && pid_count != 0)
    {
        pid_count = 0;
        list_for_each(item, pids)
        {
            w_pid = waitpid((pid_t)item->value.i, NULL, WNOHANG);
            if (w_pid == -1 || w_pid > 0)
            {
                // child exits
                M_LOG(MODULE_NAME, "Process %d exits", item->value.i);
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
        if (item->value.i != -1)
        {
            if (kill(item->value.i, SIGKILL) == -1)
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
