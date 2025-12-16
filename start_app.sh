#!/bin/sh

# Get the directory where the script is located
SCRIPT_DIR=$(cd $(dirname $0); pwd)

# Add the script directory to LD_LIBRARY_PATH so the app can find libdatachannel.so
export LD_LIBRARY_PATH=$SCRIPT_DIR:$LD_LIBRARY_PATH

echo "=================================================="
echo "Starting AIPC Application (WebRTC Streaming)"
echo "=================================================="
echo ""
echo "This application starts TWO services:"
echo ""
echo "1. HTTP Web Server (for viewing the web interface)"
echo "   Access at: http://<Device_IP>:80"
echo "   (or http://<Device_IP>:8080 if port 80 is unavailable)"
echo ""
echo "2. WebSocket Signaling Server (for WebRTC)"
echo "   Automatically used by the web interface"
echo "   Endpoint: ws://<Device_IP>:8000"
echo ""
echo "IMPORTANT: Do NOT access port 8000 directly in your browser!"
echo "           Always access the web interface at port 80 (or 8080)."
echo ""
echo "=================================================="
echo ""

# Start the AIPC application
cd $SCRIPT_DIR
./aipc
