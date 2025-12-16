document.addEventListener('DOMContentLoaded', () => {
    const video = document.getElementById('remoteVideo');
    const canvas = document.getElementById('overlay');
    const ctx = canvas.getContext('2d');
    const btnConnect = document.getElementById('btnConnect');
    const btnDisconnect = document.getElementById('btnDisconnect');
    const statusEl = document.getElementById('status');
    const chkAiEnable = document.getElementById('chkAiEnable');
    const selModel = document.getElementById('selModel');
    
    let pc = null;
    let ws = null;
    let dataChannel = null;
    
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
        const wsUrl = `${protocol}//${window.location.hostname}:8000/`;
        
        console.log(`Connecting to WebSocket: ${wsUrl}`);
        ws = new WebSocket(wsUrl);
        
        ws.onopen = () => {
            console.log('WebSocket connected');
            startWebRTC();
        };
        
        ws.onmessage = async (event) => {
            const msg = JSON.parse(event.data);
            if (msg.type === 'answer') {
                console.log('Received Answer');
                await pc.setRemoteDescription(new RTCSessionDescription({
                    type: 'answer',
                    sdp: msg.sdp
                }));
            } else if (msg.type === 'candidate') {
                console.log('Received Candidate');
                if (pc) {
                    await pc.addIceCandidate(new RTCIceCandidate({
                        candidate: msg.candidate,
                        sdpMid: msg.sdpMid,
                        sdpMLineIndex: msg.sdpMLineIndex
                    }));
                }
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

    async function startWebRTC() {
        const config = {
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
        };
        
        pc = new RTCPeerConnection(config);
        
        // Handle remote track
        pc.ontrack = (event) => {
            console.log('Received remote track');
            if (video.srcObject !== event.streams[0]) {
                video.srcObject = event.streams[0];
            }
        };
        
        // Handle ICE candidates
        pc.onicecandidate = (event) => {
            if (event.candidate && ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({
                    type: 'candidate',
                    candidate: event.candidate.candidate,
                    sdpMid: event.candidate.sdpMid,
                    sdpMLineIndex: event.candidate.sdpMLineIndex
                }));
            }
        };
        
        pc.onconnectionstatechange = () => {
            console.log('PC State:', pc.connectionState);
            if (pc.connectionState === 'connected') {
                updateStatus('Connected');
            } else if (pc.connectionState === 'disconnected' || pc.connectionState === 'failed') {
                updateStatus('Disconnected');
            }
        };

        // Create Data Channel for control/AI results
        dataChannel = pc.createDataChannel("control");
        dataChannel.onopen = () => {
            console.log("DataChannel open");
            // Send initial state
            sendControl({
                command: "set_ai",
                enabled: chkAiEnable.checked
            });
        };
        dataChannel.onmessage = (event) => {
            const data = JSON.parse(event.data);
            if (data.type === 'ai_result') {
                drawDetections(data.detections);
            }
        };

        // Create Offer
        // We want to receive video, but we might not send any
        pc.addTransceiver('video', { direction: 'recvonly' });
        
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        
        // Send Offer via WebSocket
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({
                type: 'offer',
                sdp: offer.sdp
            }));
        }
    }

    function disconnect() {
        if (pc) {
            pc.close();
            pc = null;
        }
        if (ws) {
            ws.close();
            ws = null;
        }
        updateStatus('Disconnected');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
    }

    function sendControl(cmd) {
        if (dataChannel && dataChannel.readyState === 'open') {
            dataChannel.send(JSON.stringify(cmd));
        } else if (ws && ws.readyState === WebSocket.OPEN) {
            // Fallback to WebSocket if DataChannel not ready
             ws.send(JSON.stringify({
                type: 'control',
                ...cmd
            }));
        }
    }

    function drawDetections(detections) {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        if (!detections) return;
        
        // Assuming detections are normalized [0-1] or pixel coords matching video resolution
        // If normalized, multiply by canvas.width/height
        // Let's assume the C++ sends normalized coords for flexibility
        
        ctx.strokeStyle = '#00FF00';
        ctx.lineWidth = 2;
        ctx.font = '16px Arial';
        ctx.fillStyle = '#00FF00';
        
        detections.forEach(det => {
            const x = det.x * canvas.width;
            const y = det.y * canvas.height;
            const w = det.w * canvas.width;
            const h = det.h * canvas.height;
            
            ctx.strokeRect(x, y, w, h);
            ctx.fillText(`${det.label} ${(det.prob*100).toFixed(0)}%`, x, y - 5);
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
