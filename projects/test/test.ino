/**
 * ESP32-S3 T-RGB MJPEG/JPG player with simple web UI
 * - Plays .mjpeg streams from SD
 * - Shows .jpg/.jpeg stills (centered, scaled 1/2/4/8 to fit)
 * - Double framebuffer + pushColors
 * - Wi-Fi AP with minimal control page
 */

#include <LilyGo_RGBPanel.h>
#include <SD_MMC.h>
#include "MjpegClass.h"

#include <WiFi.h>
#include <WebServer.h>
#include <vector>
#include <TJpg_Decoder.h>   // JPG support

// ----------------- Panel / MJPEG -----------------
LilyGo_RGBPanel panel;
MjpegClass mjpeg;
File mjpegFile;

int W = 0, H = 0;

// Double framebuffer
static uint16_t *fb[2] = { nullptr, nullptr };
static volatile int ready_idx = -1;
static volatile int draw_idx  = -1;

// MJPEG temp buffer (>=128KB recommended)
static uint8_t *mjpegBuf = nullptr;

// Decode callback (legacy JPEGDEC style used by MjpegClass)
static uint16_t *cur_fb = nullptr;
static inline void blit565_to_fb(int x, int y, int w, int h, const uint16_t *src) {
  for (int r = 0; r < h; ++r) {
    memcpy(cur_fb + (y + r) * W + x, src + r * w, w * sizeof(uint16_t));
  }
}
static int draw_to_fb_legacy(JPEGDRAW *p) {
  blit565_to_fb(p->x, p->y, p->iWidth, p->iHeight, (const uint16_t*)p->pPixels);
  return 1;
}

// TJpg_Decoder callback (note w/h are uint16_t)
static bool tjpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!cur_fb) return false;
  if (x < 0 || y < 0 || x + (int)w > W || y + (int)h > H) return true; // already clipped
  for (int r = 0; r < (int)h; ++r) {
    memcpy(cur_fb + (y + r) * W + x, bitmap + r * w, w * sizeof(uint16_t));
  }
  return true;
}

// ----------------- Sync primitives -----------------
static SemaphoreHandle_t semFrameReady;
static SemaphoreHandle_t semFrameUsed;

// ----------------- Playback state -----------------
enum class MediaMode { MJPEG, JPG, NONE };
static MediaMode mode = MediaMode::NONE;

// DEFAULT = JPG
static String currentPath = "/default.jpg";
static volatile bool needSwitch = false;
static String pendingPath;
static volatile bool isPaused = false;

// For JPG: render once when opened (or when “reopen” called)
static volatile bool jpgNeedsRender = false;

// ----------------- Folder lists -----------------
static std::vector<String> files_doro;
static std::vector<String> files_neko;
static std::vector<String> files_azur;
static int idx_doro = 0, idx_neko = 0, idx_azur = 0;

static std::vector<String>* listFor(const String &folder) {
  if (folder == "doro")     return &files_doro;
  if (folder == "neko")     return &files_neko;
  if (folder == "azurlane") return &files_azur;
  return nullptr;
}
static int* indexFor(const String &folder) {
  if (folder == "doro")     return &idx_doro;
  if (folder == "neko")     return &idx_neko;
  if (folder == "azurlane") return &idx_azur;
  return nullptr;
}

// Case-insensitive extension check (avoid String::toUpperCase() void-return trap)
static inline bool isExt(const String &s, const char *ext) {
  String a = s; a.toLowerCase();
  String b = String(ext); b.toLowerCase();
  return a.endsWith(b);
}
static bool isMJPEGPath(const String &p) { return isExt(p, ".mjpeg"); }
static bool isJPGPath  (const String &p) { return isExt(p, ".jpg") || isExt(p, ".jpeg"); }

static void scanFolder(const char *folder, std::vector<String> &out) {
  out.clear();
  File dir = SD_MMC.open(folder);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String full = String(folder) + "/" + String(f.name());
      full.replace("//", "/");
      if (isMJPEGPath(full) || isJPGPath(full)) out.push_back(full);
    }
    f.close();
  }
}

// ----------------- Open media -----------------
static bool open_mjpeg_path(const String &path) {
  if (mjpegFile) mjpegFile.close();
  mjpegFile = SD_MMC.open(path);
  if (!mjpegFile) return false;
  mjpeg.setup(&mjpegFile, mjpegBuf, draw_to_fb_legacy, false, 0, 0, W, H);
  mode = MediaMode::MJPEG;
  currentPath = path;
  Serial.printf("MJPEG open OK: %s\n", path.c_str());
  return true;
}
static bool open_jpg_path(const String &path) {
  if (!SD_MMC.exists(path)) { Serial.printf("JPG missing: %s\n", path.c_str()); return false; }
  mode = MediaMode::JPG;
  currentPath = path;
  jpgNeedsRender = true;
  Serial.printf("JPG open OK: %s\n", path.c_str());
  return true;
}
static bool open_media_path(const String &path) {
  Serial.printf("Open request: %s\n", path.c_str());
  if (isMJPEGPath(path)) return open_mjpeg_path(path);
  if (isJPGPath(path))   return open_jpg_path(path);
  Serial.println("Unknown extension");
  return false;
}

// ----------------- JPG helpers -----------------
static uint8_t chooseScaleFor(int32_t w, int32_t h){
  uint8_t s = 1;
  while ((w / (s * 2) > W) || (h / (s * 2) > H)) { s <<= 1; if (s >= 8) break; }
  return s;
}

static bool renderJPGToFB(const String &path) {
  // Parse width/height from file head using array overload
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { Serial.println("JPG open failed"); return false; }

  const size_t CAP = 32768; // 32 KB is plenty for SOF
  size_t n = f.size(); if (n > CAP) n = CAP;
  uint8_t *head = (uint8_t*)ps_malloc(n ? n : 1);
  if (!head) { f.close(); return false; }
  size_t got = f.read(head, n);
  f.close();

  uint16_t iw=0, ih=0;
  JRESULT jr = TJpgDec.getJpgSize(&iw, &ih, head, got);   // array overload
  if (jr != JDR_OK) {
    Serial.println("JPG: size probe failed, drawing at (0,0) scale=1");
    memset(cur_fb, 0x00, W * H * sizeof(uint16_t));
    TJpgDec.setJpgScale(1);
    JRESULT rc = TJpgDec.drawFsJpg(0, 0, path.c_str(), SD_MMC);
    free(head);
    return rc == JDR_OK;
  }
  free(head);

  uint8_t scale = chooseScaleFor(iw, ih);
  TJpgDec.setJpgScale(scale);

  int32_t ow = iw / scale;
  int32_t oh = ih / scale;
  int32_t x  = (W - ow) / 2; if (x < 0) x = 0;
  int32_t y  = (H - oh) / 2; if (y < 0) y = 0;

  memset(cur_fb, 0x00, W * H * sizeof(uint16_t));
  JRESULT rc = TJpgDec.drawFsJpg(x, y, path.c_str(), SD_MMC);
  if (rc != JDR_OK) Serial.printf("JPG draw failed rc=%d\n", rc);
  return rc == JDR_OK;
}

// ----------------- Auto-pick first playable -----------------
static bool open_first_media() {
  const char* roots[] = {"/", "/doro", "/neko", "/azurlane"};
  for (auto r : roots) {
    File dir = SD_MMC.open(r);
    if (!dir) continue;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) { f.close(); continue; }
      String p = String(r) + "/" + String(f.name()); p.replace("//","/");
      String pp = p;  // close before opening
      f.close();
      if (isJPGPath(pp) || isMJPEGPath(pp)) {
        return open_media_path(pp);
      }
    }
  }
  return false;
}

// ----------------- Display task -----------------
static uint32_t fps_counter = 0, fps_last_ms = 0;
static void displayTask(void *) {
  for (;;) {
    if (xSemaphoreTake(semFrameReady, portMAX_DELAY) == pdTRUE) {
      const int idx = ready_idx;
      if (idx >= 0) {
        panel.pushColors(0, 0, W, H, fb[idx]);
        draw_idx = idx;

        if (mode == MediaMode::MJPEG) {
          fps_counter++;
          uint32_t now = millis();
          if (now - fps_last_ms >= 1000) {
            Serial.printf("Actual FPS: %u\n", fps_counter);
            fps_counter = 0;
            fps_last_ms = now;
          }
        }
      }
      xSemaphoreGive(semFrameUsed);
    }
  }
}

// ----------------- WebServer -----------------
WebServer server(80);
const char* AP_SSID = "ESP32-MJPEG";
const char* AP_PASS = "12345678";

// UI
static void handleUI() {
  static const char html[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 MJPEG/JPG Control</title>
<style>
:root{--bg:#0f1220;--card:#151a2c;--muted:#9aa4b2;--text:#e6e9ef;--accent:#4f8cff;--ok:#34c759;--warn:#ff8a3d;--danger:#ff5d5d;--radius:14px;--shadow:0 6px 18px rgba(0,0,0,.35);--ring:0 0 0 2px rgba(79,140,255,.35)}
@media (prefers-color-scheme: light){:root{--bg:#f6f7fb;--card:#fff;--text:#1b2330;--muted:#64708a;--shadow:0 6px 18px rgba(16,24,40,.08);--ring:0 0 0 2px rgba(79,140,255,.25)}}
*{box-sizing:border-box} body{margin:0;padding:24px;font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,Arial,sans-serif;background:var(--bg);color:var(--text)}
.wrap{max-width:780px;margin:0 auto}.hdr{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}.title{font-weight:600;letter-spacing:.2px;font-size:18px}
.chip{font-size:12px;color:var(--muted);background:var(--card);border-radius:999px;padding:6px 10px;box-shadow:var(--shadow)}
.card{background:var(--card);border-radius:var(--radius);box-shadow:var(--shadow);padding:18px;margin:14px 0}
.section-h{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px}.section-h h2{margin:0;font-size:15px;font-weight:600;letter-spacing:.2px}
.status{font-size:12px;color:var(--muted)} .status strong{color:var(--text)}
.row{display:flex;flex-wrap:wrap;gap:10px}
button{-webkit-tap-highlight-color:transparent;border:1px solid transparent;background:#1a2040;color:#e8ecf6;padding:10px 14px;border-radius:10px;font-weight:600;letter-spacing:.2px;cursor:pointer;transition:.18s ease;box-shadow:var(--shadow)}
button.secondary{background:transparent;border-color:#2a3356;color:var(--text)} button.accent{background:var(--accent)} button.ok{background:var(--ok)} button.warn{background:var(--warn)} button.danger{background:var(--danger)}
button:hover{filter:brightness(1.06);transform:translateY(-1px) rotate(-2deg)} button:active{transform:scale(.98) rotate(-4deg)} button:focus-visible{outline:none;box-shadow:var(--ring)}
.grid{display:grid;gap:14px;grid-template-columns:1fr}@media (min-width:640px){.grid{grid-template-columns:repeat(2,1fr)}}
.footer{text-align:center;color:var(--muted);font-size:12px;margin-top:12px}
</style></head>
<body>
  <div class="wrap">
    <div class="hdr">
      <div class="title">ESP32 MJPEG/JPG Control</div>
      <div class="chip">Now&nbsp;Playing: <span id="cur">—</span></div>
    </div>
    <div class="card">
      <div class="section-h"><h2>Playback</h2><div class="status" id="stateLabel">state: <strong>playing</strong></div></div>
      <div class="row">
        <button id="toggleBtn" class="accent">Pause</button>
        <button class="secondary" onclick="fetchAndToast('/play?file=default.jpg')">Show default.jpg</button>
        <button class="secondary" onclick="fetchAndToast('/play?file=sample.jpg')">Show sample.jpg</button>
        <button class="secondary" onclick="refreshStatus()">Refresh</button>
      </div>
    </div>
    <div class="grid">
      <div class="card"><div class="section-h"><h2>Doro</h2><div class="status">folder: /doro</div></div>
        <div class="row">
          <button class="secondary" onclick="fetchAndToast('/prev?folder=doro')">‹ Prev</button>
          <button class="ok"        onclick="fetchAndToast('/next?folder=doro')">Next ›</button>
        </div>
      </div>
      <div class="card"><div class="section-h"><h2>Neko</h2><div class="status">folder: /neko</div></div>
        <div class="row">
          <button class="secondary" onclick="fetchAndToast('/prev?folder=neko')">‹ Prev</button>
          <button class="ok"        onclick="fetchAndToast('/next?folder=neko')">Next ›</button>
        </div>
      </div>
      <div class="card"><div class="section-h"><h2>Azurlane</h2><div class="status">folder: /azurlane</div></div>
        <div class="row">
          <button class="secondary" onclick="fetchAndToast('/prev?folder=azurlane')">‹ Prev</button>
          <button class="ok"        onclick="fetchAndToast('/next?folder=azurlane')">Next ›</button>
        </div>
      </div>
      <div class="card"><div class="section-h"><h2>Utilities</h2><div class="status">quick actions</div></div>
        <div class="row">
          <button class="warn"   onclick="fetchAndToast('/reopen')">Reopen Current</button>
          <button class="danger" onclick="fetchAndToast('/stop')">Stop</button>
          <button class="secondary" onclick="fetchAndToast('/test')">Test Frame</button>
        </div>
      </div>
    </div>
    <div class="footer">© MJPEG/JPG Player • minimal UI • no external assets</div>
  </div>
<script>
  let playing = true;
  async function refreshStatus(){
    try{
      const r = await fetch('/status',{cache:'no-store'});
      const t = await r.text();
      let path=t, state=null;
      try{const j=JSON.parse(t); path=j.path||t; state=j.state||null;}catch(_){}
      document.getElementById('cur').textContent = path || '—';
      if(state){ playing = (state === 'playing'); }
      document.getElementById('stateLabel').innerHTML = 'state: <strong>' + (playing ? 'playing' : 'paused') + '</strong>';
      document.getElementById('toggleBtn').textContent = playing ? 'Pause' : 'Resume';
    }catch(e){
      document.getElementById('cur').textContent = 'disconnected';
      document.getElementById('stateLabel').innerHTML = 'state: <strong>unknown</strong>';
    }
  }
  async function fetchAndToast(url){ try{ await fetch(url,{method:'GET',cache:'no-store'}); refreshStatus(); }catch(e){} }
  document.getElementById('toggleBtn').addEventListener('click', async () => {
    playing = !playing;
    document.getElementById('toggleBtn').textContent = playing ? 'Pause' : 'Resume';
    document.getElementById('stateLabel').innerHTML = 'state: <strong>' + (playing ? 'playing' : 'paused') + '</strong>';
    const endpoint = playing ? '/resume' : '/pause';
    await fetchAndToast(endpoint);
    refreshStatus();
  });
  refreshStatus(); setInterval(refreshStatus, 2000);
</script>
</body></html>
)HTML";
  server.send(200, "text/html; charset=utf-8", html);
}

// Status JSON
static void handleStatus() {
  String s = "{\"path\":\"" + currentPath + "\",\"state\":\"" + (isPaused ? "paused" : "playing") + "\"}";
  server.send(200, "application/json", s);
}

// Play/show specific file (.mjpeg or .jpg/.jpeg)
static void handlePlayFile() {
  if (!server.hasArg("file")) { server.send(400, "text/plain", "Missing file"); return; }
  String file = server.arg("file");
  if (!file.startsWith("/")) file = "/" + file;
  pendingPath = file;
  needSwitch = true;
  isPaused = false;
  server.send(200, "text/plain", "Open: " + pendingPath);
}

// Next / Prev inside a folder (works for both mjpeg and jpg)
static void handleNextPrev(bool next) {
  if (!server.hasArg("folder")) { server.send(400, "text/plain", "Missing folder"); return; }
  String folder = server.arg("folder");
  auto *lst = listFor(folder);
  auto *idx = indexFor(folder);
  if (!lst || !idx) { server.send(404, "text/plain", "Unknown folder"); return; }
  if (lst->empty()) { server.send(404, "text/plain", "No files in /" + folder); return; }

  if (next) *idx = (*idx + 1) % lst->size();
  else      *idx = (*idx - 1 + lst->size()) % lst->size();

  pendingPath = (*lst)[*idx];
  needSwitch = true;
  isPaused = false;
  server.send(200, "text/plain", (next ? "Next: " : "Prev: ") + pendingPath);
}
static void handleNext() { handleNextPrev(true); }
static void handlePrev() { handleNextPrev(false); }

// Pause/Resume/Reopen/Stop
static void handlePause()  { isPaused = true;  server.send(200, "text/plain", "Paused"); }
static void handleResume() { isPaused = false; server.send(200, "text/plain", "Playing"); }
static void handleReopen() { pendingPath = currentPath; needSwitch = true; server.send(200, "text/plain", "Reopen"); }
static void handleStop()   { isPaused = true; server.send(200, "text/plain", "Stopped"); }

// Test frame route (sanity check)
static void handleTest(){
  if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(200)) == pdTRUE) {
    const int next_idx = (draw_idx == 0) ? 1 : 0;
    cur_fb = fb[next_idx];
    // black background
    for (int i=0;i<W*H;i++) cur_fb[i] = 0x0000;
    // red bar at top
    for (int y=0;y<20;y++) for (int x=0;x<W;x++) cur_fb[y*W+x] = 0xF800;
    ready_idx = next_idx;
    xSemaphoreGive(semFrameReady);
  }
  server.send(200,"text/plain","test frame queued");
}

// ----------------- Arduino -----------------
void setup() {
  Serial.begin(115200);

  if (!panel.begin(LILYGO_T_RGB_2_1_INCHES_HALF_CIRCLE)) {
    Serial.println("Panel init failed!");
    while (1) delay(1000);
  }
  panel.setBrightness(32);

  if (!panel.installSD()) {
    Serial.println("SD init failed!");
    while (1) delay(1000);
  }

  W = panel.width();
  H = panel.height();

  fb[0] = (uint16_t*)ps_malloc(W * H * sizeof(uint16_t));
  fb[1] = (uint16_t*)ps_malloc(W * H * sizeof(uint16_t));
  if (!fb[0] || !fb[1]) {
    Serial.println("ps_malloc framebuffer failed!");
    while (1) delay(1000);
  }

  // Little sanity stripe just to prove the panel is alive once
  for (int i = 0; i < W * 8; ++i) fb[0][i] = 0x07E0;
  panel.pushColors(0, 0, W, 8, fb[0]);

  mjpegBuf = (uint8_t*)ps_malloc(128 * 1024);
  if (!mjpegBuf) { Serial.println("ps_malloc mjpegBuf failed!"); while (1) delay(1000); }

  // Init TJpg_Decoder
  TJpgDec.setCallback(tjpg_out);
  TJpgDec.setSwapBytes(false);   // many T-RGB builds need this (flip if colors look wrong)

  // Scan folders (collects .mjpeg + .jpg)
  scanFolder("/doro",     files_doro);
  scanFolder("/neko",     files_neko);
  scanFolder("/azurlane", files_azur);
  Serial.printf("doro:%d neko:%d azurlane:%d\n", (int)files_doro.size(), (int)files_neko.size(), (int)files_azur.size());

  // Semaphores
  semFrameReady = xSemaphoreCreateBinary();
  semFrameUsed  = xSemaphoreCreateBinary();
  xSemaphoreGive(semFrameUsed);

  // Display task on core0 (loop on core1)
  xTaskCreatePinnedToCore(displayTask, "disp", 6144, nullptr, 1, nullptr, 0);

  // Wi-Fi AP + Web
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);

  // API endpoints
  server.on("/", handleUI);
  server.on("/status", handleStatus);
  server.on("/play",   handlePlayFile);  // works for mjpeg and jpg
  server.on("/next",   handleNext);
  server.on("/prev",   handlePrev);
  server.on("/pause",  handlePause);
  server.on("/resume", handleResume);
  server.on("/reopen", handleReopen);
  server.on("/stop",   handleStop);
  server.on("/test",   handleTest);

  server.onNotFound([](){
    String u = server.uri();
    if (u == "/" || u == "/index.html") { handleUI(); return; }
    server.send(404, "text/plain", "Not Found: " + u);
  });

  server.begin();
  Serial.println("AP up. Open http://192.168.4.1");

  // Try JPG first at startup, then sample names, then auto-scan
  if (!open_media_path(currentPath) &&
      !open_media_path("/sample.jpg") &&
      !open_media_path("/video.mjpeg"))
  {
    if (!open_first_media()) {
      Serial.println("No playable media found on SD!");
    }
  }
}

// ======= Max speed mode (no throttle) =======
#define TARGET_FPS 0
static const uint32_t frame_interval = (TARGET_FPS > 0) ? (1000 / TARGET_FPS) : 0;
static uint32_t last_frame_tick = 0;
static uint32_t last_ok_frame_ms = 0;

void loop() {
  server.handleClient();

  const uint32_t now = millis();
  if (frame_interval && (now - last_frame_tick < frame_interval)) return;
  last_frame_tick = now;

  if (needSwitch) {
    if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(200)) == pdTRUE) {
      String path = pendingPath;
      if (open_media_path(path)) {
        Serial.println(("Switched to " + path).c_str());
      } else {
        Serial.println(("Open failed: " + path).c_str());
      }
      needSwitch = false;
      xSemaphoreGive(semFrameUsed);
    }
  }

  if (isPaused) { delay(5); return; }

  // ===== Mode: MJPEG (streaming) =====
  if (mode == MediaMode::MJPEG) {
    if (mjpegFile && !mjpegFile.available()) {
      mjpegFile.seek(0); // loop current
    }

    if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(100)) == pdTRUE) {
      const int next_idx = (draw_idx == 0) ? 1 : 0;
      cur_fb = fb[next_idx];

      bool got_frame = false;
      if (mjpegFile && mjpeg.readMjpegBuf()) {
        mjpeg.drawJpg();
        got_frame = true;
      }

      if (got_frame) {
        ready_idx = next_idx;
        last_ok_frame_ms = now;
        xSemaphoreGive(semFrameReady);
      } else {
        xSemaphoreGive(semFrameUsed);
      }
    }

    if (now - last_ok_frame_ms > 1500) {
      Serial.println("[WARN] >1.5s no frame, reopen current...");
      open_mjpeg_path(currentPath);
      last_ok_frame_ms = now;
    }
    return;
  }

  // ===== Mode: JPG (still image; render once) =====
  if (mode == MediaMode::JPG) {
    if (!jpgNeedsRender) { delay(10); return; } // nothing to do
    if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(200)) == pdTRUE) {
      const int next_idx = (draw_idx == 0) ? 1 : 0;
      cur_fb = fb[next_idx];

      bool ok = renderJPGToFB(currentPath);
      if (ok) {
        ready_idx = next_idx;
        xSemaphoreGive(semFrameReady);
      } else {
        xSemaphoreGive(semFrameUsed);
      }
      jpgNeedsRender = false;
    }
    return;
  }
}
