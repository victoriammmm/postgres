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
#define MAX_CLIENTS 20

extern int *shared_value;

static Endpoint endpoints[MAX_ENDPOINTS];
static Client clients[MAX_CLIENTS];

static int endpoints_count = 0;

WaitEventSet *event_set = NULL;

static int server_socket = -1;

static bool need_recreate = false;

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
        elog(LOG, "rest: socket error");
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
        elog(LOG, "rest: bind error");
        close(server_socket);
        return;
    }

    listen(server_socket, 100);

    event_set = CreateWaitEventSet(NULL, MAX_CLIENTS + 1);

    /*проблема: в waiteventset нельзя удалить или изменить fd,
    можно только занулить маску, чтобы больше не было отслеживания за этим fd 
    или пересоздать полностью waiteventset
    
    мне оба варианта не очень нравятся, но лучше пересоздавать и добавлять
    снова fd сервера, чем бесконечно добавлять клиентские fd в event set*/

    AddWaitEventToSet(event_set, WL_SOCKET_READABLE, server_socket, NULL, NULL);

    elog(LOG, "rest: server started on port 8080");
}

static void
close_slot(int slot)
{
    if (clients[slot].fd >= 0)
    {
        close(clients[slot].fd);
    }
    clients[slot].active = false;
    clients[slot].fd = -1;
    need_recreate = true;
}

int
find_free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            return i;
        }
    }
    return -1;
}

int
find_slot(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active && clients[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

void
rest_server_poll(void)
{
    if (server_socket < 0 || event_set == NULL)
    {
        return;
    }

    if (need_recreate)
    {
        FreeWaitEventSet(event_set);
        event_set = CreateWaitEventSet(NULL, MAX_CLIENTS + 1);
        AddWaitEventToSet(event_set, WL_SOCKET_READABLE, server_socket, NULL, NULL);

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].active)
            {
                AddWaitEventToSet(event_set, WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE, clients[i].fd, NULL, NULL);
            }
        }

        need_recreate = false;
    }

    WaitEvent events[MAX_CLIENTS + 1];

    int number_of_fd = WaitEventSetWait(event_set, 0, &events, MAX_CLIENTS + 1, 0);

    for (int i = 0; i < number_of_fd; i++)
    {
        if ((events[i].fd == server_socket) && (events[i].events & WL_SOCKET_READABLE))
        {
            int client_socket = accept(server_socket, NULL, NULL);

            if (client_socket < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    continue;
                }
                elog(LOG, "rest: accept error");
                continue;
            }

            int slot = find_free_slot();

            if (slot == -1) {
                elog(LOG, "rest: too many requests, try again later");
                close(client_socket);
                continue;
            }

            clients[slot].active = true;
            clients[slot].fd = client_socket;
            clients[slot].read_pos = 0;
            clients[slot].response_ready = false;
            clients[slot].response_len = 0;
            clients[slot].written = 0;
            memset(clients[slot].read_buffer, 0, sizeof(clients[slot].read_buffer));
            memset(clients[slot].response, 0, sizeof(clients[slot].response));

            int client_flags = fcntl(clients[slot].fd, F_GETFL, 0);
            fcntl(clients[slot].fd, F_SETFL, client_flags | O_NONBLOCK);

            AddWaitEventToSet(event_set, WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE, clients[slot].fd, NULL, NULL);

            elog(LOG, "rest: new connection accepted fd: %d, position: %d", client_socket, slot);
        }

        else
        {
            int slot = find_slot(events[i].fd);
            if (slot == -1) {
                elog(LOG, "rest: client not found");
                continue;
            }

            Client *client = &clients[slot];

            if (events[i].events & WL_SOCKET_READABLE && !client->response_ready)
            {
                ssize_t bytes_read = read(client->fd, client->read_buffer + client->read_pos, 
                                          sizeof(client->read_buffer) - client->read_pos - 1);
                if (bytes_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue;
                    }
                    elog(LOG, "rest: read error");
                    close_slot(slot);
                    continue;
                }
                else if (bytes_read == 0) {
                    elog(LOG, "rest: client closed connection");
                    close_slot(slot);
                    continue;
                }

                client->read_pos += bytes_read;
                client->read_buffer[client->read_pos] = '\0';

                elog(LOG, "rest: read %zd bytes, read %zu bytes in total", bytes_read, client->read_pos);

                if (strstr(client->read_buffer, "\r\n\r\n") == NULL)
                {
                    continue;
                }

                char method[16], url[256];
                if (sscanf(client->read_buffer, "%15s %255s", method, url) != 2)
                {
                    const char *error_body = "Bad request\n";
                    snprintf(client->response, sizeof(client->response),
                            "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n"
                            "%s", strlen(error_body), error_body);
                    client->response_len = strlen(client->response);
                    client->written = 0;
                    client->response_ready = true;
                }

                else
                {
                    char *body = strstr(client->read_buffer, "\r\n\r\n");
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

                            snprintf(client->response, sizeof(client->response),
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

                        snprintf(client->response, sizeof(client->response),
                                "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %zu\r\n"
                                "\r\n"
                                "%s", strlen(error_body), error_body);
                    }

                    client->response_len = strlen(client->response);
                    client->written = 0;
                    client->response_ready = true;
                    elog(LOG, "rest: response ready (%zu bytes), waiting for write", client->response_len);
                }
            }

            if (events[i].events & WL_SOCKET_WRITEABLE && client->response_ready)
            {
                size_t remaining = client->response_len - client->written;

                ssize_t bytes_written = write(client->fd, client->response + client->written, remaining);
                if (bytes_written < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue;
                    }
                    elog(LOG, "rest: write error");
                    close_slot(slot);
                    continue;
                }
                client->written += bytes_written;
                elog(LOG, "rest: %zd bytes written, %zu/%zu total", bytes_written, client->written, client->response_len);

                if (client->written >= client->response_len){
                    elog(LOG, "rest: response sent completely");
                    close_slot(slot);
                }
            }

        }
    }
}
