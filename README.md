# RIFT: Self-Hosted Tunnel Server

A lightweight tunneling server that exposes local services to the internet.

## Usage

### 1. Clone and Build
```bash
git clone https://github.com/gettinshitdun/Rift.git
cd Rift
make clean && make
```

### 2. Expose Local Service
```bash
./rift-client expose 3000
# Your local port 3000 is now accessible via:
# https://your-tunnel-id.rift.kanishakmittal.site
```

That's it! Your local service is now accessible from anywhere on the internet.
