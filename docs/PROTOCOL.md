# RIFT Protocol Specification v2

## Overview

RIFT is a binary framing protocol designed for reliable, bidirectional data transfer between tunnel clients and a central server. Version 2 adds **stream multiplexing** — a `stream_id` field in each frame so multiple browser requests can fly concurrently over a single tunnel TCP connection, eliminating the head-of-line blocking that plagued V1's serial request handling.

## Frame Format

### Binary Layout (Network Byte Order - Big Endian)

```
Offset  Field          Bytes   Type        Description
───────────────────────────────────────────────────────────
0       magic          4       uint32_t    Magic: 0x52494654 = "RIFT"
4       version        1       uint8_t     Protocol version (2)
5       reserved       1       uint8_t     Must be 0 (reserved for future)
6       type           2       uint16_t    Frame type (see types below)
8       length         4       uint32_t    Payload size (0 - 16384 bytes)
12      stream_id      4       uint32_t    Stream identifier (0 = control)
───────────────────────────────────────────────────────────
16      payload        0-16KB  binary      Frame-specific data
```

**Total header size:** 16 bytes  
**Total frame size:** 16 bytes + payload length

### Stream Multiplexing (New in V2)

Each browser request is assigned a unique `stream_id` by the server.
All frames for that request (CONNECT_REQUEST, DATA, CLOSE) carry the same
`stream_id`. The client opens a **separate local connection per stream**
and routes data by `stream_id`. This allows 128 concurrent browser
requests over a single tunnel TCP connection.

- `stream_id = 0` — control frames (REGISTER_TUNNEL, global ERROR)
- `stream_id > 0` — per-browser-request data stream

### Example Frame (hex dump)

Registration frame with tunnel_id "abc-123" (stream_id=0):

```
Hex:              ASCII:
52 49 46 54       R I F T          (magic)
02                .                (version = 2)
00                .                (reserved = 0)
00 01             . .              (type = 0x0001 = FRAME_REGISTER_TUNNEL)
00 00 00 07       . . . .          (length = 7 bytes)
00 00 00 00       . . . .          (stream_id = 0, control frame)
61 62 63 2D 31 32 33    a b c - 1 2 3   (payload)
```

## Frame Types

### 1. FRAME_REGISTER_TUNNEL (0x0001)

**Direction:** Client → Server  
**Frequency:** Once per connection  
**Purpose:** Register tunnel ID, transition from TUNNEL_INIT to TUNNEL_READY

**Payload:** Tunnel ID string (null-terminated not required)  
**Example:** `"user-tunnel-42"` (14 bytes)

**Server Response:** None (implicit ACK by staying in TUNNEL_READY state)

**Use Case:**
```
Timeline:
T0: Client connects to server:7000 (TCP SYN)
T1: Server accepts, creates connection (state = TUNNEL_INIT)
T2: Client sends FRAME_REGISTER_TUNNEL("user-tunnel-42")
T3: Server stores mapping: "user-tunnel-42" → fd
T4: Server transitions to TUNNEL_READY
T5+: Tunnel can now accept public connections
```

---

### 2. FRAME_CONNECT_REQUEST (0x0002)

**Direction:** Server → Client  
**Frequency:** Once per public connection (each gets unique stream_id)  
**Purpose:** Notify client of incoming public request, assign stream_id

**Payload:** Short status message (optional)  
**Example:** `"NEW"` (3 bytes) or `""` (0 bytes)  
**stream_id:** Unique per-browser identifier assigned by server

**Expected Client Action:**
1. Receive frame, note `stream_id`
2. Open **new** TCP connection to local service (e.g., localhost:3000)
3. Map `stream_id ↔ local_fd` in stream table
4. Start forwarding FRAME_DATA for this stream_id bidirectionally

**Timing (V2 — multiple streams concurrent):**
```
Timeline:
T0: Browser A: GET /page   → server:9000
T1: Browser B: GET /api    → server:9000
T2: Server assigns stream_id=1 for A, stream_id=2 for B
T3: Server sends FRAME_CONNECT_REQUEST(stream=1) + FRAME_DATA(stream=1)
T4: Server sends FRAME_CONNECT_REQUEST(stream=2) + FRAME_DATA(stream=2)
T5: Client opens localhost:3000 for stream 1, localhost:3000 for stream 2
T6: Both streams forward data concurrently over single tunnel TCP
```

---

### 3. FRAME_DATA (0x0003)

**Direction:** Bidirectional  
**Frequency:** Multiple times per stream  
**Purpose:** Carry actual payload (HTTP headers, body, etc.)

**Payload:** Raw bytes, interpretation depends on context  
**Size:** 0 - 16,384 bytes per frame  
**stream_id:** Must match the stream assigned by FRAME_CONNECT_REQUEST

**Flow (Public → Tunnel direction):**
```
Browser HTTP request (raw):
GET /api/users HTTP/1.1
Host: example.com
Content-Length: 5

Hello

                    ↓ (wrapped in V2 frame with stream_id)

Server:
┌──────────────────────────────────────────┐
│ FRAME_DATA header (16 bytes)             │
│ length = 62 bytes, stream_id = 42        │
├──────────────────────────────────────────┤
│ GET /api/users HTTP/1.1\r\nHost: ...    │
│ \r\n\r\nHello                            │
└──────────────────────────────────────────┘
                    ↓

Tunnel Client:
Lookup local_fd = stream_map[42]
Write payload → local_fd
```

**Flow (Tunnel → Public direction):**
```
Local service response (raw):
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: 18

{"status":"ok"}

                    ↓ (wrapped in V2 frame with stream_id)

Client:
┌──────────────────────────────────────────┐
│ FRAME_DATA header (16 bytes)             │
│ length = 77 bytes, stream_id = 42        │
├──────────────────────────────────────────┤
│ HTTP/1.1 200 OK\r\nContent-Type: ...    │
│ \r\n\r\n{"status":"ok"}                  │
└──────────────────────────────────────────┘
                    ↓

Server:
Lookup browser_fd = tunnel.streams[42]
Write payload → browser_fd
```

**Chunking:** If payload > 16KB, split into multiple frames:
```
16 MB file transfer:
File (16 MB)
├─> Chunk 1 (16 KB) → FRAME_DATA (length=16384)
├─> Chunk 2 (16 KB) → FRAME_DATA (length=16384)
├─> Chunk 3 (16 KB) → FRAME_DATA (length=16384)
... (1024 frames total)
└─> Chunk 1024 → FRAME_DATA (length=remaining)

Receiver reconstructs by reading all frames sequentially
```

---

### 4. FRAME_ERROR (0x0004)

**Direction:** Either  
**Frequency:** Occasionally  
**Purpose:** Signal error condition

**Payload:** Error description string  
**Examples:**
- `"tunnel not found"`
- `"invalid protocol version"`
- `"frame too large"`

**Typical Flow:**
```
Browser: GET / HTTP/1.1
         Host: nonexistent-tunnel

Server: Can't find tunnel, sends to browser:
        HTTP/1.1 404 Not Found
        Content-Length: 19
        
        No Tunnel Specified

        (Connection closes)
        
        Tunnel receives nothing (public connection directly errored)
```

**Reserved for Future:** Could also be sent on tunnel to signal server errors.

---

### 5. FRAME_CLOSE (0x0005)

**Direction:** Either  
**Frequency:** Once per stream  
**Purpose:** Close a specific stream (not the whole tunnel)

**Payload:** Optional close reason  
**stream_id:** The stream being closed  
**Examples:**
- `""` (no reason) — browser disconnected normally
- `"local_failed"` — client couldn't connect to local service
- `"write_error"` — data forwarding failed
- `"browser_closed"` — server detected browser EOF

**V2 Behavior:**
```
Browser A finishes (stream_id=1):
Server: FRAME_CLOSE(stream=1, "browser_closed")
        ↓
Client: Close local_fd for stream 1
        Remove stream 1 from map
        (Other streams continue unaffected)
```

---

### 6. FRAME_ACK (0x0006)

**Direction:** Either  
**Frequency:** Reserved  
**Purpose:** Reserved for future use

**Not currently used.**

**Potential Use Case:** Acknowledgment-based flow control
```
Future high-performance mode:
Client: FRAME_DATA (length=16KB)
        ↓
Server: Read + process + send FRAME_ACK
        ↓
Client: Waits for ACK before sending next frame
        (Prevents buffer overflow on receiver)
```

---

## State Machine

### Server-Side Tunnel Connection States

```
TUNNEL_INIT
  ├─ Waiting for FRAME_REGISTER_TUNNEL
  └─→ Receive FRAME_REGISTER_TUNNEL
     └─→ TUNNEL_READY

TUNNEL_READY (persistent, never leaves this state)
  ├─ Maintains stream_map[128]: stream_id ↔ browser_fd
  ├─ Can accept multiple concurrent public connections
  │
  ├─→ New public connection arrives
  │   ├─ Assign stream_id = tunnel_next_stream_id()
  │   ├─ tunnel_add_stream(stream_id, browser_fd)
  │   └─ Send FRAME_CONNECT_REQUEST(stream_id) + FRAME_DATA(stream_id)
  │
  ├─→ Receive FRAME_DATA(stream_id) from client
  │   └─ Lookup browser_fd = tunnel_find_browser(stream_id)
  │      └─ Write payload to browser_fd
  │
  ├─→ Receive FRAME_CLOSE(stream_id) from client
  │   └─ Close specific browser_fd, remove from stream_map
  │
  ├─→ Browser closes (one stream)
  │   └─ Send FRAME_CLOSE(stream_id) to tunnel, remove stream
  │      (other streams unaffected)
  │
  └─→ Tunnel TCP closes/error
      └─ tunnel_close_all_streams() → 502 to all browsers
      └─ CLOSED
```

### Server-Side Public Connection States

```
PUBLIC_INIT
  ├─ Just accepted, buffer first 4 bytes to identify protocol
  │
  ├─→ "RIFT" → Internal protocol
  │   └─→ handle_rift_frame()
  │
  ├─→ "GET " / "POST" / "HEAD" → HTTP
  │   └─→ handle_http_request()
  │       ├─ Extract Host header
  │       ├─ Find tunnel by ID
  │       └─→ PUBLIC_FORWARDING (linked)
  │
  └─→ Identification failed
      └─→ CLOSED (send HTTP 404)

PUBLIC_FORWARDING
  ├─ Linked to tunnel
  ├─ Forwarding raw bytes ←→ tunnel FRAME_DATA
  │
  ├─→ Browser sends more data
  │   └─→ Wrap in FRAME_DATA, send to tunnel
  │
  ├─→ Tunnel sends FRAME_DATA
  │   └─→ Extract payload, write to browser
  │
  └─→ Browser closes or error
      └─→ CLOSED (unlink from tunnel, tunnel stays alive)
```

### Client State Machine (V2 Stream Multiplexing)

```
INIT
  ├─ TCP connect to server:7000
  └─→ Connected
  
REGISTERED
  ├─ Sent FRAME_REGISTER_TUNNEL (stream_id=0)
  └─→ Enters multiplexed event loop
  
MULTIPLEXED (main state)
  ├─ Maintains stream_map[128]: stream_id ↔ local_fd
  │
  ├─→ FRAME_CONNECT_REQUEST(stream_id=N) arrives
  │   ├─ Open TCP to localhost:port
  │   ├─ stream_add(N, local_fd)
  │   └─ Add local_fd to epoll
  │
  ├─→ FRAME_DATA(stream_id=N) arrives
  │   ├─ local_fd = stream_find_fd(N)
  │   └─ write(local_fd, payload)
  │
  ├─→ FRAME_CLOSE(stream_id=N) arrives
  │   ├─ Close local_fd for stream N
  │   └─ stream_remove(N)
  │
  ├─→ Local fd has data (epoll)
  │   ├─ stream_id = stream_find_sid(fd)
  │   └─ frame_write(FRAME_DATA, buf, stream_id) → server
  │
  ├─→ Local fd closes (EOF)
  │   ├─ frame_write(FRAME_CLOSE, stream_id) → server
  │   └─ stream_remove(fd)
  │
  └─→ Server TCP closes/error
      └─ stream_close_all() → close all local fds
      └─ Reconnect, re-register
```

---

## Example Message Sequences

### Sequence 1: Simple GET Request (V2)

```
Timeline:
T0: Client: TCP SYN to server:7000
T1: Server: Accept, state=TUNNEL_INIT
T2: Client: FRAME_REGISTER_TUNNEL("api.tunnel", stream_id=0)
T3: Server: Store mapping, state=TUNNEL_READY

    [... tunnel idle, stream_map empty ...]

T4: Browser: TCP SYN to server:9000
T5: Server: Accept, state=PUBLIC_INIT
T6: Browser: GET /api/users HTTP/1.1\r\nHost: api.tunnel\r\n\r\n
T7: Server: Parse Host, find tunnel, assign stream_id=1
T8: Server: tunnel_add_stream(1, browser_fd)
T9: Server: FRAME_CONNECT_REQUEST("NEW", stream=1) → tunnel
T10:Server: FRAME_DATA(HTTP request, stream=1) → tunnel
T11:Client: Receive FRAME_CONNECT_REQUEST(stream=1)
T12:Client: TCP SYN to localhost:3000, stream_add(1, local_fd)
T13:Client: Receive FRAME_DATA(stream=1), write → local_fd
T14:Local:  Process request, respond
T15:Client: Read response, FRAME_DATA(stream=1) → server
T16:Server: tunnel_find_browser(1) → browser_fd, write response
T17:Browser: Receive HTTP/1.1 200 OK
T18:Browser: Close TCP connection
T19:Server: FRAME_CLOSE(stream=1) → tunnel, remove stream
T20:Client: Close local_fd, stream_remove(1)
    Tunnel stays READY, stream_map empty, ready for more
```

### Sequence 1b: Concurrent Requests (V2 key feature)

```
Timeline:
T0-T3: [Tunnel registered, stream_map empty]

T4: Browser A: GET /page → server:9000
T5: Browser B: GET /api  → server:9000
T6: Server: Assign stream_id=1 for A, stream_id=2 for B
T7: Server: CONNECT_REQUEST(stream=1) + DATA(stream=1) → tunnel
T8: Server: CONNECT_REQUEST(stream=2) + DATA(stream=2) → tunnel
T9: Client: Open local:3000 for stream 1, local:3000 for stream 2
T10:        Both streams forward data concurrently
T11:Local A responds → Client: FRAME_DATA(stream=1) → Server → Browser A
T12:Local B responds → Client: FRAME_DATA(stream=2) → Server → Browser B
T13:Both browsers receive responses without waiting for each other
```

### Sequence 2: Connection Error

```
Timeline:
T0-T3: [Same as above, tunnel registered]

T4: Browser: TCP SYN to server:9000
T5: Server: Accept, but Host header says "invalid-tunnel"
T6: Server: Can't find tunnel, send HTTP 404
T7: Browser: Receive 404, close connection
T8: Server: Close public connection
    Tunnel remains TUNNEL_READY (waiting for next request)

Note: This doesn't require any RIFT frame exchange.
Server handles it at TCP/HTTP level.
```

### Sequence 3: Large Data Transfer (100 KB)

```
Timeline:
T0-T12: [Initial connection setup, FRAME_CONNECT_REQUEST sent]

T13:Server: FRAME_DATA (16 KB chunk 1) → tunnel
T14:Client: Write chunk 1 → localhost:3000

T15:Server: FRAME_DATA (16 KB chunk 2) → tunnel
T16:Client: Write chunk 2 → localhost:3000

... [repeat for chunks 3-6] ...

T27:Server: FRAME_DATA (4 KB chunk 7, final) → tunnel
T28:Client: Write chunk 7 → localhost:3000

T29:Client: Receive 100 KB response from local service
T30:Client: Split into 7 frames (16 KB + 16 KB + ... + 4 KB)

T31:Client: FRAME_DATA chunk 1 → server
T32:Server: Write chunk 1 → browser
... [repeat] ...
T37:Client: FRAME_DATA chunk 7 → server
T38:Server: Write chunk 7 → browser

T39:Browser: Receive complete 100 KB response
```

---

## Error Handling

### Protocol Violations

**Invalid Magic Number**
- Symptom: Received 0x12345678 instead of 0x52494654
- Action: Close connection
- Log: `[frame] Magic mismatch: got 0x12345678, expected 0x52494654`
- Reason: Fatal corruption, can't recover

**Unsupported Version**
- Symptom: Received version=2 but we support version=1
- Action: Close connection
- Log: `[frame] Unsupported version: 2 (expected 1)`
- Reason: Protocol incompatibility, upgrade needed

**Frame Too Large**
- Symptom: Received length=65536 but max is 16384
- Action: Close connection
- Log: `[frame] Payload too large: 65536 (max 16384)`
- Reason: Malformed frame, might indicate attack

### Connection Errors

**Tunnel Not Found**
- Browser requests tunnel-id that doesn't exist
- Action: Send HTTP 404 to browser
- Log: `[http] No Tunnel Specified or Found`
- Reason: User error (wrong tunnel-id)

**Local Service Unavailable**
- Client can't connect to localhost:3000
- Action: Close tunnel connection
- Log: `[!] Failed to connect to local service`
- Reason: Local service down, requires manual restart

**Broken Pipe**
- Write to socket fails with EPIPE
- Action: Close connection, unlink peer
- Reason: Remote end closed connection unexpectedly

---

## Performance Considerations

### Frame Size Impact

| Payload Size | Frames Needed | Syscalls | Latency |
|--------------|---------------|----------|---------|
| 100 bytes    | 1             | 2        | ~1ms    |
| 16 KB        | 1             | 2        | ~1ms    |
| 1 MB         | 64            | 128      | ~64ms   |
| 1 GB         | 65536         | 131072   | ~65s    |

### Chunking Strategy

```c
// To send large file efficiently:
while (bytes_remaining > 0) {
    size_t chunk_size = MIN(bytes_remaining, FRAME_MAX_PAYLOAD);
    frame_write(fd, FRAME_DATA, chunk + offset, chunk_size);
    offset += chunk_size;
    bytes_remaining -= chunk_size;
}
```

### Buffering

- **Server**: Minimal (frame read buffer = 12 + 16KB = 16KB per connection)
- **Client**: Frame buffer on receive only (16KB)
- **No application-level buffer** (directly forward bytes)

---

## Appendix: Wire Format Examples

### Example 1: FRAME_REGISTER_TUNNEL (V2)

Raw bytes:
```
52 49 46 54        magic (RIFT)
02                 version (V2)
00                 reserved
00 01              type (FRAME_REGISTER_TUNNEL = 1)
00 00 00 0d        length (13 bytes)
00 00 00 00        stream_id (0 = control frame)
6d 79 2d 74 75 6e 6e 65 6c 2d 34 32    "my-tunnel-42" (13 chars)
```

Python interpretation:
```python
struct.pack('>I', 0x52494654) +       # magic
struct.pack('>B', 2) +                # version (V2)
struct.pack('>B', 0) +                # reserved
struct.pack('>H', 1) +                # type
struct.pack('>I', 13) +               # length
struct.pack('>I', 0) +                # stream_id (control)
b'my-tunnel-42'                       # payload
```

### Example 2: FRAME_DATA with HTTP Response (V2)

```
52 49 46 54        magic
02                 version (V2)
00                 reserved
00 03              type (FRAME_DATA = 3)
00 00 00 2a        length (42 bytes)
00 00 00 07        stream_id (7)
48 54 54 50 2f 31 2e 31 20 32 30 30 20 4f 4b 0d 0a  "HTTP/1.1 200 OK\r\n"
43 6f 6e 74 65 6e 74 2d 4c 65 6e 67 74 68 3a 20 30  "Content-Length: 0"
0d 0a 0d 0a        "\r\n\r\n"
```

---

## Versioning & Backward Compatibility

### Current Version: 2

**V1 → V2 Changes:**
- Header grew from 12 to 16 bytes (added `stream_id` field)
- Multiple concurrent browser requests per tunnel (stream multiplexing)
- FRAME_CLOSE is now per-stream, not per-connection
- Tunnel stays in TUNNEL_READY state always (no FORWARDING state)
- Client maintains stream map instead of single local_fd

Future versions might support:
- **v3:** Compression (gzip frame payloads)
- **v4:** Authentication (HMAC-signed frames)
- **v5:** Encryption (AES-256 frame encryption)

**Migration Path:**
```
Old client (v1) talks to new server (v2):
└─ Server detects version=1
└─ Server responds in v1 protocol
└─ Works but misses new features

New client (v2) talks to old server (v1):
└─ Client detects server's v1 frames
└─ Gracefully downgrades to v1
└─ Works but slower (no compression)
```

---

## References

- [ARCHITECTURE.md](ARCHITECTURE.md) - System design and decisions
- [README.md](README.md) - Quick start and overview
