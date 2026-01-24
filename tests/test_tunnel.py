#!/usr/bin/env python3
"""
Comprehensive test suite for RIFT tunnel server.
Tests tunnel registration, HTTP forwarding, and multiple concurrent requests.
"""

import socket
import struct
import threading
import time
import subprocess
import os
import signal

FRAME_MAGIC = 0x52494654
FRAME_VERSION = 1
FRAME_REGISTER_TUNNEL = 1
FRAME_CONNECT_REQUEST = 2
FRAME_DATA = 3
FRAME_CLOSE = 5
FRAME_TUNNEL_READY = 7

TUNNEL_PORT = 7000
PUBLIC_PORT = 9000

def pack_frame(frame_type, payload=b""):
    """Pack a frame: magic(4) + version(1) + reserved(1) + type(2) + length(4) + payload"""
    magic = struct.pack("!I", FRAME_MAGIC)
    version = struct.pack("B", FRAME_VERSION)
    reserved = struct.pack("B", 0)
    ftype = struct.pack("!H", frame_type)
    length = struct.pack("!I", len(payload))
    return magic + version + reserved + ftype + length + payload

def unpack_frame_header(data):
    """Unpack frame header (12 bytes)"""
    if len(data) < 12:
        return None
    magic, version, reserved, ftype, length = struct.unpack("!IBBBHI", data[:12])
    if magic != FRAME_MAGIC or version != FRAME_VERSION:
        return None
    return ftype, length

class MockTunnelClient:
    """Simulates a rift-client connecting to the tunnel port"""
    
    def __init__(self, tunnel_id):
        self.tunnel_id = tunnel_id
        self.sock = None
        self.running = False
    
    def connect(self):
        """Connect to tunnel port and register"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(("127.0.0.1", TUNNEL_PORT))
        
        frame = pack_frame(FRAME_REGISTER_TUNNEL, self.tunnel_id.encode())
        self.sock.sendall(frame)
        print(f"  [Tunnel] Registered: {self.tunnel_id}")
    
    def wait_for_request(self, timeout=5):
        """Wait for incoming frames until we get FRAME_DATA"""
        self.sock.settimeout(timeout)
        try:
            while True:
                header = self.sock.recv(12)
                if len(header) < 12:
                    return None
                
                magic, version, reserved, ftype, length = struct.unpack("!IBBHI", header)
                
                if magic != FRAME_MAGIC or version != FRAME_VERSION:
                    print(f"    [DEBUG] Bad frame: magic=0x{magic:08x}, version={version}")
                    return None
                
                payload = b""
                while len(payload) < length:
                    chunk = self.sock.recv(length - len(payload))
                    if not chunk:
                        break
                    payload += chunk
                
                if ftype == FRAME_DATA:
                    return payload.decode('utf-8', errors='replace')
                elif ftype == FRAME_CONNECT_REQUEST:
                    continue
                elif ftype == FRAME_TUNNEL_READY:
                    # Skip tunnel ready response and continue waiting for data
                    continue
                else:
                    print(f"    [DEBUG] Unexpected frame type: {ftype}")
        except socket.timeout:
            return None
    
    def send_response(self, data):
        """Send response back through the tunnel"""
        frame = pack_frame(FRAME_DATA, data.encode())
        self.sock.sendall(frame)
    
    def close(self):
        """Close tunnel connection"""
        if self.sock:
            self.sock.close()

class MockPublicClient:
    """Simulates a browser/Postman connecting to the public port"""
    
    def __init__(self, tunnel_id):
        self.tunnel_id = tunnel_id
        self.sock = None
    
    def connect_and_request(self, http_request=None):
        """Connect to public port and send HTTP request"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(("127.0.0.1", PUBLIC_PORT))
        
        if http_request is None:
            http_request = f"GET / HTTP/1.1\r\nx-tunnel-id: {self.tunnel_id}\r\nConnection: close\r\n\r\n"
        
        self.sock.sendall(http_request.encode())
        print(f"  [Public] Sent request to {self.tunnel_id}")
    
    def get_response(self, timeout=5):
        """Read response from server"""
        self.sock.settimeout(timeout)
        response = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                response += chunk
        except socket.timeout:
            pass
        return response.decode('utf-8', errors='replace')
    
    def close(self):
        """Close public connection"""
        if self.sock:
            self.sock.close()

def test_basic_tunnel_registration():
    """Test 1: Basic tunnel registration"""
    print("\n[TEST 1] Basic Tunnel Registration")
    tunnel = MockTunnelClient("test-tunnel-1")
    try:
        tunnel.connect()
        print("  ✓ Tunnel registered successfully")
        return True
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel.close()

def test_single_request():
    """Test 2: Single HTTP request through tunnel"""
    print("\n[TEST 2] Single HTTP Request")
    tunnel = MockTunnelClient("test-tunnel-2")
    public = MockPublicClient("test-tunnel-2")
    
    try:
        tunnel.connect()
        time.sleep(0.2)
        
        public.connect_and_request()
        time.sleep(0.2)
        
        http_request = tunnel.wait_for_request()
        if http_request and "GET" in http_request:
            print(f"  [Tunnel] Received HTTP request")
            tunnel.send_response("HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!")
            
            response = public.get_response()
            if "Hello, World!" in response:
                print("  ✓ Request/response successful")
                return True
            else:
                print(f"  ✗ Response missing expected content: {response[:100]}")
                return False
        else:
            print(f"  ✗ Tunnel did not receive HTTP request: {http_request}")
            return False
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel.close()
        public.close()

def test_multiple_sequential_requests():
    """Test 3: Multiple sequential requests from different public clients"""
    print("\n[TEST 3] Multiple Sequential Requests")
    tunnel = MockTunnelClient("test-tunnel-3")
    
    try:
        tunnel.connect()
        
        for i in range(3):
            print(f"  [Request {i+1}]")
            public = MockPublicClient("test-tunnel-3")
            
            public.connect_and_request()
            time.sleep(0.1)
            
            http_request = tunnel.wait_for_request(timeout=2)
            if http_request and "GET" in http_request:
                print(f"    Received HTTP request #{i+1}")
                tunnel.send_response(f"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nResp{i}")
                
                response = public.get_response()
                if f"Resp{i}" in response:
                    print(f"    ✓ Request #{i+1} successful")
                else:
                    print(f"    ✗ Request #{i+1} failed")
                    return False
            else:
                print(f"    ✗ Did not receive request #{i+1}")
                return False
            
            public.close()
            time.sleep(0.1)
        
        print("  ✓ All sequential requests successful")
        return True
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel.close()

def test_concurrent_requests():
    """Test 4: Handle concurrent requests (multiple clients at once)"""
    print("\n[TEST 4] Concurrent Requests")
    
    tunnel1 = MockTunnelClient("test-tunnel-4a")
    tunnel2 = MockTunnelClient("test-tunnel-4b")
    
    try:
        tunnel1.connect()
        tunnel2.connect()
        time.sleep(0.2)
        
        public1 = MockPublicClient("test-tunnel-4a")
        public2 = MockPublicClient("test-tunnel-4b")
        
        public1.connect_and_request()
        public2.connect_and_request()
        time.sleep(0.1)
        
        req1 = tunnel1.wait_for_request(timeout=2)
        req2 = tunnel2.wait_for_request(timeout=2)
        
        if req1 and "GET" in req1 and req2 and "GET" in req2:
            print("  [Tunnel1] Received request")
            print("  [Tunnel2] Received request")
            
            tunnel1.send_response("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nResp1a")
            tunnel2.send_response("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nResp2b")
            
            resp1 = public1.get_response()
            resp2 = public2.get_response()
            
            if "Resp1a" in resp1 and "Resp2b" in resp2:
                print("  ✓ Concurrent requests successful")
                return True
            else:
                print("  ✗ Responses incorrect")
                return False
        else:
            print("  ✗ Did not receive both requests")
            return False
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel1.close()
        tunnel2.close()
        public1.close()
        public2.close()

def test_request_cancellation():
    """Test 5: Cancel request mid-flight and send another"""
    print("\n[TEST 5] Request Cancellation and Recovery")
    tunnel = MockTunnelClient("test-tunnel-5")
    public1 = None
    public2 = None
    
    try:
        tunnel.connect()
        time.sleep(0.2)
        
        public1 = MockPublicClient("test-tunnel-5")
        public1.connect_and_request()
        time.sleep(0.1)
        
        req1 = tunnel.wait_for_request(timeout=1)
        if req1 and "GET" in req1:
            print("  [Request 1] Received")
            public1.close()
            print("  [Request 1] Cancelled (connection closed)")
        else:
            print("  ✗ Did not receive request 1")
            return False
        
        time.sleep(0.2)
        
        public2 = MockPublicClient("test-tunnel-5")
        public2.connect_and_request()
        time.sleep(0.1)
        
        req2 = tunnel.wait_for_request(timeout=2)
        if req2 and "GET" in req2:
            print("  [Request 2] Received after cancellation")
            tunnel.send_response("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nRecovered")
            
            resp2 = public2.get_response()
            if "Recovered" in resp2:
                print("  ✓ Recovered from cancellation")
                return True
            else:
                print("  ✗ Response not received after recovery")
                return False
        else:
            print("  ✗ Did not receive request 2 after cancellation")
            return False
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel.close()
        if public1:
            public1.close()
        if public2:
            public2.close()

def test_custom_header():
    """Test 6: Tunnel identification via custom header"""
    print("\n[TEST 6] Custom x-tunnel-id Header")
    tunnel = MockTunnelClient("test-tunnel-6")
    public = MockPublicClient("test-tunnel-6")
    
    try:
        tunnel.connect()
        time.sleep(0.2)
        
        custom_request = "GET /path HTTP/1.1\r\nx-tunnel-id: test-tunnel-6\r\nConnection: close\r\n\r\n"
        public.connect_and_request(custom_request)
        time.sleep(0.2)
        
        http_request = tunnel.wait_for_request()
        if http_request and "GET" in http_request:
            print("  ✓ Custom header routing successful")
            tunnel.send_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK")
            return True
        else:
            print("  ✗ Custom header routing failed")
            return False
    except Exception as e:
        print(f"  ✗ Failed: {e}")
        return False
    finally:
        tunnel.close()
        public.close()

def run_all_tests():
    """Run all tests"""
    print("=" * 60)
    print("RIFT TUNNEL SERVER TEST SUITE")
    print("=" * 60)
    
    tests = [
        test_basic_tunnel_registration,
        test_single_request,
        test_multiple_sequential_requests,
        test_concurrent_requests,
        test_request_cancellation,
        test_custom_header,
    ]
    
    results = []
    for test in tests:
        try:
            results.append(test())
        except Exception as e:
            print(f"  ✗ Test crashed: {e}")
            results.append(False)
        time.sleep(0.3)
    
    print("\n" + "=" * 60)
    print(f"RESULTS: {sum(results)}/{len(results)} tests passed")
    print("=" * 60)
    
    return all(results)

if __name__ == "__main__":
    print("\nNote: Ensure rift-server is running on ports 7000/9000")
    print("Start server with: ./rift-server\n")
    time.sleep(1)
    
    success = run_all_tests()
    exit(0 if success else 1)
