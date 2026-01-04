import subprocess
import time
import socket
import os
import signal

SERVER_BIN = "./rift-server"

def start_server():
    proc = subprocess.Popen(
        [SERVER_BIN],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(0.5)
    return proc

def stop_server(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()

def tcp_recv(port):
    import socket
    s = socket.socket()
    s.connect(("127.0.0.1", port))
    s.settimeout(2)
    data = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data.decode()


def tcp_send(port, msg):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", port))
    s.sendall(msg.encode())
    return s  # caller controls close

def tcp_connect(port, host="127.0.0.1", timeout=1):
    """
    Connect to a TCP server on host:port and return the socket.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    return s