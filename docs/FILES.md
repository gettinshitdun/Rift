# RIFT: Complete Project Structure & File Guide

## 📂 Project Organization

```
rift/
├── docs/                          # Complete documentation (2,300+ lines)
│   ├── INDEX.md                   # Documentation index & reading guide
│   ├── ARCHITECTURE.md            # System design, I/O model, decision rationale
│   ├── PROTOCOL.md                # Wire protocol specification
│   └── LEARNINGS.md               # Design philosophy & engineering principles
│
├── server/                        # Server implementation
│   ├── include/                   # Header files (interfaces & constants)
│   │   ├── config.h               # Configuration defaults (tunable constants)
│   │   ├── frame.h                # Protocol definitions (frame header, types)
│   │   ├── connection.h           # Connection state machine types
│   │   ├── handlers.h             # Protocol handler declarations
│   │   ├── listener.h             # TCP listener setup
│   │   ├── forward.h              # Data forwarding declarations
│   │   ├── metrics.h              # Metrics collection
│   │   └── server.h               # Server declarations
│   │
│   ├── frame.c                    # Frame read/write implementation
│   │                              # - Handles binary framing
│   │                              # - Supports non-blocking I/O
│   │                              # - Detailed comments on I/O model
│   │
│   ├── connection.c               # Connection lifecycle management
│   │                              # - Allocate/find connections
│   │                              # - Bind tunnel ↔ public pairs
│   │                              # - Graceful close with proper cleanup
│   │
│   ├── epoll_server.c             # Main event loop
│   │                              # - epoll multiplexer setup (level-triggered)
│   │                              # - Event dispatch logic
│   │                              # - Signal-safe shutdown
│   │                              # - Logging with timestamps
│   │
│   ├── handlers.c                 # Protocol handler logic
│   │                              # - HTTP request parsing
│   │                              # - RIFT frame handling
│   │                              # - Tunnel ID extraction
│   │                              # - Error responses
│   │
│   ├── listner.c                  # TCP listener setup
│   │                              # - Socket creation
│   │                              # - Bind/listen
│   │                              # - Non-blocking mode
│   │                              # - SO_REUSEADDR flag
│   │
│   ├── forward.c                  # Data forwarding (stub)
│   │                              # - Reserved for future optimizations
│   │
│   ├── metrics.c                  # Metrics collection
│   │                              # - Uptime tracking
│   │                              # - Connection counters
│   │                              # - HTTP health endpoint
│   │
│   └── main.c                     # Server entry point
│                                  # - Calls epoll_server_main()
│
├── client/                        # Client implementation
│   └── main.c                     # Tunnel client
│                                  # - Registers tunnel with server
│                                  # - Buffered frame reader (handles TCP fragmentation)
│                                  # - Local service forwarding
│                                  # - Graceful shutdown
│
├── CMakeLists.txt                 # CMake build config (unused, Makefile is primary)
├── Makefile                       # Main build configuration
│                                  # - Compiler flags: -Wall -Wextra -Werror
│                                  # - Optimization: -O2
│                                  # - Debug symbols: -g
├── README.md                      # Quick start & overview (415 lines)
│                                  # - Build instructions
│                                  # - Architecture diagrams
│                                  # - Key concepts
│                                  # - Debugging tips
│
├── IMPROVEMENTS.md                # Summary of production improvements
│                                  # - 10 major enhancements
│                                  # - Testing checklist
│                                  # - Security considerations
│
├── LICENSE                        # MIT License
├── readme.md                      # (lowercase, might be duplicate)
└── .gitignore                     # Git ignore rules

Generated (not committed):
├── rift-server                    # Server executable (output of make)
├── rift-client                    # Client executable (output of make)
└── server/*.o, client/*.o         # Object files
```

---

## 📄 File Purposes & Key Details

### Documentation Files

#### **docs/INDEX.md** (This file)
- Navigation guide for documentation
- Reading paths by role (admin, developer, architect)
- Quick reference for finding topics
- FAQ section
- Links between documents

#### **docs/ARCHITECTURE.md** (546 lines)
- System high-level diagram
- Connection state machines (3 different types)
- Protocol frame format specification
- **Event Loop & I/O Model:**
  - Why level-triggered (not edge-triggered)
  - epoll flags explained
  - Event loop execution model
- **Design Decisions (8 major):**
  - With alternatives considered
  - Trade-offs explained
  - Rationale documented
- Performance metrics
- Scalability limits

#### **docs/PROTOCOL.md** (566 lines)
- Binary frame format (12-byte header specification)
- All 6 frame types:
  - FRAME_REGISTER_TUNNEL (0x0001)
  - FRAME_CONNECT_REQUEST (0x0002)
  - FRAME_DATA (0x0003)
  - FRAME_ERROR (0x0004)
  - FRAME_CLOSE (0x0005)
  - FRAME_ACK (0x0006) - reserved
- Complete state machines with timelines
- Example message sequences (3 detailed scenarios)
- Error handling strategies
- Wire format examples (hex dumps)
- Protocol versioning for forward compatibility

#### **docs/LEARNINGS.md** (645 lines)
- Engineering principles & design philosophy
- **I/O Multiplexing:**
  - select vs poll vs epoll comparison
  - Level-triggered vs edge-triggered with examples
  - Why RIFT chose level-triggered
- **Event Loop Design:**
  - Main loop pattern
  - Timeout handling for graceful shutdown
  - Event batching rationale
- **Protocol Design:**
  - Why custom binary over HTTP
  - Protocol versioning benefits
  - Frame size rationale (16 KB)
- **Memory & Performance:**
  - Pre-allocation philosophy
  - Per-connection memory breakdown
  - Zero-copy approach
- **Reliability:**
  - Signal safety (sig_atomic_t)
  - Error handling strategy
  - Frame validation
- **General Principles:**
  - 8 key system design principles

#### **README.md** (415 lines)
- Quick start (build, run, access)
- Architecture overview with ASCII diagrams
- Three listeners explained
- Key concepts:
  - RIFT protocol introduction
  - Tunnel lifecycle
  - Connection states
  - Level-triggered epoll explanation
- Implementation details:
  - Per-connection memory
  - Frame header versioning
  - Signal-safe shutdown
  - Non-blocking I/O
- Performance characteristics
- Comparison to ngrok
- Production deployment guide
- Debugging tips

#### **IMPROVEMENTS.md**
- 10 production-grade improvements (numbered)
- Frame types reference
- Testing checklist
- Security considerations (what's done, what's TODO)
- Future enhancements (7 planned)
- Code quality metrics

---

### Server Implementation Files

#### **server/include/config.h**
**Purpose:** Centralized configuration constants (tunable)

**Sections:**
1. Network configuration (ports, backlog)
2. Connection limits (max connections, ID lengths)
3. Protocol configuration (frame size, magic, version)
4. Event loop configuration (epoll events, timeouts)
5. Timeout configuration (idle, request timeouts)
6. Frame read/write configuration (retry limits, sleep durations)
7. Logging configuration (level, timestamps)
8. Feature flags (SO_REUSEADDR, TCP_NODELAY, TCP_KEEPALIVE)
9. Performance tuning (buffer sizes)
10. Compile-time assertions (validate configuration)

**Key Values:**
- `CONFIG_TUNNEL_PORT = 7000`
- `CONFIG_PUBLIC_PORT = 9000`
- `CONFIG_HEALTH_PORT = 8080`
- `CONFIG_MAX_CONNECTIONS = 102400`
- `CONFIG_FRAME_MAX_PAYLOAD = 16384`
- `CONFIG_FRAME_VERSION = 1`
- `CONFIG_EPOLL_MAX_EVENTS = 64`

**Why:** Removes hardcoded magic numbers, enables tuning without recompilation

#### **server/include/frame.h**
**Purpose:** Protocol definitions (shared between client & server)

**Defines:**
- `FRAME_MAGIC = 0x52494654` ("RIFT")
- `FRAME_VERSION = 1`
- `FRAME_MAX_PAYLOAD = 16384`
- Frame types enum (0-6)
- `frame_header_t` struct (packed, 12 bytes)

**Functions:**
- `frame_read()` - Read frame from socket
- `frame_write()` - Write frame to socket

#### **server/include/connection.h**
**Purpose:** Connection state machine types

**Types:**
- `conn_state_t` enum (4 states)
- `connection_t` struct:
  - `fd` - Socket file descriptor
  - `state` - Current state
  - `peer_fd` - Linked connection
  - `tunnel_id[64]` - Tunnel identifier
  - `service_id[64]` - Service identifier

**Functions:**
- `connection_init()` - Initialize connection table
- `connection_alloc()` - Allocate slot
- `connection_get()` - Find by fd
- `connection_bind()` - Link tunnel ↔ public
- `connection_close()` - Graceful close with cleanup
- `connection_find_tunnel()` - Search by tunnel_id

**Key Design:** 102,400 pre-allocated slots (14 MB) for O(1) operations

#### **server/include/handlers.h**
**Purpose:** Protocol handler declarations

**Functions:**
- `handle_http_request()` - Parse HTTP, find tunnel
- `handle_rift_frame()` - Handle RIFT protocol frame

#### **server/include/listener.h**
**Purpose:** TCP listener setup

**Functions:**
- `listener_create(port)` - Create listening socket
- `listener_accept()` - Accept incoming connection
- `make_nonblocking()` - Set O_NONBLOCK flag

**Features:**
- SO_REUSEADDR for quick restarts
- Non-blocking mode for epoll
- Backlog of 128

#### **server/frame.c**
**Purpose:** Frame read/write with non-blocking I/O

**Functions:**
- `write_full()` - Retry writes until all sent
- `read_full()` - Retry reads until complete
- `frame_read()` - Parse frame and validate
- `frame_write()` - Create frame and send

**Key Features:**
- Handles partial writes (kernel buffer full)
- Handles partial reads (TCP fragmentation)
- **Detailed comments explaining:**
  - Why retry logic needed
  - Level-triggered vs edge-triggered behavior
  - Signal safety considerations
  - Timeout handling

#### **server/connection.c**
**Purpose:** Connection lifecycle management

**Functions:**
- Connection allocation/deallocation
- State tracking
- Tunnel ↔ public pairing
- Proper cleanup (close, unlink)

**Key Feature:** Persistent tunnels
- When public closes, tunnel stays open
- Tunnel ready for next public connection
- Different cleanup for tunnel vs public

#### **server/epoll_server.c**
**Purpose:** Main event loop (heart of the server)

**Components:**
1. Signal handlers (SIGINT, SIGTERM, SIGPIPE)
2. Logging functions (with timestamps, severity)
3. epoll helpers (`epoll_add_fd`)
4. Health endpoint handler
5. Listener handlers (tunnel, public)
6. Connection event handler
7. Event dispatcher
8. Main loop (`epoll_server_main`)

**Key Features:**
- Level-triggered epoll (no EPOLLET)
- Signal-safe shutdown with timeout
- Batches 64 events per poll
- Detailed logging
- Proper error handling

**Config Integration:**
- Uses `CONFIG_TUNNEL_PORT`, `CONFIG_PUBLIC_PORT`, etc.
- `CONFIG_EPOLL_MAX_EVENTS = 64`
- `CONFIG_EPOLL_TIMEOUT_SHUTDOWN = 1000`

#### **server/handlers.c**
**Purpose:** Protocol-specific logic

**Handlers:**
- `handle_http_request()`:
  - Peek first 4 bytes (GET, POST, etc.)
  - Parse Host or X-Tunnel-Id header
  - Find tunnel
  - Link public ↔ tunnel
  - Send FRAME_CONNECT_REQUEST
  - Forward HTTP header as FRAME_DATA
  - Error responses

- `handle_rift_frame()`:
  - Read frame
  - Handle FRAME_REGISTER_TUNNEL
  - Handle internal FRAME_CONNECT_REQUEST
  - Store tunnel_id
  - Update state

- `handle_health_request()`:
  - HTTP endpoint on port 8080
  - Returns status, uptime, connection counts
  - JSON-like format

#### **server/listner.c**
**Purpose:** TCP socket creation and setup

**Steps:**
1. Create socket (AF_INET, SOCK_STREAM)
2. setsockopt(SO_REUSEADDR) - Quick restart
3. bind() - Bind to 0.0.0.0:port
4. listen() - Start accepting
5. Set non-blocking (fcntl O_NONBLOCK)
6. Return fd

**Used By:** tunnel listener (7000), public listener (9000), health (8080)

#### **server/metrics.c**
**Purpose:** Monitoring and metrics

**Tracks:**
- Server startup time (uptime calculation)
- Total connections ever (counter)
- Active connections (current count)

**Exposed Via:**
- HTTP /health endpoint (port 8080)
- Connection lookup functions

#### **server/forward.c**
**Purpose:** Data forwarding (stub)

Currently unused, reserved for future optimizations (e.g., kernel-level forwarding via splice())

#### **server/main.c**
**Purpose:** Server entry point

**Simple:**
```c
int main() {
    printf("Starting Rift server...\n");
    return epoll_server_main();
}
```

---

### Client Implementation Files

#### **client/main.c**
**Purpose:** Tunnel client (exposes local service)

**Components:**
1. Random tunnel ID generation (adjective-noun-number)
2. TCP connection to server
3. RIFT frame registration
4. epoll event loop (similar to server)
5. **Buffered frame reader:**
   - Accumulates data in 16KB buffer
   - Handles TCP fragmentation
   - Prevents "magic mismatch" errors
6. Local service connection
7. Bidirectional forwarding

**Key Features:**
- Frame buffering for partial TCP packets
- Persistent local connections
- Graceful shutdown on server close
- Detailed error logging

**State:**
- TUNNEL_INIT → Connects to server
- Sends FRAME_REGISTER_TUNNEL
- LISTENING → Waits for FRAME_CONNECT_REQUEST
- Opens local connection (localhost:port)
- FORWARDING → Bidirectional data transfer

---

### Build Configuration

#### **Makefile**
**Compiler Flags:**
- `-Wall` - Enable all common warnings
- `-Wextra` - Enable extra warnings
- `-Werror` - Treat warnings as errors (strict)
- `-O2` - Optimization level 2
- `-g` - Debug symbols (for gdb)

**Header Search:**
- `-Iserver/include` - Server headers

**Targets:**
- `all` - Build server and client (default)
- `clean` - Remove generated files
- Dependencies automatically tracked

---

## 🔄 Data Flow

### Request Flow (HTTP → Local Service)

```
Browser (Internet)
    ↓
tcp:9000 (public listener)
    ↓
accept() → server/listeners.c
    ↓
connection_add_public() → connection.c
    ↓
handle_initial_frame() → epoll_server.c
    ↓
handle_http_request() → handlers.c (parse, find tunnel)
    ↓
connection_bind() → connection.c (link public ↔ tunnel)
    ↓
frame_write(FRAME_CONNECT_REQUEST) → frame.c
    ↓
Client receives (frame buffered in main.c)
    ↓
Client opens tcp:3000 (local service)
    ↓
Server sends FRAME_DATA(HTTP request) via frame.c
    ↓
Client reads → local service
    ↓
Local service sends response
    ↓
Client sends FRAME_DATA(response) via frame.c
    ↓
Server reads → send to browser
    ↓
Browser receives response
```

---

## 📊 Code Statistics

| Component | Lines | Role |
|-----------|-------|------|
| frame.c | ~150 | Binary framing |
| connection.c | ~120 | State machine |
| epoll_server.c | ~200 | Event loop |
| handlers.c | ~100 | Protocol logic |
| listner.c | ~90 | Socket setup |
| client/main.c | ~250 | Client tunnel |
| Other sources | ~100 | Metrics, main, forward |
| **Total Code** | **~1,000** | Core logic |
| **Documentation** | **~2,300** | Complete guide |
| **Total Project** | **~3,300** | Code + docs |

---

## 🎯 Key Files to Review

### To Understand Architecture
1. docs/ARCHITECTURE.md
2. server/include/connection.h
3. server/epoll_server.c (event loop)

### To Understand Protocol
1. docs/PROTOCOL.md
2. server/include/frame.h
3. server/frame.c (read/write)

### To Debug Issues
1. docs/README.md (debugging tips)
2. server/epoll_server.c (log_error calls)
3. docs/PROTOCOL.md (error handling section)

### To Modify Code
1. server/include/config.h (tune constants)
2. Relevant source file
3. Add detailed comments explaining "why"

---

## 🔗 Inter-File Dependencies

```
epoll_server.c (main loop)
    ├─→ connection.h & connection.c
    ├─→ frame.h & frame.c
    ├─→ handlers.h & handlers.c
    ├─→ listener.h & listner.c
    ├─→ metrics.h & metrics.c
    └─→ config.h

connection.c
    ├─→ connection.h
    └─→ frame.h

frame.c
    └─→ frame.h

handlers.c
    ├─→ handlers.h
    ├─→ connection.h & connection.c
    └─→ frame.h & frame.c

client/main.c
    └─→ frame.h & frame.c
```

---

**Last Updated:** January 23, 2026  
**RIFT Version:** 1.0  
**Total Documentation:** 2,300+ lines  
**Code Quality:** Production-Grade
