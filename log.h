#ifndef LOG_H
#define LOG_H

#include <syslog.h>

#define LOG_INIT(m) do { \
        setlogmask (LOG_UPTO (LOG_NOTICE)); \
	    openlog ((m), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_USER); \
    } while(0)

#ifdef DEBUG
	#define M_LOG(m, a,...) syslog ((LOG_NOTICE),m "_log@[%s: %d]: " a "\n", __FILE__, \
		__LINE__, ##__VA_ARGS__)
#else
    #define M_LOG(m, a,...) do{}while(0)
#endif
#define M_ERROR(m, a,...) syslog ((LOG_ERR),m "_error@[%s: %d]: " a "\n", __FILE__, \
		__LINE__, ##__VA_ARGS__)
#endif