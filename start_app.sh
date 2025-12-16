#!/bin/sh

# Get the directory where the script is located
SCRIPT_DIR=$(cd $(dirname $0); pwd)
WWW_DIR=$SCRIPT_DIR/www

echo "Starting Web Server..."

# Try to find a web server
if command -v httpd >/dev/null 2>&1; then
    echo "Using system httpd"
    httpd -p 80 -h $WWW_DIR
elif command -v busybox >/dev/null 2>&1; then
    echo "Using busybox httpd"
    busybox httpd -p 80 -h $WWW_DIR
else
    echo "Error: No HTTP server found (httpd or busybox). Web interface will not be available."
fi

echo "------------------------------------------------"
echo "Please open your browser and visit:"
echo "   http://<Device_IP_Address>"
echo "   (Do NOT use port 8000, that is for WebSocket only)"
echo "------------------------------------------------"

# Start the AIPC application
echo "Starting AIPC application..."
cd $SCRIPT_DIR
./aipc
