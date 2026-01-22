#!/usr/bin/env python3
"""
Advanced browser simulation test - Keep-Alive with multiple requests on same connection
"""

import socket
import time
import sys

def keepalive_test(tunnel_id="keepalive-test", num_requests=5):
    """Simulate browser with HTTP Keep-Alive making multiple requests on same connection"""
    print(f"\n=== ADVANCED BROWSER TEST: Keep-Alive with {num_requests} Requests ===")
    print(f"Tunnel ID: '{tunnel_id}'\n")
    
    try:
        # Single persistent connection
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 9000))
        print(f"[Connection] Established to 127.0.0.1:9000\n")
        
        responses = []
        for req_num in range(1, num_requests + 1):
            # Keep-Alive: Connection header stays open
            http_request = (
                f"GET /request{req_num} HTTP/1.1\r\n"
                f"Host: localhost:9000\r\n"
                f"x-tunnel-id: {tunnel_id}\r\n"
                f"Connection: keep-alive\r\n"
                f"User-Agent: Advanced-Test-Browser\r\n"
                f"Accept: */*\r\n"
                f"\r\n"
            )
            
            print(f"[Request {req_num}] Sending on persistent connection...")
            try:
                sock.sendall(http_request.encode())
                
                # Read response headers first
                response = b""
                sock.settimeout(3)
                headers_complete = False
                headers = ""
                
                while not headers_complete:
                    chunk = sock.recv(1024)
                    if not chunk:
                        break
                    response += chunk
                    response_str = response.decode('utf-8', errors='replace')
                    if "\r\n\r\n" in response_str:
                        headers_complete = True
                
                response_str = response.decode('utf-8', errors='replace')
                lines = response_str.split('\r\n')
                status = lines[0] if lines else "No status"
                
                if "200" in status or "404" in status or "HTTP" in status:
                    print(f"[Request {req_num}] ✓ Status: {status}")
                    print(f"  Response bytes: {len(response)}")
                    responses.append(True)
                else:
                    print(f"[Request {req_num}] ✗ Unexpected response: {status}")
                    responses.append(False)
                    
            except socket.timeout:
                print(f"[Request {req_num}] ✗ Timeout waiting for response")
                responses.append(False)
                break
            except Exception as e:
                print(f"[Request {req_num}] ✗ Error: {e}")
                responses.append(False)
                break
            
            print()
            time.sleep(0.2)  # Small delay between requests
        
        sock.close()
        print(f"[Connection] Closed\n")
        
        # Summary
        successful = sum(responses)
        print(f"{'='*60}")
        print(f"RESULTS: {successful}/{num_requests} requests successful")
        if successful == num_requests:
            print("✓ Keep-Alive persistent connection TEST PASSED")
        else:
            print(f"✗ Failed {num_requests - successful} requests")
        print(f"{'='*60}\n")
        
        return successful == num_requests
        
    except Exception as e:
        print(f"[Connection] ✗ Failed to connect: {e}\n")
        return False

if __name__ == "__main__":
    tunnel_id = sys.argv[1] if len(sys.argv) > 1 else "keepalive-test"
    num_requests = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    
    print("Note: Ensure rift-server and rift-client are running")
    print("Start with: ./rift-server &")
    print("           ./rift-client expose 3000 &\n")
    time.sleep(1)
    
    success = keepalive_test(tunnel_id, num_requests)
    sys.exit(0 if success else 1)
