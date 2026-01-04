from test_utils import start_server, stop_server, tcp_connect
import threading
import time
import random

NUM_CONNECTIONS = 500  # total tunnel/public pairs
BATCH_SIZE = 50        # spawn threads in batches
DATA = b"ping\n"

success_count = 0
lock = threading.Lock()

def connection_pair(i):
    """Open tunnel/public pair, send data, verify forwarding."""
    global success_count
    try:
        time.sleep(random.uniform(0, 0.01))  # small stagger
        tunnel = tcp_connect(7000)
        public = tcp_connect(9000)
        time.sleep(0.05)  # wait for server pairing

        # Send from tunnel
        tunnel.sendall(DATA)

        # Receive on public
        public.settimeout(2.0)
        try:
            recv = public.recv(1024)
            if recv == DATA:
                with lock:
                    success_count += 1
            else:
                print(f"[pair {i}] unexpected data: {recv}")
        except Exception as e:
            print(f"[pair {i}] receive timeout/error: {e}")

        tunnel.close()
        public.close()
    except Exception as e:
        print(f"[pair {i}] connection error: {e}")

def main():
    server = start_server()
    threads = []
    start_time = time.time()

    for batch_start in range(0, NUM_CONNECTIONS, BATCH_SIZE):
        batch_end = min(batch_start + BATCH_SIZE, NUM_CONNECTIONS)
        for i in range(batch_start, batch_end):
            t = threading.Thread(target=connection_pair, args=(i,))
            threads.append(t)
            t.start()
        # Optional: slight delay between batches
        time.sleep(0.05)

    for t in threads:
        t.join()

    duration = time.time() - start_time
    print(f"\n✅ Load test completed")
    print(f"Connections attempted: {NUM_CONNECTIONS}")
    print(f"Successful forwards : {success_count}")
    print(f"Total time          : {duration:.2f}s")
    print(f"Throughput          : {success_count/duration:.2f} messages/sec")

    stop_server(server)

if __name__ == "__main__":
    main()
