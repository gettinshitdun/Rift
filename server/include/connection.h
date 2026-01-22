#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#include <stdint.h>

/*
 * Maximum number of simultaneous sockets the server can track.
 * This includes:
 *  - tunnel connections
 *  - public (ingress) connections
 *  - control/health sockets
 */
#define MAX_CONNECTIONS 102400

/*
 * Maximum length for logical identifiers exchanged in control frames.
 */
#define ID_LEN 64

/* ============================================================
 * Connection State Machine
 *
 * Each TCP socket accepted by the server transitions through
 * one of the following states.
 * ============================================================
 */
typedef enum {
    /* --------------------------------------------------------
     * Tunnel lifecycle (client → server persistent connection)
     * --------------------------------------------------------
     */

    /*
     * Tunnel socket accepted but has not yet sent REGISTER frame.
     * At this stage:
     *  - no tunnel_id is known
     *  - no traffic is allowed
     */
    CONN_TUNNEL_INIT,

    /*
     * Tunnel is registered and ready to accept public traffic.
     * This connection stays open for a long time.
     */
    CONN_TUNNEL_READY,

    /*
     * Tunnel is actively paired with a public connection
     * and forwarding data bidirectionally.
     */
    CONN_TUNNEL_FORWARDING,

    /* --------------------------------------------------------
     * Public lifecycle (external user → server connection)
     * --------------------------------------------------------
     */

    /*
     * Public socket accepted but has not yet specified
     * which tunnel/service it wants to connect to.
     */
    CONN_PUBLIC_INIT,

    /*
     * Public connection is actively forwarding traffic
     * to its paired tunnel connection.
     */
    CONN_PUBLIC_FORWARDING

} conn_state_t;

/* ============================================================
 * Connection Object
 *
 * Represents exactly ONE TCP socket.
 * ============================================================
 */
typedef struct {

    /* Socket file descriptor */
    int fd;

    /* Current state in the connection state machine */
    conn_state_t state;

    /*
     * Peer socket fd.
     * Valid ONLY when state == CONN_PUBLIC_FORWARDING.
     * For tunnels, this is set when a public connection
     * is temporarily attached.
     */
    int peer_fd;

    /*
     * Unique identifier for a tunnel.
     * Set during REGISTER handshake from client.
     * Example: "user123:app1"
     */
    char tunnel_id[ID_LEN];

    /*
     * Requested service identifier from public connection.
     * Used to find the correct tunnel.
     */
    char service_id[ID_LEN];

} connection_t;


/*
 * Initialize internal connection table.
 * Must be called once during server startup.
 */
void connection_init(void);


/*
 * Register a newly accepted tunnel socket.
 * Initial state will be CONN_TUNNEL_INIT.
 */
void connection_add_tunnel(int fd);

/*
 * Register a newly accepted public socket.
 * Initial state will be CONN_PUBLIC_INIT.
 */
void connection_add_public(int fd);


/*
 * Retrieve connection object by file descriptor.
 * Returns NULL if fd is not tracked.
 */
connection_t* connection_get(int fd);


/*
 * Bind two connections together for bidirectional forwarding.
 * This sets peer_fd on both sides and transitions state.
 */
void connection_bind(int fd1, int fd2);

/*
 * Completely close a connection, cleanup both sides.
 */
void connection_close(int epfd, int fd);

/*
 * Reset a public connection back to INIT state after local service closes.
 * Allows browser to send another request on same Keep-Alive connection.
 */
void connection_reset_public(int fd);

int connection_active_count(void);

connection_t* connection_find_tunnel(const char *tunnel_id);

#endif /* RIFT_CONNECTION_H */
