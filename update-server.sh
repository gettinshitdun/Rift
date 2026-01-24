#!/bin/bash
# Simple server deployment script
# Run this directly on the server

# Clone if not exists
if [ ! -d "/home/ubuntu/rift" ]; then
  cd /home/ubuntu
  git clone https://github.com/gettinshitdun/Rift.git rift
fi

# Install build tools if needed
if ! command -v gcc &> /dev/null; then
  echo "Installing build tools..."
  sudo apt-get update -qq
  sudo apt-get install -y -qq build-essential git
fi

cd /home/ubuntu/rift
git fetch origin
git checkout v1.0.0
make clean && make
sudo cp /home/ubuntu/rift/rift-server /usr/local/bin/rift-server
sudo systemctl restart rift-server
sleep 2
sudo systemctl status rift-server --no-pager | head -5
echo ""
echo "Health check:"
curl -s http://localhost:8080/health && echo "" || echo "Health check failed"
