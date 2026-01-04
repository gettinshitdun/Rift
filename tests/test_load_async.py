# tests/test_load_async.py
import asyncio
import random
from test_utils import start_server, stop_server

NUM_CONNECTIONS = 500
DATA = b"ping\n"
MAX_DELAY = 0.5  # small stagger for connections

success_count = 0
lock = asyncio.Lock()

async def connection_pair(i):
    global success_count
    await asyncio.sleep(random.uniform(0, MAX_DELAY))
    try:
        reader_t, writer_t = await asyncio.open_connection("127.0.0.1", 7000)
        reader_p, writer_p = await asyncio.open_connection("127.0.0.1", 9000)

        # send data
        writer_t.write(DATA)
        await writer_t.drain()

        # receive
        try:
            recv = await asyncio.wait_for(reader_p.read(len(DATA)), timeout=5)
            if recv == DATA:
                async with lock:
                    success_count += 1
        except asyncio.TimeoutError:
            print(f"[pair {i}] timed out")

        writer_t.close()
        writer_p.close()
        await writer_t.wait_closed()
        await writer_p.wait_closed()
    except Exception as e:
        print(f"[pair {i}] error: {e}")

async def main_async():
    server = start_server()
    tasks = [connection_pair(i) for i in range(NUM_CONNECTIONS)]
    await asyncio.gather(*tasks)
    stop_server(server)
    print(f"\n✅ Async load test completed")
    print(f"Connections attempted: {NUM_CONNECTIONS}")
    print(f"Successful forwards : {success_count}")

def main():
    asyncio.run(main_async())

if __name__ == "__main__":
    main()
