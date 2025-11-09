// os/log.h stub for host testing
#pragma once

// Stub os_log_t
typedef struct os_log_s* os_log_t;

// Default log handle stub
#define OS_LOG_DEFAULT ((os_log_t)0)

// Log type stubs
#define OS_LOG_TYPE_DEFAULT  0
#define OS_LOG_TYPE_INFO     1
#define OS_LOG_TYPE_DEBUG    2
#define OS_LOG_TYPE_ERROR    3
#define OS_LOG_TYPE_FAULT    4

// Stub macros - do nothing in tests
#define os_log(log, format, ...) ((void)0)
#define os_log_type(log, type, format, ...) ((void)0)
