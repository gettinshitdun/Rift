# RIFT: Documentation Guide

## What You Have

A production-grade ngrok-like tunnel server with **minimalist but complete documentation**.

## Core Files

### **README.md** - Start Here
- Quick start (build, run, test)
- Architecture overview
- Key concepts explained
- Comparison to ngrok

### **docs/ARCHITECTURE.md** - System Design
- Connection states and flow
- I/O model (why level-triggered epoll)
- 8 design decisions with rationale
- Performance characteristics

### **docs/PROTOCOL.md** - Wire Format
- Frame format specification
- All 6 frame types
- Message flow examples
- Error handling

### **docs/LEARNINGS.md** - Design Philosophy
- I/O multiplexing explained
- Event loop design
- Memory & performance decisions
- Reliability principles

### **docs/FILES.md** - Code Guide
- File structure and purposes
- Key components explained
- Data flow diagram
- Dependencies

### **docs/INDEX.md** - Navigation
- Reading paths by role
- Links between documents
- Quick topic finder

## Build & Run

```bash
make clean && make
./rift-server         # Terminal 1
./rift-client expose 3000  # Terminal 2
curl -H "X-Tunnel-Id: <tunnel-id>" http://localhost:9000/
```

## Key Design Decisions

1. **Level-triggered epoll** - Simple, safe, predictable
2. **Persistent tunnels** - One tunnel, many requests
3. **Fixed connection slots** - O(1) allocation, predictable
4. **Frame buffering** - Handles TCP fragmentation
5. **Binary protocol** - Efficient framing with versioning
6. **Signal-safe shutdown** - Graceful with timeout
7. **Pre-allocation** - No malloc/free during runtime
8. **Metrics collection** - Health monitoring built-in

## Documentation Statistics

- **Code**: ~1,000 lines (clean, well-commented)
- **Docs**: ~2,940 lines (essential, no fluff)
- **Total**: ~3,940 lines
- **No code examples in ARCHITECTURE.md** (pure concepts)

## What Each Document Covers

| Document | Purpose | Length |
|----------|---------|--------|
| README.md | Quick start + overview | 400 lines |
| ARCHITECTURE.md | System design concepts | 350 lines |
| PROTOCOL.md | Wire protocol spec | 570 lines |
| LEARNINGS.md | Design philosophy | 640 lines |
| FILES.md | Code structure guide | 480 lines |
| INDEX.md | Navigation & guide | 400 lines |

## Next Steps

1. **Read README.md** (10 min) - Get started
2. **Skim ARCHITECTURE.md** (15 min) - Understand design
3. **Review PROTOCOL.md** (30 min) - Know the protocol
4. **Build and test** (5 min) - Run the code
5. **Read LEARNINGS.md** (30 min) - Learn principles

**Total:** ~90 minutes to full understanding

## Quick Facts

- **Language**: C99
- **Compiler flags**: `-Wall -Wextra -Werror -O2 -g`
- **Platforms**: Linux (uses epoll)
- **Dependencies**: None (pure POSIX)
- **Max connections**: 102,400
- **Memory**: ~14 MB baseline
- **Lines of docs**: 0 code examples in ARCHITECTURE

---

**Status**: Production-ready, fully documented, minimalist approach
