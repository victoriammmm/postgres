#ifndef HTTP_HANDLE_H
#define HTTP_HANDLE_H

#include "rest_server.h"

extern const char *handler_get_value(const char *method, const char *body, void *user_data);
extern const char *handler_post_value(const char *method, const char *body, void *user_data);

#endif