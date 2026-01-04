from test_utils import start_server, stop_server, tcp_send
import time

def main():
    server = start_server()

    tunnel = tcp_send(7000, "ping\n")
    time.sleep(0.2)

    public = tcp_send(9000, "")
    time.sleep(0.2)

    tunnel.close()  # simulate Ctrl+C
    time.sleep(0.5)

    try:
        public.send(b"test")
        raise AssertionError("Public socket still alive after tunnel closed")
    except Exception:
        print("✅ disconnect propagation test passed")

    stop_server(server)

if __name__ == "__main__":
    main()
