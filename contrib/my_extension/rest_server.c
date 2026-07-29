#include "rest_server.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>

#define MAX_ENDPOINTS 100

extern int *shared_value;

static Endpoint endpoints[MAX_ENDPOINTS];
static int endpoints_count = 0;

WaitEventSet *event_set = NULL;

static int server_socket = -1;

void
register_endpoint(const char *url, endpoint_handler handler)
{
    if (endpoints_count < MAX_ENDPOINTS)
    {
        endpoints[endpoints_count].url = url;
        endpoints[endpoints_count].handler = handler;
        endpoints_count++;
    }
}

void
process_rest(void)
{
    server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        elog(LOG, "process_rest: socket error");
        return;
    }

    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        elog(LOG, "process_rest: bind error");
        close(server_socket);
        return;
    }

    listen(server_socket, 100);

    event_set = CreateWaitEventSet(NULL, 1);

    AddWaitEventToSet(event_set, WL_SOCKET_READABLE, server_socket, NULL, NULL);

    elog(LOG, "rest: server started on port 8080");
}

void
rest_server_poll(void)
{
    if (server_socket < 0 || event_set == NULL)
    {
        return;
    }

    WaitEvent event;

    int number_of_fd = WaitEventSetWait(event_set, 0, &event, 1, 0);

    if (number_of_fd > 0 && event.events & WL_SOCKET_READABLE)
    {
        int client_socket = accept(server_socket, NULL, NULL);
        elog(LOG, "rest_server_poll: new connection accepted");

        if (client_socket >= 0)
        {
            char buffer[4096];
            memset(buffer, 0, sizeof(buffer));

            read(client_socket, buffer, sizeof(buffer)-1);

            char method[16], url[256];
            sscanf(buffer, "%s %s", method, url);

            char *body = strstr(buffer, "\r\n\r\n");
            if (body)
            {
                body += 4;
            }

            char response[4096];

            for (int i = 0; i < endpoints_count; i++)
            {

                if (strcmp(url, endpoints[i].url) == 0)
                {
                    const char *response_body = endpoints[i].handler(method, body, NULL);

                    snprintf(response, sizeof(response),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n"
                            "%s", strlen(response_body), response_body);
                    break;
                }
                else
                {
                    const char *error_body = "Invalid request.\n"
                            "Try: \ncurl -X <method> http:/<host>:<port>/<endpoint> -H <headers> -d <body>\n\n"
                            "example:\n"
                            "curl -X POST http:/localhost:8080/value/set "
                            "-H 'Content-Type: application/json' "
                            "-d '{\"value\": 300}'\n";

                    snprintf(response, sizeof(response), 
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n"
                            "%s", strlen(error_body), error_body);
                }
            }
            write(client_socket, response, strlen(response));

            close(client_socket);
        }
    }

}