#!/bin/bash
set -e

# Variables
PEM=~/Desktop/dev.pem
USER=ubuntu
HOST=ec2-43-205-120-186.ap-south-1.compute.amazonaws.com
REMOTE_BIN=/opt/rift-server-dist/bin/rift-server
LOCAL_BIN=rift-server
TMP_BIN=/tmp/rift-server

# 1. Build locally (optional, comment if not needed)
echo "[+] Building rift-server locally..."
make clean && make

# 2. Copy binary to EC2
echo "[+] Copying binary to EC2..."
scp -i "$PEM" "$LOCAL_BIN" "$USER@$HOST:$TMP_BIN"

# 3. SSH and deploy on EC2
echo "[+] Deploying on EC2..."
ssh -i "$PEM" "$USER@$HOST" <<EOF
  set -e
  sudo systemctl stop rift-server || true
  sudo cp $TMP_BIN $REMOTE_BIN
  sudo chmod +x $REMOTE_BIN
  sudo systemctl start rift-server
  echo '[+] rift-server restarted.'
EOF

echo "[+] Deployment complete."
