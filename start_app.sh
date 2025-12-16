#!/bin/sh

# Get the directory where the script is located
SCRIPT_DIR=$(cd $(dirname $0); pwd)

# Add the script directory to LD_LIBRARY_PATH so the app can find libdatachannel.so
export LD_LIBRARY_PATH=$SCRIPT_DIR:$LD_LIBRARY_PATH

echo "------------------------------------------------"
echo "Starting AIPC application (Embedded HTTP Server)..."
echo "Please open your browser and visit:"
echo "   http://<Device_IP_Address>"
echo "------------------------------------------------"

# Start the AIPC application
cd $SCRIPT_DIR
./aipc
