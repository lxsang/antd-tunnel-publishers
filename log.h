#ifndef LOG_H
#define LOG_H

#include <syslog.h>
#include <assert.h>

#define LOG_INIT(m)                                                           \
    do                                                                        \
    {                                                                         \
        if ((getenv("debug") != NULL) && (strcmp(getenv("debug"), "1") == 0)) \
        {                                                                     \
            setlogmask(LOG_UPTO(LOG_INFO));                                   \
            M_LOG(MODULE_NAME, "DEBUG ENABLED");                              \
        }                                                                     \
        else                                                                  \
        {                                                                     \
            setlogmask(LOG_UPTO(LOG_NOTICE));                                 \
        }                                                                     \
        openlog((m), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_USER);              \
    } while (0)

#define M_LOG(m, a, ...) syslog((LOG_NOTICE), m "_log@[%s: %d]: " a "\n", __FILE__, \
                                __LINE__, ##__VA_ARGS__)
#define M_DEBUG(m, a, ...) syslog((LOG_INFO), m "_log@[%s: %d]: " a "\n", __FILE__, \
                                  __LINE__, ##__VA_ARGS__)
#define M_ERROR(m, a, ...) syslog((LOG_ERR), m "_error@[%s: %d]: " a "\n", __FILE__, \
                                  __LINE__, ##__VA_ARGS__)
#define ASSERT(b, m, ...)                                        \
    if (!(b))                                                    \
    {                                                            \
        M_ERROR(MODULE_NAME, "ASSERT ERROR: " m, ##__VA_ARGS__); \
        assert(b);                                               \
    }
#endif