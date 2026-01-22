# RIFT: Engineering Learnings & Design Philosophy

## Table of Contents
1. [I/O Multiplexing](#io-multiplexing)
2. [Event Loop Design](#event-loop-design)
3. [Protocol Design](#protocol-design)
4. [Connection Management](#connection-management)
5. [Memory & Performance](#memory--performance)
6. [Reliability & Robustness](#reliability--robustness)

---

## I/O Multiplexing

### The Problem We're Solving

Imagine you want to serve 10,000 concurrent users, but you have only one thread. How?

```c
// Naive approach (BLOCKING): Doesn't work
while (1) {
    int client_fd = accept(listener_fd);  // Blocks until someone connects
    handle_client(client_fd);              // Blocks until client is done
    // Can't do anything else while blocking!
}
```

This handles one client at a time. 9,999 others are waiting.

### Solution: epoll (Event Polling)

```c
// Better approach: Non-blocking + epoll
for (int i = 0; i < 10000; i++) {
    int client_fd = accept(listener_fd);
    if (client_fd > 0) {
        fcntl(client_fd, F_SETFL, O_NONBLOCK);  // Non-blocking
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
    }
}

// Now serve all 10,000 in the loop
while (1) {
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
    // Returns when ANY of the 10,000 has data ready
    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;
        ssize_t n = read(fd, buf, sizeof(buf));  // Non-blocking, won't hang
        // Process n bytes
    }
}
```

**Why epoll over select/poll?**
- `select`: O(n) complexity, limits to 1024 fds
- `poll`: O(n) complexity, but no fd limit
- `epoll`: O(1) complexity (using red-black tree internally), supports 100k+ fds

### Level-Triggered vs Edge-Triggered

This is THE critical design decision in RIFT.

#### Level-Triggered (RIFT's Choice)

**Semantics:** "epoll fires while condition is true"

```
Socket has 100 bytes available
    ↓
epoll_wait() returns event
    ↓
Process 30 bytes
    ↓
70 bytes still available
    ↓
epoll_wait() fires AGAIN ← It's checking the current state
    ↓
Process 30 more bytes
    ↓
... repeat until all consumed
```

**Implementation:**
```c
epoll_add_fd(epfd, fd, EPOLLIN);  // No EPOLLET flag
```

**Advantages:**
- Easy to implement correctly
- Forgiving if you don't drain all data
- No data loss possible
- Predictable behavior

**Disadvantages:**
- Extra syscalls if not draining fully
- Slightly more CPU overhead

#### Edge-Triggered (Not used here)

**Semantics:** "epoll fires when state changes"

```
Socket state changes: 0 bytes → 100 bytes available
    ↓
epoll_wait() returns event ONCE
    ↓
You MUST drain all 100 bytes, or miss them
    ↓
If you only read 30 bytes, the remaining 70 are lost
    ↓
epoll won't fire again (state didn't change, data arrived before)
```

**Implementation:**
```c
epoll_add_fd(epfd, fd, EPOLLIN | EPOLLET);  // With EPOLLET flag
```

**Advantages:**
- Fewer syscalls
- Better performance at scale
- Less CPU overhead

**Disadvantages:**
- Much harder to implement correctly
- Must drain ALL available data on every event
- Must handle EAGAIN carefully
- Easy to miss data and have bugs

### RIFT's Choice: Why Level-Triggered?

```
Cost of extra syscalls:  ~1-5 microseconds
Cost of data loss bug:   ~$100,000 in production incidents
```

For a tunneling server where **correctness > performance**, level-triggered is the right choice.

**Real-world scenario with edge-triggered:**
```c
// You receive an event (100 bytes)
while ((n = read(fd, buf, 1024)) > 0) {
    process(buf, n);
}
// You read 70 bytes before some other code path exits the loop
// The remaining 30 bytes are LOST
// Browser gets incomplete response, request fails
// Debugging this is a nightmare
```

---

## Event Loop Design

### The Main Loop Pattern

```c
while (!should_shutdown) {
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
    
    for (int i = 0; i < nfds; i++) {
        dispatch_event(events[i]);
    }
}
```

### Timeout Handling (Graceful Shutdown)

**Problem:** When Ctrl+C is pressed, signal handler sets flag. But we're blocked in epoll_wait()!

```c
while (1) {
    // This blocks forever, never checking should_shutdown
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
}
```

**Solution: Timeout**

```c
while (!should_shutdown) {
    // Use timeout during shutdown, infinity during normal operation
    int timeout = should_shutdown ? 1000 : -1;
    
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
    
    if (nfds == 0) {
        // Timeout fired, check should_shutdown and continue
        continue;
    }
    
    // Process events
}
```

**Flow with Ctrl+C:**
```
T0: epoll_wait(timeout=1000) ← Waiting
T1: Ctrl+C pressed
    └─ signal_handler fires
    └─ should_shutdown = 1
T2: No events, but timeout fires
    └─ Check should_shutdown
    └─ Exit loop
T3: Graceful cleanup
```

**Why not 100ms or 100s?**
- Too short (10ms): Wasted syscalls, CPU spinning
- Too long (60s): Slow shutdown
- 1000ms: Good balance (shutdown within ~1 second)

### Event Batching

```c
// Process multiple events before yielding
struct epoll_event events[64];  // Batch 64 events

int nfds = epoll_wait(epfd, events, 64, -1);
for (int i = 0; i < nfds; i++) {
    // Process all 64 events before next epoll_wait
}
```

**Why batch?**
- Reduce syscall overhead
- Better CPU cache locality
- Smoother request handling (no starvation)

---

## Protocol Design

### Why Custom Binary Protocol (Not HTTP)?

**Option 1: Use HTTP for everything**
```
Browser: GET / HTTP/1.1
         Host: example.com
Server: HTTP/1.1 200 OK
        Content-Length: 100
        ...
Client: [receives as HTTP, must parse]
```

**Problem:** Tunnel protocol needs bidirectional, framed data. HTTP is designed for request-response.

**Option 2: Custom binary protocol (RIFT)**
```
Server: [binary header: 12 bytes]
        [frame type: FRAME_DATA]
        [length: 100]
        [payload: 100 bytes]
Client: [parse header, read exact 100 bytes]
        [done, ready for next frame]
```

**Benefits:**
- Clear message boundaries (length in header)
- Efficient (no text parsing)
- Bidirectional (no request-response constraint)
- Extensible (type field allows new frame types)

### Protocol Versioning

**Design:** Include version byte from day 1

```c
#define FRAME_VERSION 1

// Frame header
typedef struct {
    uint32_t magic;      // "RIFT"
    uint8_t version;     // 1
    uint8_t reserved;    // future use
    uint16_t type;
    uint32_t length;
} frame_header_t;
```

**Why?**

Future protocol evolution:
- Version 1: Basic tunneling
- Version 2: Add compression
- Version 3: Add encryption
- Version 4: Add authentication

**Without versioning:**
- Can't tell if frame is corrupted or from different protocol
- No upgrade path
- Breaking changes break everything

**With versioning:**
```c
if (version != FRAME_VERSION) {
    fprintf(stderr, "Unsupported version: %d\n", version);
    return -1;  // Clear error
}
```

### Fixed Frame Size (16 KB)

**Why not 4 KB?**
- Small HTTP requests need multiple frames
- More syscalls
- Slower throughput

**Why not 64 KB?**
- Most HTTP headers are <4 KB
- TCP might fragment into 3-4 packets anyway
- More memory per buffer
- No practical performance benefit

**16 KB is the sweet spot:**
- Fits in kernel buffer
- Covers 95% of HTTP requests
- Proven in production systems (nginx default)
- Divisible by memory pages (4KB, 8KB)

---

## Connection Management

### State Machine Pattern

```c
typedef enum {
    TUNNEL_INIT,        // Just connected
    TUNNEL_READY,       // Registered, can accept requests
    PUBLIC_INIT,        // Just accepted
    PUBLIC_FORWARDING,  // Linked to tunnel
} conn_state_t;
```

**Why explicit states?**

```c
// Bad approach (implicit state via side effects):
if (c->peer_fd > 0 && strlen(c->tunnel_id) > 0) {
    // What state are we in? Unknown!
}

// Good approach (explicit state):
if (c->state == TUNNEL_READY) {
    // Clear: tunnel is registered and ready for connections
}
```

**Benefits:**
- Self-documenting code
- Easy to understand valid transitions
- Prevents invalid operations
- Simplifies debugging

### Persistent Tunnels

**Design:** Tunnel stays alive after public connection closes

```
Public connection 1:  Browser → Server → Tunnel → Local service
                                               ↓ closes
Tunnel stays open
Public connection 2:  Browser → Server → Tunnel → Local service
```

**Why?**

Without persistence:
- Close tunnel after every request
- Client must re-register for next request
- ~100ms latency per request
- Not practical for real websites

With persistence:
- One registration per tunnel
- Fast connection reuse
- Realistic for 1000s of requests per tunnel
- Matches real ngrok behavior

---

## Memory & Performance

### Connection Slots (Pre-allocation)

```c
#define MAX_CONNECTIONS 102400
static connection_t connections[MAX_CONNECTIONS];
```

**Why pre-allocate?**

```c
// Naive approach (dynamic):
connection_t *connections = malloc(sizeof(connection_t) * num_connections);
// Allocates heap memory
// Causes fragmentation over time
// GC pause (if language has GC)
// Unpredictable latency
```

**Pre-allocation approach:**
```c
// Fixed array on stack (or data segment)
static connection_t connections[102400];
// O(1) allocation (done at compile time)
// No fragmentation
// No GC
// Predictable latency
// Memory known at startup (~14 MB)
```

**Trade-off:**
- ✓ Predictable performance
- ✓ Simple O(1) lookup
- ✓ No memory fragmentation
- ✗ Can't exceed 102,400 connections
- ✗ Memory always allocated (even if not full)

**This is the nginx approach.** It works.

### Per-Connection Memory

```c
typedef struct {
    int fd;              // 4 B
    int state;           // 4 B
    int peer_fd;         // 4 B
    char tunnel_id[64];  // 64 B
    char service_id[64]; // 64 B
    // TOTAL: 140 bytes
} connection_t;

// 102,400 connections × 140 bytes = 14.3 MB
```

**Memory usage is transparent:**
- Startup: +14 MB (worth it for predictability)
- Per active connection: +0 MB (already allocated)
- No hidden malloc/free overhead

### Zero-Copy Philosophy

```c
// Frame flow:
Browser → Socket read → Frame parse → Local service write
                ↓
        No intermediate buffers
        Only copy when necessary (network to app)
```

**vs. Buffering everything:**
```c
// Bad: Extra copies
Browser → Socket read → Buffer 1 → Process → Buffer 2 → Local service write
```

---

## Reliability & Robustness

### Signal Safety

```c
// UNSAFE:
void bad_signal_handler(int sig) {
    printf("Got signal %d\n", sig);   // ✗ printf is not signal-safe
    malloc(100);                       // ✗ malloc is not signal-safe
}

// SAFE:
static volatile sig_atomic_t flag = 0;

void good_signal_handler(int sig) {
    flag = 1;  // ✓ Assignment to sig_atomic_t is atomic
}
```

**Why?**

Signal handler can interrupt code anywhere. If you call non-safe function:

```c
// In main code:
buffer[0] = 'a';     // ← Signal interrupts HERE
buffer[1] = 'b';

// In signal handler:
malloc(100);         // ← Might deadlock! malloc uses locks.
```

**RIFT's approach:**
```c
// In signal handler: Just set flag
should_shutdown = 1;

// In main loop: Check flag
if (should_shutdown) {
    // Safe to do cleanup
}
```

### Error Handling Strategy

```c
// Principle 1: Fail early
if (port <= 0 || port > 65535) {
    fprintf(stderr, "Error: Invalid port %d\n", port);
    return 1;  // Don't continue with bad config
}

// Principle 2: Log with context
if (frame_write(fd, FRAME_DATA, buf, len) < 0) {
    log_error("Failed to send to tunnel (fd %d): %s", fd, strerror(errno));
}

// Principle 3: Graceful degradation
if (local_fd < 0) {
    printf("[!] Failed to connect to local service\n");
    // Drop this one request, keep tunnel alive
    close(public_fd);
} else {
    // Connection succeeded, proceed
}
```

### Frame Validation

```c
// Check 1: Magic number
if (magic != FRAME_MAGIC) {
    fprintf(stderr, "[frame] Magic mismatch: got 0x%08x\n", magic);
    return -1;  // Fatal corruption
}

// Check 2: Version compatibility
if (version != FRAME_VERSION) {
    fprintf(stderr, "[frame] Unsupported version: %d\n", version);
    return -1;  // Protocol mismatch
}

// Check 3: Size bounds
if (length > FRAME_MAX_PAYLOAD) {
    fprintf(stderr, "[frame] Payload too large: %u\n", length);
    return -1;  // Buffer overflow protection
}
```

---

## General Principles

### 1. Choose Simplicity Over Cleverness

```c
// Clever but confusing:
connection_t *c = &connections[(fd ^ ~0xFF) & 0xFFFF];

// Simple and clear:
connection_t *c = connection_get(fd);
```

RIFT prioritizes:
- Direct epoll API (not a wrapper library)
- Explicit state machines (not implicit state)
- Simple data structures (not complex abstractions)

### 2. Make Failure Loud

```c
// Don't fail silently:
if (frame_write(fd, type, buf, len) < 0) {
    // ✓ Log error
    log_error("Frame write failed: %s", strerror(errno));
    
    // ✓ Take action (close connection)
    connection_close(epfd, fd);
}
```

### 3. Document Why, Not What

```c
// Bad comment (explains "what", which code already shows):
// Loop through events
for (int i = 0; i < nfds; i++) {

// Good comment (explains "why"):
// Process events in order. Level-triggered epoll means this function
// will be called again if data remains, so it's safe to process one
// frame at a time rather than draining all available data.
for (int i = 0; i < nfds; i++) {
```

### 4. Avoid Premature Optimization

RIFT doesn't have:
- ✗ Connection pooling (unnecessary complexity)
- ✗ Advanced scheduling (epoll + FIFO is fine)
- ✗ Compression (can add later)
- ✗ Load balancing (single instance, then shard)

RIFT focuses on:
- ✓ Correctness
- ✓ Clarity
- ✓ Maintainability
- ✓ Proven performance

### 5. Test Real Scenarios

Don't just test happy path:

```
✓ High connection count (1000+ simultaneous)
✓ Large data transfer (10+ MB frames)
✓ Slow clients (throttle to 1 KB/s)
✓ Rapid connects/disconnects
✓ Network failures (kill -9 client)
✓ Load spikes
```

---

## HTTP/1.1 Keep-Alive & Connection Persistence

### The Problem

When a browser makes multiple requests to the same service via RIFT, it uses HTTP/1.1 Keep-Alive to reuse the TCP connection:

```
Request 1: GET /page1
Response 1: ✓ 200 OK
[Connection stays open]
Request 2: GET /page2  ← On same TCP connection
Response 2: ✓ 200 OK
```

But the initial implementation had a flaw:

### Original Issue

After forwarding response 1 to the browser, the server kept the public connection in `CONN_PUBLIC_FORWARDING` state. When browser sent Request 2 on the same connection:

1. Browser: Sends HTTP request on persistent connection
2. Server (public fd): Receives raw HTTP data
3. Server (forwarding logic): Wraps HTTP data as binary FRAME_DATA to tunnel
4. Tunnel: Receives invalid frame format (raw HTTP is not valid RIFT frame)
5. Result: Connection breaks with "Broken pipe"

### The Fix

Three-part solution:

**1. Client notification** (`client/main.c`):
When local service closes connection after response, client sends `FRAME_CLOSE`:

```c
// When localhost:3000 closes after response
if (n == 0) {  // EOF
    frame_write(server_fd, FRAME_CLOSE, NULL, 0);  // NEW: Notify server
    close(local_fd);
    local_fd = -1;
}
```

**2. Connection reset function** (`server/connection.c`):
```c
void connection_reset_public(int fd) {
    connection_t *c = connection_get(fd);
    if (c->state == CONN_PUBLIC_FORWARDING) {
        c->peer_fd = 0;
        c->state = CONN_PUBLIC_INIT;  // Ready for next HTTP request
        c->service_id[0] = '\0';
    }
}
```

**3. Server-side handling** (`server/epoll_server.c`):
When tunnel receives `FRAME_CLOSE`, reset public connection instead of closing:

```c
case CONN_TUNNEL_FORWARDING:
    if (type == FRAME_CLOSE) {
        // Response complete, reset public connection for next request
        if (c->peer_fd > 0) {
            connection_reset_public(c->peer_fd);  // Reset, don't close
        }
        c->state = CONN_TUNNEL_READY;
        c->peer_fd = 0;  // Tunnel waits for next FRAME_CONNECT_REQUEST
    }
```

### Result

Now browsers can send multiple requests on the same Keep-Alive connection:

```
Request 1 → CONN_PUBLIC_INIT → Link to tunnel → CONN_PUBLIC_FORWARDING
  ↓
Response → FRAME_CLOSE sent by client
  ↓
Public reset to CONN_PUBLIC_INIT ← Browser can send Request 2!
```

Test coverage: "TEST 3 - Multiple Sequential Requests" validates this with 3 back-to-back requests.

---

## Lessons for Building Systems

1. **Start with the I/O model.** Choose epoll/kqueue early.
2. **Use level-triggered by default.** Switch to edge-triggered only if profiling shows it matters.
3. **Design protocols for clarity.** Binary framing > text parsing.
4. **Version from day 1.** It costs almost nothing and saves later.
5. **Pre-allocate memory.** Predictable latency beats dynamic scaling.
6. **Make failures visible.** Loud logs help you debug faster.
7. **Test under stress.** 1000 connections behaves differently than 10.
8. **Document decisions.** Future-you will thank present-you.

---

## Further Reading

- "The C10K Problem" - Dan Kegel (seminal article on scalability)
- "Nginx Architecture" - Igor Sysoev
- Linux man pages: `epoll(7)`, `signal-safety(7)`, `tcp(7)`
- "Unix Network Programming" - Stevens & Rago (classic textbook)
- eBPF and XDP for extreme performance (future exploration)

