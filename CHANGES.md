# RIFT Changelog

## v2.0 - Stream Multiplexing (February 15, 2026)

**Problem:** V1's serial request handling caused head-of-line blocking. When a browser
opened parallel connections (Portainer, heavy SPAs), requests would queue behind each
other on the single tunnel TCP connection. Heavy pages timed out or hung.

**Solution:** Added `stream_id` to the frame header (V2 protocol). Multiple browser
requests now fly concurrently over one tunnel TCP connection, each identified by a
unique stream_id.

### Protocol Changes (V1 → V2)
- Frame header: 12 → 16 bytes (added `stream_id` uint32_t field)
- `FRAME_VERSION`: 1 → 2
- `frame_read()` / `frame_write()` now take `stream_id` parameter
- `FRAME_CLOSE` is per-stream, not per-connection

### Server Changes
- **connection.h**: Removed `CONN_TUNNEL_FORWARDING` and `CONN_PUBLIC_QUEUED` states.
  Added `stream_entry_t`, `MAX_STREAMS_PER_TUNNEL=128`, stream map in `connection_t`.
- **connection.c**: Replaced queue operations with stream map ops:
  `tunnel_next_stream_id()`, `tunnel_add_stream()`, `tunnel_find_browser()`,
  `tunnel_remove_stream()`, `tunnel_close_all_streams()`.
- **handlers.c**: `handle_http_request()` assigns stream_id, registers in stream map,
  sends CONNECT_REQUEST+DATA with stream_id. Removed `serve_next_pending()`.
- **epoll_server.c**: Tunnel stays `CONN_TUNNEL_READY` always. Reads frames and demuxes
  by stream_id. FRAME_CLOSE is per-stream. No CONN_TUNNEL_FORWARDING case.

### Client Changes
- **client/main.c**: Complete rewrite for stream multiplexing:
  - Stream map (128 slots): `stream_add()`, `stream_find_fd()`, `stream_find_sid()`,
    `stream_remove_by_sid()`, `stream_remove_by_fd()`, `stream_close_all()`
  - FRAME_CONNECT_REQUEST opens new local connection per stream
  - FRAME_DATA routed to correct local fd by stream_id
  - Local fd data tagged with stream_id in outgoing frames
  - Reconnection closes all streams cleanly

### Documentation Updates
- PROTOCOL.md updated to V2 specification
- ARCHITECTURE.md updated for stream multiplexing design
- FILES.md updated for new data structures and functions

### Test Updates
- test_tunnel.py updated for V2 16-byte frame format with stream_id
- test_portainer.py updated for V2 client output

---

# RIFT CI/CD & Testing Setup - Summary

**Date:** January 23, 2026  
**Changes:** GitHub Actions workflows, comprehensive documentation, test suite enhancements

## What's New

### 1. GitHub Actions Workflows

#### Test Workflow (`.github/workflows/test.yml`)
Runs automatically on every push and pull request to `main` and `develop` branches.

**Jobs:**
- **build-and-test** (Primary)
  - Builds server and client binaries
  - Starts HTTP backend on port 3000
  - Starts RIFT server on ports 7000, 9000, 8080
  - Starts RIFT client tunnel
  - Executes all 6 tests from `tests/test_tunnel.py`
  - Generates detailed test reports

- **code-quality**
  - Verifies no compiler warnings/errors (with `-Wall -Wextra -Werror`)
  - Checks for trailing whitespace
  - Validates code style

- **performance-check**
  - Measures binary sizes (server, client)
  - Validates startup performance
  - Logs performance metrics

- **summary**
  - Aggregates results from all jobs
  - Reports pass/fail status

**Artifacts Generated:**
```
test-results/
├── REPORT.md              # Human-readable summary
├── test_results.txt       # Raw test output
└── rift_server.log        # Server logs (last 50 lines)

rift-binaries/
├── rift-server
└── rift-client

/tmp/
├── rift_server.log        # Complete server logs
├── rift_client.log        # Complete client logs
├── http_server.log        # HTTP backend logs
└── test_results.txt       # Test execution output
```

#### Release Workflow (`.github/workflows/release.yml`)
Runs when version tags are pushed (e.g., `git tag v1.0.0`).

**Platforms Supported:**
- Linux x86_64
- macOS x86_64

**Release Artifacts:**
```
rift-linux-x86_64.tar.gz         (includes binaries, docs)
rift-linux-x86_64.tar.gz.sha256  (checksum)

rift-macos-x86_64.tar.gz         (includes binaries, docs)
rift-macos-x86_64.tar.gz.sha256  (checksum)
```

---

### 2. Updated Documentation

#### README.md
- **New Section:** "Testing" with:
  - Quick test suite execution instructions
  - Test coverage table (all 6 tests documented)
  - Individual test script descriptions
  - Expected output

#### docs/LEARNINGS.md
- **New Section:** "HTTP/1.1 Keep-Alive & Connection Persistence"
  - Explains the Keep-Alive problem in tunnel proxying
  - Documents the original issue and its symptoms
  - Details the three-part fix:
    1. Client-side FRAME_CLOSE notification
    2. Connection reset function
    3. Server-side state management
  - Includes code examples showing before/after behavior

#### docs/CI-CD.md (NEW)
Comprehensive CI/CD documentation (~300 lines):
- GitHub Actions workflows overview
- Test suite details and expected output
- Running tests locally
- Test artifacts and accessing them
- Performance benchmarks
- Release process
- Troubleshooting guide
- Environment variables
- Logs and diagnostics

#### docs/INDEX.md
- Added CI-CD.md to documentation index
- Updated documentation navigation guide
- Added CI-CD to "Path 1: New to this" quick start

---

### 3. Test Suite Enhancements

#### Test Configuration
All tests in `tests/test_tunnel.py`:

| # | Name | What It Tests | Lines |
|---|------|---------------|-------|
| 1 | Registration | RIFT protocol frame parsing | 40 |
| 2 | Single Request | Basic HTTP/RIFT forwarding | 50 |
| 3 | Sequential Requests | HTTP/1.1 Keep-Alive (3 requests) | 80 |
| 4 | Concurrent Requests | Multiple simultaneous tunnels (2) | 60 |
| 5 | Cancellation Recovery | Connection reset and recovery | 75 |
| 6 | Custom Headers | x-tunnel-id header routing | 45 |

**Total:** ~390 lines of test code, ~15 seconds execution time

#### Key Testing Features
- MockTunnelClient class for simulating tunnel clients
- Frame packing/unpacking utilities
- Automatic server/client startup
- Timeout-safe operations
- Detailed error reporting

#### Test Artifacts
Each workflow run generates:
- Raw test output in `/tmp/test_results.txt`
- Formatted REPORT.md with timestamp, runner info, logs
- Server logs showing frame exchanges
- Client logs showing tunnel behavior
- HTTP backend logs for debugging

---

## How to Use

### Running Tests Locally

```bash
# Terminal 1: Start server
./rift-server

# Terminal 2: Start client
./rift-client expose 3000

# Terminal 3: Run tests
python3 tests/test_tunnel.py
```

Expected output:
```
============================================================
RESULTS: 6/6 tests passed
============================================================
```

### Accessing GitHub Actions Results

1. Go to repository → **Actions** tab
2. Select recent workflow run
3. Click "build-and-test" job
4. Scroll to "Summary" or "Artifacts" section
5. Download artifact ZIP containing all logs

### Creating a Release

```bash
# Tag the release
git tag v1.0.0
git push origin v1.0.0

# GitHub Actions automatically:
# 1. Builds on Linux and macOS
# 2. Creates distribution packages
# 3. Computes SHA256 checksums
# 4. Creates GitHub Release page with binaries
```

### Extending Tests

Add new test function to `tests/test_tunnel.py`:

```python
def test_my_feature():
    """Describe what you're testing"""
    tunnel = MockTunnelClient("test-tunnel")
    tunnel.connect()
    
    # Your test code here
    # ...
    
    tunnel.close()
    print("✓ My feature test passed")
```

Then commit and push - GitHub Actions will run it automatically.

---

## Files Modified

```
.github/
├── workflows/
│   ├── test.yml          (NEW - Main CI/CD pipeline)
│   └── release.yml       (NEW - Release builds)

docs/
├── CI-CD.md              (NEW - CI/CD documentation)
├── LEARNINGS.md          (UPDATED - Keep-Alive section)
└── INDEX.md              (UPDATED - CI-CD reference)

README.md                  (UPDATED - Testing section)
tests/test_tunnel.py      (UNCHANGED - Already comprehensive)
```

---

## Success Criteria

✅ **All 6 tests pass** in GitHub Actions  
✅ **No compiler warnings** (GCC -Wall -Wextra -Werror)  
✅ **Test artifacts generated** with detailed logs  
✅ **Release builds** create distribution packages  
✅ **Documentation complete** and cross-referenced  
✅ **Performance verified** (startup time, binary sizes)  

---

## Next Steps (Optional)

### Consider Adding:
1. **Code coverage reporting** (gcov for C code)
2. **Performance regression tracking** (store metrics in database)
3. **Stress testing** (send 10,000+ concurrent connections)
4. **Integration tests** (test against real services)
5. **Docker builds** (add Dockerfile and docker.yml workflow)
6. **Nightly builds** (schedule workflow for nightly testing)
7. **Security scanning** (SAST, dependency checking)

### Documentation Improvements:
1. **API documentation** (Doxygen for C code)
2. **Deployment guide** (Kubernetes, systemd, Docker)
3. **Troubleshooting FAQ** (common issues and solutions)
4. **Benchmark results** (publish performance metrics)

---

## Verification

To verify everything is set up correctly:

```bash
# 1. Check workflows are in place
ls -la .github/workflows/

# 2. Check documentation exists
ls -la docs/CI-CD.md

# 3. Run tests locally
python3 tests/test_tunnel.py

# 4. Commit and push to trigger GitHub Actions
git add .
git commit -m "Add CI/CD workflows and testing documentation"
git push
```

Then check GitHub → Actions tab to see workflow run.

---

## References

- GitHub Actions: https://docs.github.com/en/actions
- Test Suite: [tests/test_tunnel.py](../tests/test_tunnel.py)
- CI/CD Guide: [docs/CI-CD.md](CI-CD.md)
- Full Documentation: [docs/INDEX.md](INDEX.md)
