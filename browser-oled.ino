// =================================================================
//  ESP32 Mini OLED Webcam Stream
//  Streams your browser camera to a 128x64 SSD1306 OLED via WiFi.
//  Seven 1-bit styles (Dithered, Edges, Motion Trail, Scanlines,
//  Skull, Glitch, SHODAN). Skull and SHODAN use MediaPipe face
//  landmarks; the rest work on any camera feed.
//
//  Serves HTTP (port 80) + HTTPS (port 443) because browsers
//  block getUserMedia on non-HTTPS LAN IPs (yes, including Chrome).
//
//  Uses ESP-IDF's built-in esp_http_server / esp_https_server
//  (no external library needed). Part of ESP32 Arduino core 3.x.
//
//  BEFORE YOU CAN COMPILE: paste your own self-signed certificate
//  and private key into the cert_pem[] and key_pem[] blocks below.
//  See README.md ("Generate a certificate") for the 2-command
//  setup. Do not commit a private key to a public repo.
// =================================================================

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Built-in ESP-IDF HTTP/HTTPS server
#include <esp_http_server.h>
#include <esp_https_server.h>

// =============================
//  Edit these before uploading
// =============================
const char* ssid     = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ==============================================================
//  Self-signed SSL certificate. MUST be filled in before compile.
//
//  Generate your own with (see README for a helper script):
//    openssl req -x509 -newkey rsa:2048 -nodes \
//      -keyout key.pem -out cert.pem -days 3650 \
//      -subj "/CN=esp32.local"
//
//  Then paste the PEM contents inside the two string blocks below,
//  one line per "...\n" entry. Keep the BEGIN/END lines.
//  Never commit a private key to a public repository.
// ==============================================================
static const char cert_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"PASTE YOUR CERTIFICATE HERE\n"
"-----END CERTIFICATE-----\n";

static const char key_pem[] =
"-----BEGIN PRIVATE KEY-----\n"
"PASTE YOUR PRIVATE KEY HERE\n"
"-----END PRIVATE KEY-----\n";

// OLED and display setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SDA_PIN 21
#define SCL_PIN 22
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Frame buffer for remote face data (128x64 = 1024 bytes at 1 bit per pixel)
uint8_t oledBuffer[SCREEN_WIDTH * SCREEN_HEIGHT / 8];
bool hasRemoteFrame = false;
bool newFrameReady = false;

// Dual servers: HTTP on 80, HTTPS on 443
httpd_handle_t httpServer = nullptr;
httpd_handle_t httpsServer = nullptr;

// ---- Debug stats ----
unsigned long frameRecvCount = 0;
unsigned long frameDisplayCount = 0;
unsigned long lastStatsTime = 0;
unsigned long lastDecodeUs = 0;
unsigned long lastDisplayUs = 0;

#if __has_include(<cert_check_stub>)
// never true, just keeps compilers quiet about the check below
#endif
#ifdef __cplusplus
static_assert(sizeof(cert_pem) > 200, "cert_pem is empty. See README 'Generate a certificate'.");
static_assert(sizeof(key_pem)  > 200, "key_pem is empty. See README 'Generate a certificate'.");
#endif

// ==============================================================
//  HTML page (inside a function so Arduino preprocessor ignores JS)
// ==============================================================

String pageHtml() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>OLED Webcam Stream</title>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <style>
    *{box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;max-width:520px;margin:0 auto;padding:8px;background:#1a1a2e;color:#eee}
    h1{font-size:16px;margin:0 0 6px}
    .controls{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:8px}
    button{padding:8px 14px;font-size:14px;border-radius:6px;border:none;cursor:pointer;background:#0f3460;color:#eee;transition:background 0.2s}
    button:hover{background:#1a508b}
    button.stop{background:#c0392b}
    button.stop:hover{background:#e74c3c}
    select{padding:6px;border-radius:6px;border:1px solid #444;background:#16213e;color:#eee;font-size:13px}
    .settings{display:flex;flex-wrap:wrap;gap:10px;font-size:12px;color:#aaa;margin-bottom:8px}
    .settings label{display:flex;align-items:center;gap:4px}
    .settings input[type="range"]{width:80px}
    .previews{display:flex;gap:10px;align-items:flex-start;margin-bottom:8px}
    video{width:180px;height:135px;border-radius:6px;border:1px solid #333;background:#000;object-fit:cover}
    #tiny{image-rendering:pixelated;width:256px;height:128px;border:2px solid #e94560;border-radius:4px;background:#000}
    @media(max-width:520px){
      video{width:140px;height:105px}
      #tiny{width:200px;height:100px}
    }
    #status{padding:6px 10px;background:#16213e;border-radius:6px;font-size:12px;color:#aaa}
    details{margin-top:8px}
    summary{cursor:pointer;font-size:12px;color:#aaa}
    #debugStats{margin:6px 0;padding:8px;background:#0d1117;border-radius:6px;font-size:11px;color:#7ee787;line-height:1.5;white-space:pre}
    .privacy{margin:0 0 8px;padding:8px 10px;background:#16213e;border-radius:6px;font-size:12px;color:#aaa}
    .privacy summary{color:#ccc}
    .privacy p{margin:6px 0 0;line-height:1.5}
    .privacy a{color:#7ee787}
  </style>
</head>
<body>
  <h1>OLED Webcam Stream</h1>

  <details class="privacy">
    <summary>Why did my browser warn me?</summary>
    <p>Your ESP32 uses a self-signed certificate because no public authority signs certs for devices on your private WiFi. That is why your browser warns you. It does not mean this page is unsafe.</p>
    <p>Nothing leaves your network. Your camera feed is processed entirely in your browser. The only data sent to the ESP32 is the final 1-bit 128x64 image, which is all it can display. Source code: <a href="https://github.com/sanderdesnaijer/esp32-mini-oled-webcam-stream-mediapipe" target="_blank" rel="noopener">GitHub</a>.</p>
  </details>

  <div class="controls">
    <button id="toggleBtn">Start</button>
    <select id="cameraSelect">
      <option value="user">Front</option>
      <option value="environment">Back</option>
    </select>
    <select id="styleSelect">
      <option value="dithered">Dithered</option>
      <option value="edges">Edges</option>
      <option value="motiontrail">Motion Trail</option>
      <option value="scanlines">Scanlines</option>
      <option value="skull">Skull</option>
      <option value="glitch">Glitch</option>
      <option value="shodan">SHODAN</option>
    </select>
  </div>

  <div class="settings">
    <label>Threshold: <input type="range" id="threshold" min="0" max="255" value="128"/><span id="threshVal">128</span></label>
    <label>FPS: <input type="range" id="fpsSlider" min="1" max="30" value="10"/><span id="fpsVal">10</span></label>
  </div>

  <div class="previews">
    <video id="video" autoplay playsinline muted></video>
    <canvas id="tiny" width="128" height="64"></canvas>
  </div>

  <div id="status">Loading MediaPipe...</div>

<script type="module">
import{FaceLandmarker,FilesetResolver}from"https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.18";

const video=document.getElementById("video");
const tinyCanvas=document.getElementById("tiny");
const tinyCtx=tinyCanvas.getContext("2d",{willReadFrequently:true});
const statusEl=document.getElementById("status");
const toggleBtn=document.getElementById("toggleBtn");
const styleSelect=document.getElementById("styleSelect");
const cameraSelect=document.getElementById("cameraSelect");
const thresholdSlider=document.getElementById("threshold");
const threshValSpan=document.getElementById("threshVal");
const fpsSlider=document.getElementById("fpsSlider");
const fpsValSpan=document.getElementById("fpsVal");

const ESP_URL=window.location.origin;

const tmpCanvas=document.createElement("canvas");
tmpCanvas.width=128;tmpCanvas.height=64;
const tmpCtx=tmpCanvas.getContext("2d",{willReadFrequently:true});

const frameBuf=new Uint8Array(1024);
const grayBuf=new Float32Array(128*64);

let faceLandmarker=null;
let streaming=false;
let sendingFrame=false;
let lastFrameTime=0;
let frameCount=0;
let fpsCountStart=0;
let cameraStream=null;
let browserSendTimes=[];

const BAYER4_FLAT=new Uint8Array([0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5]);

thresholdSlider.addEventListener("input",()=>{threshValSpan.textContent=thresholdSlider.value});
fpsSlider.addEventListener("input",()=>{fpsValSpan.textContent=fpsSlider.value});

// ---- Init MediaPipe ----
async function initMediaPipe(){
  setStatus("Loading MediaPipe Face Landmarker...");
  const vision=await FilesetResolver.forVisionTasks("https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.18/wasm");
  faceLandmarker=await FaceLandmarker.createFromOptions(vision,{
    baseOptions:{modelAssetPath:"https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task",delegate:"GPU"},
    runningMode:"VIDEO",numFaces:1,outputFaceBlendshapes:false
  });
  setStatus("Ready. Press Start.");
}

// ---- Open camera (front/back) ----
async function openCamera(facing){
  if(cameraStream){cameraStream.getTracks().forEach(t=>t.stop());cameraStream=null}
  try{
    cameraStream=await navigator.mediaDevices.getUserMedia({video:{width:640,height:480,facingMode:{exact:facing}}});
  }catch(e){
    cameraStream=await navigator.mediaDevices.getUserMedia({video:{width:640,height:480,facingMode:facing}});
  }
  video.srcObject=cameraStream;
  await video.play();
}

// ---- Live-swap camera while streaming ----
cameraSelect.addEventListener("change",async()=>{
  if(!streaming)return;
  try{setStatus("Switching camera...");await openCamera(cameraSelect.value);setStatus("Streaming...")}
  catch(e){setStatus("Camera switch failed: "+e.message)}
});

// ---- Single toggle button: Start / Stop ----
toggleBtn.addEventListener("click",async()=>{
  if(streaming){
    streaming=false;
    if(cameraStream){cameraStream.getTracks().forEach(t=>t.stop());cameraStream=null}
    video.srcObject=null;
    toggleBtn.textContent="Start";
    toggleBtn.classList.remove("stop");
    setStatus("Stopped.");
    return;
  }
  try{
    await openCamera(cameraSelect.value);
    streaming=true;
    toggleBtn.textContent="Stop";
    toggleBtn.classList.add("stop");
    frameCount=0;
    fpsCountStart=performance.now();
    setStatus("Streaming...");
    requestAnimationFrame(detectLoop);
    streamLoop();
  }catch(e){
    setStatus("Camera error: "+e.message);
  }
});

// ---- Detection loop ----
let lastDetection=null;
function detectLoop(){
  if(!streaming)return;
  if(!faceLandmarker||video.readyState<2){requestAnimationFrame(detectLoop);return}
  const now=performance.now();
  if(now<=lastFrameTime){requestAnimationFrame(detectLoop);return}
  lastFrameTime=now;
  lastDetection=faceLandmarker.detectForVideo(video,now);
  renderToTinyCanvas(lastDetection);
  requestAnimationFrame(detectLoop);
}

// ---- Render dispatcher ----
function renderToTinyCanvas(result){
  const s=styleSelect.value;
  tinyCtx.fillStyle="#000";tinyCtx.fillRect(0,0,128,64);
  if(s==="dithered")renderDitheredCrop(result);
  else if(s==="edges")renderEdgeOutline(result);
  else if(s==="motiontrail")renderMotionTrail(result);
  else if(s==="scanlines")renderScanlines(result);
  else if(s==="skull")renderSkullOverlay(result);
  else if(s==="glitch")renderScanlineGlitch(result);
  else if(s==="shodan")renderShodan(result);
}

// ---- Helper: face bounding box ----
function getFaceBounds(lm,padX,padY){
  let minX=1,maxX=0,minY=1,maxY=0;
  for(const p of lm){if(p.x<minX)minX=p.x;if(p.x>maxX)maxX=p.x;if(p.y<minY)minY=p.y;if(p.y>maxY)maxY=p.y}
  return{sx:Math.max(0,minX-padX)*video.videoWidth,sy:Math.max(0,minY-padY)*video.videoHeight,sw:Math.min(1,maxX-minX+padX*2)*video.videoWidth,sh:Math.min(1,maxY-minY+padY*2)*video.videoHeight};
}

// ======== STYLES ========

// Dithered Crop
function renderDitheredCrop(result){
  if(result&&result.faceLandmarks&&result.faceLandmarks.length>0){const b=getFaceBounds(result.faceLandmarks[0],0.15,0.15);tmpCtx.drawImage(video,b.sx,b.sy,b.sw,b.sh,0,0,128,64)}else{tmpCtx.drawImage(video,0,0,128,64)}
  const srcData=tmpCtx.getImageData(0,0,128,64).data;
  const outImg=tinyCtx.createImageData(128,64);const out=outImg.data;
  for(let y=0;y<64;y++){const bayerRow=(y&3)<<2;for(let x=0;x<128;x++){const i=(y*128+x)<<2;const gray=srcData[i]*77+srcData[i+1]*150+srcData[i+2]*29;const b16=(gray+2048)>>12;const on=(b16===0)?0:(b16>=16)?255:(BAYER4_FLAT[bayerRow+(x&3)]<b16?255:0);out[i]=out[i+1]=out[i+2]=on;out[i+3]=255}}
  tinyCtx.putImageData(outImg,0,0);
}

// Edge Outline
function renderEdgeOutline(result){
  if(result&&result.faceLandmarks&&result.faceLandmarks.length>0){const b=getFaceBounds(result.faceLandmarks[0],0.12,0.12);tmpCtx.drawImage(video,b.sx,b.sy,b.sw,b.sh,0,0,128,64)}else{tmpCtx.drawImage(video,0,0,128,64)}
  const srcData=tmpCtx.getImageData(0,0,128,64).data;
  for(let i=0;i<8192;i++){const j=i<<2;grayBuf[i]=srcData[j]*0.299+srcData[j+1]*0.587+srcData[j+2]*0.114}
  const outImg=tinyCtx.createImageData(128,64);const out=outImg.data;
  const thresh=parseInt(thresholdSlider.value)*0.5;
  for(let y=1;y<63;y++){const row=y*128;for(let x=1;x<127;x++){const tl=grayBuf[row-128+x-1];const tr=grayBuf[row-128+x+1];const ml=grayBuf[row+x-1];const mr=grayBuf[row+x+1];const bl=grayBuf[row+128+x-1];const br=grayBuf[row+128+x+1];const tc=grayBuf[row-128+x];const bc=grayBuf[row+128+x];const gx=-tl+tr-2*ml+2*mr-bl+br;const gy=-tl-2*tc-tr+bl+2*bc+br;const mag=Math.abs(gx)+Math.abs(gy);const i=(y*128+x)<<2;const on=mag>thresh?255:0;out[i]=out[i+1]=out[i+2]=on;out[i+3]=255}}
  tinyCtx.putImageData(outImg,0,0);
}

// Motion Trail
const trailHistory=[];
function renderMotionTrail(result){
  if(result&&result.faceLandmarks&&result.faceLandmarks.length>0){const b=getFaceBounds(result.faceLandmarks[0],0.15,0.15);tmpCtx.drawImage(video,b.sx,b.sy,b.sw,b.sh,0,0,128,64)}else{tmpCtx.drawImage(video,0,0,128,64)}
  const srcData=tmpCtx.getImageData(0,0,128,64).data;
  const currentFrame=new Uint8Array(128*64);
  for(let y=0;y<64;y++){const bayerRow=(y&3)<<2;for(let x=0;x<128;x++){const i=(y*128+x)<<2;const gray=srcData[i]*77+srcData[i+1]*150+srcData[i+2]*29;const b16=(gray+2048)>>12;currentFrame[y*128+x]=(b16===0)?0:(b16>=16)?1:(BAYER4_FLAT[bayerRow+(x&3)]<b16?1:0)}}
  trailHistory.push(currentFrame);if(trailHistory.length>4)trailHistory.shift();
  const outImg=tinyCtx.createImageData(128,64);const out=outImg.data;const brightnesses=[3,5,8,16];
  for(let y=0;y<64;y++){const bayerRow=(y&3)<<2;for(let x=0;x<128;x++){const idx=y*128+x;let on=false;for(let f=0;f<trailHistory.length;f++){if(trailHistory[f][idx]){const age=trailHistory.length-1-f;const brightness=brightnesses[3-age]||3;if(brightness>=16||BAYER4_FLAT[bayerRow+(x&3)]<brightness){on=true;break}}}const i=idx<<2;out[i]=out[i+1]=out[i+2]=on?255:0;out[i+3]=255}}
  tinyCtx.putImageData(outImg,0,0);
}

// Scanlines
function renderScanlines(result){
  if(result&&result.faceLandmarks&&result.faceLandmarks.length>0){const b=getFaceBounds(result.faceLandmarks[0],0.15,0.15);tmpCtx.drawImage(video,b.sx,b.sy,b.sw,b.sh,0,0,128,64)}else{tmpCtx.drawImage(video,0,0,128,64)}
  const srcData=tmpCtx.getImageData(0,0,128,64).data;const outImg=tinyCtx.createImageData(128,64);const out=outImg.data;
  for(let y=0;y<64;y++){if(y%2===1){for(let x=0;x<128;x++){const i=(y*128+x)<<2;out[i]=out[i+1]=out[i+2]=0;out[i+3]=255}continue}const bayerRow=(y&3)<<2;for(let x=0;x<128;x++){const i=(y*128+x)<<2;const gray=srcData[i]*77+srcData[i+1]*150+srcData[i+2]*29;const b16=(gray+2048)>>12;const on=(b16===0)?0:(b16>=16)?255:(BAYER4_FLAT[bayerRow+(x&3)]<b16?255:0);out[i]=out[i+1]=out[i+2]=on;out[i+3]=255}}
  tinyCtx.putImageData(outImg,0,0);
}

// Skull Overlay
function renderSkullOverlay(result){
  if(!result||!result.faceLandmarks||result.faceLandmarks.length===0){tinyCtx.fillStyle="#fff";tinyCtx.font="8px monospace";tinyCtx.fillText("No face",40,34);return}
  const lm=result.faceLandmarks[0];tinyCtx.strokeStyle="#fff";tinyCtx.fillStyle="#fff";tinyCtx.lineWidth=2;
  function px(i){return{x:Math.round(lm[i].x*128),y:Math.round(lm[i].y*64)}}
  const faceOval=[10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109,10];
  tinyCtx.beginPath();const first=px(faceOval[0]);tinyCtx.moveTo(first.x,first.y);for(let i=1;i<faceOval.length;i++){const p=px(faceOval[i]);tinyCtx.lineTo(p.x,p.y)}tinyCtx.closePath();tinyCtx.stroke();
  const leftEyeCenter=px(159);const rightEyeCenter=px(386);const leSize=6;
  tinyCtx.beginPath();tinyCtx.moveTo(leftEyeCenter.x,leftEyeCenter.y-leSize);tinyCtx.lineTo(leftEyeCenter.x-leSize,leftEyeCenter.y+leSize*0.6);tinyCtx.lineTo(leftEyeCenter.x+leSize,leftEyeCenter.y+leSize*0.6);tinyCtx.closePath();tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(rightEyeCenter.x,rightEyeCenter.y-leSize);tinyCtx.lineTo(rightEyeCenter.x-leSize,rightEyeCenter.y+leSize*0.6);tinyCtx.lineTo(rightEyeCenter.x+leSize,rightEyeCenter.y+leSize*0.6);tinyCtx.closePath();tinyCtx.stroke();
  for(const idx of[468,473]){const p=px(idx);tinyCtx.beginPath();tinyCtx.arc(p.x,p.y,1.5,0,Math.PI*2);tinyCtx.fill()}
  const noseTip=px(4);tinyCtx.beginPath();tinyCtx.moveTo(noseTip.x-3,noseTip.y-1);tinyCtx.lineTo(noseTip.x+3,noseTip.y-1);tinyCtx.lineTo(noseTip.x,noseTip.y+3);tinyCtx.closePath();tinyCtx.stroke();
  const mouthLeft=px(61);const mouthRight=px(291);const mouthTop=px(0);const mouthBottom=px(17);const mTop=mouthTop.y;const mBot=mouthBottom.y;
  tinyCtx.beginPath();tinyCtx.moveTo(mouthLeft.x,mTop);tinyCtx.lineTo(mouthRight.x,mTop);tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(mouthLeft.x,mBot);tinyCtx.lineTo(mouthRight.x,mBot);tinyCtx.stroke();
  const teethCount=6;const mWidth=mouthRight.x-mouthLeft.x;tinyCtx.lineWidth=1;
  for(let t=1;t<teethCount;t++){const tx=mouthLeft.x+(mWidth/teethCount)*t;tinyCtx.beginPath();tinyCtx.moveTo(tx,mTop);tinyCtx.lineTo(tx,mBot);tinyCtx.stroke()}
}

// Scanline Glitch
function renderScanlineGlitch(result){
  if(result&&result.faceLandmarks&&result.faceLandmarks.length>0){const b=getFaceBounds(result.faceLandmarks[0],0.15,0.15);tmpCtx.drawImage(video,b.sx,b.sy,b.sw,b.sh,0,0,128,64)}else{tmpCtx.drawImage(video,0,0,128,64)}
  const srcData=tmpCtx.getImageData(0,0,128,64).data;const outImg=tinyCtx.createImageData(128,64);const out=outImg.data;
  const glitchCount=3+Math.floor(Math.random()*6);const glitchRows={};
  for(let g=0;g<glitchCount;g++){const row=Math.floor(Math.random()*64);glitchRows[row]=Math.floor(Math.random()*20)-10}
  for(let y=0;y<64;y++){const bayerRow=(y&3)<<2;const rowOffset=glitchRows[y]||0;for(let x=0;x<128;x++){let srcX=x-rowOffset;if(srcX<0)srcX+=128;if(srcX>=128)srcX-=128;const srcI=(y*128+srcX)<<2;const gray=srcData[srcI]*77+srcData[srcI+1]*150+srcData[srcI+2]*29;const b16=(gray+2048)>>12;const on=(b16===0)?0:(b16>=16)?255:(BAYER4_FLAT[bayerRow+(x&3)]<b16?255:0);const outI=(y*128+x)<<2;out[outI]=out[outI+1]=out[outI+2]=on;out[outI+3]=255}}
  tinyCtx.putImageData(outImg,0,0);
}

// SHODAN
let shodanFrame=0;
function renderShodan(result){
  shodanFrame++;
  if(!result||!result.faceLandmarks||result.faceLandmarks.length===0){
    tinyCtx.fillStyle="#fff";tinyCtx.font="8px monospace";const gx=Math.floor(Math.random()*6)-3;const gy=Math.floor(Math.random()*4)-2;tinyCtx.fillText("I AM SHODAN",28+gx,30+gy);
    for(let i=0;i<3;i++){const y=Math.floor(Math.random()*64);const x=Math.floor(Math.random()*60);tinyCtx.fillRect(x,y,Math.floor(Math.random()*40)+20,1)}return;
  }
  const lm=result.faceLandmarks[0];tinyCtx.strokeStyle="#fff";tinyCtx.fillStyle="#fff";
  function px(i){return{x:Math.round(lm[i].x*128),y:Math.round(lm[i].y*64)}}
  const noseBridge=px(6);const chin=px(152);const forehead=px(10);
  tinyCtx.lineWidth=1;tinyCtx.setLineDash([2,3]);
  for(let i=0;i<5;i++){const angle=-Math.PI*0.3+(i/4)*Math.PI*0.6;const len=20+(shodanFrame*3+i*7)%25;const x1=forehead.x;const y1=forehead.y;const x2=x1+Math.cos(angle)*len;const y2=y1+Math.sin(angle)*len;tinyCtx.beginPath();tinyCtx.moveTo(x1,y1);const midX=x1+Math.cos(angle)*len*0.6;tinyCtx.lineTo(midX,y1);tinyCtx.lineTo(x2,y2);tinyCtx.stroke()}
  const jawLeft=px(132);const jawRight=px(361);
  for(const side of[jawLeft,jawRight]){for(let i=0;i<3;i++){const dir=side===jawLeft?-1:1;const y=side.y-5+i*6;const len=10+(shodanFrame*2+i*5)%20;tinyCtx.beginPath();tinyCtx.moveTo(side.x,y);tinyCtx.lineTo(side.x+dir*len,y);tinyCtx.lineTo(side.x+dir*len,y+3);tinyCtx.stroke()}}
  tinyCtx.setLineDash([]);tinyCtx.lineWidth=1;
  const faceOval=[10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109,10];
  tinyCtx.beginPath();const first=px(faceOval[0]);tinyCtx.moveTo(first.x,first.y);for(let i=1;i<faceOval.length;i++){const p=px(faceOval[i]);tinyCtx.lineTo(p.x,p.y)}tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(forehead.x,forehead.y);tinyCtx.lineTo(noseBridge.x,noseBridge.y);tinyCtx.lineTo(chin.x,chin.y);tinyCtx.stroke();
  const leftCheek=px(234);const rightCheek=px(454);tinyCtx.beginPath();tinyCtx.moveTo(leftCheek.x,leftCheek.y);tinyCtx.lineTo(rightCheek.x,rightCheek.y);tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(forehead.x,forehead.y);tinyCtx.lineTo(jawLeft.x,jawLeft.y);tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(forehead.x,forehead.y);tinyCtx.lineTo(jawRight.x,jawRight.y);tinyCtx.stroke();
  tinyCtx.lineWidth=2;const leftEyeC=px(159);const rightEyeC=px(386);const eyeW=7,eyeH=4;
  for(const ec of[leftEyeC,rightEyeC]){tinyCtx.beginPath();tinyCtx.moveTo(ec.x-eyeW,ec.y);tinyCtx.lineTo(ec.x,ec.y-eyeH);tinyCtx.lineTo(ec.x+eyeW,ec.y);tinyCtx.lineTo(ec.x,ec.y+eyeH);tinyCtx.closePath();tinyCtx.stroke()}
  tinyCtx.lineWidth=1;for(const idx of[468,473]){const p=px(idx);tinyCtx.beginPath();tinyCtx.arc(p.x,p.y,2,0,Math.PI*2);tinyCtx.fill()}
  const mouthL=px(61);const mouthR=px(291);const mouthT=px(0);const mouthB=px(17);
  tinyCtx.beginPath();tinyCtx.moveTo(mouthL.x,mouthT.y);tinyCtx.lineTo(mouthT.x,mouthT.y-1);tinyCtx.lineTo(mouthR.x,mouthT.y);tinyCtx.stroke();
  tinyCtx.beginPath();tinyCtx.moveTo(mouthL.x,mouthB.y);tinyCtx.lineTo(mouthB.x,mouthB.y+1);tinyCtx.lineTo(mouthR.x,mouthB.y);tinyCtx.stroke();
  const imgData=tinyCtx.getImageData(0,0,128,64);const d=imgData.data;
  const glitchIntensity=2+Math.floor(Math.random()*4);
  for(let g=0;g<glitchIntensity;g++){const y=Math.floor(Math.random()*64);const offset=Math.floor(Math.random()*16)-8;const height=1+Math.floor(Math.random()*3);for(let dy=0;dy<height&&(y+dy)<64;dy++){const row=(y+dy)*128;if(offset>0){for(let x=127;x>=offset;x--){const dst=(row+x)<<2;const src=(row+x-offset)<<2;d[dst]=d[src];d[dst+1]=d[src+1];d[dst+2]=d[src+2]}}else if(offset<0){for(let x=0;x<128+offset;x++){const dst=(row+x)<<2;const src=(row+x-offset)<<2;d[dst]=d[src];d[dst+1]=d[src+1];d[dst+2]=d[src+2]}}}}
  for(let i=0;i<30;i++){const x=Math.floor(Math.random()*128);const y=Math.floor(Math.random()*64);const idx=(y*128+x)<<2;d[idx]=d[idx+1]=d[idx+2]=255;d[idx+3]=255}
  if(Math.random()<0.3){const y=Math.floor(Math.random()*64);for(let x=0;x<128;x++){const idx=(y*128+x)<<2;d[idx]=d[idx+1]=d[idx+2]=255}}
  tinyCtx.putImageData(imgData,0,0);
  if(shodanFrame%30<4){tinyCtx.fillStyle="#fff";tinyCtx.font="6px monospace";const texts=["INSECT","I AM SHODAN","LOOK AT YOU HACKER","PERFECT","SUBMIT"];const txt=texts[Math.floor(Math.random()*texts.length)];tinyCtx.fillText(txt,Math.floor(Math.random()*60),Math.floor(Math.random()*60)+6)}
}

// ---- Pack canvas to SSD1306 page format ----
function packCanvasToSSD1306(){
  const pixels=tinyCtx.getImageData(0,0,128,64).data;
  for(let page=0;page<8;page++){const pageOffset=page*128;const yBase=page*8;for(let x=0;x<128;x++){let byte=0;for(let bit=0;bit<8;bit++){const i=((yBase+bit)*128+x)<<2;if(pixels[i]>128)byte|=(1<<bit)}frameBuf[pageOffset+x]=byte}}
  return frameBuf;
}

// ---- Base64 encode ----
function uint8ToBase64(bytes){
  const chunks=[];for(let i=0;i<bytes.length;i+=512)chunks.push(String.fromCharCode.apply(null,bytes.subarray(i,i+512)));return btoa(chunks.join(""));
}

// ---- Send frame to ESP32 ----
async function sendFrame(){
  if(sendingFrame)return;sendingFrame=true;
  const buf=packCanvasToSSD1306();const b64=uint8ToBase64(buf);
  const t0=performance.now();
  try{
    const resp=await fetch(ESP_URL+"/frame",{method:"POST",headers:{"Content-Type":"text/plain"},body:b64});
    const rtt=performance.now()-t0;browserSendTimes.push(rtt);if(browserSendTimes.length>30)browserSendTimes.shift();
    frameCount++;const elapsed=performance.now()-fpsCountStart;
    if(elapsed>2000){const actualFps=(frameCount/elapsed*1000).toFixed(1);const avgRtt=(browserSendTimes.reduce((a,b)=>a+b,0)/browserSendTimes.length).toFixed(0);setStatus("Streaming "+actualFps+" fps | RTT "+avgRtt+"ms");frameCount=0;fpsCountStart=performance.now()}
    if(!resp.ok){const txt=await resp.text();setStatus("ESP32 error: "+resp.status+" "+txt)}
  }catch(e){setStatus("Send failed: "+e.message)}
  sendingFrame=false;
}

// ---- Stream loop ----
async function streamLoop(){
  if(!streaming)return;
  await sendFrame();
  if(streaming){const fps=parseInt(fpsSlider.value);setTimeout(streamLoop,Math.max(1,Math.round(1000/fps)))}
}

function setStatus(msg){statusEl.textContent=msg}

// ---- Boot ----
initMediaPipe();
</script>
</body>
</html>
)rawliteral";
}

// ==============================================================
//  Fast base64 decode with lookup table
// ==============================================================
static const int8_t B64_LOOKUP[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

int base64DecodeFast(const char* input, int inputLen, uint8_t* output, int maxOutput) {
  int outIdx = 0;
  int i = 0;
  while (i + 3 < inputLen && outIdx < maxOutput) {
    int8_t a = B64_LOOKUP[(uint8_t)input[i]];
    int8_t b = B64_LOOKUP[(uint8_t)input[i+1]];
    int8_t c = B64_LOOKUP[(uint8_t)input[i+2]];
    int8_t d = B64_LOOKUP[(uint8_t)input[i+3]];
    if (a < 0 || b < 0) break;
    if (outIdx < maxOutput) output[outIdx++] = (a << 2) | (b >> 4);
    if (c < 0) break;
    if (outIdx < maxOutput) output[outIdx++] = ((b & 0x0F) << 4) | (c >> 2);
    if (d < 0) break;
    if (outIdx < maxOutput) output[outIdx++] = ((c & 0x03) << 6) | d;
    i += 4;
  }
  return outIdx;
}

// ==============================================================
//  Route handlers
// ==============================================================

// ---- GET / (HTTPS) ----
static esp_err_t h_root(httpd_req_t *req) {
  String page = pageHtml();
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page.c_str(), page.length());
}

// ---- GET / (HTTP -> redirect to HTTPS, iOS camera requires HTTPS) ----
static esp_err_t h_root_redirect(httpd_req_t *req) {
  String url = "https://" + WiFi.localIP().toString() + "/";
  httpd_resp_set_status(req, "301 Moved Permanently");
  httpd_resp_set_hdr(req, "Location", url.c_str());
  return httpd_resp_send(req, "Redirecting to HTTPS...", HTTPD_RESP_USE_STRLEN);
}

// ---- POST /frame ----
static esp_err_t h_frame(httpd_req_t *req) {
  const int expected = SCREEN_WIDTH * SCREEN_HEIGHT / 8; // 1024
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  int bodyLen = req->content_len;
  if (bodyLen <= 0 || bodyLen > 2048) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "Missing or oversized body", HTTPD_RESP_USE_STRLEN);
  }

  char body[2048];
  int bytesRead = 0;
  while (bytesRead < bodyLen) {
    int chunk = httpd_req_recv(req, body + bytesRead, bodyLen - bytesRead);
    if (chunk <= 0) {
      if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "recv failed", HTTPD_RESP_USE_STRLEN);
    }
    bytesRead += chunk;
  }
  body[bytesRead] = '\0';

  unsigned long t0 = micros();
  int decoded = base64DecodeFast(body, bytesRead, oledBuffer, expected);
  lastDecodeUs = micros() - t0;

  if (decoded != expected) {
    char msg[80];
    snprintf(msg, sizeof(msg), "Wrong size: got %d expected %d from b64 len %d", decoded, expected, bytesRead);
    Serial.println(msg);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
  }

  hasRemoteFrame = true;
  newFrameReady = true;
  frameRecvCount++;
  return httpd_resp_send(req, "ok", 2);
}

// ---- OPTIONS /frame (CORS preflight) ----
static esp_err_t h_frame_options(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, "", 0);
}

// ---- Register routes on a given server ----
static inline void regRoute(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*h)(httpd_req_t*)) {
  httpd_uri_t r = {};
  r.uri      = uri;
  r.method   = method;
  r.handler  = h;
  r.user_ctx = nullptr;
  httpd_register_uri_handler(server, &r);
}

static void registerRoutes(httpd_handle_t server, bool isHttps) {
  regRoute(server, "/",      HTTP_GET,     isHttps ? h_root : h_root_redirect);
  regRoute(server, "/frame", HTTP_POST,    h_frame);
  regRoute(server, "/frame", HTTP_OPTIONS, h_frame_options);
}

// ==============================================================
//  Display rendering
// ==============================================================

void drawBootScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Webcam Stream");
  display.println("Ready");
  display.println();
  display.println("Open in browser:");
  display.print("https://");
  display.println(WiFi.localIP());
  display.println();
  display.println("Waiting...");
  display.display();
}

void renderRemoteFrame() {
  uint8_t* displayBuf = display.getBuffer();
  memcpy(displayBuf, oledBuffer, SCREEN_WIDTH * SCREEN_HEIGHT / 8);
}

// ==============================================================
//  Setup
// ==============================================================

void setup() {
  Serial.begin(115200);

  // Fast I2C: 400kHz
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found");
    while (true) {}
  }

  display.setRotation(0);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 28);
  display.println("Starting up...");
  display.display();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi connected");
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.println();
  display.println("Starting servers...");
  display.display();

  // ---- Start HTTP server on port 80 ----
  {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port   = 32768;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&httpServer, &cfg);
    if (err != ESP_OK) {
      Serial.printf("HTTP start failed: %d\n", err);
    } else {
      registerRoutes(httpServer, false);
      Serial.printf("HTTP  listening on http://%s/\n", WiFi.localIP().toString().c_str());
    }
  }

  // ---- Start HTTPS server on port 443 ----
  {
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = (const uint8_t*) cert_pem;
    cfg.servercert_len = sizeof(cert_pem);
    cfg.prvtkey_pem    = (const uint8_t*) key_pem;
    cfg.prvtkey_len    = sizeof(key_pem);
    cfg.port_secure    = 443;
    cfg.port_insecure  = 0;
    cfg.httpd.ctrl_port = 32769;
    cfg.httpd.max_uri_handlers = 12;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.stack_size = 10240;

    esp_err_t err = httpd_ssl_start(&httpsServer, &cfg);
    if (err != ESP_OK) {
      Serial.printf("HTTPS start failed: %d\n", err);
    } else {
      registerRoutes(httpsServer, true);
      Serial.printf("HTTPS listening on https://%s/\n", WiFi.localIP().toString().c_str());
    }
  }

  // Show ready state on OLED. Stays up until the first frame arrives.
  drawBootScreen();

  lastStatsTime = millis();
}

// ==============================================================
//  Main loop
// ==============================================================

void loop() {
  // esp_http_server / esp_https_server run in their own FreeRTOS tasks,
  // no service call needed here.

  if (newFrameReady) {
    newFrameReady = false;
    renderRemoteFrame();
    unsigned long t0 = micros();
    display.display();
    lastDisplayUs = micros() - t0;
    frameDisplayCount++;
  } else {
    // No frame yet: boot screen stays up (set in setup()).
    // Once hasRemoteFrame is true we never re-draw it, so it cleanly
    // gets replaced by the first incoming frame.
    delay(hasRemoteFrame ? 1 : 50);
  }

  // Print stats every 5 seconds
  if (millis() - lastStatsTime >= 5000) {
    float elapsed = (millis() - lastStatsTime) / 1000.0;
    Serial.printf("[stats] recv: %.1f fps | display: %.1f fps | decode: %lu us | i2c push: %lu us | heap: %u bytes\n",
      frameRecvCount / elapsed,
      frameDisplayCount / elapsed,
      lastDecodeUs,
      lastDisplayUs,
      ESP.getFreeHeap()
    );
    frameRecvCount = 0;
    frameDisplayCount = 0;
    lastStatsTime = millis();
  }
}
