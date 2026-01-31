/**
 * WebRTC 客户端 - 用于浏览器查看视频流
 * 
 * 信令协议：
 * - 设备端发送 offer，浏览器返回 answer
 * - ICE 候选双向交换
 * 
 * @author 好软，好温暖
 * @date 2026-01-31
 */

document.addEventListener('DOMContentLoaded', () => {
    const video = document.getElementById('remoteVideo');
    const canvas = document.getElementById('overlay');
    const ctx = canvas.getContext('2d');
    const btnConnect = document.getElementById('btnConnect');
    const btnDisconnect = document.getElementById('btnDisconnect');
    const statusEl = document.getElementById('status');
    const chkAiEnable = document.getElementById('chkAiEnable');
    const selModel = document.getElementById('selModel');
    const valFps = document.getElementById('valFps');
    const valBitrate = document.getElementById('valBitrate');
    
    let pc = null;
    let ws = null;
    let dataChannel = null;
    let deviceId = 'browser_' + Math.random().toString(36).substr(2, 9);
    let peerDeviceId = '';
    let statsInterval = null;
    let lastBytesReceived = 0;
    let lastTimestamp = 0;
    
    // Adjust canvas size to match video
    function resizeCanvas() {
        if (video.videoWidth) {
            canvas.width = video.videoWidth;
            canvas.height = video.videoHeight;
        }
    }
    video.addEventListener('loadedmetadata', resizeCanvas);
    video.addEventListener('resize', resizeCanvas);

    function updateStatus(state) {
        statusEl.textContent = state;
        statusEl.className = 'status ' + (state === 'Connected' ? 'connected' : 'disconnected');
        btnConnect.disabled = state === 'Connected' || state === 'Connecting';
        btnDisconnect.disabled = state === 'Disconnected';
    }

    function connect() {
        updateStatus('Connecting');
        
        // WebSocket URL: ws://<host>:8000/
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = \`\${protocol}//\${window.location.hostname}:8000/\`;
        
        console.log(\`Connecting to WebSocket: \${wsUrl}\`);
        ws = new WebSocket(wsUrl);
        
        ws.onopen = () => {
            console.log('WebSocket connected');
            sendSignaling({
                type: 'join',
                from: deviceId,
                to: 'server',
                time: Date.now()
            });
        };
        
        ws.onmessage = async (event) => {
            const msg = JSON.parse(event.data);
            console.log('Received:', msg.type);
            
            switch (msg.type) {
                case 'role':
                    peerDeviceId = msg.from;
                    console.log(\`Paired with: \${peerDeviceId}, role: \${msg.data?.role}\`);
                    break;
                    
                case 'offer':
                    peerDeviceId = msg.from;
                    await handleOffer(msg.data?.sdp || msg.sdp);
                    break;
                    
                case 'answer':
                    await handleAnswer(msg.data?.sdp || msg.sdp);
                    break;
                    
                case 'ice':
                    const candidate = msg.data?.candidate || msg.candidate;
                    const sdpMid = msg.data?.sdpMid || msg.sdpMid || '0';
                    const sdpMLineIndex = msg.data?.sdpMLineIndex || msg.sdpMLineIndex || 0;
                    if (pc && candidate) {
                        try {
                            await pc.addIceCandidate(new RTCIceCandidate({
                                candidate: candidate,
                                sdpMid: sdpMid,
                                sdpMLineIndex: sdpMLineIndex
                            }));
                            console.log('Added ICE candidate');
                        } catch (e) {
                            console.error('Error adding ICE candidate:', e);
                        }
                    }
                    break;
                    
                case 'get_connect':
                    console.log('Received connection request');
                    createPeerConnection(false);
                    break;
                    
                case 'info':
                    console.log('Room info:', msg.data);
                    break;
                    
                case 'error':
                    console.error('Signaling error:', msg.data);
                    break;
            }
        };
        
        ws.onclose = () => {
            console.log('WebSocket closed');
            disconnect();
        };
        
        ws.onerror = (err) => {
            console.error('WebSocket error:', err);
            updateStatus('Error');
        };
    }

    function createPeerConnection(isOfferer = false) {
        const config = {
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
        };
        
        pc = new RTCPeerConnection(config);
        
        pc.ontrack = (event) => {
            console.log('Received remote track:', event.track.kind);
            if (video.srcObject !== event.streams[0]) {
                video.srcObject = event.streams[0];
            }
        };
        
        pc.onicecandidate = (event) => {
            if (event.candidate && ws && ws.readyState === WebSocket.OPEN) {
                sendSignaling({
                    type: 'ice',
                    from: deviceId,
                    to: peerDeviceId,
                    data: {
                        candidate: event.candidate.candidate,
                        sdpMid: event.candidate.sdpMid,
                        sdpMLineIndex: event.candidate.sdpMLineIndex
                    },
                    time: Date.now()
                });
            }
        };
        
        pc.onconnectionstatechange = () => {
            console.log('PC State:', pc.connectionState);
            if (pc.connectionState === 'connected') {
                updateStatus('Connected');
                startStats();
            } else if (pc.connectionState === 'disconnected' || pc.connectionState === 'failed') {
                updateStatus('Disconnected');
                stopStats();
            }
        };

        pc.ondatachannel = (event) => {
            dataChannel = event.channel;
            setupDataChannel();
        };

        if (isOfferer) {
            dataChannel = pc.createDataChannel("message");
            setupDataChannel();
        }
    }
    
    function setupDataChannel() {
        if (!dataChannel) return;
        
        dataChannel.onopen = () => {
            console.log("DataChannel open");
            sendControl({
                command: "set_ai",
                enabled: chkAiEnable.checked
            });
        };
        
        dataChannel.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                if (data.type === 'ai_result') {
                    drawDetections(data.detections);
                }
            } catch (e) {
                console.log('DataChannel message:', event.data);
            }
        };
        
        dataChannel.onclose = () => {
            console.log("DataChannel closed");
        };
    }

    async function handleOffer(sdp) {
        console.log('Processing offer...');
        
        if (!pc) {
            createPeerConnection(false);
        }
        
        try {
            const transceivers = pc.getTransceivers();
            if (transceivers.length === 0) {
                pc.addTransceiver('video', { direction: 'recvonly' });
            }
            
            await pc.setRemoteDescription(new RTCSessionDescription({
                type: 'offer',
                sdp: sdp
            }));
            
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            
            sendSignaling({
                type: 'answer',
                from: deviceId,
                to: peerDeviceId,
                data: { sdp: answer.sdp },
                time: Date.now()
            });
            
            console.log('Sent answer');
        } catch (e) {
            console.error('Error handling offer:', e);
        }
    }
    
    async function handleAnswer(sdp) {
        if (!pc) return;
        
        try {
            await pc.setRemoteDescription(new RTCSessionDescription({
                type: 'answer',
                sdp: sdp
            }));
            console.log('Processed answer');
        } catch (e) {
            console.error('Error handling answer:', e);
        }
    }
    
    function sendSignaling(msg) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(msg));
        }
    }

    function disconnect() {
        stopStats();
        
        if (pc) {
            pc.close();
            pc = null;
        }
        if (ws) {
            ws.close();
            ws = null;
        }
        dataChannel = null;
        peerDeviceId = '';
        
        updateStatus('Disconnected');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        video.srcObject = null;
    }

    function sendControl(cmd) {
        if (dataChannel && dataChannel.readyState === 'open') {
            dataChannel.send(JSON.stringify(cmd));
        } else if (ws && ws.readyState === WebSocket.OPEN) {
            sendSignaling({
                type: 'control',
                from: deviceId,
                to: peerDeviceId,
                data: cmd,
                time: Date.now()
            });
        }
    }
    
    function startStats() {
        lastBytesReceived = 0;
        lastTimestamp = Date.now();
        
        statsInterval = setInterval(async () => {
            if (!pc) return;
            
            const stats = await pc.getStats();
            let frameRate = 0;
            let bytesReceived = 0;
            
            stats.forEach(report => {
                if (report.type === 'inbound-rtp' && report.kind === 'video') {
                    frameRate = report.framesPerSecond || 0;
                    bytesReceived = report.bytesReceived || 0;
                }
            });
            
            const now = Date.now();
            const timeDiff = (now - lastTimestamp) / 1000;
            const bytesDiff = bytesReceived - lastBytesReceived;
            const bitrate = timeDiff > 0 ? Math.round((bytesDiff * 8) / timeDiff / 1000) : 0;
            
            lastBytesReceived = bytesReceived;
            lastTimestamp = now;
            
            valFps.textContent = Math.round(frameRate);
            valBitrate.textContent = bitrate;
        }, 1000);
    }
    
    function stopStats() {
        if (statsInterval) {
            clearInterval(statsInterval);
            statsInterval = null;
        }
        valFps.textContent = '0';
        valBitrate.textContent = '0';
    }

    function drawDetections(detections) {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        if (!detections || !Array.isArray(detections)) return;
        
        ctx.strokeStyle = '#00FF00';
        ctx.lineWidth = 2;
        ctx.font = '16px Arial';
        ctx.fillStyle = '#00FF00';
        
        detections.forEach(det => {
            let x = det.x, y = det.y, w = det.w, h = det.h;
            
            if (x <= 1 && y <= 1 && w <= 1 && h <= 1) {
                x *= canvas.width;
                y *= canvas.height;
                w *= canvas.width;
                h *= canvas.height;
            }
            
            ctx.strokeRect(x, y, w, h);
            const label = det.label || 'object';
            const prob = det.prob || det.confidence || 0;
            ctx.fillText(\`\${label} \${(prob * 100).toFixed(0)}%\`, x, y - 5);
        });
    }

    btnConnect.addEventListener('click', connect);
    btnDisconnect.addEventListener('click', disconnect);
    
    chkAiEnable.addEventListener('change', () => {
        sendControl({
            command: "set_ai",
            enabled: chkAiEnable.checked
        });
    });
    
    selModel.addEventListener('change', () => {
        sendControl({
            command: "switch_model",
            model: selModel.value
        });
    });
});
