#!/usr/bin/env python3
"""Self-contained E2E test: starts server+client, tests Portainer through tunnel."""
import subprocess, time, re, sys, os, signal, urllib.request, urllib.error

os.chdir('/home/kanishak/Desktop/rift')

LOCAL_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 3001

# Kill any lingering processes
for name in ['rift-server', 'rift-client']:
    subprocess.run(['pkill', '-f', name], capture_output=True)
time.sleep(0.5)

procs = []
def cleanup():
    for p in procs:
        try: p.kill(); p.wait(timeout=2)
        except: pass

# 1. Start server (log to file for diagnosis)
print("Starting rift-server...")
server_log = open('/tmp/rift-server.log', 'w')
server = subprocess.Popen(['./rift-server'], stdout=server_log, stderr=subprocess.STDOUT)
procs.append(server)
time.sleep(1)

# 2. Start client
print(f"Starting rift-client (tunneling to localhost:{LOCAL_PORT})...")
env = os.environ.copy()
env['RIFT_SERVER_IP'] = '127.0.0.1'
client = subprocess.Popen(
    ['stdbuf', '-oL', './rift-client', '--verbose', 'expose', str(LOCAL_PORT)],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True
)
procs.append(client)

# Extract tunnel ID
tunnel_id = None
for _ in range(30):
    line = client.stdout.readline()
    if not line: break
    print(f"  {line.rstrip()}")
    m = re.search(r'Tunnel ID:\s+(\S+)', line)
    if m: tunnel_id = m.group(1)
    if 'Event loop started' in line: break

if not tunnel_id:
    print("ERROR: Could not extract tunnel ID")
    cleanup()
    sys.exit(1)

print(f"\n{'='*60}")
print(f"  Tunnel: {tunnel_id}  ->  localhost:{LOCAL_PORT}")
print(f"{'='*60}\n")
time.sleep(0.5)

BASE = "http://localhost:9000"
HEADERS = {"X-Tunnel-Id": tunnel_id}

def do_request(name, path, timeout=15):
    url = BASE + path
    try:
        req = urllib.request.Request(url, headers=HEADERS)
        resp = urllib.request.urlopen(req, timeout=timeout)
        body = resp.read()
        print(f"  [PASS] {name}: HTTP {resp.status} | {len(body):,} bytes")
        return True
    except urllib.error.HTTPError as e:
        body = e.read()
        print(f"  [PASS] {name}: HTTP {e.code} | {len(body):,} bytes (app error, tunnel OK)")
        return True
    except Exception as e:
        print(f"  [FAIL] {name}: {e}")
        return False

# --- Test Suite ---
passed = 0
failed = 0

print("--- Test 1: Portainer Index Page ---")
if do_request("Index", "/"): passed += 1
else: failed += 1
time.sleep(1)

print("\n--- Test 2: Portainer API ---")
if do_request("API status", "/api/status"): passed += 1
else: failed += 1
time.sleep(1)

if do_request("API settings", "/api/settings/public"): passed += 1
else: failed += 1
time.sleep(1)

print("\n--- Test 3: Sequential Rapid Requests (15x) ---")
rapid_ok = 0
for i in range(15):
    try:
        req = urllib.request.Request(BASE + "/api/status", headers=HEADERS)
        resp = urllib.request.urlopen(req, timeout=10)
        body = resp.read()
        rapid_ok += 1
    except urllib.error.HTTPError:
        rapid_ok += 1  # app-level error = tunnel still works
    except Exception as e:
        print(f"    Request {i+1}/15 failed: {e}")
    time.sleep(0.5)

print(f"  {rapid_ok}/15 requests completed through tunnel")
if rapid_ok >= 13: passed += 1
else: failed += 1

print(f"\n--- Test 4: Parallel Requests (simulating browser) ---")
import concurrent.futures, threading
para_ok = 0
para_lock = threading.Lock()
PARALLEL_COUNT = 10

def parallel_req(idx):
    global para_ok
    try:
        req = urllib.request.Request(BASE + f"/api/status?r={idx}", headers=HEADERS)
        resp = urllib.request.urlopen(req, timeout=20)
        body = resp.read()
        with para_lock: para_ok += 1
    except urllib.error.HTTPError:
        with para_lock: para_ok += 1
    except Exception as e:
        print(f"    Parallel #{idx} failed: {e}")

with concurrent.futures.ThreadPoolExecutor(max_workers=PARALLEL_COUNT) as pool:
    futures = [pool.submit(parallel_req, i) for i in range(PARALLEL_COUNT)]
    concurrent.futures.wait(futures, timeout=60)

print(f"  {para_ok}/{PARALLEL_COUNT} parallel requests completed through tunnel")
if para_ok >= PARALLEL_COUNT - 1: passed += 1
else: failed += 1

# Summary
print(f"\n{'='*60}")
print(f"  RESULTS: {passed} passed, {failed} failed")
print(f"{'='*60}")

cleanup()
server_log.close()
print("\n--- Server log (last 60 lines) ---")
with open('/tmp/rift-server.log') as f:
    lines = f.readlines()
    for line in lines[-60:]:
        print(f"  {line.rstrip()}")
print("\nAll processes stopped. Done.")
