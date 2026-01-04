from test_utils import start_server, stop_server, tcp_recv

def main():
    server = start_server()

    data = tcp_recv(8080)

    # Split headers from body
    if "\r\n\r\n" in data:
        _, body = data.split("\r\n\r\n", 1)
    elif "\n\n" in data:
        _, body = data.split("\n\n", 1)
    else:
        body = data  # fallback

    print(body)
    assert "status: OK" in body
    assert "active_connections" in body
    assert "total_connections" in body

    print("✅ health test passed")
    stop_server(server)

if __name__ == "__main__":
    main()
