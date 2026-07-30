#include "rest_server.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdbool.h>

#define MAX_ENDPOINTS 100

extern int *shared_value;

static Endpoint endpoints[MAX_ENDPOINTS];
static int endpoints_count = 0;

WaitEventSet *event_set = NULL;

static int server_socket = -1;
static int client_fd = -1;

static bool need_recreate = false;

static char client_response[4096];
static size_t client_response_len = 0;
static size_t client_written = 0;
static bool client_response_ready = false;

static char client_read_buffer[4096];
static size_t client_read_pos = 0;

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
rest_init(void)
{
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
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

    event_set = CreateWaitEventSet(NULL, 2);

    /*проблема: в waiteventset нельзя удалить или изменить fd,
    можно только занулить маску, чтобы больше не было отслеживания за этим fd 
    или пересоздать полностью waiteventset
    
    мне оба варианта не очень нравятся, но лучше пересоздавать и добавлять
    снова fd сервера, чем бесконечно добавлять клиентские fd в event set*/

    AddWaitEventToSet(event_set, WL_SOCKET_READABLE, server_socket, NULL, NULL);

    elog(LOG, "rest: server started on port 8080");
}

static void
reset_client(void)
{
    if (client_fd >= 0){
        close(client_fd);
    }
    client_fd = -1;
    client_response_ready = false;
    client_written = 0;
    client_response_len = 0;
    need_recreate = true;
    client_read_pos = 0;
    memset(client_read_buffer, 0, sizeof(client_read_buffer));
}

void
rest_server_poll(void)
{
    if (server_socket < 0 || event_set == NULL)
    {
        return;
    }

    if (need_recreate && client_fd == -1)
    {
        FreeWaitEventSet(event_set);
        event_set = CreateWaitEventSet(NULL, 2);
        AddWaitEventToSet(event_set, WL_SOCKET_READABLE, server_socket, NULL, NULL);
        need_recreate = false;
    }

    WaitEvent event;

    int number_of_fd = WaitEventSetWait(event_set, 0, &event, 1, 0);

    if (number_of_fd > 0)
    {
        if ((event.fd == server_socket) && (event.events & WL_SOCKET_READABLE))
        {
            int client_socket = accept(server_socket, NULL, NULL);

            if (client_socket < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    return;
                }
                elog(LOG, "rest_server_poll: accept error");
                return;
            }

            client_fd = client_socket;

            AddWaitEventToSet(event_set, WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE, client_fd, NULL, NULL);

            client_response_ready = false;
            client_written = 0;
            client_response_len = 0;

            elog(LOG, "rest_server_poll: new connection accepted fd: %d", client_socket);
        }

        else if (event.fd == client_fd)
        {
            if (event.events & WL_SOCKET_READABLE && !client_response_ready)
            {
                ssize_t bytes_read = read(client_fd, client_read_buffer + client_read_pos, 
                                          sizeof(client_read_buffer) - client_read_pos - 1);
                if (bytes_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        return;
                    }
                    elog(LOG, "rest_server_poll: read error");
                    reset_client();
                    return;
                }
                else if (bytes_read == 0) {
                    elog(LOG, "rest_server_poll: client closed connection");
                    reset_client();
                    return;
                }

                client_read_pos += bytes_read;
                client_read_buffer[client_read_pos] = '\0';

                elog(LOG, "rest_server_poll: read %zd bytes, read %d bytes in total", bytes_read, strlen(client_read_buffer));

                if (strstr(client_read_buffer, "\r\n\r\n") == NULL)
                {
                    return;
                }

                char method[16], url[256];
                if (sscanf(client_read_buffer, "%15s %255s", method, url) != 2)
                {
                    const char *error_body = "Bad request\n";
                    snprintf(client_response, sizeof(client_response),
                            "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n"
                            "%s", strlen(error_body), error_body);
                    client_response_len = strlen(client_response);
                    client_written = 0;
                    client_response_ready = true;
                }

                else
                {
                    char *body = strstr(client_read_buffer, "\r\n\r\n");
                    if (body)
                    {
                        body += 4;
                    }

                    bool endpoint_found = false;

                    for (int i = 0; i < endpoints_count; i++)
                    {

                        if (strcmp(url, endpoints[i].url) == 0)
                        {
                            const char *response_body = endpoints[i].handler(method, body, NULL);

                            snprintf(client_response, sizeof(client_response),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: %zu\r\n"
                                    "\r\n"
                                    "%s", strlen(response_body), response_body);
                            endpoint_found = true;
                            break;
                        }
                    }
                    if (!endpoint_found)
                    {
                        const char *error_body = "Invalid request.\n"
                                "Try: \ncurl -X <method> http:/<host>:<port>/<endpoint> -H <headers> -d <body>\n\n"
                                "example:\n"
                                "curl -X POST http:/localhost:8080/value/set "
                                "-H 'Content-Type: application/json' "
                                "-d '{\"value\": 300}'\n";

                        snprintf(client_response, sizeof(client_response),
                                "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %zu\r\n"
                                "\r\n"
                                "%s", strlen(error_body), error_body);
                    }

                    client_response_len = strlen(client_response);
                    client_written = 0;
                    client_response_ready = true;
                    elog(LOG, "rest_server_poll: response ready (%zu bytes), waiting for write", client_response_len);
                }
            }

            if (event.events & WL_SOCKET_WRITEABLE && client_response_ready)
            {
                size_t remaining = client_response_len - client_written;

                ssize_t bytes_written = write(client_fd, client_response + client_written, remaining);
                if (bytes_written < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        return;
                    }
                    elog(LOG, "rest_server_poll: write error");
                    reset_client();
                    return;
                }
                client_written += bytes_written;
                elog(LOG, "rest_server_poll: %zd bytes written, %zu/%zu total", bytes_written, client_written, client_response_len);

                if (client_written >= client_response_len){
                    elog(LOG, "rest_server_poll: response sent completely");
                    reset_client();
                }
            }

        }
    }
}