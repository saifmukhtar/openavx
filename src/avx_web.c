/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_web.c - Embedded HTTP and fragmented MP4 preview server
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "openavx.h"
#include "avx_web.h"
#include "avx_rtsp.h"
#include "avx_platform.h"

#if defined(AVX_OS_POSIX)
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#endif

/* Embedded HTML5 Low-Latency Media Source Extensions Player */
static const char *k_player_html =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">\n"
"<title>OpenAVX Web Preview</title>\n"
"<style>\n"
" * { margin:0; padding:0; box-sizing:border-box; background:#080808; }\n"
" html, body { width:100%; height:100%; overflow:hidden; font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif; }\n"
" #wrap { position:relative; width:100%; height:100%; display:flex; align-items:center; justify-content:center; }\n"
" video { width:100%; height:100%; object-fit:contain; }\n"
" #hud { position:absolute; top:12px; left:12px; display:flex; align-items:center; gap:8px; background:rgba(12,12,12,0.75); backdrop-filter:blur(6px); -webkit-backdrop-filter:blur(6px); padding:6px 12px; border-radius:6px; border:1px solid #222; font-size:12px; color:#eee; pointer-events:none; box-shadow:0 4px 12px rgba(0,0,0,0.5); }\n"
" .dot { width:8px; height:8px; border-radius:50%; background:#00ff66; box-shadow:0 0 8px #00ff66; transition:all 0.3s; }\n"
" .dot.offline { background:#ff3333; box-shadow:0 0 8px #ff3333; }\n"
" #title { font-weight:600; letter-spacing:0.5px; }\n"
" #stats { color:#888; font-family:monospace; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div id=\"wrap\">\n"
"  <video id=\"v\" autoplay muted playsinline></video>\n"
"  <div id=\"hud\">\n"
"    <div id=\"dot\" class=\"dot\"></div>\n"
"    <span id=\"title\">OPENAVX LIVE</span>\n"
"    <span id=\"stats\">Connecting...</span>\n"
"  </div>\n"
"</div>\n"
"<script>\n"
"(function(){\n"
"  const v = document.getElementById('v');\n"
"  const dot = document.getElementById('dot');\n"
"  const stats = document.getElementById('stats');\n"
"  let ms, sb, queue = [], frames = 0, lastTime = performance.now();\n"
"\n"
"  setInterval(() => {\n"
"    const now = performance.now();\n"
"    const fps = ((frames * 1000) / (now - lastTime)).toFixed(1);\n"
"    frames = 0;\n"
"    lastTime = now;\n"
"    if (sb && !v.paused) stats.textContent = fps + ' fps | <100ms';\n"
"  }, 1000);\n"
"\n"
"  async function connect() {\n"
"    if (!window.MediaSource) {\n"
"      stats.textContent = 'MediaSource not supported';\n"
"      dot.className = 'dot offline';\n"
"      return;\n"
"    }\n"
"    ms = new MediaSource();\n"
"    v.src = URL.createObjectURL(ms);\n"
"    ms.onsourceopen = async () => {\n"
"      try {\n"
"        const res = await fetch('/live.mp4');\n"
"        if (!res.ok) throw new Error('HTTP ' + res.status);\n"
"        const mime = res.headers.get('Content-Type') || 'video/mp4; codecs=\"avc1.640028\"';\n"
"        sb = ms.addSourceBuffer(mime);\n"
"        sb.mode = 'segments';\n"
"        sb.onupdateend = () => {\n"
"          if (queue.length > 0 && !sb.updating) sb.appendBuffer(queue.shift());\n"
"          if (v.buffered.length > 0) {\n"
"            const end = v.buffered.end(v.buffered.length - 1);\n"
"            if (end - v.currentTime > 0.25) v.currentTime = end - 0.05;\n"
"          }\n"
"        };\n"
"        v.play().catch(()=>{});\n"
"        dot.className = 'dot';\n"
"        stats.textContent = 'Streaming';\n"
"        const reader = res.body.getReader();\n"
"        while (true) {\n"
"          const {done, value} = await reader.read();\n"
"          if (done) break;\n"
"          frames++;\n"
"          if (!sb.updating && queue.length === 0) sb.appendBuffer(value);\n"
"          else queue.push(value);\n"
"        }\n"
"      } catch (e) {\n"
"        dot.className = 'dot offline';\n"
"        stats.textContent = 'Offline (retrying...)';\n"
"        setTimeout(connect, 1500);\n"
"      }\n"
"    };\n"
"  }\n"
"  connect();\n"
"})();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* Embedded Smartphone Camera Ingress Web App */
static const char *k_camera_html =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover\">\n"
"<title>OpenAVX Mobile Camera</title>\n"
"<style>\n"
" * { margin:0; padding:0; box-sizing:border-box; user-select:none; -webkit-user-select:none; }\n"
" html, body { width:100%; height:100%; overflow:hidden; background:#08080a; font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif; color:#eee; }\n"
" #wrap { position:relative; width:100%; height:100%; display:flex; align-items:center; justify-content:center; }\n"
" video { width:100%; height:100%; object-fit:cover; }\n"
" #hud { position:absolute; top:env(safe-area-inset-top, 16px); left:16px; right:16px; display:flex; align-items:center; justify-content:space-between; pointer-events:none; z-index:10; }\n"
" .badge { display:inline-flex; align-items:center; gap:8px; background:rgba(15,15,18,0.78); backdrop-filter:blur(10px); -webkit-backdrop-filter:blur(10px); border:1px solid rgba(255,255,255,0.12); border-radius:20px; padding:6px 14px; font-size:12px; font-weight:600; box-shadow:0 4px 16px rgba(0,0,0,0.6); }\n"
" .dot { width:8px; height:8px; border-radius:50%; background:#888; transition:all 0.3s; }\n"
" .dot.live { background:#00ff66; box-shadow:0 0 10px #00ff66; }\n"
" .dot.warn { background:#ffaa00; box-shadow:0 0 10px #ffaa00; }\n"
" .dot.err { background:#ff3333; box-shadow:0 0 10px #ff3333; }\n"
" #stats { font-family:monospace; font-size:11px; color:#aaa; }\n"
" #controls { position:absolute; bottom:calc(env(safe-area-inset-bottom, 20px) + 12px); left:50%; transform:translateX(-50%); display:flex; align-items:center; gap:12px; background:rgba(18,18,22,0.85); backdrop-filter:blur(16px); -webkit-backdrop-filter:blur(16px); border:1px solid rgba(255,255,255,0.15); border-radius:32px; padding:8px 16px; box-shadow:0 8px 32px rgba(0,0,0,0.7); z-index:10; max-width:calc(100% - 32px); }\n"
" .btn { background:rgba(255,255,255,0.08); color:#fff; border:1px solid rgba(255,255,255,0.12); border-radius:24px; padding:10px 16px; font-size:13px; font-weight:600; cursor:pointer; display:flex; align-items:center; gap:6px; transition:all 0.2s; white-space:nowrap; }\n"
" .btn:active { transform:scale(0.96); background:rgba(255,255,255,0.18); }\n"
" .btn-stream { background:#0070f3; border-color:#0070f3; color:#fff; min-width:110px; justify-content:center; }\n"
" .btn-stream.active { background:#e00; border-color:#e00; }\n"
" select { background:rgba(255,255,255,0.08); color:#fff; border:1px solid rgba(255,255,255,0.12); border-radius:24px; padding:10px 14px; font-size:12px; font-weight:600; outline:none; }\n"
" select option { background:#16161a; color:#fff; }\n"
" #secWarn { position:absolute; top:70px; left:16px; right:16px; background:rgba(30,20,10,0.92); border:1px solid #d97706; border-radius:12px; padding:14px 18px; font-size:12px; line-height:1.5; color:#fef3c7; z-index:20; display:none; box-shadow:0 6px 20px rgba(0,0,0,0.6); }\n"
" #secWarn b { color:#f59e0b; font-size:13px; }\n"
" #secWarn code { background:rgba(0,0,0,0.4); padding:2px 6px; border-radius:4px; font-family:monospace; color:#fff; }\n"
" #tallyBorder { position:fixed; top:0; left:0; right:0; bottom:0; border:10px solid transparent; pointer-events:none; z-index:90; box-sizing:border-box; transition:border-color 0.2s ease, box-shadow 0.2s ease; }\n"
" body.tally-program #tallyBorder { border-color:#ff1133; box-shadow:inset 0 0 50px rgba(255,17,51,0.85); }\n"
" body.tally-preview #tallyBorder { border-color:#00e64d; box-shadow:inset 0 0 50px rgba(0,230,77,0.85); }\n"
" body.tally-recording #tallyBorder { border-color:#f59e0b; box-shadow:inset 0 0 50px rgba(245,158,11,0.85); }\n"
" body.tally-program.tally-recording #tallyBorder { border-color:#ff1133; box-shadow:inset 0 0 50px rgba(255,17,51,0.85), inset 0 0 100px rgba(245,158,11,0.6); }\n"
" body.tally-preview.tally-recording #tallyBorder { border-color:#00e64d; box-shadow:inset 0 0 50px rgba(0,230,77,0.85), inset 0 0 100px rgba(245,158,11,0.6); }\n"
" body.tally-program #dot { background:#ff1133 !important; box-shadow:0 0 16px #ff1133 !important; }\n"
" body.tally-preview #dot { background:#00e64d !important; box-shadow:0 0 16px #00e64d !important; }\n"
" body.tally-recording #dot { background:#f59e0b !important; box-shadow:0 0 16px #f59e0b !important; }\n"
" #recPill { display:none; align-items:center; gap:4px; background:#b91c1c; color:#fff; font-weight:700; font-size:11px; padding:2px 8px; border-radius:12px; margin-left:6px; animation:blinkRec 1.2s infinite; }\n"
" @keyframes blinkRec { 0%, 100% { opacity:1; } 50% { opacity:0.3; } }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div id=\"wrap\">\n"
"  <div id=\"tallyBorder\"></div>\n"
"  <video id=\"v\" autoplay playsinline muted></video>\n"
"  <div id=\"hud\">\n"
"    <div class=\"badge\">\n"
"      <div id=\"dot\" class=\"dot\"></div>\n"
"      <span id=\"title\">OPENAVX CAM</span>\n"
"      <span id=\"recPill\">&#x25cf; REC</span>\n"
"      <span id=\"stats\">Ready</span>\n"
"    </div>\n"
"  </div>\n"
"  <div id=\"secWarn\">\n"
"    <b>Secure Context Required</b><br>\n"
"    Mobile browsers require HTTPS or localhost to access camera.<br>\n"
"    &bull; <b>USB (Recommended &bull; 0 latency):</b> Run on laptop: <code>adb reverse tcp:8080 tcp:8080</code> then open <code>http://localhost:8080/camera</code><br>\n"
"    &bull; <b>Wi-Fi (Chrome Android):</b> Open <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>, add this URL and restart Chrome.\n"
"  </div>\n"
"  <div id=\"controls\">\n"
"    <button id=\"btnFlip\" class=\"btn\" title=\"Flip Camera\">&#x21bb; Flip</button>\n"
"    <select id=\"selRes\">\n"
"      <option value=\"1080\" selected>1080p</option>\n"
"      <option value=\"720\">720p</option>\n"
"      <option value=\"2160\">4K</option>\n"
"      <option value=\"480\">480p</option>\n"
"    </select>\n"
"    <button id=\"btnStream\" class=\"btn btn-stream\">Start Cam</button>\n"
"  </div>\n"
"</div>\n"
"<canvas id=\"cvs\" style=\"display:none\"></canvas>\n"
"<script>\n"
"(function(){\n"
"  const v = document.getElementById('v');\n"
"  const dot = document.getElementById('dot');\n"
"  const stats = document.getElementById('stats');\n"
"  const btnStream = document.getElementById('btnStream');\n"
"  const btnFlip = document.getElementById('btnFlip');\n"
"  const selRes = document.getElementById('selRes');\n"
"  const secWarn = document.getElementById('secWarn');\n"
"  const cvs = document.getElementById('cvs');\n"
"  const ctx = cvs.getContext('2d');\n"
"\n"
"  let ws = null, stream = null, enc = null, isStreaming = false;\n"
"  let facingMode = 'environment', curW = 1920, curH = 1080;\n"
"  let frames = 0, bytes = 0, lastTime = performance.now(), frameIdx = 0;\n"
"  let lastFrameTime = 0;\n"
"  const frameIntervalMs = 1000 / 30;\n"
"  let codecName = 'Detecting...';\n"
"  let wakeLock = null;\n"
"\n"
"  async function acquireWakeLock() {\n"
"    if ('wakeLock' in navigator) {\n"
"      try { wakeLock = await navigator.wakeLock.request('screen'); } catch(e){}\n"
"    }\n"
"  }\n"
"  function releaseWakeLock() {\n"
"    if (wakeLock) {\n"
"      try { wakeLock.release(); } catch(e){}\n"
"      wakeLock = null;\n"
"    }\n"
"  }\n"
"\n"
"  if (!window.isSecureContext && location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') {\n"
"    secWarn.style.display = 'block';\n"
"  }\n"
"\n"
"  setInterval(() => {\n"
"    if (!isStreaming) return;\n"
"    const now = performance.now();\n"
"    const dt = (now - lastTime) / 1000;\n"
"    const fps = (frames / dt).toFixed(1);\n"
"    const mbps = ((bytes * 8) / (dt * 1000000)).toFixed(2);\n"
"    frames = 0; bytes = 0; lastTime = now;\n"
"    stats.textContent = `${curW}x${curH} | ${fps} fps | ${mbps} Mb/s | ${codecName}`;\n"
"  }, 1000);\n"
"\n"
"  function connectWS() {\n"
"    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
"    ws = new WebSocket(`${proto}//${location.host}/camera/ws`);\n"
"    ws.binaryType = 'arraybuffer';\n"
"    ws.onopen = () => {\n"
"      dot.className = isStreaming ? 'dot live' : 'dot warn';\n"
"    };\n"
"    ws.onmessage = (evt) => {\n"
"      try {\n"
"        const msg = JSON.parse(evt.data);\n"
"        if (msg.type === 'tally') {\n"
"          document.body.classList.remove('tally-program', 'tally-preview', 'tally-recording');\n"
"          const title = document.getElementById('title');\n"
"          const recPill = document.getElementById('recPill');\n"
"          if (recPill) recPill.style.display = msg.recording ? 'inline-flex' : 'none';\n"
"          if (msg.recording) document.body.classList.add('tally-recording');\n"
"          if (msg.program) {\n"
"            document.body.classList.add('tally-program');\n"
"            if (title) title.textContent = msg.recording ? 'LIVE + REC' : 'ON AIR (LIVE)';\n"
"          } else if (msg.preview) {\n"
"            document.body.classList.add('tally-preview');\n"
"            if (title) title.textContent = msg.recording ? 'CUED + REC' : 'PREVIEW (CUED)';\n"
"          } else if (msg.recording) {\n"
"            if (title) title.textContent = 'RECORDING';\n"
"          } else {\n"
"            if (title) title.textContent = 'OPENAVX CAM';\n"
"          }\n"
"        }\n"
"      } catch(e) {}\n"
"    };\n"
"    ws.onclose = () => {\n"
"      dot.className = 'dot err';\n"
"      document.body.classList.remove('tally-program', 'tally-preview', 'tally-recording');\n"
"      const recPill = document.getElementById('recPill');\n"
"      if (recPill) recPill.style.display = 'none';\n"
"      if (isStreaming) setTimeout(connectWS, 1500);\n"
"    };\n"
"    ws.onerror = () => { dot.className = 'dot err'; };\n"
"  }\n"
"\n"
"  async function startCapture() {\n"
"    const resVal = parseInt(selRes.value) || 1080;\n"
"    if (resVal === 2160) { curW = 3840; curH = 2160; }\n"
"    else if (resVal === 720) { curW = 1280; curH = 720; }\n"
"    else if (resVal === 480) { curW = 854; curH = 480; }\n"
"    else { curW = 1920; curH = 1080; }\n"
"\n"
"    try {\n"
"      if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {\n"
"        secWarn.style.display = 'block';\n"
"        throw new Error('getUserMedia unavailable in this context');\n"
"      }\n"
"      stream = await navigator.mediaDevices.getUserMedia({\n"
"        video: { facingMode: { ideal: facingMode }, width: { ideal: curW }, height: { ideal: curH }, frameRate: { ideal: 30 } },\n"
"        audio: false\n"
"      });\n"
"      v.srcObject = stream;\n"
"      await v.play();\n"
"      curW = v.videoWidth || curW;\n"
"      curH = v.videoHeight || curH;\n"
"      if (facingMode === 'user') v.style.transform = 'scaleX(-1)';\n"
"      else v.style.transform = 'scaleX(1)';\n"
"\n"
"      await acquireWakeLock();\n"
"      if (!ws || ws.readyState !== 1) connectWS();\n"
"\n"
"      /* Setup Hardware H.264 Encoder if WebCodecs supported */\n"
"      if (window.VideoEncoder) {\n"
"        try {\n"
"          enc = new VideoEncoder({\n"
"            output: (chunk) => {\n"
"              if (!ws || ws.readyState !== 1) return;\n"
"              const chunkLen = chunk.byteLength;\n"
"              const pkt = new Uint8Array(16 + chunkLen);\n"
"              const dv = new DataView(pkt.buffer);\n"
"              dv.setUint32(0, 0x5856414F, true); /* 'OAVX' Little Endian */\n"
"              dv.setUint32(4, 0x34363248, true); /* 'H264' Little Endian */\n"
"              dv.setUint16(8, curW, true);\n"
"              dv.setUint16(10, curH, true);\n"
"              dv.setUint32(12, chunk.type === 'key' ? 1 : 0, true);\n"
"              chunk.copyTo(new Uint8Array(pkt.buffer, 16, chunkLen));\n"
"              ws.send(pkt.buffer);\n"
"              frames++; bytes += pkt.byteLength;\n"
"            },\n"
"            error: (e) => { console.warn('VideoEncoder error', e); enc = null; }\n"
"          });\n"
"          const safeW = Math.max(320, Math.floor(curW / 16) * 16);\n"
"          const safeH = Math.max(240, Math.floor(curH / 16) * 16);\n"
"          const candProfiles = (safeW > 1280 || safeH > 720)\n"
"            ? ['avc1.420028', 'avc1.42e028', 'avc1.4d0028', 'avc1.42001f']\n"
"            : ['avc1.42001f', 'avc1.42e01f', 'avc1.42001e', 'avc1.4d001f'];\n"
"          let confOk = false;\n"
"          for (const p of candProfiles) {\n"
"            try {\n"
"              enc.configure({\n"
"                codec: p,\n"
"                width: safeW,\n"
"                height: safeH,\n"
"                bitrate: 3500000,\n"
"                framerate: 30,\n"
"                latencyMode: 'realtime',\n"
"                avc: { format: 'annexb' }\n"
"              });\n"
"              confOk = true;\n"
"              break;\n"
"            } catch(ce) {}\n"
"          }\n"
"          if (!confOk) throw new Error('No profile supported');\n"
"          codecName = 'H.264 HW';\n"
"        } catch (e) {\n"
"          enc = null;\n"
"          codecName = 'JPEG';\n"
"        }\n"
"      } else {\n"
"        codecName = 'JPEG';\n"
"      }\n"
"\n"
"      isStreaming = true;\n"
"      btnStream.textContent = 'Stop Cam';\n"
"      btnStream.className = 'btn btn-stream active';\n"
"      dot.className = 'dot live';\n"
"      lastFrameTime = performance.now();\n"
"      pumpFrames();\n"
"    } catch (err) {\n"
"      alert('Camera error: ' + err.message);\n"
"      stopCapture();\n"
"    }\n"
"  }\n"
"\n"
"  function stopCapture() {\n"
"    isStreaming = false;\n"
"    releaseWakeLock();\n"
"    if (stream) {\n"
"      stream.getTracks().forEach(t => t.stop());\n"
"      stream = null;\n"
"    }\n"
"    if (enc) { try { enc.close(); } catch(e){} enc = null; }\n"
"    v.srcObject = null;\n"
"    btnStream.textContent = 'Start Cam';\n"
"    btnStream.className = 'btn btn-stream';\n"
"    dot.className = (ws && ws.readyState === 1) ? 'dot warn' : 'dot err';\n"
"    stats.textContent = 'Stopped';\n"
"  }\n"
"\n"
"  async function pumpFrames(now) {\n"
"    if (!isStreaming) return;\n"
"    if (!now) now = performance.now();\n"
"    if (now - lastFrameTime >= frameIntervalMs) {\n"
"      lastFrameTime = now;\n"
"      if (enc && enc.state === 'configured') {\n"
"        if (enc.encodeQueueSize <= 2 && (!ws || ws.bufferedAmount < 512 * 1024)) {\n"
"          try {\n"
"            const vf = new VideoFrame(v, { timestamp: Math.round(now * 1000) });\n"
"            const needKey = (frameIdx++ % 60 === 0);\n"
"            enc.encode(vf, { keyFrame: needKey });\n"
"            vf.close();\n"
"          } catch(e) {}\n"
"        }\n"
"      } else {\n"
"        if (!ws || ws.bufferedAmount < 512 * 1024) {\n"
"          cvs.width = curW;\n"
"          cvs.height = curH;\n"
"          ctx.drawImage(v, 0, 0, curW, curH);\n"
"          cvs.toBlob((blob) => {\n"
"            if (!blob || !ws || ws.readyState !== 1 || !isStreaming) return;\n"
"            blob.arrayBuffer().then((ab) => {\n"
"              const pkt = new Uint8Array(16 + ab.byteLength);\n"
"              const dv = new DataView(pkt.buffer);\n"
"              dv.setUint32(0, 0x5856414F, true); /* 'OAVX' */\n"
"              dv.setUint32(4, 0x47504A4D, true); /* 'MJPG' */\n"
"              dv.setUint16(8, curW, true);\n"
"              dv.setUint16(10, curH, true);\n"
"              dv.setUint32(12, 1, true);\n"
"              pkt.set(new Uint8Array(ab), 16);\n"
"              ws.send(pkt.buffer);\n"
"              frames++; bytes += pkt.byteLength;\n"
"            });\n"
"          }, 'image/jpeg', 0.80);\n"
"        }\n"
"      }\n"
"    }\n"
"    if (isStreaming) requestAnimationFrame(pumpFrames);\n"
"  }\n"
"\n"
"  btnStream.onclick = () => {\n"
"    if (isStreaming) stopCapture();\n"
"    else startCapture();\n"
"  };\n"
"\n"
"  btnFlip.onclick = () => {\n"
"    facingMode = (facingMode === 'environment') ? 'user' : 'environment';\n"
"    if (isStreaming) {\n"
"      stopCapture();\n"
"      startCapture();\n"
"    }\n"
"  };\n"
"\n"
"  connectWS();\n"
"})();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ========================================================================= */
/* 1. ISOBMFF BOX WRITING HELPERS                                            */
/* ========================================================================= */

static inline void avx_put_u32(uint8_t **p, uint32_t val) {
    (*p)[0] = (uint8_t)(val >> 24);
    (*p)[1] = (uint8_t)(val >> 16);
    (*p)[2] = (uint8_t)(val >> 8);
    (*p)[3] = (uint8_t)(val);
    *p += 4;
}

static inline void avx_put_u16(uint8_t **p, uint16_t val) {
    (*p)[0] = (uint8_t)(val >> 8);
    (*p)[1] = (uint8_t)(val);
    *p += 2;
}

static inline void avx_put_u8(uint8_t **p, uint8_t val) {
    (*p)[0] = val;
    *p += 1;
}

static inline void avx_put_fourcc(uint8_t **p, const char *fourcc) {
    memcpy(*p, fourcc, 4);
    *p += 4;
}

static inline void avx_put_u64(uint8_t **p, uint64_t val) {
    avx_put_u32(p, (uint32_t)(val >> 32));
    avx_put_u32(p, (uint32_t)(val & 0xFFFFFFFFULL));
}

static inline void avx_box_patch_size(uint8_t *box_start, uint8_t *box_end) {
    uint32_t sz = (uint32_t)(box_end - box_start);
    box_start[0] = (uint8_t)(sz >> 24);
    box_start[1] = (uint8_t)(sz >> 16);
    box_start[2] = (uint8_t)(sz >> 8);
    box_start[3] = (uint8_t)(sz);
}

/* ========================================================================= */
/* 2. PURE C99 fMP4 GENERATION (ftyp, moov, moof, mdat)                      */
/* ========================================================================= */

/*
 * Generates ISO/IEC 14496-12 fMP4 initialization segment (ftyp and moov boxes)
 * containing track metadata and avcC/hvcC codec configuration records.
 */
OPENAVX_API size_t avx_fmp4_generate_init_segment(
    avx_rtsp_codec_e codec,
    uint32_t width,
    uint32_t height,
    const uint8_t *sps,
    size_t sps_len,
    const uint8_t *pps,
    size_t pps_len,
    const uint8_t *vps,
    size_t vps_len,
    uint8_t *out_buf,
    size_t out_max
) {
    if (!out_buf || out_max < 1024) return 0;
    if (width == 0) width = 1920;
    if (height == 0) height = 1080;

    /* Strip leading Annex B start code from SPS/PPS if present */
    if (sps && sps_len >= 4) {
        if (sps[0] == 0 && sps[1] == 0 && sps[2] == 1) {
            sps += 3; sps_len -= 3;
        } else if (sps[0] == 0 && sps[1] == 0 && sps[2] == 0 && sps[3] == 1) {
            sps += 4; sps_len -= 4;
        }
    }
    if (pps && pps_len >= 4) {
        if (pps[0] == 0 && pps[1] == 0 && pps[2] == 1) {
            pps += 3; pps_len -= 3;
        } else if (pps[0] == 0 && pps[1] == 0 && pps[2] == 0 && pps[3] == 1) {
            pps += 4; pps_len -= 4;
        }
    }
    if (vps && vps_len >= 4) {
        if (vps[0] == 0 && vps[1] == 0 && vps[2] == 1) {
            vps += 3; vps_len -= 3;
        } else if (vps[0] == 0 && vps[1] == 0 && vps[2] == 0 && vps[3] == 1) {
            vps += 4; vps_len -= 4;
        }
    }

    uint8_t *p = out_buf;

    /* 1. ftyp Box (32 bytes) */
    uint8_t *b_ftyp = p;
    avx_put_u32(&p, 0); /* size */
    avx_put_fourcc(&p, "ftyp");
    avx_put_fourcc(&p, "isom"); /* major brand */
    avx_put_u32(&p, 0x00000200); /* minor version */
    avx_put_fourcc(&p, "isom");
    avx_put_fourcc(&p, "iso2");
    avx_put_fourcc(&p, (codec == AVX_RTSP_CODEC_HEVC) ? "hvc1" : ((codec == AVX_RTSP_CODEC_AV1) ? "av01" : "avc1"));
    avx_put_fourcc(&p, "mp41");
    avx_box_patch_size(b_ftyp, p);

    /* 2. moov Box */
    uint8_t *b_moov = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "moov");

    /* 2.1. mvhd Box (108 bytes) */
    uint8_t *b_mvhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "mvhd");
    avx_put_u8(&p, 0); /* version = 0 */
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0); /* flags = 0 */
    avx_put_u32(&p, 0); /* creation_time */
    avx_put_u32(&p, 0); /* modification_time */
    avx_put_u32(&p, 90000); /* timescale = 90kHz */
    avx_put_u32(&p, 0); /* duration = 0 (live fragment) */
    avx_put_u32(&p, 0x00010000); /* rate = 1.0 */
    avx_put_u16(&p, 0x0100); /* volume = 1.0 */
    memset(p, 0, 10); p += 10; /* reserved */
    /* Identity matrix (36 bytes) */
    avx_put_u32(&p, 0x00010000); avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_put_u32(&p, 0); avx_put_u32(&p, 0x00010000); avx_put_u32(&p, 0);
    avx_put_u32(&p, 0); avx_put_u32(&p, 0); avx_put_u32(&p, 0x40000000);
    memset(p, 0, 24); p += 24; /* pre_defined */
    avx_put_u32(&p, 2); /* next_track_ID = 2 */
    avx_box_patch_size(b_mvhd, p);

    /* 2.2. trak Box */
    uint8_t *b_trak = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "trak");

    /* 2.2.1. tkhd Box (92 bytes) */
    uint8_t *b_tkhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "tkhd");
    avx_put_u8(&p, 0); /* version = 0 */
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 7); /* flags = TrackEnabled | InMovie | InPreview */
    avx_put_u32(&p, 0); /* creation_time */
    avx_put_u32(&p, 0); /* modification_time */
    avx_put_u32(&p, 1); /* track_id = 1 */
    avx_put_u32(&p, 0); /* reserved */
    avx_put_u32(&p, 0); /* duration */
    memset(p, 0, 8); p += 8; /* reserved */
    avx_put_u16(&p, 0); /* layer */
    avx_put_u16(&p, 0); /* alternate_group */
    avx_put_u16(&p, 0); /* volume (video = 0) */
    avx_put_u16(&p, 0); /* reserved */
    /* Matrix (36 bytes) */
    avx_put_u32(&p, 0x00010000); avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_put_u32(&p, 0); avx_put_u32(&p, 0x00010000); avx_put_u32(&p, 0);
    avx_put_u32(&p, 0); avx_put_u32(&p, 0); avx_put_u32(&p, 0x40000000);
    avx_put_u32(&p, width << 16);  /* width (16.16 fixed) */
    avx_put_u32(&p, height << 16); /* height (16.16 fixed) */
    avx_box_patch_size(b_tkhd, p);

    /* 2.2.2. mdia Box */
    uint8_t *b_mdia = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "mdia");

    /* mdhd (32 bytes) */
    uint8_t *b_mdhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "mdhd");
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0);
    avx_put_u32(&p, 0); /* creation_time */
    avx_put_u32(&p, 0); /* modification_time */
    avx_put_u32(&p, 90000); /* timescale */
    avx_put_u32(&p, 0); /* duration */
    avx_put_u16(&p, 0x55C4); /* language = 'und' */
    avx_put_u16(&p, 0);
    avx_box_patch_size(b_mdhd, p);

    /* hdlr (45 bytes) */
    uint8_t *b_hdlr = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "hdlr");
    avx_put_u32(&p, 0); /* ver/flags */
    avx_put_u32(&p, 0); /* pre_defined */
    avx_put_fourcc(&p, "vide"); /* handler_type = video */
    memset(p, 0, 12); p += 12; /* reserved */
    const char *hdlr_name = "VideoHandler";
    memcpy(p, hdlr_name, strlen(hdlr_name) + 1);
    p += strlen(hdlr_name) + 1;
    avx_box_patch_size(b_hdlr, p);

    /* minf Box */
    uint8_t *b_minf = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "minf");

    /* vmhd Box (20 bytes) */
    uint8_t *b_vmhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "vmhd");
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 1);
    avx_put_u16(&p, 0); /* graphicsmode */
    memset(p, 0, 6); p += 6; /* opcolor */
    avx_box_patch_size(b_vmhd, p);

    /* dinf Box (36 bytes) */
    uint8_t *b_dinf = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "dinf");
    /* dref */
    uint8_t *b_dref = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "dref");
    avx_put_u32(&p, 0); /* ver/flags */
    avx_put_u32(&p, 1); /* entry_count = 1 */
    /* url */
    uint8_t *b_url = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "url ");
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 1); /* self-contained */
    avx_box_patch_size(b_url, p);
    avx_box_patch_size(b_dref, p);
    avx_box_patch_size(b_dinf, p);

    /* stbl Box */
    uint8_t *b_stbl = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "stbl");

    /* stsd Box */
    uint8_t *b_stsd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "stsd");
    avx_put_u32(&p, 0); /* ver/flags */
    avx_put_u32(&p, 1); /* entry_count = 1 */

    if (codec == AVX_RTSP_CODEC_H264) {
        /* avc1 Box */
        uint8_t *b_avc1 = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "avc1");
        memset(p, 0, 6); p += 6; /* reserved */
        avx_put_u16(&p, 1); /* data_reference_index */
        avx_put_u16(&p, 0); avx_put_u16(&p, 0); /* pre_defined / reserved */
        memset(p, 0, 12); p += 12; /* pre_defined */
        avx_put_u16(&p, (uint16_t)width);
        avx_put_u16(&p, (uint16_t)height);
        avx_put_u32(&p, 0x00480000); /* 72 dpi horiz */
        avx_put_u32(&p, 0x00480000); /* 72 dpi vert */
        avx_put_u32(&p, 0); /* reserved */
        avx_put_u16(&p, 1); /* frame_count */
        /* compressorname (32 bytes) */
        p[0] = 7;
        memcpy(p + 1, "OpenAVX", 7);
        memset(p + 8, 0, 24);
        p += 32;
        avx_put_u16(&p, 0x0018); /* depth = 24 */
        avx_put_u16(&p, 0xFFFF); /* pre_defined = -1 */

        /* avcC Box */
        uint8_t *b_avcC = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "avcC");
        avx_put_u8(&p, 1); /* configurationVersion = 1 */
        avx_put_u8(&p, (sps && sps_len > 1) ? sps[1] : 0x64); /* AVCProfileIndication (High = 100) */
        avx_put_u8(&p, (sps && sps_len > 2) ? sps[2] : 0x00); /* profile_compatibility */
        avx_put_u8(&p, (sps && sps_len > 3) ? sps[3] : 0x28); /* AVCLevelIndication (Level 4.0 = 40) */
        avx_put_u8(&p, 0xFF); /* lengthSizeMinusOne = 3 (4 bytes) | 0xFC */
        avx_put_u8(&p, (sps && sps_len > 0) ? 0xE1 : 0xE0); /* numOfSequenceParameterSets = 1 | 0xE0 */
        if (sps && sps_len > 0) {
            avx_put_u16(&p, (uint16_t)sps_len);
            memcpy(p, sps, sps_len);
            p += sps_len;
        }
        avx_put_u8(&p, (pps && pps_len > 0) ? 1 : 0); /* numOfPictureParameterSets */
        if (pps && pps_len > 0) {
            avx_put_u16(&p, (uint16_t)pps_len);
            memcpy(p, pps, pps_len);
            p += pps_len;
        }
        avx_box_patch_size(b_avcC, p);
        avx_box_patch_size(b_avc1, p);
    } else if (codec == AVX_RTSP_CODEC_HEVC) {
        /* hvc1 Box */
        uint8_t *b_hvc1 = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "hvc1");
        memset(p, 0, 6); p += 6;
        avx_put_u16(&p, 1);
        avx_put_u16(&p, 0); avx_put_u16(&p, 0);
        memset(p, 0, 12); p += 12;
        avx_put_u16(&p, (uint16_t)width);
        avx_put_u16(&p, (uint16_t)height);
        avx_put_u32(&p, 0x00480000);
        avx_put_u32(&p, 0x00480000);
        avx_put_u32(&p, 0);
        avx_put_u16(&p, 1);
        p[0] = 7;
        memcpy(p + 1, "OpenAVX", 7);
        memset(p + 8, 0, 24);
        p += 32;
        avx_put_u16(&p, 0x0018);
        avx_put_u16(&p, 0xFFFF);

        /* hvcC Box */
        uint8_t *b_hvcC = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "hvcC");
        avx_put_u8(&p, 1); /* configurationVersion */
        avx_put_u8(&p, 0x01); /* general_profile_space(0) | general_tier_flag(0) | Main profile(1) */
        avx_put_u32(&p, 0x60000000); /* general_profile_compatibility_flags */
        memset(p, 0, 6); p += 6; /* general_constraint_indicator_flags */
        avx_put_u8(&p, 93); /* general_level_idc = 93 (Level 3.1) */
        avx_put_u16(&p, 0xF000); /* min_spatial_segmentation_idc */
        avx_put_u8(&p, 0xFC); /* parallelismType */
        avx_put_u8(&p, 0xFD); /* chromaFormat = 4:2:0 */
        avx_put_u8(&p, 0xF8); /* bitDepthLumaMinus8 = 0 */
        avx_put_u8(&p, 0xF8); /* bitDepthChromaMinus8 = 0 */
        avx_put_u16(&p, 0); /* avgFrameRate */
        avx_put_u8(&p, 0x0F); /* constantFrameRate=0, numTemporalLayers=1, temporalIdNested=1, lengthSizeMinusOne=3 */

        uint8_t num_arrays = 0;
        if (vps && vps_len > 0) num_arrays++;
        if (sps && sps_len > 0) num_arrays++;
        if (pps && pps_len > 0) num_arrays++;
        avx_put_u8(&p, num_arrays);

        if (vps && vps_len > 0) {
            avx_put_u8(&p, 0x80 | 32); /* completeness=1, type=32 (VPS) */
            avx_put_u16(&p, 1); /* count=1 */
            avx_put_u16(&p, (uint16_t)vps_len);
            memcpy(p, vps, vps_len);
            p += vps_len;
        }
        if (sps && sps_len > 0) {
            avx_put_u8(&p, 0x80 | 33); /* completeness=1, type=33 (SPS) */
            avx_put_u16(&p, 1);
            avx_put_u16(&p, (uint16_t)sps_len);
            memcpy(p, sps, sps_len);
            p += sps_len;
        }
        if (pps && pps_len > 0) {
            avx_put_u8(&p, 0x80 | 34); /* completeness=1, type=34 (PPS) */
            avx_put_u16(&p, 1);
            avx_put_u16(&p, (uint16_t)pps_len);
            memcpy(p, pps, pps_len);
            p += pps_len;
        }
        avx_box_patch_size(b_hvcC, p);
        avx_box_patch_size(b_hvc1, p);
    } else {
        /* av01 Box for AV1 */
        uint8_t *b_av01 = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "av01");
        memset(p, 0, 6); p += 6;
        avx_put_u16(&p, 1);
        avx_put_u16(&p, 0); avx_put_u16(&p, 0);
        memset(p, 0, 12); p += 12;
        avx_put_u16(&p, (uint16_t)width);
        avx_put_u16(&p, (uint16_t)height);
        avx_put_u32(&p, 0x00480000);
        avx_put_u32(&p, 0x00480000);
        avx_put_u32(&p, 0);
        avx_put_u16(&p, 1);
        p[0] = 7;
        memcpy(p + 1, "OpenAVX", 7);
        memset(p + 8, 0, 24);
        p += 32;
        avx_put_u16(&p, 0x0018);
        avx_put_u16(&p, 0xFFFF);

        /* av1C Box (4 bytes header + config) */
        uint8_t *b_av1C = p;
        avx_put_u32(&p, 0);
        avx_put_fourcc(&p, "av1C");
        avx_put_u8(&p, 0x81); /* marker=1, version=1 */
        avx_put_u8(&p, 0x00); /* seq_profile=0, seq_level_idx_0=0 */
        avx_put_u8(&p, 0x00); /* tier=0, depth=8bit, mono=0, 4:2:0 */
        avx_put_u8(&p, 0x00);
        if (sps && sps_len > 0) {
            memcpy(p, sps, sps_len);
            p += sps_len;
        }
        avx_box_patch_size(b_av1C, p);
        avx_box_patch_size(b_av01, p);
    }

    avx_box_patch_size(b_stsd, p);

    /* Empty table boxes required by ISOBMFF specification */
    /* stts (16 bytes) */
    uint8_t *b_stts = p;
    avx_put_u32(&p, 0); avx_put_fourcc(&p, "stts");
    avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_box_patch_size(b_stts, p);

    /* stsc (16 bytes) */
    uint8_t *b_stsc = p;
    avx_put_u32(&p, 0); avx_put_fourcc(&p, "stsc");
    avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_box_patch_size(b_stsc, p);

    /* stsz (20 bytes) */
    uint8_t *b_stsz = p;
    avx_put_u32(&p, 0); avx_put_fourcc(&p, "stsz");
    avx_put_u32(&p, 0); avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_box_patch_size(b_stsz, p);

    /* stco (16 bytes) */
    uint8_t *b_stco = p;
    avx_put_u32(&p, 0); avx_put_fourcc(&p, "stco");
    avx_put_u32(&p, 0); avx_put_u32(&p, 0);
    avx_box_patch_size(b_stco, p);

    avx_box_patch_size(b_stbl, p);
    avx_box_patch_size(b_minf, p);
    avx_box_patch_size(b_mdia, p);
    avx_box_patch_size(b_trak, p);

    /* 2.3. mvex Box (Movie Extends Box) */
    uint8_t *b_mvex = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "mvex");

    /* trex Box (32 bytes) */
    uint8_t *b_trex = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "trex");
    avx_put_u32(&p, 0); /* ver/flags */
    avx_put_u32(&p, 1); /* track_id = 1 */
    avx_put_u32(&p, 1); /* default_sample_description_index = 1 */
    avx_put_u32(&p, 0); /* default_sample_duration = 0 */
    avx_put_u32(&p, 0); /* default_sample_size = 0 */
    avx_put_u32(&p, 0); /* default_sample_flags = 0 */
    avx_box_patch_size(b_trex, p);

    avx_box_patch_size(b_mvex, p);
    avx_box_patch_size(b_moov, p);

    return (size_t)(p - out_buf);
}

/*
 * Generates fMP4 movie fragment (moof and mdat boxes) wrapping single video Access Unit
 * with sample duration, size, flags, and decode timestamp (tfdt).
 */
OPENAVX_API size_t avx_fmp4_generate_media_fragment(
    uint32_t sequence_number,
    uint64_t base_decode_time_90khz,
    uint32_t sample_duration_90khz,
    bool is_keyframe,
    const uint8_t *sample_avcc_data,
    size_t sample_avcc_len,
    uint8_t *out_buf,
    size_t out_max
) {
    if (!out_buf || !sample_avcc_data || sample_avcc_len == 0) return 0;
    if (out_max < (128 + sample_avcc_len)) return 0;
    if (sample_duration_90khz == 0) sample_duration_90khz = 1500; /* 60fps default */

    uint8_t *p = out_buf;

    /* 1. moof Box (Movie Fragment Box) */
    uint8_t *b_moof = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "moof");

    /* 1.1. mfhd Box (Movie Fragment Header, 16 bytes) */
    uint8_t *b_mfhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "mfhd");
    avx_put_u32(&p, 0); /* ver/flags */
    avx_put_u32(&p, sequence_number);
    avx_box_patch_size(b_mfhd, p);

    /* 1.2. traf Box (Track Fragment Box) */
    uint8_t *b_traf = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "traf");

    /* 1.2.1. tfhd Box (Track Fragment Header, 16 bytes) */
    uint8_t *b_tfhd = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "tfhd");
    /* flags: 0x020000 = default-base-is-moof */
    avx_put_u8(&p, 0); avx_put_u8(&p, 0x02); avx_put_u8(&p, 0); avx_put_u8(&p, 0);
    avx_put_u32(&p, 1); /* track_id = 1 */
    avx_box_patch_size(b_tfhd, p);

    /* 1.2.2. tfdt Box (Base Media Decode Time, 20 bytes) */
    uint8_t *b_tfdt = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "tfdt");
    avx_put_u8(&p, 1); /* version = 1 (64-bit timestamp) */
    avx_put_u8(&p, 0); avx_put_u8(&p, 0); avx_put_u8(&p, 0);
    avx_put_u64(&p, base_decode_time_90khz);
    avx_box_patch_size(b_tfdt, p);

    /* 1.2.3. trun Box (Track Fragment Run) */
    uint8_t *b_trun = p;
    avx_put_u32(&p, 0);
    avx_put_fourcc(&p, "trun");
    /* flags: data-offset(0x01) | duration(0x100) | size(0x200) | flags(0x400) = 0x000701 */
    avx_put_u8(&p, 0); avx_put_u8(&p, 0x00); avx_put_u8(&p, 0x07); avx_put_u8(&p, 0x01);
    avx_put_u32(&p, 1); /* sample_count = 1 */

    /* Placeholder for data_offset */
    uint8_t *p_data_offset = p;
    avx_put_u32(&p, 0);

    /* Sample 0 description */
    avx_put_u32(&p, sample_duration_90khz);
    avx_put_u32(&p, (uint32_t)sample_avcc_len);
    /* sample_flags: sync sample (keyframe) vs non-sync sample */
    uint32_t sample_flags = is_keyframe ? 0x02000000 : 0x01010000;
    avx_put_u32(&p, sample_flags);

    avx_box_patch_size(b_trun, p);
    avx_box_patch_size(b_traf, p);
    avx_box_patch_size(b_moof, p);

    /* Patch data_offset in trun: distance from moof start to mdat payload */
    uint32_t moof_size = (uint32_t)(p - b_moof);
    uint32_t data_offset = moof_size + 8; /* +8 for mdat header */
    p_data_offset[0] = (uint8_t)(data_offset >> 24);
    p_data_offset[1] = (uint8_t)(data_offset >> 16);
    p_data_offset[2] = (uint8_t)(data_offset >> 8);
    p_data_offset[3] = (uint8_t)(data_offset);

    /* 2. mdat Box (Media Data Box) */
    avx_put_u32(&p, (uint32_t)(8 + sample_avcc_len));
    avx_put_fourcc(&p, "mdat");
    memcpy(p, sample_avcc_data, sample_avcc_len);
    p += sample_avcc_len;

    return (size_t)(p - out_buf);
}

/* ========================================================================= */
/* RFC 3174 FIPS 180-1 SHA-1 ENGINE & RFC 6455 WEBSOCKET ACCEPT KEY GENERATOR */
/* ========================================================================= */

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} avx_sha1_ctx_t;

static void avx_sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (v << 1) | (v >> 31);
    }
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void avx_sha1_init(avx_sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count[0] = ctx->count[1] = 0;
}

static void avx_sha1_update(avx_sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i, j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);
    if ((j + len) > 63) {
        i = 64 - j;
        memcpy(&ctx->buffer[j], data, i);
        avx_sha1_transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            avx_sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void avx_sha1_final(avx_sha1_ctx_t *ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    uint8_t c = 0x80;
    avx_sha1_update(ctx, &c, 1);
    while ((ctx->count[0] & 504) != 448) {
        c = 0x00;
        avx_sha1_update(ctx, &c, 1);
    }
    avx_sha1_update(ctx, finalcount, 8);
    for (int i = 0; i < 20; ++i) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

OPENAVX_API void avx_sha1_hash(const uint8_t *data, size_t len, uint8_t digest[20]) {
    avx_sha1_ctx_t ctx;
    avx_sha1_init(&ctx);
    if (data && len > 0) {
        avx_sha1_update(&ctx, data, len);
    }
    avx_sha1_final(&ctx, digest);
}

/* Computes RFC 6455 Sec-WebSocket-Accept response key via SHA-1 and Base64 encoding. */
OPENAVX_API size_t avx_websocket_compute_accept(const char *client_key, char *out_accept, size_t out_max) {
    if (!client_key || !out_accept || out_max < 30) return 0;
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", client_key, guid);

    uint8_t digest[20];
    avx_sha1_hash((const uint8_t*)combined, strlen(combined), digest);

    return avx_base64_encode(digest, sizeof(digest), out_accept, out_max);
}

/* ========================================================================= */
/* 3. HTTP / fMP4 SERVER ENGINE IMPLEMENTATION                              */
/* ========================================================================= */

typedef struct avx_web_client_t {
    avx_socket_t fd;
    bool active;
    bool streaming;
    bool received_keyframe;
    bool is_websocket;
    bool is_sse;
    uint8_t *ws_buf;
    size_t ws_cap;
    size_t ws_len;
    char req_buf[16384];
    size_t req_len;
} avx_web_client_t;

struct avx_web_server_t {
    avx_socket_t listen_fd;
    uint16_t port;
    char stream_name[128];
    avx_rtsp_codec_e codec;
    void (*on_keyframe_request)(void *user_ctx);
    avx_web_ingress_frame_cb on_ingress_frame;
    void (*on_tally_change)(void *user_ctx, const oavx_tally_t *tally);
    void *user_ctx;

    oavx_tally_t current_tally;

    avx_thread_t thread;
    avx_mutex_t lock;
    volatile bool running;

    avx_web_client_t clients[AVX_WEB_MAX_CLIENTS];

    /* Parameter cache */
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
    uint8_t *vps;
    size_t vps_len;
    uint32_t width;
    uint32_t height;
    uint8_t profile;
    uint8_t level;

    /* Pre-generated Init Segment */
    uint8_t init_segment[4096];
    size_t init_segment_len;

    /* Frame Sequence & Timestamp tracking */
    uint32_t seq_num;
    uint64_t last_pts_90k;

    /* Reusable Scratch Buffers */
    uint8_t *avcc_buf;
    size_t avcc_cap;
    uint8_t *frag_buf;
    size_t frag_cap;
};

static void close_client_locked(avx_web_client_t *c) {
    if (!c->active) return;
    if (avx_socket_is_valid(c->fd)) {
        avx_socket_close(c->fd);
        c->fd = AVX_INVALID_SOCKET;
    }
    c->active = false;
    c->streaming = false;
    c->received_keyframe = false;
    c->is_websocket = false;
    c->is_sse = false;
    if (c->ws_buf) {
        free(c->ws_buf);
        c->ws_buf = NULL;
    }
    c->ws_cap = 0;
    c->ws_len = 0;
    c->req_len = 0;
}

static void send_ws_text_frame_locked(avx_socket_t fd, const char *text) {
    if (!avx_socket_is_valid(fd) || !text) return;
    size_t len = strlen(text);
    uint8_t hdr[10];
    size_t hdr_len = 2;
    hdr[0] = 0x81; /* FIN + Text frame (opcode 0x1) */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        for (int k = 0; k < 8; ++k) {
            hdr[2 + k] = (uint8_t)((len >> (56 - k * 8)) & 0xFF);
        }
        hdr_len = 10;
    }
    send(fd, (const char*)hdr, hdr_len, MSG_NOSIGNAL);
    if (len > 0) {
        send(fd, text, len, MSG_NOSIGNAL);
    }
}

static const char* avx_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack != '\0'; ++haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0) {
            return haystack;
        }
    }
    return NULL;
}

static uint8_t parse_tally_flags_from_str(const char *str, uint8_t current_flags) {
    if (!str) return current_flags;
    if (strstr(str, "off") || strstr(str, "none") || strstr(str, "clear")) {
        return OAVX_TALLY_NONE;
    }
    uint8_t flags = 0;
    if (strstr(str, "program") || strstr(str, "pgm") || strstr(str, "\"program\":true") || strstr(str, "\"program\": true")) {
        flags |= OAVX_TALLY_PROGRAM;
    }
    if (strstr(str, "preview") || strstr(str, "pvw") || strstr(str, "\"preview\":true") || strstr(str, "\"preview\": true")) {
        flags |= OAVX_TALLY_PREVIEW;
    }
    if (strstr(str, "recording") || strstr(str, "rec") || strstr(str, "\"recording\":true") || strstr(str, "\"recording\": true")) {
        flags |= OAVX_TALLY_RECORDING;
    }
    const char *f = strstr(str, "flags=");
    if (!f) f = strstr(str, "\"flags\":");
    if (f) {
        while (*f && (*f < '0' || *f > '9')) f++;
        if (*f >= '0' && *f <= '9') {
            flags = (uint8_t)atoi(f);
        }
    }
    return flags;
}

static void broadcast_tally_locked(avx_web_server_t *server, const oavx_tally_t *tally) {
    if (!server) return;
    if (tally) {
        server->current_tally = *tally;
    }
    bool prog = (server->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
    bool prev = (server->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
    bool rec  = (server->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
    char json[160];
    snprintf(json, sizeof(json),
        "{\"type\":\"tally\",\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}",
        prog ? "true" : "false",
        prev ? "true" : "false",
        rec  ? "true" : "false",
        server->current_tally.flags);

    char sse_data[256];
    snprintf(sse_data, sizeof(sse_data),
        "data: {\"type\":\"tally\",\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}\n\n",
        prog ? "true" : "false",
        prev ? "true" : "false",
        rec  ? "true" : "false",
        server->current_tally.flags);

    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        if (!server->clients[i].active || !avx_socket_is_valid(server->clients[i].fd)) continue;
        if (server->clients[i].is_websocket) {
            send_ws_text_frame_locked(server->clients[i].fd, json);
        } else if (server->clients[i].is_sse) {
            send(server->clients[i].fd, sse_data, strlen(sse_data), MSG_NOSIGNAL);
        }
    }
}

/* HTTP router servicing low-latency player UI, fMP4 live streaming, and WebSocket upgrades. */
static void handle_client_http_request(avx_web_server_t *srv, avx_web_client_t *c) {
    c->req_buf[c->req_len] = '\0';
    char method[16] = {0};
    char path[2048] = {0};

    if (sscanf(c->req_buf, "%15s %2047s", method, path) < 2) {
        close_client_locked(c);
        return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        const char *resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Connection: close\r\n\r\n";
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
        close_client_locked(c);
    } else if (strcmp(method, "GET") == 0) {
        /* Check for WebSocket upgrade (RFC 6455) */
        const char *upgrade_hdr = avx_strcasestr(c->req_buf, "Upgrade:");
        bool is_ws_upgrade = false;
        if (upgrade_hdr) {
            const char *eol = strstr(upgrade_hdr, "\r\n");
            if (!eol) eol = strchr(upgrade_hdr, '\n');
            if (eol) {
                char uval[128] = {0};
                size_t ulen = (size_t)(eol - upgrade_hdr);
                if (ulen < sizeof(uval)) {
                    memcpy(uval, upgrade_hdr, ulen);
                    if (avx_strcasestr(uval, "websocket")) {
                        is_ws_upgrade = true;
                    }
                }
            } else if (avx_strcasestr(upgrade_hdr, "websocket")) {
                is_ws_upgrade = true;
            }
        }
        const char *key_hdr = avx_strcasestr(c->req_buf, "Sec-WebSocket-Key:");
        if (is_ws_upgrade && key_hdr) {
            char client_key[64] = {0};
            key_hdr += 18; /* Skip "Sec-WebSocket-Key:" */
            while (*key_hdr == ' ') key_hdr++;
            sscanf(key_hdr, "%63s", client_key);

            char accept_str[64] = {0};
            if (avx_websocket_compute_accept(client_key, accept_str, sizeof(accept_str)) > 0) {
                char ws_resp[256];
                snprintf(ws_resp, sizeof(ws_resp),
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n\r\n",
                    accept_str);
                send(c->fd, ws_resp, strlen(ws_resp), MSG_NOSIGNAL);
                c->is_websocket = true;
                c->streaming = false;
                c->received_keyframe = false;
                c->ws_cap = 256 * 1024;
                c->ws_buf = (uint8_t*)malloc(c->ws_cap);
                c->ws_len = 0;

                /* Push initial tally status immediately upon upgrade */
                bool prog = (srv->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
                bool prev = (srv->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
                bool rec  = (srv->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
                char tally_json[160];
                snprintf(tally_json, sizeof(tally_json),
                    "{\"type\":\"tally\",\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}",
                    prog ? "true" : "false",
                    prev ? "true" : "false",
                    rec  ? "true" : "false",
                    srv->current_tally.flags);
                send_ws_text_frame_locked(c->fd, tally_json);
                return;
            } else {
                close_client_locked(c);
                return;
            }
        }

        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            char hdr[512];
            size_t html_len = strlen(k_player_html);
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n",
                html_len);
#if defined(AVX_OS_POSIX)
            struct iovec iov[2];
            iov[0].iov_base = hdr;
            iov[0].iov_len = strlen(hdr);
            iov[1].iov_base = (void*)k_player_html;
            iov[1].iov_len = html_len;
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = iov;
            msg.msg_iovlen = 2;
            (void)sendmsg(c->fd, &msg, MSG_NOSIGNAL);
#else
            send(c->fd, hdr, (int)strlen(hdr), MSG_NOSIGNAL);
            send(c->fd, k_player_html, (int)html_len, MSG_NOSIGNAL);
#endif
            close_client_locked(c);
        } else if (strcmp(path, "/camera") == 0 || strcmp(path, "/camera/") == 0 || strcmp(path, "/camera.html") == 0) {
            char hdr[512];
            size_t html_len = strlen(k_camera_html);
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n",
                html_len);
#if defined(AVX_OS_POSIX)
            struct iovec iov[2];
            iov[0].iov_base = hdr;
            iov[0].iov_len = strlen(hdr);
            iov[1].iov_base = (void*)k_camera_html;
            iov[1].iov_len = html_len;
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = iov;
            msg.msg_iovlen = 2;
            (void)sendmsg(c->fd, &msg, MSG_NOSIGNAL);
#else
            send(c->fd, hdr, (int)strlen(hdr), MSG_NOSIGNAL);
            send(c->fd, k_camera_html, (int)html_len, MSG_NOSIGNAL);
#endif
            close_client_locked(c);
        } else if (strcmp(path, "/live.mp4") == 0 || strncmp(path, "/live.mp4?", 10) == 0) {
            const char *codec_mime = "video/mp4; codecs=\"avc1.640028\"";
            if (srv->codec == AVX_RTSP_CODEC_HEVC) {
                codec_mime = "video/mp4; codecs=\"hvc1.1.6.L93.B0\"";
            } else if (srv->codec == AVX_RTSP_CODEC_AV1) {
                codec_mime = "video/mp4; codecs=\"av01.0.08M.10\"";
            }

            char hdr[512];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Expires: 0\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n",
                codec_mime);
            send(c->fd, hdr, strlen(hdr), MSG_NOSIGNAL);

            c->streaming = true;
            c->received_keyframe = false;

            /* If init segment is available, send it immediately */
            if (srv->init_segment_len > 0) {
                send(c->fd, (const char*)srv->init_segment, srv->init_segment_len, MSG_NOSIGNAL);
            }

            /* Request instant IDR keyframe from upstream encoder */
            if (srv->on_keyframe_request) {
                srv->on_keyframe_request(srv->user_ctx);
            }
        } else if (strcmp(path, "/status") == 0 || strcmp(path, "/api/status") == 0) {
            int client_count = 0;
            for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
                if (srv->clients[i].active && srv->clients[i].streaming) client_count++;
            }
            const char *cname = (srv->codec == AVX_RTSP_CODEC_HEVC) ? "HEVC" :
                               ((srv->codec == AVX_RTSP_CODEC_AV1) ? "AV1" : "H264");
            char body[256];
            snprintf(body, sizeof(body),
                "{\"status\":\"online\",\"name\":\"%s\",\"codec\":\"%s\",\"port\":%u,\"clients\":%d,\"width\":%u,\"height\":%u}\n",
                srv->stream_name, cname, srv->port, client_count, srv->width, srv->height);
            char resp_full[1024];
            snprintf(resp_full, sizeof(resp_full),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                "%s",
                strlen(body), body);
            send(c->fd, resp_full, strlen(resp_full), MSG_NOSIGNAL);
            close_client_locked(c);
        } else if (strcmp(path, "/tally") == 0 || strcmp(path, "/api/tally") == 0 ||
                   strncmp(path, "/tally?", 7) == 0 || strncmp(path, "/api/tally?", 11) == 0) {
            void (*cb)(void*, const oavx_tally_t*) = NULL;
            void *cb_ctx = NULL;
            oavx_tally_t cb_tally = {0};

            const char *q = strchr(path, '?');
            if (q && strstr(q, "set=")) {
                srv->current_tally.flags = parse_tally_flags_from_str(q, srv->current_tally.flags);
                broadcast_tally_locked(srv, &srv->current_tally);
                cb = srv->on_tally_change;
                cb_ctx = srv->user_ctx;
                cb_tally = srv->current_tally;
            }
            bool prog = (srv->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
            bool prev = (srv->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
            bool rec  = (srv->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
            char body[160];
            snprintf(body, sizeof(body),
                "{\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}\n",
                prog ? "true" : "false",
                prev ? "true" : "false",
                rec  ? "true" : "false",
                srv->current_tally.flags);
            char resp_full[512];
            snprintf(resp_full, sizeof(resp_full),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                "%s",
                strlen(body), body);
            send(c->fd, resp_full, strlen(resp_full), MSG_NOSIGNAL);
            close_client_locked(c);
            if (cb) {
                avx_mutex_unlock(&srv->lock);
                cb(cb_ctx, &cb_tally);
                avx_mutex_lock(&srv->lock);
            }
        } else if (strcmp(path, "/tally/text") == 0 || strcmp(path, "/api/tally/text") == 0) {
            bool prog = (srv->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
            bool prev = (srv->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
            bool rec  = (srv->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
            const char *status_str = "OFF";
            if (prog && rec) status_str = "PROGRAM+RECORDING";
            else if (prog) status_str = "PROGRAM";
            else if (prev && rec) status_str = "PREVIEW+RECORDING";
            else if (prev) status_str = "PREVIEW";
            else if (rec) status_str = "RECORDING";

            char resp_full[256];
            snprintf(resp_full, sizeof(resp_full),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                "%s\n",
                strlen(status_str) + 1, status_str);
            send(c->fd, resp_full, strlen(resp_full), MSG_NOSIGNAL);
            close_client_locked(c);
        } else if (strcmp(path, "/tally/sse") == 0 || strcmp(path, "/api/tally/sse") == 0 || strcmp(path, "/events") == 0) {
            const char *sse_hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache, no-transform\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n";
            send(c->fd, sse_hdr, strlen(sse_hdr), MSG_NOSIGNAL);
            c->is_sse = true;
            c->streaming = false;
            c->received_keyframe = false;

            bool prog = (srv->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
            bool prev = (srv->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
            bool rec  = (srv->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
            char initial_event[256];
            snprintf(initial_event, sizeof(initial_event),
                "data: {\"type\":\"tally\",\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}\n\n",
                prog ? "true" : "false",
                prev ? "true" : "false",
                rec  ? "true" : "false",
                srv->current_tally.flags);
            send(c->fd, initial_event, strlen(initial_event), MSG_NOSIGNAL);
            return;
        } else {
            const char *resp =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 9\r\n"
                "Connection: close\r\n\r\n"
                "Not Found";
            send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
            close_client_locked(c);
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/tally") == 0 || strcmp(path, "/api/tally") == 0 ||
            strcmp(path, "/tally/set") == 0 || strcmp(path, "/api/tally/set") == 0) {
            const char *body_start = strstr(c->req_buf, "\r\n\r\n");
            if (body_start) body_start += 4;
            srv->current_tally.flags = parse_tally_flags_from_str(body_start ? body_start : c->req_buf, srv->current_tally.flags);
            broadcast_tally_locked(srv, &srv->current_tally);
            void (*cb)(void*, const oavx_tally_t*) = srv->on_tally_change;
            void *cb_ctx = srv->user_ctx;
            oavx_tally_t cb_tally = srv->current_tally;

            bool prog = (srv->current_tally.flags & OAVX_TALLY_PROGRAM) != 0;
            bool prev = (srv->current_tally.flags & OAVX_TALLY_PREVIEW) != 0;
            bool rec  = (srv->current_tally.flags & OAVX_TALLY_RECORDING) != 0;
            char body[160];
            snprintf(body, sizeof(body),
                "{\"ok\":true,\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}\n",
                prog ? "true" : "false",
                prev ? "true" : "false",
                rec  ? "true" : "false",
                srv->current_tally.flags);
            char resp_full[512];
            snprintf(resp_full, sizeof(resp_full),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                "%s",
                strlen(body), body);
            send(c->fd, resp_full, strlen(resp_full), MSG_NOSIGNAL);
            close_client_locked(c);
            if (cb) {
                avx_mutex_unlock(&srv->lock);
                cb(cb_ctx, &cb_tally);
                avx_mutex_lock(&srv->lock);
            }
        } else {
            const char *resp =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 9\r\n"
                "Connection: close\r\n\r\n"
                "Not Found";
            send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
            close_client_locked(c);
        }
    } else {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
        close_client_locked(c);
    }
}

static void* avx_web_server_worker(void *arg) {
    avx_web_server_t *srv = (avx_web_server_t*)arg;

    while (srv->running) {
        avx_pollfd_t pfds[AVX_WEB_MAX_CLIENTS + 1];
        int client_indices[AVX_WEB_MAX_CLIENTS + 1];
        int nfds = 0;

        avx_mutex_lock(&srv->lock);
        if (avx_socket_is_valid(srv->listen_fd)) {
            pfds[nfds].fd = srv->listen_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            client_indices[nfds] = -1;
            nfds++;
        }

        for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
            if (srv->clients[i].active && (!srv->clients[i].streaming || srv->clients[i].is_websocket || srv->clients[i].is_sse) && avx_socket_is_valid(srv->clients[i].fd)) {
                pfds[nfds].fd = srv->clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                client_indices[nfds] = i;
                nfds++;
            }
        }
        avx_mutex_unlock(&srv->lock);

        if (nfds == 0) {
            avx_sleep_ms(10);
            continue;
        }

        int pr = avx_poll(pfds, (avx_nfds_t)nfds, 30);
        if (pr <= 0) continue;

        for (int j = 0; j < nfds; ++j) {
            if (!(pfds[j].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            if (client_indices[j] == -1) {
                /* New connection on HTTP listener */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                avx_socket_t cfd = accept(srv->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
                if (avx_socket_is_valid(cfd)) {
                    avx_socket_set_tcp_nodelay(cfd, true);
                    avx_socket_set_nonblocking(cfd, true);

                    avx_mutex_lock(&srv->lock);
                    int slot = -1;
                    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
                        if (!srv->clients[i].active) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot >= 0) {
                        srv->clients[slot].fd = cfd;
                        srv->clients[slot].active = true;
                        srv->clients[slot].streaming = false;
                        srv->clients[slot].received_keyframe = false;
                        srv->clients[slot].is_websocket = false;
                        srv->clients[slot].is_sse = false;
                        srv->clients[slot].ws_buf = NULL;
                        srv->clients[slot].ws_cap = 0;
                        srv->clients[slot].ws_len = 0;
                        srv->clients[slot].req_len = 0;
                    } else {
                        avx_socket_close(cfd);
                    }
                    avx_mutex_unlock(&srv->lock);
                }
            } else {
                int idx = client_indices[j];
                avx_mutex_lock(&srv->lock);
                if (srv->clients[idx].active) {
                    avx_web_client_t *c = &srv->clients[idx];
                    if (c->is_websocket) {
                        if (!c->ws_buf) {
                            c->ws_cap = 256 * 1024;
                            c->ws_buf = (uint8_t*)malloc(c->ws_cap);
                            c->ws_len = 0;
                        }
                        if (c->ws_len + 65536 > c->ws_cap) {
                            size_t ncap = c->ws_cap * 2;
                            if (ncap > 16 * 1024 * 1024) ncap = 16 * 1024 * 1024;
                            uint8_t *nb = (uint8_t*)realloc(c->ws_buf, ncap);
                            if (nb) {
                                c->ws_buf = nb;
                                c->ws_cap = ncap;
                            }
                        }

                        ssize_t n = recv(c->fd, (char*)(c->ws_buf + c->ws_len), c->ws_cap - c->ws_len, 0);
                        if (n > 0) {
                            c->ws_len += (size_t)n;
                            while (c->ws_len >= 2) {
                                uint8_t b0 = c->ws_buf[0];
                                uint8_t b1 = c->ws_buf[1];
                                uint8_t opcode = b0 & 0x0F;
                                bool masked = (b1 & 0x80) != 0;
                                uint64_t payload_len = b1 & 0x7F;
                                size_t header_len = 2;

                                if (payload_len == 126) {
                                    if (c->ws_len < 4) break;
                                    payload_len = ((uint64_t)c->ws_buf[2] << 8) | c->ws_buf[3];
                                    header_len = 4;
                                } else if (payload_len == 127) {
                                    if (c->ws_len < 10) break;
                                    payload_len = 0;
                                    for (int k = 0; k < 8; ++k) {
                                        payload_len = (payload_len << 8) | c->ws_buf[2 + k];
                                    }
                                    header_len = 10;
                                }

                                uint8_t mask[4] = {0};
                                if (masked) {
                                    if (c->ws_len < header_len + 4) break;
                                    memcpy(mask, c->ws_buf + header_len, 4);
                                    header_len += 4;
                                }

                                if (c->ws_len < header_len + payload_len) {
                                    /* If complete frame exceeds current buffer capacity, expand buffer */
                                    if (header_len + payload_len > c->ws_cap) {
                                        size_t ncap = (header_len + payload_len + 65535) & ~65535;
                                        if (ncap > 16 * 1024 * 1024) ncap = 16 * 1024 * 1024;
                                        uint8_t *nb = (uint8_t*)realloc(c->ws_buf, ncap);
                                        if (nb) {
                                            c->ws_buf = nb;
                                            c->ws_cap = ncap;
                                        }
                                    }
                                    break; /* Incomplete frame, wait for more data */
                                }

                                uint8_t *payload = c->ws_buf + header_len;
                                if (masked) {
                                    for (size_t k = 0; k < payload_len; ++k) {
                                        payload[k] ^= mask[k & 3];
                                    }
                                }

                                if (opcode == 0x8) {
                                    /* Connection Close */
                                    uint8_t cr[2] = { 0x88, 0x00 };
                                    send(c->fd, (const char*)cr, 2, MSG_NOSIGNAL);
                                    close_client_locked(c);
                                    break;
                                } else if (opcode == 0x9) {
                                    /* Ping -> Send Pong */
                                    uint8_t pong[10];
                                    pong[0] = 0x8A;
                                    size_t plen = 2;
                                    if (payload_len < 126) {
                                        pong[1] = (uint8_t)payload_len;
                                    } else {
                                        pong[1] = 126;
                                        pong[2] = (uint8_t)(payload_len >> 8);
                                        pong[3] = (uint8_t)(payload_len & 0xFF);
                                        plen = 4;
                                    }
                                    send(c->fd, (const char*)pong, plen, MSG_NOSIGNAL);
                                    if (payload_len > 0) {
                                        send(c->fd, (const char*)payload, (size_t)payload_len, MSG_NOSIGNAL);
                                    }
                                } else if (opcode == 0x2) {
                                    /* Binary Video Frame */
                                    if (srv->on_ingress_frame && payload_len > 0) {
                                        uint32_t fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
                                        uint32_t w = 0, h = 0;
                                        bool is_key = false;
                                        const uint8_t *vbytes = payload;
                                        size_t vsize = payload_len;

                                        if (payload_len >= sizeof(avx_web_frame_header_t)) {
                                            const avx_web_frame_header_t *whdr = (const avx_web_frame_header_t*)payload;
                                            if (whdr->magic == AVX_WEB_FRAME_MAGIC || whdr->magic == 0x4F415658U) {
                                                if (whdr->magic == 0x4F415658U) {
                                                    /* Network / Big Endian fallback */
                                                    fourcc = ((whdr->fourcc >> 24) & 0xff) | ((whdr->fourcc >> 8) & 0xff00) |
                                                             ((whdr->fourcc << 8) & 0xff0000) | ((whdr->fourcc << 24) & 0xff000000);
                                                    w = ((whdr->width >> 8) & 0xff) | ((whdr->width << 8) & 0xff00);
                                                    h = ((whdr->height >> 8) & 0xff) | ((whdr->height << 8) & 0xff00);
                                                    is_key = (whdr->flags != 0);
                                                } else {
                                                    fourcc = whdr->fourcc;
                                                    w = whdr->width;
                                                    h = whdr->height;
                                                    is_key = (whdr->flags & 0x01) != 0;
                                                }
                                                vbytes = payload + sizeof(avx_web_frame_header_t);
                                                vsize = payload_len - sizeof(avx_web_frame_header_t);
                                            }
                                        }

                                        avx_web_ingress_frame_cb cb = srv->on_ingress_frame;
                                        void *uctx = srv->user_ctx;
                                        avx_mutex_unlock(&srv->lock);
                                        cb(vbytes, vsize, fourcc, w, h, is_key, uctx);
                                        avx_mutex_lock(&srv->lock);
                                        if (!c->active) break;
                                    }
                                }

                                size_t total_consumed = header_len + payload_len;
                                size_t rem = c->ws_len - total_consumed;
                                if (rem > 0) {
                                    memmove(c->ws_buf, c->ws_buf + total_consumed, rem);
                                }
                                c->ws_len = rem;
                            }
                        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            close_client_locked(c);
                        }
                    } else if (c->is_sse) {
                        char dummy[64];
                        ssize_t n = recv(c->fd, dummy, sizeof(dummy), 0);
                        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            close_client_locked(c);
                        }
                    } else if (!c->streaming) {
                        if (c->req_len >= sizeof(c->req_buf) - 1) {
                            const char *resp = "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
                            send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
                            close_client_locked(c);
                            continue;
                        }
                        ssize_t n = recv(c->fd, c->req_buf + c->req_len, sizeof(c->req_buf) - 1 - c->req_len, 0);
                        if (n > 0) {
                            c->req_len += (size_t)n;
                            c->req_buf[c->req_len] = '\0';
                            if (strstr(c->req_buf, "\r\n\r\n") || strstr(c->req_buf, "\n\n")) {
                                handle_client_http_request(srv, c);
                            }
                        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            close_client_locked(c);
                        }
                    }
                }
                avx_mutex_unlock(&srv->lock);
            }
        }
    }

    return NULL;
}

/* ========================================================================= */
/* 4. PUBLIC API IMPLEMENTATION                                              */
/* ========================================================================= */

OPENAVX_API avx_web_server_t* avx_web_server_create(const avx_web_server_config_t *config) {
    if (!config) return NULL;
    avx_platform_sockets_init();

    avx_web_server_t *srv = (avx_web_server_t*)calloc(1, sizeof(avx_web_server_t));
    if (!srv) return NULL;

    srv->port = config->port;
    srv->codec = config->codec;
    srv->on_keyframe_request = config->on_keyframe_request;
    srv->on_ingress_frame = config->on_ingress_frame;
    srv->on_tally_change = config->on_tally_change;
    srv->user_ctx = config->user_ctx;
    srv->width = 1920;
    srv->height = 1080;
    srv->seq_num = 1;

    if (config->stream_name && config->stream_name[0]) {
        snprintf(srv->stream_name, sizeof(srv->stream_name), "%s", config->stream_name);
    } else {
        snprintf(srv->stream_name, sizeof(srv->stream_name), "OpenAVX");
    }

    avx_mutex_init(&srv->lock);
    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        srv->clients[i].fd = AVX_INVALID_SOCKET;
    }

    /* Allocate scratch buffers */
    srv->avcc_cap = 2 * 1024 * 1024; /* 2MB default */
    srv->avcc_buf = (uint8_t*)malloc(srv->avcc_cap);
    srv->frag_cap = srv->avcc_cap + 4096;
    srv->frag_buf = (uint8_t*)malloc(srv->frag_cap);

    if (!srv->avcc_buf || !srv->frag_buf) {
        avx_web_server_destroy(srv);
        return NULL;
    }

    /* Create TCP socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!avx_socket_is_valid(srv->listen_fd)) {
        avx_web_server_destroy(srv);
        return NULL;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(srv->port);

    if (bind(srv->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        avx_web_server_destroy(srv);
        return NULL;
    }

    if (srv->port == 0) {
        socklen_t sl = sizeof(sin);
        if (getsockname(srv->listen_fd, (struct sockaddr*)&sin, &sl) == 0) {
            srv->port = ntohs(sin.sin_port);
        }
    }

    if (listen(srv->listen_fd, 16) != 0) {
        avx_web_server_destroy(srv);
        return NULL;
    }

    avx_socket_set_nonblocking(srv->listen_fd, true);

    srv->running = true;
    if (avx_thread_create(&srv->thread, avx_web_server_worker, srv) != 0) {
        srv->running = false;
        avx_web_server_destroy(srv);
        return NULL;
    }

    return srv;
}

OPENAVX_API void avx_web_server_destroy(avx_web_server_t *server) {
    if (!server) return;

    server->running = false;
    if (avx_socket_is_valid(server->listen_fd)) {
        avx_socket_close(server->listen_fd);
        server->listen_fd = AVX_INVALID_SOCKET;
    }

    if (server->thread) {
        avx_thread_join(server->thread);
        server->thread = 0;
    }

    avx_mutex_lock(&server->lock);
    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        close_client_locked(&server->clients[i]);
    }
    free(server->sps);
    free(server->pps);
    free(server->vps);
    free(server->avcc_buf);
    free(server->frag_buf);
    server->sps = NULL;
    server->pps = NULL;
    server->vps = NULL;
    server->avcc_buf = NULL;
    server->frag_buf = NULL;
    avx_mutex_unlock(&server->lock);

    avx_mutex_destroy(&server->lock);
    free(server);
}

OPENAVX_API uint16_t avx_web_server_get_port(const avx_web_server_t *server) {
    return server ? server->port : 0;
}

OPENAVX_API int avx_web_server_get_client_count(const avx_web_server_t *server) {
    if (!server) return 0;
    avx_web_server_t *srv = (avx_web_server_t*)server;
    avx_mutex_lock(&srv->lock);
    int count = 0;
    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        if (srv->clients[i].active && srv->clients[i].streaming) count++;
    }
    avx_mutex_unlock(&srv->lock);
    return count;
}

OPENAVX_API void avx_web_server_set_ingress_callback(
    avx_web_server_t *server,
    avx_web_ingress_frame_cb cb,
    void *user_ctx
) {
    if (!server) return;
    avx_mutex_lock(&server->lock);
    server->on_ingress_frame = cb;
    server->user_ctx = user_ctx;
    avx_mutex_unlock(&server->lock);
}

OPENAVX_API void avx_web_server_broadcast_tally(avx_web_server_t *server, const oavx_tally_t *tally) {
    if (!server) return;
    avx_mutex_lock(&server->lock);
    broadcast_tally_locked(server, tally);
    avx_mutex_unlock(&server->lock);
}

/* Packages incoming video frame into fMP4 fragment and broadcasts to active HTTP/WS clients. */
OPENAVX_API bool avx_web_server_feed_frame(
    avx_web_server_t *server,
    const uint8_t *frame_data,
    size_t frame_size,
    uint64_t pts_ns,
    bool is_keyframe
) {
    if (!server || !frame_data || frame_size == 0) return false;

    avx_mutex_lock(&server->lock);

    /* 1. Extract NALUs from Annex-B bitstream */
    typedef struct {
        const uint8_t *ptr;
        size_t len;
    } nalu_entry_t;
    nalu_entry_t nalus[128];
    size_t num_nalus = 0;

    size_t pos = 0;
    while (pos + 3 <= frame_size && num_nalus < 128) {
        size_t sc_len = 0;
        if (frame_data[pos] == 0 && frame_data[pos+1] == 0) {
            if (frame_data[pos+2] == 1) sc_len = 3;
            else if (pos + 4 <= frame_size && frame_data[pos+2] == 0 && frame_data[pos+3] == 1) sc_len = 4;
        }

        if (sc_len > 0) {
            size_t nalu_start = pos + sc_len;
            size_t next_pos = nalu_start;
            while (next_pos + 3 <= frame_size) {
                if (frame_data[next_pos] == 0 && frame_data[next_pos+1] == 0 &&
                    (frame_data[next_pos+2] == 1 || (next_pos + 4 <= frame_size && frame_data[next_pos+2] == 0 && frame_data[next_pos+3] == 1))) {
                    break;
                }
                next_pos++;
            }
            if (next_pos + 3 > frame_size) next_pos = frame_size;

            size_t nalu_len = next_pos - nalu_start;
            if (nalu_len > 0) {
                nalus[num_nalus].ptr = frame_data + nalu_start;
                nalus[num_nalus].len = nalu_len;
                num_nalus++;
            }
            pos = next_pos;
        } else {
            pos++;
        }
    }

    if (num_nalus == 0) {
        nalus[0].ptr = frame_data;
        nalus[0].len = frame_size;
        num_nalus = 1;
    }

    /* 2. Inspect NALUs for Parameter Sets & Keyframe Slices */
    bool params_updated = false;

    for (size_t i = 0; i < num_nalus; ++i) {
        const uint8_t *np = nalus[i].ptr;
        size_t nl = nalus[i].len;
        if (nl == 0) continue;

        if (server->codec == AVX_RTSP_CODEC_H264) {
            uint8_t type = np[0] & 0x1F;
            if (type == 7) { /* SPS */
                free(server->sps);
                server->sps = (uint8_t*)malloc(nl);
                if (server->sps) {
                    memcpy(server->sps, np, nl);
                    server->sps_len = nl;
                    int w = 0, h = 0, prof = 0, lvl = 0;
                    if (avx_rtsp_parse_sps_h264(server->sps, server->sps_len, &w, &h, &prof, &lvl)) {
                        server->width = (uint32_t)w;
                        server->height = (uint32_t)h;
                        server->profile = (uint8_t)prof;
                        server->level = (uint8_t)lvl;
                    }
                    params_updated = true;
                }
            } else if (type == 8) { /* PPS */
                free(server->pps);
                server->pps = (uint8_t*)malloc(nl);
                if (server->pps) {
                    memcpy(server->pps, np, nl);
                    server->pps_len = nl;
                    params_updated = true;
                }
            } else if (avx_rtsp_is_keyframe(AVX_RTSP_CODEC_H264, np, nl)) {
                is_keyframe = true;
            }
        } else if (server->codec == AVX_RTSP_CODEC_HEVC) {
            uint8_t type = (np[0] >> 1) & 0x3F;
            if (type == 32) { /* VPS */
                free(server->vps);
                server->vps = (uint8_t*)malloc(nl);
                if (server->vps) {
                    memcpy(server->vps, np, nl);
                    server->vps_len = nl;
                    params_updated = true;
                }
            } else if (type == 33) { /* SPS */
                free(server->sps);
                server->sps = (uint8_t*)malloc(nl);
                if (server->sps) {
                    memcpy(server->sps, np, nl);
                    server->sps_len = nl;
                    int w = 0, h = 0, prof = 0, lvl = 0;
                    if (avx_rtsp_parse_sps_hevc(server->sps, server->sps_len, &w, &h, &prof, &lvl)) {
                        server->width = (uint32_t)w;
                        server->height = (uint32_t)h;
                        server->profile = (uint8_t)prof;
                        server->level = (uint8_t)lvl;
                    }
                    params_updated = true;
                }
            } else if (type == 34) { /* PPS */
                free(server->pps);
                server->pps = (uint8_t*)malloc(nl);
                if (server->pps) {
                    memcpy(server->pps, np, nl);
                    server->pps_len = nl;
                    params_updated = true;
                }
            } else if (avx_rtsp_is_keyframe(AVX_RTSP_CODEC_HEVC, np, nl)) { /* IDR / BLA / CRA */
                is_keyframe = true;
            }
        } else if (server->codec == AVX_RTSP_CODEC_AV1) {
            uint8_t type = (np[0] >> 3) & 0x0F;
            if (type == 1) { /* Sequence Header */
                free(server->sps);
                server->sps = (uint8_t*)malloc(nl);
                if (server->sps) {
                    memcpy(server->sps, np, nl);
                    server->sps_len = nl;
                    params_updated = true;
                }
            }
            if (!is_keyframe && avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, np, nl)) {
                is_keyframe = true;
            }
        }
    }

    /* Update Init Segment if parameter sets changed */
    if (params_updated || server->init_segment_len == 0) {
        server->init_segment_len = avx_fmp4_generate_init_segment(
            server->codec,
            server->width,
            server->height,
            server->sps,
            server->sps_len,
            server->pps,
            server->pps_len,
            server->vps,
            server->vps_len,
            server->init_segment,
            sizeof(server->init_segment)
        );
    }

    /* 3. Check if any streaming clients are active */
    bool has_streaming_clients = false;
    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        if (server->clients[i].active && server->clients[i].streaming) {
            has_streaming_clients = true;
            break;
        }
    }
    if (!has_streaming_clients) {
        avx_mutex_unlock(&server->lock);
        return true;
    }

    /* 4. Convert NALUs to AVCC format in scratch buffer */
    size_t total_avcc_len = 0;
    for (size_t i = 0; i < num_nalus; ++i) {
        total_avcc_len += 4 + nalus[i].len;
    }

    if (total_avcc_len > server->avcc_cap) {
        server->avcc_cap = total_avcc_len + (256 * 1024);
        uint8_t *nb = (uint8_t*)realloc(server->avcc_buf, server->avcc_cap);
        if (!nb) {
            avx_mutex_unlock(&server->lock);
            return false;
        }
        server->avcc_buf = nb;
    }

    uint8_t *ap = server->avcc_buf;
    for (size_t i = 0; i < num_nalus; ++i) {
        avx_put_u32(&ap, (uint32_t)nalus[i].len);
        memcpy(ap, nalus[i].ptr, nalus[i].len);
        ap += nalus[i].len;
    }

    /* 5. Compute Timestamps */
    uint64_t pts_90k;
    if (pts_ns > 0) {
        pts_90k = (pts_ns * 9) / 100000ULL;
    } else {
        pts_90k = server->last_pts_90k + 1500;
    }
    server->last_pts_90k = pts_90k;

    /* 6. Generate fMP4 Media Fragment (moof + mdat) */
    size_t needed_frag_cap = total_avcc_len + 256;
    if (needed_frag_cap > server->frag_cap) {
        server->frag_cap = needed_frag_cap + (256 * 1024);
        uint8_t *fb = (uint8_t*)realloc(server->frag_buf, server->frag_cap);
        if (!fb) {
            avx_mutex_unlock(&server->lock);
            return false;
        }
        server->frag_buf = fb;
    }

    size_t frag_len = avx_fmp4_generate_media_fragment(
        server->seq_num++,
        pts_90k,
        1500, /* 60fps duration @ 90kHz */
        is_keyframe,
        server->avcc_buf,
        total_avcc_len,
        server->frag_buf,
        server->frag_cap
    );

    if (frag_len == 0) {
        avx_mutex_unlock(&server->lock);
        return false;
    }

    /* 7. Fan-out Fragment to all Streaming Clients */
    for (int i = 0; i < AVX_WEB_MAX_CLIENTS; ++i) {
        avx_web_client_t *c = &server->clients[i];
        if (!c->active || !c->streaming) continue;

        if (!c->received_keyframe) {
            if (is_keyframe) {
                /* Client's first keyframe: send init segment first, then fragment */
                if (server->init_segment_len > 0) {
                    send(c->fd, (const char*)server->init_segment, server->init_segment_len, MSG_NOSIGNAL);
                }
                c->received_keyframe = true;
            } else {
                /* Skip non-keyframe while waiting for initial IDR */
                continue;
            }
        }

        /* Non-blocking push */
#ifdef MSG_DONTWAIT
        const int send_flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
        const int send_flags = MSG_NOSIGNAL;
#endif
        ssize_t sent = send(c->fd, (const char*)server->frag_buf, frag_len, send_flags);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Client socket buffer full: drop frame to prevent lagging/stall */
                continue;
            } else if (errno == EPIPE || errno == ECONNRESET) {
                /* Browser tab closed */
                close_client_locked(c);
            }
        }
    }

    avx_mutex_unlock(&server->lock);
    return true;
}
