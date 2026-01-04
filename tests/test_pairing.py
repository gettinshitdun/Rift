from test_utils import start_server, stop_server, tcp_connect
import time

def main():
    server = start_server()

    # Connect tunnel and public clients
    tunnel_c = tcp_connect(7000)
    public_c = tcp_connect(9000)

    # Give server some time to pair
    time.sleep(0.2)

    # Send data from tunnel
    tunnel_c.sendall(b"hello-from-tunnel\n")

    # Give server time to forward
    time.sleep(0.2)

    # Receive on public
    data = public_c.recv(1000).decode()
    print(data)
    assert "hello-from-tunnel" in data

    print("✅ pairing test passed")

    # Close connections
    tunnel_c.close()
    public_c.close()
    stop_server(server)

if __name__ == "__main__":
    main()
