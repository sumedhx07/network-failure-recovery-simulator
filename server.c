#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "network.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define REQUEST_CAPACITY 16384
#define RESPONSE_CAPACITY 32768

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Any other existing #include statements */

#define PORT 8080
/* Keep your other existing #define lines here */

/* Paste the helper function here */
static const char *find_case_insensitive(const char *text, const char *pattern)
{
    size_t pattern_length = strlen(pattern);

    if (pattern_length == 0) {
        return text;
    }

    for (; *text != '\0'; text++) {
        if (strncasecmp(text, pattern, pattern_length) == 0) {
            return text;
        }
    }

    return NULL;
}

/* Your existing functions continue below */

static volatile sig_atomic_t server_running = 1;

typedef struct {
    char method[8];
    char path[256];
    char *body;
} HttpRequest;

static void stop_server(int signal_number) {
    (void)signal_number;
    server_running = 0;
}

static int send_all(int socket_fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(socket_fd, data + sent, length - sent, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static const char *status_text(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

static void send_response(int socket_fd, int status, const char *content_type,
                          const char *body, size_t body_length) {
    char header[512];
    int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n",
        status, status_text(status), content_type, body_length);

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return;
    }
    if (send_all(socket_fd, header, (size_t)header_length) == 0) {
        (void)send_all(socket_fd, body, body_length);
    }
}

static void send_json_error(int socket_fd, int status, const char *message) {
    char body[256];
    int length = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return;
    }
    send_response(socket_fd, status, "application/json; charset=utf-8", body,
                  (size_t)length);
}

static int parse_content_length(const char *headers) {
    const char *field = find_case_insensitive(headers, "Content-Length:");
    if (field == NULL) {
        return 0;
    }

    char *end = NULL;
    long length = strtol(field + strlen("Content-Length:"), &end, 10);
    if (end == field + strlen("Content-Length:") || length < 0 ||
        length > REQUEST_CAPACITY) {
        return -1;
    }

    return (int)length;
}

static int read_http_request(int socket_fd, char *buffer, size_t capacity,
                             HttpRequest *request) {
    size_t used = 0;
    size_t header_length = 0;
    int content_length = -1;
    char *headers_end = NULL;

    while (used < capacity - 1) {
        ssize_t received = recv(socket_fd, buffer + used, capacity - used - 1, 0);
        if (received == 0) {
            return 0;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        used += (size_t)received;
        buffer[used] = '\0';

        if (headers_end == NULL) {
            headers_end = strstr(buffer, "\r\n\r\n");
            if (headers_end != NULL) {
                header_length = (size_t)(headers_end - buffer) + 4;
                content_length = parse_content_length(buffer);
                if (content_length < 0) {
                    return -2;
                }
            }
        }

        if (headers_end != NULL &&
            used >= header_length + (size_t)content_length) {
            break;
        }
    }

    if (headers_end == NULL || used < header_length + (size_t)content_length) {
        return -2;
    }

    if (sscanf(buffer, "%7s %255s", request->method, request->path) != 2) {
        return -1;
    }
    request->body = buffer + header_length;
    return 1;
}

static bool read_json_string(const char *body, const char *key, char *value,
                             size_t value_capacity) {
    char search[64];
    int search_length = snprintf(search, sizeof(search), "\"%s\"", key);
    if (search_length < 0 || (size_t)search_length >= sizeof(search)) {
        return false;
    }

    const char *cursor = strstr(body, search);
    if (cursor == NULL) {
        return false;
    }
    cursor += (size_t)search_length;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor++ != ':') {
        return false;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor++ != '"') {
        return false;
    }

    size_t length = 0;
    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\' || length + 1 >= value_capacity) {
            return false;
        }
        value[length++] = *cursor++;
    }
    if (*cursor != '"') {
        return false;
    }
    value[length] = '\0';
    return true;
}

static bool read_json_integer(const char *body, const char *key, int *value) {
    char search[64];
    int search_length = snprintf(search, sizeof(search), "\"%s\"", key);
    if (search_length < 0 || (size_t)search_length >= sizeof(search)) {
        return false;
    }

    const char *cursor = strstr(body, search);
    if (cursor == NULL) {
        return false;
    }
    cursor += (size_t)search_length;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor++ != ':') {
        return false;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || parsed < INT_MIN ||
        parsed > INT_MAX) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != ',' && *end != '}' && *end != '\0') {
        return false;
    }

    *value = (int)parsed;
    return true;
}

static void serve_static_file(int socket_fd, const char *path) {
    const char *file_path = NULL;
    const char *content_type = NULL;

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        file_path = "index.html";
        content_type = "text/html; charset=utf-8";
    } else if (strcmp(path, "/styles.css") == 0) {
        file_path = "styles.css";
        content_type = "text/css; charset=utf-8";
    } else if (strcmp(path, "/app.js") == 0) {
        file_path = "app.js";
        content_type = "text/javascript; charset=utf-8";
    } else if (strcmp(path, "/favicon.svg") == 0) {
        file_path = "favicon.svg";
        content_type = "image/svg+xml";
    }

    if (file_path == NULL) {
        send_json_error(socket_fd, 404, "Resource not found.");
        return;
    }

    FILE *file = fopen(file_path, "rb");
    if (file == NULL) {
        send_json_error(socket_fd, 404, "Dashboard asset not found.");
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_json_error(socket_fd, 500, "Could not read dashboard asset.");
        return;
    }
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_json_error(socket_fd, 500, "Could not read dashboard asset.");
        return;
    }

    char *contents = malloc((size_t)file_size + 1);
    if (contents == NULL) {
        fclose(file);
        send_json_error(socket_fd, 500, "Not enough memory to serve asset.");
        return;
    }

    size_t bytes_read = fread(contents, 1, (size_t)file_size, file);
    bool read_failed = ferror(file) != 0 || bytes_read != (size_t)file_size;
    fclose(file);
    if (read_failed) {
        free(contents);
        send_json_error(socket_fd, 500, "Could not read dashboard asset.");
        return;
    }

    send_response(socket_fd, 200, content_type, contents, bytes_read);
    free(contents);
}

static void handle_api_request(int socket_fd, NetworkState *state,
                               const HttpRequest *request) {
    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/sim-api/v1/state") == 0) {
        char body[RESPONSE_CAPACITY];
        int length = network_to_json(state, body, sizeof(body));
        if (length < 0) {
            send_json_error(socket_fd, 500, "Could not build network snapshot.");
            return;
        }
        send_response(socket_fd, 200, "application/json; charset=utf-8", body,
                      (size_t)length);
        return;
    }

    if (strcmp(request->method, "GET") == 0) {
        send_json_error(socket_fd, 404, "API endpoint not found.");
        return;
    }
    if (strcmp(request->method, "POST") != 0) {
        send_json_error(socket_fd, 405, "Use GET or POST for this endpoint.");
        return;
    }

    if (strcmp(request->path, "/sim-api/v1/router-count") == 0) {
        int router_count;
        if (!read_json_integer(request->body, "count", &router_count)) {
            send_json_error(socket_fd, 400,
                            "Request body must include an integer count.");
            return;
        }
        if (network_set_router_count(state, router_count) < 0) {
            send_json_error(socket_fd, 400,
                            "Router count must be between 2 and 12.");
            return;
        }
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }

    if (strcmp(request->path, "/sim-api/v1/topology") == 0) {
        char topology[16];
        if (!read_json_string(request->body, "topology", topology,
                              sizeof(topology))) {
            send_json_error(socket_fd, 400,
                            "Request body must include a topology string.");
            return;
        }
        if (network_set_topology(state, topology) < 0) {
            send_json_error(socket_fd, 400,
                            "Topology must be star, ring, or mesh.");
            return;
        }
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }

    if (strcmp(request->path, "/sim-api/v1/routers/fail-all") == 0) {
        network_set_all_online(state, false);
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }
    if (strcmp(request->path, "/sim-api/v1/routers/recover-all") == 0) {
        network_set_all_online(state, true);
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }
    if (strcmp(request->path, "/sim-api/v1/reset") == 0) {
        network_reset(state);
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }

    int router_id = 0;
    char action[16];
    char trailing_character;
    int matched = sscanf(request->path, "/sim-api/v1/routers/%d/%15[^/]%c",
                         &router_id, action, &trailing_character);
    if (matched == 2 &&
        (strcmp(action, "fail") == 0 || strcmp(action, "recover") == 0)) {
        bool online = strcmp(action, "recover") == 0;
        if (network_set_router_online(state, router_id, online) < 0) {
            send_json_error(socket_fd, 400,
                            "Router ID is not in the current router range.");
            return;
        }
        send_response(socket_fd, 200, "application/json; charset=utf-8",
                      "{\"ok\":true}", strlen("{\"ok\":true}"));
        return;
    }

    send_json_error(socket_fd, 404, "API endpoint not found.");
}

static void handle_client(int socket_fd, NetworkState *state) {
    char buffer[REQUEST_CAPACITY];
    HttpRequest request = {0};
    int result = read_http_request(socket_fd, buffer, sizeof(buffer), &request);

    if (result == 0) {
        return;
    }
    if (result == -2) {
        send_json_error(socket_fd, 413, "Request is too large or incomplete.");
        return;
    }
    if (result < 0) {
        send_json_error(socket_fd, 400, "Malformed HTTP request.");
        return;
    }

    if (strncmp(request.path, "/sim-api/", 9) == 0) {
        handle_api_request(socket_fd, state, &request);
    } else if (strcmp(request.method, "GET") == 0) {
        serve_static_file(socket_fd, request.path);
    } else {
        send_json_error(socket_fd, 404, "Resource not found.");
    }
}

static int parse_port(const char *text, int *port) {
    if (text == NULL || *text == '\0') {
        *port = 8080;
        return 0;
    }

    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (*end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }
    *port = (int)parsed;
    return 0;
}

int main(void) {
    int port;
    if (parse_port(getenv("PORT"), &port) != 0) {
        fprintf(stderr, "Invalid PORT; expected a value from 1 to 65535.\n");
        return EXIT_FAILURE;
    }

    const char *host = getenv("HOST");
    if (host == NULL || *host == '\0') {
        host = "127.0.0.1";
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid HOST; use an IPv4 address such as 127.0.0.1.\n");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse_address = 1;
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                     sizeof(reuse_address));
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (listen(server_fd, 32) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {0};
    stop_action.sa_handler = stop_server;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;
    if (sigaction(SIGINT, &stop_action, NULL) < 0 ||
        sigaction(SIGTERM, &stop_action, NULL) < 0) {
        perror("sigaction");
        close(server_fd);
        return EXIT_FAILURE;
    }

    NetworkState state;
    network_init(&state);
    fprintf(stderr, "Network simulator listening on http://%s:%d\n", host, port);

    while (server_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server_running) {
                perror("accept");
            }
            break;
        }

        handle_client(client_fd, &state);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
