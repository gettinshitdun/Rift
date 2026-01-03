
**Rift** is a reverse TCP tunneling system written in C, built from scratch
to explore Linux epoll, TCP multiplexing, backpressure, and OS-level I/O.

## ✅ Implementation Checklist (Learning Progress)

This project is being built incrementally to deeply understand
**Linux networking, epoll, TCP behavior, and OS I/O fundamentals**.

### Core Networking
- [ ] TCP reverse tunnel (NAT traversal)
- [ ] Persistent client → server connection
- [ ] Public port exposure for local services

### Protocol
- [ ] Custom binary framing protocol
- [ ] Length-prefixed messages
- [ ] Stream identifiers
- [ ] Partial read/write handling

### Event-Driven I/O
- [ ] Non-blocking sockets
- [ ] epoll-based event loop
- [ ] Level-triggered epoll
- [ ] Proper FD lifecycle management

### Multiplexing
- [ ] Multiple public connections over one tunnel
- [ ] Stream open / close semantics
- [ ] Stream ↔ socket mapping

### Flow Control
- [ ] Per-connection write buffers
- [ ] Backpressure handling
- [ ] Read throttling on buffer pressure

### Failure Handling
- [ ] Graceful client disconnects
- [ ] Half-open connection detection
- [ ] Heartbeat / keepalive mechanism
- [ ] Clean resource cleanup

### OS-Level Optimizations
- [ ] Zero-copy forwarding (`splice` / `sendfile`)
- [ ] TCP socket tuning
- [ ] Reduced syscall overhead

### Security & Polish (Optional)
- [ ] Authentication handshake
- [ ] Replay protection
- [ ] TLS support
- [ ] CLI configuration
---
