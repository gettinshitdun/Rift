#!/usr/bin/env python3
"""
Keep-Alive browser test - Registers tunnel then makes multiple requests
"""

import socket
import time
import threading
import subprocess

def register_tunnel(tunnel_id):
    """Register a tunnel by connecting to server port 7000"""
    time.sleep(0.5)  # Let server start
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 7000))
        
        # Send RIFT frame: magic + version + reserved + type + length + payload
        magic = b'RIFT'
        version = (1).to_bytes(1, 'big')
        reserved = (0).to_bytes(1, 'big')
        frame_type = (1).to_bytes(2, 'big')  # FRAME_REGISTER_TUNNEL
        length = len(tunnel_id.encode()).to_bytes(4, 'big')
        
        frame = magic + version + reserved + frame_type + length + tunnel_id.encode()
        sock.sendall(frame)
        
        sock.close()
        print(f"[Register] Tunnel '{tunnel_id}' registered\n")
    except Exception as e:
        print(f"[Register] Failed: {e}\n")

def keepalive_test(tunnel_id="keepalive-test", num_requests=5):
    """Simulate browser with HTTP Keep-Alive on registered tunnel"""
    print(f"=== BROWSER TEST: Keep-Alive with {num_requests} Requests ===")
    print(f"Tunnel ID: '{tunnel_id}'\n")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 9000))
        print(f"[Connection] Established to 127.0.0.1:9000\n")
        
        responses = []
        for req_num in range(1, num_requests + 1):
            http_request = (
                f"GET /request{req_num} HTTP/1.1\r\n"
                f"Host: localhost:9000\r\n"
                f"x-tunnel-id: {tunnel_id}\r\n"
                f"Connection: keep-alive\r\n"
                f"User-Agent: Browser-Test\r\n"
                f"\r\n"
            )
            
            print(f"[Request {req_num}] Sending...")
            try:
                sock.sendall(http_request.encode())
                
                # Read response
                response = b""
                sock.settimeout(2)
                while True:
                    try:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        response += chunk
                    except socket.timeout:
                        break
                
                response_str = response.decode('utf-8', errors='replace')
                lines = response_str.split('\r\n')
                status = lines[0] if lines else "No status"
                
                if "HTTP" in status:
                    print(f"[Request {req_num}] ✓ {status}")
                    responses.append(True)
                else:
                    print(f"[Request {req_num}] ✗ Unexpected: {status}")
                    responses.append(False)
                    
            except Exception as e:
                print(f"[Request {req_num}] ✗ Error: {e}")
                responses.append(False)
                if "Broken pipe" in str(e) or "Connection reset" in str(e):
                    break
            
            time.sleep(0.1)
        
        sock.close()
        print(f"\n[Connection] Closed\n")
        
        successful = sum(responses)
        print(f"{'='*60}")
        print(f"RESULTS: {successful}/{num_requests} requests successful")
        if successful == num_requests:
            print("✓ Keep-Alive test PASSED")
            return True
        else:
            print(f"✗ {num_requests - successful} requests failed")
            return False
        
    except Exception as e:
        print(f"[Error] {e}\n")
        return False

if __name__ == "__main__":
    import sys
    tunnel_id = sys.argv[1] if len(sys.argv) > 1 else "keepalive-test"
    num_requests = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    
    print("Note: Ensure rift-server and rift-client are running\n")
    
    # Register tunnel in background
    register_thread = threading.Thread(target=register_tunnel, args=(tunnel_id,), daemon=True)
    register_thread.start()
    
    time.sleep(1)
    success = keepalive_test(tunnel_id, num_requests)
    sys.exit(0 if success else 1)
