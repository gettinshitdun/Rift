# RIFT: Architecture & Design Documentation

## Table of Contents
1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Protocol Design](#protocol-design)
4. [Event Loop & I/O Model](#event-loop--io-model)
5. [Connection Lifecycle](#connection-lifecycle)
6. [Design Decisions](#design-decisions)

---

## Overview

**RIFT** is a lightweight, production-grade tunneling server similar to ngrok. It enables secure public access to local services without exposing them directly to the internet. The core design philosophy prioritizes simplicity, scalability, and reliability.

### Core Components
- **Tunnel Server** (Port 7000): Client-facing persistent connection
- **Public Ingress** (Port 9000): External request handler
- **Health Endpoint** (Port 8080): Monitoring and status

### Typical Use Case
```
Internet Browser          RIFT Server              Local Service
   (9000) ──────────────> (Public) ──────────────> (3000)
                               │
                          (Tunnel)
                               │
       RIFT Client <──────────── (7000)
       (localhost:3000)
```

---

## System Architecture

### High-Level Diagram
```
┌─────────────────────────────────────────────────────────┐
│                    RIFT Server (epoll)                   │
├─────────────────────────────────────────────────────────┤
│                                                           │
│  ┌──────────────────┐  ┌──────────────────┐              │
│  │  Tunnel Listener │  │  Public Listener │  Health      │
│  │    (7000)        │  │    (9000)        │  (8080)      │
│  └────────┬─────────┘  └────────┬─────────┘  │           │
│           │                     │            │           │
│           ▼                     ▼            ▼           │
│  ┌────────────────────────────────────────────────┐     │
│  │            epoll Event Multiplexer             │     │
│  │         (Level-Triggered, 64 events)           │     │
│  └────────────────────────────────────────────────┘     │
│           │                                              │
│           ▼                                              │
│  ┌────────────────────────────────────────────────┐     │
│  │    Connection State Machine (102,400 slots)    │     │
│  │                                                 │     │
│  │  ┌─────────────────────────────────────────┐  │     │
│  │  │ Tunnel Connections (TUNNEL_READY)       │  │     │
│  │  │ - Hold persistent client connections    │  │     │
│  │  │ - Receive framed data from browser      │  │     │
│  │  └─────────────────────────────────────────┘  │     │
│  │                                                 │     │
│  │  ┌─────────────────────────────────────────┐  │     │
│  │  │ Public Connections (PUBLIC_FORWARDING)  │  │     │
│  │  │ - Handle incoming HTTP requests         │  │     │
│  │  │ - Paired with tunnels via tunnel_id    │  │     │
│  │  └─────────────────────────────────────────┘  │     │
│  │                                                 │     │
│  └────────────────────────────────────────────────┘     │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

### Connection Object Structure (V2)
```c
typedef struct {
    uint32_t stream_id;              // Stream identifier
    int      browser_fd;             // Linked browser socket
} stream_entry_t;

typedef struct {
    int fd;                          // Socket file descriptor
    conn_state_t state;              // TUNNEL_INIT/READY or PUBLIC_INIT/FORWARDING
    int peer_fd;                     // Linked tunnel fd (for browsers)
    char tunnel_id[64];              // Unique tunnel identifier
    char service_id[64];             // Requested service
    uint32_t stream_id;              // Browser's assigned stream (PUBLIC_FORWARDING)
    stream_entry_t streams[128];     // Stream map (TUNNEL_READY only)
    int stream_count;                // Active stream count
} connection_t;
```

### Connection State Machine (V2)
```
Tunnel Connections:
    TUNNEL_INIT ──[FRAME_REGISTER_TUNNEL]──> TUNNEL_READY
                                                    │
                                         (stays here permanently)
                                         (concurrent streams via stream_map)

Public Connections:
    PUBLIC_INIT ──[Parse HTTP Host]──> PUBLIC_FORWARDING ──[EOF/error]──> CLOSED
                                      (carries stream_id)
```

---

## Protocol Design

### Frame Format (12 bytes + payload)

**Binary Structure (Network Byte Order):**

- **Magic** (4 bytes): Protocol identifier (0x52494654 = "RIFT")
- **Version** (1 byte): Protocol version (currently 1)
- **Reserved** (1 byte): Future use, must be 0
- **Type** (2 bytes): Frame type identifier (big-endian)
- **Length** (4 bytes): Payload size in bytes, 0-16,384 (big-endian)
- **Payload** (0-16 KB): Frame-specific data

**Total header:** 12 bytes | **Total frame:** 12 bytes + payload

### Frame Types

| Type | Value | Direction | Purpose | Payload |
|------|-------|-----------|---------|---------|
| `FRAME_REGISTER_TUNNEL` | 1 | Client→Server | Client announces tunnel ID | Tunnel ID string |
| `FRAME_CONNECT_REQUEST` | 2 | Server→Client | New public request incoming | Status code or reason |
| `FRAME_DATA` | 3 | Bidirectional | Forwarded HTTP data | Raw bytes |
| `FRAME_ERROR` | 4 | Server→Client | Error notification | Error message |
| `FRAME_CLOSE` | 5 | Either | Graceful close | Optional reason |
| `FRAME_ACK` | 6 | Either | Acknowledgment | Unused |

### Message Flow Example: HTTP GET Request

```
1. Initial Setup (persistent)
   Client:  FRAME_REGISTER_TUNNEL("tunnel-123") ──────────────────> Server
            [Tunnel stays open indefinitely]

2. Incoming Public Request
   Browser: GET / HTTP/1.1          ──────────────────> Server:9000
   Server:  FRAME_CONNECT_REQUEST   ──────────────────> Client:7000
   Client:  [Opens TCP to localhost:3000]

3. Data Forwarding
   Client:  FRAME_DATA(HTTP response bytes) ────────────> Server
   Server:  [Writes to browser socket] ──────────────────> Browser

4. Cleanup
   Browser: [closes]
   Server:  [Terminates public connection]
   Client:  [Closes local connection, stays registered]
```

---

## Event Loop & I/O Model

### epoll Configuration: Level-Triggered (Not Edge-Triggered)

**Current Implementation:**
```c
epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
```

**Key Point:** Notice the **absence of `EPOLLET`** flag. This means we're using **LEVEL-TRIGGERED** mode.

### Level-Triggered vs Edge-Triggered

| Aspect | Level-Triggered (Current) | Edge-Triggered |
|--------|---------------------------|-----------------|
| **Fires** | Whenever data available | Only on state change |
| **Buffer empty?** | Fires again next poll | Only on new data |
| **Simplicity** | Easy to implement ✓ | Requires careful handling |
| **Missed data** | Cannot miss data | Can miss if not draining fully |
| **Retry loop needed?** | Yes (but forgiving) | No (must drain all) |
| **Performance** | More syscalls | Fewer, faster syscalls |
| **Use case** | General purpose ✓ | High-frequency I/O |

### Why Level-Triggered?

1. **Simplicity**: We can safely process one frame at a time. If more data exists, epoll fires again.
2. **Correctness**: No risk of losing data between epoll calls.
3. **Non-blocking I/O**: Combined with non-blocking sockets, it's safe and predictable.
4. **Debugging**: Stack traces match actual socket state clearly.

**Trade-off:** Slight extra syscall overhead vs absolute reliability. For a tunnel server, reliability > microsecond latency.

### Event Loop Execution

```c
// Main event loop structure
while (!should_shutdown) {
    // Wait max 1 second (allows graceful shutdown check)
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
    
    if (nfds < 0) {
        if (errno == EINTR) continue;  // Retry on signal
        break;
    }
    
    // Process events in order (level-triggered, can repeat)
    for (int i = 0; i < nfds; i++) {
        dispatch_event(...);
    }
}
```

**Important Flags:**

| Flag | Meaning | Used |
|------|---------|------|
| `EPOLLIN` | Data available to read | ✓ |
| `EPOLLOUT` | Socket writable | ✗ (non-blocking writes only) |
| `EPOLLET` | Edge-triggered mode | ✗ (using level-triggered) |
| `EPOLLHUP` | Connection hung up | ✓ |
| `EPOLLERR` | Error state | ✓ |
| `EPOLLONESHOT` | Fire once, then disable | ✗ |

---

## Connection Lifecycle

### Tunnel Connection (Server Side)
```
1. ACCEPT on listener:7000
   └─> connection_add_tunnel(fd)
       └─> State = TUNNEL_INIT
       └─> epoll_ctl(ADD, fd, EPOLLIN)

2. FRAME_READ (expects FRAME_REGISTER_TUNNEL)
   └─> Store tunnel_id
   └─> State = TUNNEL_READY
   └─> Connection persists, stream_map ready

3. PUBLIC CONNECTS (can happen concurrently)
   └─> Assign stream_id = tunnel_next_stream_id()
   └─> tunnel_add_stream(stream_id, browser_fd)
   └─> Send FRAME_CONNECT_REQUEST(stream_id) + FRAME_DATA(stream_id)
   └─> Tunnel stays in TUNNEL_READY (unchanged)

4. DATA FLOW (multiplexed)
   Browser → Server → FRAME_DATA(stream_id) → Tunnel → Client
   Client  → FRAME_DATA(stream_id) → Server demux → Browser

5. ONE BROWSER CLOSES
   └─> tunnel_remove_stream(stream_id)
   └─> Send FRAME_CLOSE(stream_id) to tunnel
   └─> Other streams continue unaffected

6. TUNNEL CLOSES (client disconnect)
   └─> tunnel_close_all_streams() → 502 to all browsers
   └─> connection_close(tunnel_fd)
   └─> epoll_ctl(DEL, fd)
   └─> close(fd)
```

### Public Connection (Server Side)
```
1. ACCEPT on listener:9000
   └─> connection_add_public(fd)
   └─> State = PUBLIC_INIT
   └─> Buffer first 4 bytes to detect protocol

2. IDENTIFY PROTOCOL
   ├─ "RIFT" → handle_rift_frame() [internal protocol]
   └─ "GET " / "POST" → handle_http_request() [HTTP]

3. EXTRACT TUNNEL ID
   ├─ From Host header: "tunnel-123.rift.local"
   └─ From X-Tunnel-Id header: "tunnel-123"

4. FIND TUNNEL & ASSIGN STREAM
   └─> connection_find_tunnel(tunnel_id)
   └─> Must be in TUNNEL_READY state
   └─> Assign stream_id = tunnel_next_stream_id()
   └─> tunnel_add_stream(stream_id, browser_fd)

5. FORWARD
   └─> State = PUBLIC_FORWARDING
   └─> browser.stream_id = assigned stream
   └─> Send FRAME_CONNECT_REQUEST(stream_id) + FRAME_DATA(stream_id)
   └─> Client opens local service, forwards per stream_id

6. DATA FLOW
   Browser read → frame_write(FRAME_DATA, stream_id) → Tunnel
   Tunnel FRAME_DATA(stream_id) → demux → write to this browser

7. CLOSE
   └─> Browser closes or error occurs
   └─> tunnel_remove_stream(stream_id)
   └─> Send FRAME_CLOSE(stream_id) to tunnel
   └─> Tunnel stays READY for other streams
```

---

## Design Decisions

### 1. Persistent Tunnel Connections

**Decision:** Keep tunnel connections alive across multiple public requests.

**Why:**
- Multiple requests from different browsers to same service
- Avoid re-establishing client connection (expensive)
- Client can keep local service warm

**Alternative Considered:** Close tunnel after each request
- Would require client to re-register every request
- Higher latency, more complex retry logic
- Not suitable for long-lived tunnels

### Code Impact

The persistent tunnel design means when a public connection closes, the tunnel remains in `TUNNEL_READY` state rather than cascading closure. The tunnel is available for the next incoming request.

### 2. Buffered Frame Reading (Client Side)

**Decision:** Client buffers incoming frames to handle TCP packet fragmentation.

**Why:**
- TCP doesn't preserve message boundaries
- A single frame might arrive as 2-3 TCP packets
- Without buffering: frame header incomplete, causes "magic mismatch" error

**Implementation**

The client maintains a sliding window buffer. As data arrives on the socket, it's appended to the buffer. When a complete frame is available (header + full payload), it's extracted and removed. The buffer ensures fragmented TCP packets are properly reassembled into whole frames before processing.

### 3. Protocol Versioning

**Decision**

Include version byte in frame header from day 1. This enables graceful protocol evolution when clients and servers are at different versions.

### 4. Non-Blocking I/O with Graceful Shutdown

**Decision**

Use 1-second epoll timeout during graceful shutdown to enable checking the shutdown flag periodically. This avoids blocking indefinitely in epoll_wait() while still maintaining efficiency during normal operation (infinite timeout).

### 5. Fixed Connection Slots (102,400)

**Decision**

Pre-allocate a fixed array of 102,400 connection slots at server startup. This trades fixed memory overhead (~14 MB) for deterministic O(1) allocation and simple index-based lookups.

### 6. Frame Size Limit: 16 KB

**Decision**

Limit frame payload to 16 KB. This size accommodates typical HTTP headers and responses while remaining efficient over TCP networks.

### 7. Signal Safety

**Decision**

Use `volatile sig_atomic_t` for the shutdown flag. This is the only variable type guaranteed to be safely assignable within a signal handler without risk of corruption.

### 8. Per-Connection Metrics (Future)

**Decision**

Store per-tunnel metrics (uptime, connection counts) in a metrics module. This enables health monitoring without modifying the core event loop.

---

## Summary: Why These Choices?

| Choice | Alternative | Winner | Reason |
|--------|-------------|--------|--------|
| Level-triggered epoll | Edge-triggered | Level | Simplicity + safety > throughput |
| Persistent tunnels | Per-request | Persistent | Real-world use cases (multiple requests) |
| Fixed connection slots | Dynamic allocation | Fixed | Deterministic, predictable |
| Frame buffering | Raw TCP | Buffering | Correctness (message boundaries) |
| 16 KB frames | 4 KB or 64 KB | 16 KB | Balance of typical sizes |
| Versioning | None | Versioning | Forward compatibility |
| Non-blocking I/O | Blocking | Non-blocking | Single-threaded scalability |

---

## Appendix: Performance Characteristics

### Throughput Bottlenecks (in priority order)
1. **Network I/O** (fastest limit): NIC bandwidth
2. **Kernel buffer copies**: memcpy in kernel space
3. **Frame processing**: Small (12-byte header parsing)
4. **epoll overhead**: Minimal (one syscall per batch of events)
5. **Application logic**: Minimal (mostly forwarding)

### Scalability
- **Connections:** Limited by OS ulimit (usually 1024-65536, set to 102400 in code)
- **Concurrent transfers:** Limited by epoll (63 events per poll in RIFT)
- **Data throughput:** Limited by NIC + kernel buffers

### Typical Latencies
- **Request establishment:** ~5ms (TCP SYN + FRAME_CONNECT)
- **First byte latency:** ~5ms + local service latency
- **Frame overhead:** < 100 microseconds per frame

---

## See Also
- [PROTOCOL.md](PROTOCOL.md) - Detailed protocol specification
- [DEPLOYMENT.md](DEPLOYMENT.md) - Production deployment guide
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - Common issues and solutions
