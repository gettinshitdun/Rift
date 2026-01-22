# RIFT CI/CD & Testing Guide

## GitHub Actions Workflows

RIFT uses GitHub Actions for continuous integration and automated testing.

### Workflows

#### 1. **Test Suite** (`.github/workflows/test.yml`)
Runs on every push and pull request.

**Jobs:**
- **build-and-test**: Builds binaries and runs all 6 tests
- **code-quality**: Checks for compiler warnings and code style
- **performance-check**: Validates binary sizes and startup performance
- **summary**: Aggregates results

**Artifacts Generated:**
- `test-results/` - Detailed test reports and logs
- `rift-binaries/` - Compiled server and client binaries
- `/tmp/rift_server.log` - Server logs during testing
- `/tmp/rift_client.log` - Client logs during testing
- `/tmp/test_results.txt` - Raw test output

**Success Criteria:**
- All 6 tests pass
- No compiler warnings (GCC: `-Wall -Wextra -Werror`)
- No trailing whitespace in code
- Binaries under expected size

#### 2. **Release Build** (`.github/workflows/release.yml`)
Runs when tags are pushed (e.g., `v1.0.0`).

**Platforms:**
- Linux x86_64
- macOS x86_64

**Artifacts Generated:**
- `rift-linux-x86_64.tar.gz` - Linux distribution package
- `rift-macos-x86_64.tar.gz` - macOS distribution package
- SHA256 checksums for each

---

## Running Tests Locally

### Quick Start

```bash
# Terminal 1: Start server
make clean && make
./rift-server

# Terminal 2: Start client tunnel
./rift-client expose 3000

# Terminal 3: Run tests
python3 tests/test_tunnel.py
```

### Expected Output

```
============================================================
[TEST 1] Tunnel Registration
  ✓ Tunnel registration successful
[TEST 2] Single HTTP Request
  ✓ Single request successful
[TEST 3] Multiple Sequential Requests
  ✓ All sequential requests successful
[TEST 4] Concurrent Requests
  ✓ Concurrent requests successful
[TEST 5] Request Cancellation and Recovery
  ✓ Recovered from cancellation
[TEST 6] Custom x-tunnel-id Header
  ✓ Custom header routing successful
============================================================
RESULTS: 6/6 tests passed
============================================================
```

### Test Suite Details

| Test | File | Purpose | Duration |
|------|------|---------|----------|
| 1 | `test_tunnel.py:test_registration()` | RIFT protocol frame parsing | 1s |
| 2 | `test_tunnel.py:test_single_request()` | HTTP/RIFT forwarding | 2s |
| 3 | `test_tunnel.py:test_sequential_requests()` | Keep-Alive persistence | 3s |
| 4 | `test_tunnel.py:test_concurrent_requests()` | Multiple simultaneous tunnels | 3s |
| 5 | `test_tunnel.py:test_cancellation_recovery()` | Connection reset handling | 2s |
| 6 | `test_tunnel.py:test_custom_headers()` | Header routing logic | 2s |

**Total Time:** ~15 seconds

---

## Test Artifacts

### Directory Structure

```
test-reports/
├── REPORT.md              # Human-readable summary
├── test_results.txt       # Raw Python output
├── rift_server.log        # Server stderr/stdout (last 50 lines)
└── (archived with build artifacts)
```

### Accessing Artifacts

**From GitHub Actions:**
1. Go to repository → "Actions" tab
2. Select the workflow run
3. Scroll to "Artifacts" section
4. Download the artifact ZIP

**Contents:**
- `test-results/REPORT.md` - Test summary
- `rift-binaries/` - Compiled binaries (if tests passed)
- Server/client/HTTP server logs

### CI/CD Dashboard

View all workflow runs:
- GitHub: Repository → Actions → All workflows
- Status badge in README shows latest run

---

## Troubleshooting

### Tests Hang or Timeout

**Symptoms:**
- GitHub Actions workflow times out (>30 min)
- Tests waiting indefinitely

**Causes:**
- Server failed to start (port in use)
- Client failed to connect
- Frames malformed

**Solution:**
```bash
# Kill any lingering processes
pkill -f rift-server
pkill -f rift-client
pkill -f "http.server"

# Check logs
tail -20 /tmp/rift_server.log
tail -20 /tmp/rift_client.log
```

### Tests Fail with "Connection reset by peer"

**Expected in some tests** (intentionally test cancellation).

**Unexpected failure:**
- Check that localhost:3000 HTTP server is running
- Verify server/client binaries exist
- Look at full test output for context

### Artifacts Not Generated

**Check:**
1. Workflow must complete (not timeout)
2. Artifacts are uploaded even if tests fail
3. Download from GitHub Actions UI

---

## Extending Tests

### Adding a New Test

```python
def test_my_feature():
    """Test description"""
    tunnel = MockTunnelClient("my-tunnel")
    tunnel.connect()
    
    # Make request
    sock = socket.socket()
    sock.connect(("127.0.0.1", 9000))
    sock.sendall(b"GET / HTTP/1.1\r\nx-tunnel-id: my-tunnel\r\n\r\n")
    
    response = sock.recv(4096)
    assert b"HTTP" in response
    
    sock.close()
    tunnel.close()
    print("✓ My feature test passed")
```

Add to `tests/test_tunnel.py` before the main block.

### Local Testing Before PR

```bash
# Run tests locally
python3 tests/test_tunnel.py

# If all pass, commit and push
git add tests/
git commit -m "Add new test: my_feature"
git push origin feature-branch
```

GitHub Actions will automatically run tests on your PR.

---

## Performance Benchmarks

### Expected Performance

| Metric | Value |
|--------|-------|
| Server startup | <100ms |
| Tunnel registration | <50ms |
| Single request latency | <10ms (local) |
| Throughput | 1000+ concurrent connections |
| Memory per connection | ~2KB |

### Monitoring in CI/CD

Performance checks run as separate job:
- Binary size: Should be <500KB
- Startup time: Logged for comparison
- Test duration: Benchmarked against baseline

---

## Release Process

### Creating a Release

```bash
# Tag the release
git tag v1.0.0
git push origin v1.0.0

# GitHub Actions automatically:
# 1. Builds on Linux and macOS
# 2. Creates distribution packages
# 3. Computes SHA256 checksums
# 4. Creates GitHub Release with binaries
```

### Verifying Releases

```bash
# Download and verify
wget https://github.com/...releases/download/v1.0.0/rift-linux-x86_64.tar.gz
wget https://github.com/...releases/download/v1.0.0/rift-linux-x86_64.tar.gz.sha256

# Verify checksum
sha256sum -c rift-linux-x86_64.tar.gz.sha256

# Extract and run
tar xzf rift-linux-x86_64.tar.gz
cd rift-dist
./bin/rift-server
```

---

## Environment Variables

For local testing or custom workflows:

```bash
# Server configuration
export RIFT_TUNNEL_PORT=7000
export RIFT_PUBLIC_PORT=9000
export RIFT_HEALTH_PORT=8080

# Test configuration
export TEST_TIMEOUT=10        # seconds per test
export TEST_VERBOSE=1         # enable debug output
```

---

## Logs and Diagnostics

### Accessing Workflow Logs

**From GitHub:**
1. Actions → Workflow run
2. Click "Build and Test" job
3. Expand any step to see logs

**Common Log Files:**
- Server: `/tmp/rift_server.log`
- Client: `/tmp/rift_client.log`
- HTTP backend: `/tmp/http_server.log`
- Test output: `/tmp/test_results.txt`

### Debug Mode

To add debug output to workflow:

```yaml
- name: Run test suite (debug)
  env:
    DEBUG: 1
  run: |
    python3 -u tests/test_tunnel.py -v
```

---

## References

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Test Suite](../tests/test_tunnel.py)
- [Main Test Configuration](test.yml)
- [Release Configuration](release.yml)
