import socket
import struct
import threading
import time

# Protocol Configuration
MAGIC = 0x52494654
TYPE_REGISTER = 1
TYPE_CONNECT = 2
TUNNEL_ID = b"test-service"
HEADER_FORMAT = "!IHH"  # Magic (4), Type (2), Length (2)

def create_frame(frame_type, payload):
    """Encapsulates data into the RIFT frame format"""
    header = struct.pack(HEADER_FORMAT, MAGIC, frame_type, len(payload))
    return header + payload

def tunnel_backend_worker():
    """Acts as the tunnel client (backend service)"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", 7000))
            
            # Step 1: Register the tunnel
            reg_frame = create_frame(TYPE_REGISTER, TUNNEL_ID)
            s.sendall(reg_frame)
            print(f"--- [Tunnel] Sent Registration for: {TUNNEL_ID.decode()}")

            # Step 2: Wait for data from the public side via the server
            while True:
                data = s.recv(4096)
                if not data:
                    print("--- [Tunnel] Connection closed by server.")
                    break
                
                print(f"--- [Tunnel] Received payload: {data.decode(errors='replace')}")
                
                # Step 3: Echo data back to the public client
                response = b"ACK from Backend: " + data
                s.sendall(response)
    except Exception as e:
        print(f"--- [Tunnel] Error: {e}")

def public_client_worker():
    """Acts as the public user connecting to port 9000"""
    time.sleep(1) # Give the tunnel a second to register
    
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", 9000))
            
            # Step 1: Send Connect Request Frame
            conn_frame = create_frame(TYPE_CONNECT, TUNNEL_ID)
            s.sendall(conn_frame)
            print(f"+++ [Public] Requested connection to: {TUNNEL_ID.decode()}")

            # Step 2: Send raw data to be forwarded
            time.sleep(0.5)
            message = b"Ping through Rift!"
            print(f"+++ [Public] Sending data: {message.decode()}")
            s.sendall(message)

            # Step 3: Receive echoed data
            reply = s.recv(4096)
            print(f"+++ [Public] Received reply: {reply.decode(errors='replace')}")
            
    except Exception as e:
        print(f"+++ [Public] Error: {e}")

if __name__ == "__main__":
    print("Starting Integrated Rift Test...")
    
    # Run the tunnel in a background thread
    tunnel_thread = threading.Thread(target=tunnel_backend_worker, daemon=True)
    tunnel_thread.start()

    # Run the public client in the main thread
    public_client_worker()
    
    print("\nTest Sequence Finished.")