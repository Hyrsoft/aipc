<script>
  import { onMount, onDestroy } from 'svelte';
  
  // =====================================================================
  // 状态变量
  // =====================================================================
  let status = 'loading';
  let aiEnabled = false;
  let modelName = 'none';
  let previewMode = 'websocket'; // 'websocket' | 'webrtc'
  let logs = [];
  
  // 服务状态
  let services = {
    rtsp: { enabled: false, running: false, valid: false },
    webrtc: { enabled: false, running: false },
    recording: { enabled: false, active: false, output_dir: '' }
  };

  // AI 统计
  let aiStats = {
      frames_processed: 0,
      avg_inference_ms: 0,
      total_detections: 0
  };

  // WebSocket H.264 相关
  let wsConnection = null;
  let jmuxer = null;
  let wsConnected = false;
  let wsConnecting = false;
  let wsBytesReceived = 0;
  let wsLastBytesReceived = 0;
  let wsLastStatsTime = 0;
  let wsFrameCount = 0;
  let wsLastFrameCount = 0;
  let wsStatsTimer = null;
  let wsVideoEl = null;
  let wsStats = { resolution: '-', fps: '- fps', bitrate: '- kbps', received: '0 B' };

  // WebRTC 相关
  let peerConnection = null;
  let webrtcConnected = false;
  let webrtcConnecting = false;
  let pendingIceCandidates = [];
  let answerSent = false;
  let rtcVideoEl = null;
  let rtcStatsTimer = null;
  let rtcLastBytesReceived = 0;
  let rtcLastStatsTime = 0;
  let rtcStats = { resolution: '-', fps: '- fps', bitrate: '- kbps', received: '0 B' };

  let pollInterval;
  const isDev = import.meta.env.DEV;
  const API_BASE = '';

  // =====================================================================
  // 工具函数
  // =====================================================================
  function formatBytes(bytes) {
      if (bytes === 0) return '0 B';
      const k = 1024;
      const sizes = ['B', 'KB', 'MB', 'GB'];
      const i = Math.floor(Math.log(bytes) / Math.log(k));
      return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  }

  function addLog(msg, type = 'info') {
      const time = new Date().toLocaleTimeString();
      const prefix = type === 'error' ? '❌' : type === 'success' ? '✓' : '•';
      logs = [`[${time}] ${prefix} ${msg}`, ...logs].slice(0, 50);
  }

  function getDeviceHost() {
      return isDev ? 'localhost' : location.hostname;
  }

  // =====================================================================
  // API 调用
  // =====================================================================
  async function apiCall(method, endpoint, body = null) {
      try {
          const options = {
              method: method,
              headers: { 'Content-Type': 'application/json' }
          };
          if (body) options.body = JSON.stringify(body);
          const response = await fetch(API_BASE + endpoint, options);
          return await response.json();
      } catch (error) {
          console.error('API Error:', error);
          return { success: false, message: error.message };
      }
  }

  // =====================================================================
  // 状态轮询
  // =====================================================================
  async function fetchStatus() {
    try {
      if (isDev) {
         status = 'online';
         return;
      }

      const statusRes = await fetch('/api/status');
      if (statusRes.ok) {
          const statusData = await statusRes.json();
          if (statusData.success) {
              services = statusData.data;
              status = 'online';
          }
      }

      const aiRes = await fetch('/api/ai/status');
      if (aiRes.ok) {
          const aiData = await aiRes.json();
          if (aiData.success) {
            const d = aiData.data;
            aiEnabled = d.has_model && d.model_type && d.model_type.toLowerCase() !== 'none';
            modelName = d.model_type ? d.model_type.toLowerCase() : 'none';
            if (d.stats) {
                aiStats = d.stats;
            }
          }
      }
    } catch (e) {
      console.error('Failed to fetch status', e);
      status = 'offline';
    }
  }

  // =====================================================================
  // AI 控制
  // =====================================================================
  async function toggleAI() {
    const targetModel = aiEnabled ? 'none' : 'yolov5';
    await switchModel(targetModel);
  }

  async function switchModel(name) {
      addLog(`切换 AI 模型: ${name}...`);
      try {
          if(!isDev) {
              const res = await fetch('/api/ai/switch', {
                   method: 'POST',
                   headers: { 'Content-Type': 'application/json' },
                   body: JSON.stringify({ model: name })
              });
              const data = await res.json();
              if (data.success) {
                  modelName = name;
                  aiEnabled = name !== 'none';
                  addLog(`模型切换成功: ${name}`, 'success');
              } else {
                  addLog(`切换失败: ${data.message}`, 'error');
              }
          } else {
             modelName = name;
             aiEnabled = name !== 'none';
             addLog(`(Dev) 切换到 ${name}`, 'success');
          }
      } catch(e) {
          addLog(`模型切换异常: ${e.message}`, 'error');
      }
  }

  // =====================================================================
  // 服务控制
  // =====================================================================
  async function toggleService(serviceName, action) {
      addLog(`${serviceName} -> ${action}...`);
      const url = `/api/${serviceName}/${action}`;
      try {
          if (!isDev) {
              const res = await fetch(url, { method: 'POST' });
              const data = await res.json();
              if (data.success) {
                  addLog(`${serviceName} ${action} 成功`, 'success');
                  fetchStatus();
              } else {
                  addLog(`${serviceName} ${action} 失败: ${data.message}`, 'error');
              }
          } else {
              addLog(`(Dev) ${serviceName} ${action}`, 'success');
          }
      } catch (e) {
          addLog(`请求异常: ${e.message}`, 'error');
      }
  }

  // =====================================================================
  // WebSocket H.264 播放器
  // =====================================================================
  function wsConnect() {
      if (wsConnection) {
          wsDisconnect();
      }

      wsConnecting = true;
      addLog('正在建立 WebSocket 连接...');
      
      wsBytesReceived = 0;
      wsFrameCount = 0;

      try {
          // 创建 jMuxer 实例
          if (!wsVideoEl) {
              addLog('视频元素不存在', 'error');
              wsConnecting = false;
              return;
          }
          
          // @ts-ignore
          jmuxer = new JMuxer({
              node: wsVideoEl,
              mode: 'video',
              flushingTime: 1,
              fps: 30,
              clearBuffer: true,
              debug: false,
              onReady: () => {
                  console.log('jMuxer ready');
              },
              onError: (err) => {
                  console.error('jMuxer error:', err);
              }
          });

          const wsUrl = `ws://${getDeviceHost()}:8082`;
          wsConnection = new WebSocket(wsUrl);
          wsConnection.binaryType = 'arraybuffer';

          wsConnection.onopen = () => {
              wsConnected = true;
              wsConnecting = false;
              addLog('WebSocket 连接成功', 'success');
              startWsStatsUpdate();
          };

          wsConnection.onclose = (event) => {
              wsConnected = false;
              wsConnecting = false;
              stopWsStatsUpdate();
              addLog(`WebSocket 已断开 (code=${event.code})`);
          };

          wsConnection.onerror = (err) => {
              console.error('WebSocket error:', err);
              addLog('WebSocket 连接错误', 'error');
          };

          wsConnection.onmessage = (event) => {
              if (event.data instanceof ArrayBuffer) {
                  const data = new Uint8Array(event.data);
                  wsBytesReceived += data.length;
                  wsFrameCount++;
                  
                  if (jmuxer) {
                      try {
                          jmuxer.feed({ video: data });
                      } catch (e) {
                          console.error('Feed error:', e);
                      }
                  }
              }
          };

      } catch (error) {
          console.error('WebSocket connect error:', error);
          addLog('WebSocket 连接失败: ' + error.message, 'error');
          wsConnecting = false;
      }
  }

  function wsDisconnect() {
      stopWsStatsUpdate();
      
      if (wsConnection) {
          wsConnection.close();
          wsConnection = null;
      }

      if (jmuxer) {
          jmuxer.destroy();
          jmuxer = null;
      }

      wsConnected = false;
      wsConnecting = false;
      wsStats = { resolution: '-', fps: '- fps', bitrate: '- kbps', received: '0 B' };
      addLog('WebSocket 已断开');
  }

  function startWsStatsUpdate() {
      wsLastBytesReceived = 0;
      wsLastFrameCount = 0;
      wsLastStatsTime = Date.now();
      wsStatsTimer = setInterval(updateWsVideoStats, 1000);
  }

  function stopWsStatsUpdate() {
      if (wsStatsTimer) {
          clearInterval(wsStatsTimer);
          wsStatsTimer = null;
      }
  }

  function updateWsVideoStats() {
      if (!wsVideoEl) return;
      
      // 分辨率
      if (wsVideoEl.videoWidth && wsVideoEl.videoHeight) {
          wsStats.resolution = `${wsVideoEl.videoWidth}x${wsVideoEl.videoHeight}`;
      }
      
      const now = Date.now();
      const timeDiff = (now - wsLastStatsTime) / 1000;
      
      if (timeDiff > 0) {
          const framesDiff = wsFrameCount - wsLastFrameCount;
          wsStats.fps = (framesDiff / timeDiff).toFixed(1) + ' fps';
          
          const bytesDiff = wsBytesReceived - wsLastBytesReceived;
          wsStats.bitrate = (bytesDiff * 8 / timeDiff / 1000).toFixed(0) + ' kbps';
      }
      
      wsLastBytesReceived = wsBytesReceived;
      wsLastFrameCount = wsFrameCount;
      wsLastStatsTime = now;
      wsStats.received = formatBytes(wsBytesReceived);
      wsStats = wsStats; // trigger reactivity
  }

  // =====================================================================
  // WebRTC 播放器
  // =====================================================================
  async function webrtcConnect() {
      if (peerConnection) {
          webrtcDisconnect();
      }

      webrtcConnecting = true;
      addLog('正在建立 WebRTC 连接...');
      
      pendingIceCandidates = [];
      answerSent = false;

      try {
          // 获取 offer
          const offerResp = await apiCall('POST', '/api/webrtc/offer');
          if (!offerResp.success) {
              throw new Error(offerResp.message || '获取 offer 失败');
          }

          const { sdp, ice_servers } = offerResp.data;

          // 创建 PeerConnection
          const config = {
              iceServers: ice_servers || [{ urls: 'stun:stun.l.google.com:19302' }]
          };
          peerConnection = new RTCPeerConnection(config);

          // ICE candidate 处理
          peerConnection.onicecandidate = async (event) => {
              if (event.candidate) {
                  if (answerSent) {
                      await apiCall('POST', '/api/webrtc/ice', {
                          candidate: event.candidate.candidate,
                          sdpMid: event.candidate.sdpMid,
                          sdpMLineIndex: event.candidate.sdpMLineIndex
                      });
                  } else {
                      pendingIceCandidates.push({
                          candidate: event.candidate.candidate,
                          sdpMid: event.candidate.sdpMid,
                          sdpMLineIndex: event.candidate.sdpMLineIndex
                      });
                  }
              }
          };

          // 连接状态
          peerConnection.onconnectionstatechange = () => {
              console.log('Connection state:', peerConnection.connectionState);
              switch(peerConnection.connectionState) {
                  case 'connected':
                      webrtcConnected = true;
                      webrtcConnecting = false;
                      addLog('WebRTC 连接成功', 'success');
                      startRtcStatsUpdate();
                      break;
                  case 'disconnected':
                  case 'failed':
                  case 'closed':
                      webrtcConnected = false;
                      webrtcConnecting = false;
                      stopRtcStatsUpdate();
                      break;
              }
          };

          // 处理远程流
          peerConnection.ontrack = (event) => {
              console.log('Received track:', event.track.kind);
              if (rtcVideoEl && event.streams && event.streams[0]) {
                  rtcVideoEl.srcObject = event.streams[0];
              }
          };

          // 设置远程描述
          await peerConnection.setRemoteDescription(new RTCSessionDescription({
              type: 'offer',
              sdp: sdp
          }));

          // 创建 answer
          const answer = await peerConnection.createAnswer();
          await peerConnection.setLocalDescription(answer);

          // 发送 answer
          const answerResp = await apiCall('POST', '/api/webrtc/answer', {
              sdp: answer.sdp
          });

          if (!answerResp.success) {
              throw new Error(answerResp.message || '发送 answer 失败');
          }
          
          answerSent = true;
          for (const candidate of pendingIceCandidates) {
              await apiCall('POST', '/api/webrtc/ice', candidate);
          }
          pendingIceCandidates = [];

      } catch (error) {
          console.error('WebRTC connect error:', error);
          addLog('WebRTC 连接失败: ' + error.message, 'error');
          webrtcConnecting = false;
      }
  }

  function webrtcDisconnect() {
      stopRtcStatsUpdate();
      
      if (peerConnection) {
          peerConnection.close();
          peerConnection = null;
      }

      if (rtcVideoEl && rtcVideoEl.srcObject) {
          rtcVideoEl.srcObject.getTracks().forEach(track => track.stop());
          rtcVideoEl.srcObject = null;
      }

      webrtcConnected = false;
      webrtcConnecting = false;
      rtcStats = { resolution: '-', fps: '- fps', bitrate: '- kbps', received: '0 B' };
      addLog('WebRTC 已断开');
  }

  function startRtcStatsUpdate() {
      rtcLastBytesReceived = 0;
      rtcLastStatsTime = Date.now();
      rtcStatsTimer = setInterval(updateRtcVideoStats, 1000);
  }

  function stopRtcStatsUpdate() {
      if (rtcStatsTimer) {
          clearInterval(rtcStatsTimer);
          rtcStatsTimer = null;
      }
  }

  async function updateRtcVideoStats() {
      if (!peerConnection) return;

      try {
          const stats = await peerConnection.getStats();
          stats.forEach(report => {
              if (report.type === 'inbound-rtp' && report.kind === 'video') {
                  if (report.frameWidth && report.frameHeight) {
                      rtcStats.resolution = `${report.frameWidth}x${report.frameHeight}`;
                  }
                  
                  if (report.framesPerSecond) {
                      rtcStats.fps = report.framesPerSecond.toFixed(1) + ' fps';
                  }
                  
                  const now = Date.now();
                  const bytesReceived = report.bytesReceived || 0;
                  if (rtcLastStatsTime > 0) {
                      const timeDiff = (now - rtcLastStatsTime) / 1000;
                      const bytesDiff = bytesReceived - rtcLastBytesReceived;
                      rtcStats.bitrate = (bytesDiff * 8 / timeDiff / 1000).toFixed(0) + ' kbps';
                  }
                  rtcLastBytesReceived = bytesReceived;
                  rtcLastStatsTime = now;
                  rtcStats.received = formatBytes(bytesReceived);
                  rtcStats = rtcStats;
              }
          });
      } catch (e) {
          console.error('Stats error:', e);
      }
  }

  // =====================================================================
  // 生命周期
  // =====================================================================
  onMount(() => {
    addLog('控制台已加载');
    fetchStatus();
    pollInterval = setInterval(fetchStatus, 3000);
  });

  onDestroy(() => {
    clearInterval(pollInterval);
    wsDisconnect();
    webrtcDisconnect();
  });
</script>

<main class="min-h-screen bg-gray-50 p-4 font-sans">
  <div class="max-w-[1600px] mx-auto space-y-4">
    <!-- 顶部标题栏 -->
    <header class="bg-white rounded-xl shadow-sm p-4 flex items-center justify-between border-b-4 border-primary/20">
      <div class="flex items-center space-x-4">
        <div class="w-10 h-10 bg-primary/10 rounded-lg flex items-center justify-center text-primary">
          <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/></svg>
        </div>
        <div>
          <h1 class="text-xl font-bold text-gray-800">AIPC 控制台</h1>
          <p class="text-gray-500 text-xs">Luckfox Pico RV1106 AI Embedded System</p>
        </div>
      </div>
      <div class="flex items-center space-x-2">
         <span class={`w-2 h-2 rounded-full ${status === 'online' ? 'bg-green-500' : 'bg-red-500'}`}></span>
         <span class="text-sm font-medium text-gray-600">{status === 'online' ? '在线' : '离线'}</span>
      </div>
    </header>

    <!-- 核心布局 -->
    <div class="grid grid-cols-1 lg:grid-cols-4 gap-4">
       
       <!-- 左侧: 视频预览区域 -->
       <div class="lg:col-span-3 flex flex-col gap-4">
            <!-- 视频卡片 -->
           <div class="bg-white rounded-xl shadow-sm overflow-hidden border border-gray-100 flex flex-col">
               <!-- 视频头部 Tabs -->
               <div class="border-b border-gray-100 flex items-center bg-gray-50 px-2">
                   <button 
                       class={`px-4 py-3 text-sm font-bold flex items-center gap-2 border-b-2 transition-colors ${previewMode === 'websocket' ? 'border-primary text-primary bg-white' : 'border-transparent text-gray-500 hover:text-gray-700'}`}
                       on:click={() => previewMode = 'websocket'}
                   >
                       <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z" /></svg>
                       WS H.264
                   </button>
                   <button 
                       class={`px-4 py-3 text-sm font-bold flex items-center gap-2 border-b-2 transition-colors ${previewMode === 'webrtc' ? 'border-primary text-primary bg-white' : 'border-transparent text-gray-500 hover:text-gray-700'}`}
                       on:click={() => previewMode = 'webrtc'}
                   >
                       <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z" /></svg>
                       WebRTC
                   </button>
                   
                   <!-- 连接控制按钮 -->
                   <div class="ml-auto mr-2 flex items-center gap-2">
                       {#if previewMode === 'websocket'}
                           <span class={`w-2 h-2 rounded-full ${wsConnected ? 'bg-green-500' : wsConnecting ? 'bg-yellow-500 animate-pulse' : 'bg-gray-400'}`}></span>
                           <span class="text-xs text-gray-500">{wsConnected ? '已连接' : wsConnecting ? '连接中...' : '未连接'}</span>
                           {#if !wsConnected && !wsConnecting}
                               <button class="px-3 py-1 text-xs bg-green-500 text-white rounded hover:bg-green-600" on:click={wsConnect}>连接</button>
                           {:else}
                               <button class="px-3 py-1 text-xs bg-red-500 text-white rounded hover:bg-red-600" on:click={wsDisconnect}>断开</button>
                           {/if}
                       {:else}
                           <span class={`w-2 h-2 rounded-full ${webrtcConnected ? 'bg-green-500' : webrtcConnecting ? 'bg-yellow-500 animate-pulse' : 'bg-gray-400'}`}></span>
                           <span class="text-xs text-gray-500">{webrtcConnected ? '已连接' : webrtcConnecting ? '连接中...' : '未连接'}</span>
                           {#if !webrtcConnected && !webrtcConnecting}
                               <button class="px-3 py-1 text-xs bg-green-500 text-white rounded hover:bg-green-600" on:click={webrtcConnect}>连接</button>
                           {:else}
                               <button class="px-3 py-1 text-xs bg-red-500 text-white rounded hover:bg-red-600" on:click={webrtcDisconnect}>断开</button>
                           {/if}
                       {/if}
                   </div>
               </div>
               
               <!-- 视频内容区 -->
               <div class="w-full aspect-video bg-black relative flex items-center justify-center overflow-hidden">
                   {#if previewMode === 'websocket'}
                       <!-- WebSocket H.264 播放器 -->
                       <video bind:this={wsVideoEl} autoplay playsinline muted class="w-full h-full object-contain"></video>
                       {#if !wsConnected}
                           <div class="absolute inset-0 flex items-center justify-center bg-black/80">
                               <div class="text-center">
                                   <svg class="w-16 h-16 text-gray-600 mx-auto mb-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                                       <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z" />
                                   </svg>
                                   <p class="text-gray-400 text-sm mb-2">WebSocket H.264 流预览</p>
                                   <p class="text-gray-600 text-xs">ws://{getDeviceHost()}:8082</p>
                                   <button class="mt-3 px-4 py-2 bg-primary text-white rounded-lg hover:bg-primary/90" on:click={wsConnect}>
                                       {wsConnecting ? '连接中...' : '点击连接'}
                                   </button>
                               </div>
                           </div>
                       {/if}
                   {:else}
                       <!-- WebRTC 播放器 -->
                       <video bind:this={rtcVideoEl} autoplay playsinline muted class="w-full h-full object-contain"></video>
                       {#if !webrtcConnected}
                           <div class="absolute inset-0 flex items-center justify-center bg-black/80">
                               <div class="text-center">
                                   <svg class="w-16 h-16 text-gray-600 mx-auto mb-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                                       <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1" d="M13 10V3L4 14h7v7l9-11h-7z" />
                                   </svg>
                                   <p class="text-gray-400 text-sm mb-2">WebRTC 低延迟预览</p>
                                   <p class="text-gray-600 text-xs mb-1">需要先启动 WebRTC 服务</p>
                                   <p class="text-gray-600 text-xs">状态: <span class={services.webrtc?.running ? 'text-green-400' : 'text-red-400'}>{services.webrtc?.running ? '服务已启动' : '服务未启动'}</span></p>
                                   <button class="mt-3 px-4 py-2 bg-primary text-white rounded-lg hover:bg-primary/90" on:click={webrtcConnect} disabled={!services.webrtc?.running}>
                                       {webrtcConnecting ? '连接中...' : '点击连接'}
                                   </button>
                               </div>
                           </div>
                       {/if}
                   {/if}

                   <!-- AI 状态覆盖 -->
                   {#if aiEnabled}
                     <div class="absolute top-4 left-4 z-10">
                         <div class="bg-black/70 backdrop-blur-sm text-white px-3 py-2 rounded-lg text-xs border border-white/10 shadow-lg">
                             <div class="font-bold text-primary mb-1 flex items-center">
                                 <span class="w-2 h-2 bg-green-500 rounded-full mr-2 animate-pulse"></span>
                                 AI {modelName.toUpperCase()}
                             </div>
                         </div>
                     </div>
                   {/if}
               </div>

               <!-- 视频底部参数栏 -->
               <div class="bg-gray-900 text-gray-400 text-xs px-4 py-2 flex justify-between items-center border-t border-gray-800">
                   <div class="flex space-x-4 font-mono">
                       <span>RES: <span class="text-white">{previewMode === 'websocket' ? wsStats.resolution : rtcStats.resolution}</span></span>
                       <span>FPS: <span class="text-white">{previewMode === 'websocket' ? wsStats.fps : rtcStats.fps}</span></span>
                       <span>BITRATE: <span class="text-white">{previewMode === 'websocket' ? wsStats.bitrate : rtcStats.bitrate}</span></span>
                       <span>RX: <span class="text-white">{previewMode === 'websocket' ? wsStats.received : rtcStats.received}</span></span>
                   </div>
                   <div class="flex space-x-4">
                       <span>INF: <span class="text-yellow-400">{aiStats.avg_inference_ms || 0}ms</span></span>
                       <span>DET: <span class="text-green-400">{aiStats.total_detections || 0}</span></span>
                   </div>
               </div>
           </div>
           
           <!-- 日志区域 -->
           <div class="bg-white rounded-xl shadow-sm border border-gray-100 flex flex-col h-40 overflow-hidden">
               <div class="px-4 py-2 bg-gray-50 border-b border-gray-100 text-xs font-bold text-gray-500 uppercase tracking-wider flex justify-between">
                   <span>系统日志</span>
                   <button class="text-gray-400 cursor-pointer hover:text-gray-600" on:click={() => logs = []}>清除</button>
               </div>
               <div class="flex-1 p-2 overflow-y-auto font-mono text-xs space-y-1 bg-gray-50/50">
                   {#each logs as log}
                       <div class="text-gray-600 border-l-2 border-primary/20 pl-2 py-0.5 hover:bg-white transition-colors">
                           {log}
                       </div>
                   {/each}
                   {#if logs.length === 0}
                       <div class="text-center text-gray-400 italic py-4">暂无日志</div>
                   {/if}
               </div>
           </div>
       </div>

       <!-- 右侧: 控制面板 -->
       <div class="space-y-4 overflow-y-auto">
           <!-- AI 控制 -->
           <div class="bg-white rounded-xl shadow-sm p-5 border border-gray-100">
               <h3 class="text-gray-500 text-xs font-bold mb-4 uppercase tracking-wider">AI 模型</h3>
               
               <div class="flex items-center justify-between mb-4 bg-gray-50 p-3 rounded-lg">
                   <span class="font-bold text-gray-800 text-sm">启用推理</span>
                   <button 
                     on:click={toggleAI}
                     class={`relative inline-flex h-6 w-11 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-primary focus:ring-offset-2 ${aiEnabled ? 'bg-primary' : 'bg-gray-300'}`}
                   >
                     <span class={`inline-block h-4 w-4 transform rounded-full bg-white transition-transform ${aiEnabled ? 'translate-x-6' : 'translate-x-1'}`} />
                   </button>
               </div>
               
               <div class="space-y-2">
                   {#each ['none', 'yolov5', 'retinaface'] as model}
                   <button 
                     class={`w-full py-2.5 px-3 rounded-lg text-xs font-medium transition-all flex items-center justify-between group ${modelName === model ? 'bg-primary text-white shadow-lg shadow-primary/20 ring-1 ring-primary' : 'bg-white border border-gray-200 text-gray-600 hover:border-primary/50 hover:text-primary'}`}
                     on:click={() => switchModel(model)}
                   >
                     <span class="uppercase">{model === 'none' ? '关闭 AI' : model}</span>
                     {#if modelName === model}
                        <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 13l4 4L19 7"/></svg>
                     {/if}
                   </button>
                   {/each}
               </div>
           </div>

           <!-- 服务控制 -->
           <div class="bg-white rounded-xl shadow-sm p-5 border border-gray-100">
               <h3 class="text-gray-500 text-xs font-bold mb-4 uppercase tracking-wider">服务管理</h3>
               <div class="space-y-3">
                   <!-- RTSP -->
                   <div class="bg-gray-50 p-3 rounded-lg border border-gray-100">
                       <div class="flex justify-between items-center mb-2">
                           <span class="text-sm font-bold text-gray-700">RTSP 流</span>
                           <span class={`w-2 h-2 rounded-full ${services.rtsp?.running ? 'bg-green-500' : 'bg-red-400'}`}></span>
                       </div>
                       <button 
                         on:click={() => toggleService('rtsp', services.rtsp?.running ? 'stop' : 'start')}
                         class={`w-full py-1.5 rounded text-xs font-bold transition-colors ${services.rtsp?.running ? 'bg-white border border-red-200 text-red-600 hover:bg-red-50' : 'bg-green-500 text-white hover:bg-green-600'}`}
                       >
                           {services.rtsp?.running ? '停止服务' : '启动服务'}
                       </button>
                   </div>
                   
                   <!-- WebRTC -->
                   <div class="bg-gray-50 p-3 rounded-lg border border-gray-100">
                       <div class="flex justify-between items-center mb-2">
                           <span class="text-sm font-bold text-gray-700">WebRTC</span>
                           <span class={`w-2 h-2 rounded-full ${services.webrtc?.running ? 'bg-green-500' : 'bg-red-400'}`}></span>
                       </div>
                       <button 
                         on:click={() => toggleService('webrtc', services.webrtc?.running ? 'stop' : 'start')}
                         class={`w-full py-1.5 rounded text-xs font-bold transition-colors ${services.webrtc?.running ? 'bg-white border border-red-200 text-red-600 hover:bg-red-50' : 'bg-green-500 text-white hover:bg-green-600'}`}
                       >
                           {services.webrtc?.running ? '停止服务' : '启动服务'}
                       </button>
                   </div>
                   
                   <!-- Recording -->
                   <div class="bg-gray-50 p-3 rounded-lg border border-gray-100">
                       <div class="flex justify-between items-center mb-2">
                           <span class="text-sm font-bold text-gray-700">MP4 录制</span>
                           {#if services.recording?.active}
                                <span class="animate-pulse text-red-500 text-xs font-bold">● REC</span>
                           {/if}
                       </div>
                       <button 
                          on:click={() => toggleService('record', services.recording?.active ? 'stop' : 'start')}
                          class={`w-full py-1.5 rounded text-xs font-bold transition-colors ${services.recording?.active ? 'bg-red-500 text-white hover:bg-red-600' : 'bg-white border border-gray-300 text-gray-700 hover:bg-gray-200'}`}
                       >
                           {services.recording?.active ? '停止录制' : '开始录制'}
                       </button>
                   </div>
               </div>
           </div>

            <!-- 网络信息 -->
           <div class="bg-white rounded-xl shadow-sm p-4 border border-gray-100">
                <h3 class="text-gray-500 text-xs font-bold mb-2 uppercase tracking-wider">连接信息</h3> 
                <div class="text-xs space-y-2 text-gray-600 break-all font-mono">
                    <div>
                        <span class="text-gray-400 block">Console:</span>
                        http://{getDeviceHost()}:8080
                    </div>
                    <div>
                        <span class="text-gray-400 block">RTSP:</span>
                        rtsp://{getDeviceHost()}:554/live/0
                    </div>
                    <div>
                        <span class="text-gray-400 block">WS Preview:</span>
                        ws://{getDeviceHost()}:8082
                    </div>
                </div>
           </div>
       </div>
    </div>
  </div>
</main>
