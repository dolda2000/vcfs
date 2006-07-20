#ifndef _LOG_H
#define _LOG_H

#include <syslog.h>

extern void (*flog)(int level, char *msg, ...);

void logstderr(int level, char *msg, ...);
void logsyslog(int level, char *msg, ...);

#endif
