#!/usr/bin/env python3
"""
Browser simulation test - Multiple requests from same tunnel
"""

import socket
import time
import subprocess
import re
import os
import signal

def extract_tunnel_id(output):
    """Extract tunnel ID from rift-client output"""
    match = re.search(r'Tunnel ID:\s+(\S+)', output)
    return match.group(1) if match else None

def start_client_and_get_tunnel_id(timeout=5):
    """Start rift-client and extract its tunnel ID"""
    try:
        # Start the client with RIFT_SERVER_IP set to localhost for testing
        env = os.environ.copy()
        env['RIFT_SERVER_IP'] = '127.0.0.1'
        
        proc = subprocess.Popen(
            ['./rift-client', 'expose', '3000'],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env
        )
        
        # Read output until we get the tunnel ID
        tunnel_id = None
        start_time = time.time()
        
        for line in proc.stdout:
            print(f"[Client] {line.rstrip()}")
            if tunnel_id is None:
                tunnel_id = extract_tunnel_id(line)
            
            if tunnel_id and (time.time() - start_time) > 2:
                print(f"✓ Client started with tunnel ID: {tunnel_id}\n")
                return proc, tunnel_id
        
        # If we get here, process exited or timed out
        print("✗ Failed to start client or extract tunnel ID")
        return None, None
        
    except Exception as e:
        print(f"✗ Error starting client: {e}")
        return None, None

def browser_test(tunnel_id):
    """Simulate browser making multiple sequential requests"""
    print(f"=== BROWSER TEST: Multiple Requests to '{tunnel_id}' ===\n")
    
    test_passed = True
    for req_num in range(1, 4):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", 9000))
            
            http_request = (
                f"GET /request{req_num} HTTP/1.1\r\n"
                f"Host: localhost\r\n"
                f"x-tunnel-id: {tunnel_id}\r\n"
                f"Connection: close\r\n"
                f"User-Agent: Test-Browser\r\n"
                f"\r\n"
            )
            
            print(f"[Request {req_num}] Sending...")
            sock.sendall(http_request.encode())
            
            response = b""
            sock.settimeout(5)
            try:
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    response += chunk
            except socket.timeout:
                pass
            
            response_str = response.decode('utf-8', errors='replace')
            
            # Check for errors
            if "404" in response_str and "Tunnel not found" in response_str:
                print(f"[Request {req_num}] ✗ Tunnel not found error!")
                test_passed = False
            elif "200" in response_str:
                print(f"[Request {req_num}] ✓ Received 200 OK ({len(response)} bytes)")
            else:
                print(f"[Request {req_num}] ⚠ Unexpected response ({len(response)} bytes)")
                print(f"  First 100 chars: {response_str[:100]}\n")
            
            sock.close()
            time.sleep(0.5)
        except Exception as e:
            print(f"[Request {req_num}] ✗ Error: {e}\n")
            test_passed = False
    
    return test_passed

if __name__ == "__main__":
    import sys
    
    tunnel_id = sys.argv[1] if len(sys.argv) > 1 else None
    
    client_proc = None
    try:
        if not tunnel_id:
            # Auto-start client and extract tunnel ID
            print("Starting rift-client...\n")
            client_proc, tunnel_id = start_client_and_get_tunnel_id()
            
            if not tunnel_id:
                print("✗ Could not start client or extract tunnel ID")
                sys.exit(1)
        
        time.sleep(1)  # Give server time to process registration
        
        # Run the browser test
        test_passed = browser_test(tunnel_id)
        
        if test_passed:
            print("\n✓ All tests passed!")
        else:
            print("\n✗ Some tests failed")
            sys.exit(1)
    
    finally:
        if client_proc:
            try:
                os.kill(client_proc.pid, signal.SIGTERM)
                client_proc.wait(timeout=2)
            except:
                try:
                    os.kill(client_proc.pid, signal.SIGKILL)
                except:
                    pass
