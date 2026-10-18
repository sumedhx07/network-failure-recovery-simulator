#define _POSIX_C_SOURCE 200809L

#include "network.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} JsonWriter;

static bool topology_has_edge(Topology topology, int router_count, int first,
                              int second) {
    if (first == second) {
        return false;
    }

    switch (topology) {
    case TOPOLOGY_STAR:
        return first == 0 || second == 0;
    case TOPOLOGY_RING:
        return (first + 1) % router_count == second ||
               (second + 1) % router_count == first;
    case TOPOLOGY_MESH:
        return true;
    }

    return false;
}

static void append_event(NetworkState *state, const char *type,
                         const char *message, int router_id) {
    if (state->event_count == MAX_EVENTS) {
        memmove(&state->events[0], &state->events[1],
                (MAX_EVENTS - 1) * sizeof(state->events[0]));
        state->event_count--;
    }

    NetworkEvent *event = &state->events[state->event_count++];
    time_t now = time(NULL);
    struct tm timestamp;

    if (gmtime_r(&now, &timestamp) != NULL) {
        strftime(event->timestamp, sizeof(event->timestamp),
                 "%Y-%m-%dT%H:%M:%SZ", &timestamp);
    } else {
        snprintf(event->timestamp, sizeof(event->timestamp),
                 "1970-01-01T00:00:00Z");
    }

    snprintf(event->type, sizeof(event->type), "%s", type);
    snprintf(event->message, sizeof(event->message), "%s", message);
    event->router_id = router_id;
}

static void json_appendf(JsonWriter *writer, const char *format, ...) {
    if (writer->failed) {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(writer->buffer + writer->length,
                            writer->capacity - writer->length, format,
                            arguments);
    va_end(arguments);

    if (written < 0 || (size_t)written >= writer->capacity - writer->length) {
        writer->failed = true;
        return;
    }

    writer->length += (size_t)written;
}

static void json_append_string(JsonWriter *writer, const char *value) {
    json_appendf(writer, "\"");

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0' && !writer->failed; cursor++) {
        switch (*cursor) {
        case '"':
            json_appendf(writer, "\\\"");
            break;
        case '\\':
            json_appendf(writer, "\\\\");
            break;
        case '\b':
            json_appendf(writer, "\\b");
            break;
        case '\f':
            json_appendf(writer, "\\f");
            break;
        case '\n':
            json_appendf(writer, "\\n");
            break;
        case '\r':
            json_appendf(writer, "\\r");
            break;
        case '\t':
            json_appendf(writer, "\\t");
            break;
        default:
            if (*cursor < 0x20) {
                json_appendf(writer, "\\u%04x", *cursor);
            } else {
                json_appendf(writer, "%c", *cursor);
            }
        }
    }

    json_appendf(writer, "\"");
}

void network_init(NetworkState *state) {
    memset(state, 0, sizeof(*state));
    state->topology = TOPOLOGY_STAR;
    state->router_count = DEFAULT_ROUTERS;

    for (int index = 0; index < state->router_count; index++) {
        state->routers[index].id = index + 1;
        state->routers[index].online = true;
    }
}

const char *network_topology_name(Topology topology) {
    switch (topology) {
    case TOPOLOGY_STAR:
        return "star";
    case TOPOLOGY_RING:
        return "ring";
    case TOPOLOGY_MESH:
        return "mesh";
    }

    return "unknown";
}

int network_set_topology(NetworkState *state, const char *topology) {
    Topology requested;

    if (strcmp(topology, "star") == 0) {
        requested = TOPOLOGY_STAR;
    } else if (strcmp(topology, "ring") == 0) {
        requested = TOPOLOGY_RING;
    } else if (strcmp(topology, "mesh") == 0) {
        requested = TOPOLOGY_MESH;
    } else {
        return -1;
    }

    if (state->topology == requested) {
        return 0;
    }

    state->topology = requested;
    char message[EVENT_MESSAGE_LENGTH];
    snprintf(message, sizeof(message), "Topology changed to %s.",
             network_topology_name(requested));
    append_event(state, "topology", message, 0);
    return 1;
}

int network_set_router_count(NetworkState *state, int router_count) {
    if (router_count < MIN_ROUTERS || router_count > MAX_ROUTERS) {
        return -1;
    }
    if (state->router_count == router_count) {
        return 0;
    }

    int previous_count = state->router_count;
    state->router_count = router_count;
    for (int index = previous_count; index < router_count; index++) {
        state->routers[index].id = index + 1;
        state->routers[index].online = true;
    }

    char message[EVENT_MESSAGE_LENGTH];
    snprintf(message, sizeof(message), "Router count changed from %d to %d.",
             previous_count, router_count);
    append_event(state, "configuration", message, 0);
    return 1;
}

int network_set_router_online(NetworkState *state, int router_id, bool online) {
    if (router_id < 1 || router_id > state->router_count) {
        return -1;
    }

    Router *router = &state->routers[router_id - 1];
    if (router->online == online) {
        return 0;
    }

    router->online = online;
    char message[EVENT_MESSAGE_LENGTH];
    snprintf(message, sizeof(message), "R%d %s.", router_id,
             online ? "recovered" : "failed");
    append_event(state, online ? "recovery" : "failure", message, router_id);
    return 1;
}

int network_set_all_online(NetworkState *state, bool online) {
    int changed = 0;
    for (int index = 0; index < state->router_count; index++) {
        changed += network_set_router_online(state, index + 1, online) == 1;
    }
    return changed;
}

void network_reset(NetworkState *state) {
    state->topology = TOPOLOGY_STAR;
    state->router_count = DEFAULT_ROUTERS;
    for (int index = 0; index < state->router_count; index++) {
        state->routers[index].id = index + 1;
        state->routers[index].online = true;
    }
    append_event(state, "reset", "Simulation reset to the initial state.", 0);
}

size_t network_get_links(const NetworkState *state, NetworkLink *links,
                         size_t capacity) {
    size_t count = 0;

    for (int first = 0; first < state->router_count; first++) {
        for (int second = first + 1; second < state->router_count; second++) {
            if (!topology_has_edge(state->topology, state->router_count, first,
                                   second)) {
                continue;
            }

            if (count < capacity) {
                links[count].source = first + 1;
                links[count].target = second + 1;
                links[count].active = state->routers[first].online &&
                                      state->routers[second].online;
            }
            count++;
        }
    }

    return count;
}

void network_get_metrics(const NetworkState *state, NetworkMetrics *metrics) {
    NetworkLink links[MAX_NETWORK_LINKS];
    size_t link_count = network_get_links(state, links, MAX_NETWORK_LINKS);

    metrics->total_routers = state->router_count;
    metrics->online_routers = 0;
    metrics->failed_routers = 0;
    metrics->active_links = 0;
    metrics->total_links = (int)link_count;

    for (int index = 0; index < state->router_count; index++) {
        if (state->routers[index].online) {
            metrics->online_routers++;
        } else {
            metrics->failed_routers++;
        }
    }

    for (size_t index = 0; index < link_count; index++) {
        metrics->active_links += links[index].active;
    }

    metrics->availability =
        (double)metrics->online_routers * 100.0 / (double)state->router_count;
}

void network_get_reachability(const NetworkState *state,
                              bool reachable[MAX_ROUTERS]) {
    int queue[MAX_ROUTERS];
    int head = 0;
    int tail = 0;

    memset(reachable, 0, sizeof(bool) * MAX_ROUTERS);
    if (!state->routers[0].online) {
        return;
    }

    reachable[0] = true;
    queue[tail++] = 0;

    while (head < tail) {
        int current = queue[head++];

        for (int neighbor = 0; neighbor < state->router_count; neighbor++) {
            if (reachable[neighbor] || !state->routers[neighbor].online ||
                !topology_has_edge(state->topology, state->router_count,
                                   current, neighbor)) {
                continue;
            }

            reachable[neighbor] = true;
            queue[tail++] = neighbor;
        }
    }
}

int network_to_json(const NetworkState *state, char *destination,
                    size_t capacity) {
    if (capacity == 0) {
        return -1;
    }

    JsonWriter writer = {
        .buffer = destination,
        .capacity = capacity,
        .length = 0,
        .failed = false,
    };
    destination[0] = '\0';

    NetworkLink links[MAX_NETWORK_LINKS];
    size_t link_count = network_get_links(state, links, MAX_NETWORK_LINKS);
    NetworkMetrics metrics;
    bool reachable[MAX_ROUTERS];
    network_get_metrics(state, &metrics);
    network_get_reachability(state, reachable);

    json_appendf(&writer, "{\"topology\":");
    json_append_string(&writer, network_topology_name(state->topology));
    json_appendf(&writer, ",\"routers\":[");
    for (int index = 0; index < state->router_count; index++) {
        if (index > 0) {
            json_appendf(&writer, ",");
        }
        json_appendf(&writer, "{\"id\":%d,\"name\":\"R%d\",\"online\":%s}",
                     state->routers[index].id, state->routers[index].id,
                     state->routers[index].online ? "true" : "false");
    }

    json_appendf(&writer, "],\"links\":[");
    for (size_t index = 0; index < link_count; index++) {
        if (index > 0) {
            json_appendf(&writer, ",");
        }
        json_appendf(&writer,
                     "{\"source\":%d,\"target\":%d,\"active\":%s}",
                     links[index].source, links[index].target,
                     links[index].active ? "true" : "false");
    }

    json_appendf(
        &writer,
        "],\"metrics\":{\"totalRouters\":%d,\"onlineRouters\":%d,"
        "\"failedRouters\":%d,\"activeLinks\":%d,\"totalLinks\":%d,"
        "\"availability\":%.1f},\"reachableRouters\":[",
        metrics.total_routers, metrics.online_routers, metrics.failed_routers,
        metrics.active_links, metrics.total_links, metrics.availability);

    bool first_id = true;
    for (int index = 0; index < state->router_count; index++) {
        if (reachable[index]) {
            json_appendf(&writer, "%s%d", first_id ? "" : ",", index + 1);
            first_id = false;
        }
    }

    json_appendf(&writer, "],\"unreachableRouters\":[");
    first_id = true;
    for (int index = 0; index < state->router_count; index++) {
        if (!reachable[index]) {
            json_appendf(&writer, "%s%d", first_id ? "" : ",", index + 1);
            first_id = false;
        }
    }

    json_appendf(&writer, "],\"events\":[");
    for (size_t index = 0; index < state->event_count; index++) {
        const NetworkEvent *event = &state->events[index];
        if (index > 0) {
            json_appendf(&writer, ",");
        }
        json_appendf(&writer, "{\"timestamp\":");
        json_append_string(&writer, event->timestamp);
        json_appendf(&writer, ",\"type\":");
        json_append_string(&writer, event->type);
        json_appendf(&writer, ",\"message\":");
        json_append_string(&writer, event->message);
        if (event->router_id > 0) {
            json_appendf(&writer, ",\"routerId\":%d", event->router_id);
        }
        json_appendf(&writer, "}");
    }
    json_appendf(&writer, "]}");

    return writer.failed ? -1 : (int)writer.length;
}
