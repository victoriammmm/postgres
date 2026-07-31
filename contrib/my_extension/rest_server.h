#ifndef REST_SERVER_H
#define REST_SERVER_H

#include "postgres.h"
#include <stdbool.h>

typedef const char *(*endpoint_handler)(const char *method, const char *body, void *user_data);

typedef struct
{
    const char *url;
    endpoint_handler handler;

} Endpoint;

typedef struct
{
    bool active;
    int fd;
    char response[4096];
    size_t response_len;
    char read_buffer[4096];
    size_t read_pos;
    size_t written;
    bool response_ready;

} Client;

extern void rest_init(void);
extern void register_endpoint(const char *url, endpoint_handler handler);
extern void rest_server_poll(void);

#endif
