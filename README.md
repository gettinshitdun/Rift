# RIFT: Self-Hosted Tunnel Server

A lightweight tunneling server that exposes local services to the internet.

## Usage

### Option 1: Download Executable (Quick Start)
```bash
# Download latest release
wget https://github.com/gettinshitdun/Rift/releases/latest/download/rift-client
chmod +x rift-client
```

### Option 2: Build from Source
```bash
git clone https://github.com/gettinshitdun/Rift.git
cd Rift
make clean && make
```

### Expose Local Service
```bash
./rift-client expose 3000
# Your local port 3000 is now accessible via:
# https://your-tunnel-id.rift.kanishakmittal.site
```

That's it! Your local service is now accessible from anywhere on the internet.
