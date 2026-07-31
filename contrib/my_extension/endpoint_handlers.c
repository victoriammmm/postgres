#include "postgres.h"
#include "endpoint_handlers.h"
#include <stdio.h>

extern int *shared_value;

const char*
handler_get_value(const char *method, const char *body, void *user_data)
{
    static char result[64];
    snprintf(result, sizeof(result), "{\"value\": %d}\n", *shared_value);
    return result;
}

const char *
handler_post_value(const char *method, const char *body, void *user_data)
{
    int new_value;
    sscanf(body, "{\"value\": %d}", &new_value);
    *shared_value = new_value;
    return "{\"status\": \"ok\"}\n";
}