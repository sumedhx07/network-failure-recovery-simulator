#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>

#define MIN_ROUTERS 2
#define DEFAULT_ROUTERS 6
#define MAX_ROUTERS 12
#define MAX_NETWORK_LINKS ((MAX_ROUTERS * (MAX_ROUTERS - 1)) / 2)
#define MAX_EVENTS 100
#define EVENT_MESSAGE_LENGTH 160

typedef enum {
    TOPOLOGY_STAR,
    TOPOLOGY_RING,
    TOPOLOGY_MESH
} Topology;

typedef struct {
    int id;
    bool online;
} Router;

typedef struct {
    int source;
    int target;
    bool active;
} NetworkLink;

typedef struct {
    int total_routers;
    int online_routers;
    int failed_routers;
    int active_links;
    int total_links;
    double availability;
} NetworkMetrics;

typedef struct {
    char timestamp[32];
    char type[24];
    char message[EVENT_MESSAGE_LENGTH];
    int router_id;
} NetworkEvent;

typedef struct {
    Topology topology;
    int router_count;
    Router routers[MAX_ROUTERS];
    NetworkEvent events[MAX_EVENTS];
    size_t event_count;
} NetworkState;

/* Initialize a new simulation: star topology and six routers online. */
void network_init(NetworkState *state);

/* Return 1 when changed, 0 when unchanged, and -1 for invalid input. */
int network_set_topology(NetworkState *state, const char *topology);
int network_set_router_count(NetworkState *state, int router_count);
int network_set_router_online(NetworkState *state, int router_id, bool online);

/* Change every router and return the number of routers whose state changed. */
int network_set_all_online(NetworkState *state, bool online);

/* Restore the initial state and append a reset event to the timeline. */
void network_reset(NetworkState *state);

const char *network_topology_name(Topology topology);
size_t network_get_links(const NetworkState *state, NetworkLink *links,
                         size_t capacity);
void network_get_metrics(const NetworkState *state, NetworkMetrics *metrics);
void network_get_reachability(const NetworkState *state,
                              bool reachable[MAX_ROUTERS]);

/*
 * Serialize the complete dashboard snapshot as JSON.
 * Returns the number of bytes written, or -1 if the destination is too small.
 */
int network_to_json(const NetworkState *state, char *destination,
                    size_t capacity);

#endif
