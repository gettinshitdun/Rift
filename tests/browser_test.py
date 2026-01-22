#!/usr/bin/env python3
"""
Browser simulation test - Multiple requests from same tunnel
"""

import socket
import time

def browser_test(tunnel_id="browser-test"):
    """Simulate browser making multiple sequential requests"""
    print(f"\n=== BROWSER TEST: Multiple Requests to '{tunnel_id}' ===\n")
    
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
            
            if "200 OK" in response_str or len(response_str) > 0:
                print(f"[Request {req_num}] ✓ Received response ({len(response)} bytes)")
                print(f"  First 100 chars: {response_str[:100]}\n")
            else:
                print(f"[Request {req_num}] ✗ No response\n")
            
            sock.close()
            time.sleep(0.5)
        except Exception as e:
            print(f"[Request {req_num}] ✗ Error: {e}\n")

if __name__ == "__main__":
    import sys
    tunnel_id = sys.argv[1] if len(sys.argv) > 1 else "browser-test"
    print("Note: Start rift-client first with: ./rift-client expose 3000")
    print("Then start browser test\n")
    time.sleep(1)
    browser_test(tunnel_id)
