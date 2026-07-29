#ifndef REST_SERVER_H
#define REST_SERVER_H

#include "postgres.h"

typedef const char *(*endpoint_handler)(const char *method, const char *body, void *user_data);

typedef struct
{
    const char *url;
    endpoint_handler handler;

} Endpoint;

extern void process_rest(void);
extern void register_endpoint(const char *url, endpoint_handler handler);
extern void rest_server_poll(void);

#endif