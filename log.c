#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <stdarg.h>

#include "log.h"

void logstderr(int level, char *msg, ...)
{
    va_list args;
    
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    fputc('\n', stderr);
    va_end(args);
}

void logsyslog(int level, char *msg, ...)
{
    va_list args;
    
    va_start(args, msg);
    vsyslog(level, msg, args);
    va_end(args);
}

void (*flog)(int, char *, ...) = logstderr;
