# AIPC Media Architecture Summary (Snapshot)

## Overview
The AIPC project uses a Producer-Distribution architecture for RV1106.

## Core Modules
- media_producer: capture + encode + mode switching
- media_distribution: RTSP/WebRTC/WebSocket/File outputs
- http api: status/switch/deploy control plane

## Producer Modes
- SimpleIPC: pure monitoring mode, high performance
- VisionG: python-driven AI pipeline mode

## Key Endpoints
- /api/status
- /api/ai/switch
- /api/python/deploy
- /api/python/status

## Main Flow
1. init logger and services
2. init MediaManager in startup mode
3. register stream consumers
4. run io event loop

## Notes
- cold switch destroys old producer and creates new producer
- output services are controlled by StreamManager
