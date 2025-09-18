#include <LilyGo_RGBPanel.h>
#include <SD_MMC.h>
#include "MjpegClass.h"

#include <WiFi.h>
#include <WebServer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ----------------- 面板 / MJPEG -----------------
LilyGo_RGBPanel panel;
MjpegClass mjpeg;
File mjpegFile;

int W = 0, H = 0;

// 双帧缓冲：480*480*2B ≈ 450KB/帧
static uint16_t *fb[2] = { nullptr, nullptr };
static volatile int ready_idx = -1;   // 解码好等待显示的帧
static volatile int draw_idx  = -1;   // 正在显示的帧

// MJPEG 解码临时缓冲（>=128KB）
static uint8_t *mjpegBuf = nullptr;

// 将 MCU 块写入“当前目标帧缓冲”
static uint16_t *cur_fb = nullptr;
static int draw_to_fb(JPEGDRAW *p) {
  const uint16_t *src = (const uint16_t*)p->pPixels;  // RGB565 小端
  const int x = p->x, y = p->y, w = p->iWidth, h = p->iHeight;
  for (int r = 0; r < h; ++r) {
    memcpy(cur_fb + (y + r) * W + x, src + r * w, w * sizeof(uint16_t));
  }
  return 1;
}

// ----------------- 同步原语 -----------------
static SemaphoreHandle_t semFrameReady;  // 解码好一帧 -> 通知显示任务
static SemaphoreHandle_t semFrameUsed;   // 显示完一帧 -> 通知解码继续

// ----------------- 当前播放文件 & 切换请求 -----------------
static String currentPath = "/video.mjpeg";   // 默认文件
static volatile bool needSwitch = false;
static String pendingPath;

// ----------------- 打开视频（给当前路径） -----------------
static bool open_video_path(const String &path) {
  if (mjpegFile) mjpegFile.close();
  mjpegFile = SD_MMC.open(path);
  if (!mjpegFile) return false;
  mjpeg.setup(&mjpegFile, mjpegBuf, draw_to_fb, false, 0, 0, W, H);
  currentPath = path;
  return true;
}

// ----------------- 显示任务（独立核并行推屏） -----------------
static uint32_t fps_counter = 0, fps_last_ms = 0;

static void displayTask(void *) {
  for (;;) {
    if (xSemaphoreTake(semFrameReady, portMAX_DELAY) == pdTRUE) {
      const int idx = ready_idx;
      if (idx >= 0) {
        panel.pushColors(0, 0, W, H, fb[idx]);       // 推整帧
        draw_idx = idx;

        // FPS 统计
        fps_counter++;
        uint32_t now = millis();
        if (now - fps_last_ms >= 1000) {
          Serial.printf("Actual FPS: %u\n", fps_counter);
          fps_counter = 0;
          fps_last_ms = now;
        }
      }
      xSemaphoreGive(semFrameUsed); // 本帧已显示
    }
  }
}

// ----------------- WebServer（按钮切换） -----------------
WebServer server(80);
const char* AP_SSID = "Ciallo ~ (∠・ω < )";
const char* AP_PASS = "12345678";

static void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width, initial-scale=1.0"></head>
<body style="font-family: sans-serif;">
  <h2>ESP32 MJPEG Player</h2>
  <p>Current: <span id="cur"></span></p>
  <button onclick="fetch('/play?id=1')">Play 1</button>
  <button onclick="fetch('/play?id=2')">Play 2</button>
  <button onclick="fetch('/play?id=3')">Play 3</button>
  <button onclick="fetch('/play?id=0')">Play default</button>
  <script>
    async function refresh(){
      const r = await fetch('/status'); 
      const t = await r.text();
      document.getElementById('cur').textContent = t;
    }
    refresh(); setInterval(refresh, 1000);
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

static void handleStatus() {
  server.send(200, "text/plain", currentPath);
}

static void handlePlay() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing id");
    return;
  }
  int id = server.arg("id").toInt();
  String path;
  if      (id == 1) path = "/1.mjpeg";
  else if (id == 2) path = "/2.mjpeg";
  else if (id == 3) path = "/3.mjpeg";
  else              path = "/video.mjpeg"; // 默认

  pendingPath = path;
  needSwitch = true;                        // 标记切换
  server.send(200, "text/plain", "Switch to " + path);
}

// ----------------- Arduino 标准流程 -----------------
void setup() {
  Serial.begin(115200);

  if (!panel.begin(LILYGO_T_RGB_2_1_INCHES_HALF_CIRCLE)) {
    Serial.println("Panel init failed!");
    while (1);
  }
  panel.setBrightness(32);

  if (!panel.installSD()) {
    Serial.println("SD init failed!");
    while (1);
  }

  W = panel.width();
  H = panel.height();

  fb[0] = (uint16_t*)ps_malloc(W * H * sizeof(uint16_t));
  fb[1] = (uint16_t*)ps_malloc(W * H * sizeof(uint16_t));
  if (!fb[0] || !fb[1]) {
    Serial.println("ps_malloc framebuffer failed!");
    while (1);
  }

  // 上电可视测试（8px 绿条）
  for (int i = 0; i < W * 8; ++i) fb[0][i] = 0x07E0;
  panel.pushColors(0, 0, W, 8, fb[0]);

  // 解码读帧缓冲
  mjpegBuf = (uint8_t*)ps_malloc(128 * 1024);
  if (!mjpegBuf) {
    Serial.println("ps_malloc mjpegBuf failed!");
    while (1);
  }

  // 打开默认视频
  if (!open_video_path(currentPath)) {
    Serial.println("default video not found");
    while (1);
  }

  // 信号量
  semFrameReady = xSemaphoreCreateBinary();
  semFrameUsed  = xSemaphoreCreateBinary();
  xSemaphoreGive(semFrameUsed); // 允许先解码

  // 显示任务，绑到 core0（loop 默认 core1）
  xTaskCreatePinnedToCore(displayTask, "disp", 6144, nullptr, 1, nullptr, 0);

  // Wi-Fi AP + Web
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/play", handlePlay);
  server.begin();

  Serial.println("Ready. Connect to WiFi and open http://192.168.4.1/");
}

// ======= 节奏控制：极速模式（不节流） =======
#define TARGET_FPS 0
static const uint32_t frame_interval = (TARGET_FPS > 0) ? (1000 / TARGET_FPS) : 0;
static uint32_t last_frame_tick = 0;
static uint32_t last_ok_frame_ms = 0;

void loop() {
  server.handleClient();

  const uint32_t now = millis();
  if (frame_interval && (now - last_frame_tick < frame_interval)) return;
  last_frame_tick = now;

  // —— 处理“切换到新文件”的请求（安全点切换）——
  if (needSwitch) {
    // 等显示线程确认“上一帧已用完”，避免撕裂
    if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(200)) == pdTRUE) {
      String path = pendingPath;  // 拷贝到本地避免竞争
      if (open_video_path(path)) {
        Serial.println(("Switched to " + path).c_str());
      } else {
        Serial.println(("Open failed: " + path).c_str());
      }
      needSwitch = false;
      // 允许继续解码
      xSemaphoreGive(semFrameUsed);
    }
    // 继续往下跑本帧
  }

  if (!mjpegFile.available()) {
    mjpegFile.seek(0);                                  // 循环播放
  }

  // 等上一帧推屏完再覆盖后备缓冲
  if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(100)) == pdTRUE) {
    const int next_idx = (draw_idx == 0) ? 1 : 0;
    cur_fb = fb[next_idx];

    bool got_frame = false;
    if (mjpeg.readMjpegBuf()) {
      mjpeg.drawJpg();                                  // 解码到 cur_fb
      got_frame = true;
    }

    if (got_frame) {
      ready_idx = next_idx;                             // 标注可显示帧
      last_ok_frame_ms = now;
      xSemaphoreGive(semFrameReady);                    // 通知显示任务
    } else {
      // 本轮没解到完整帧，释放使用权，下一轮重试
      xSemaphoreGive(semFrameUsed);
    }
  }

  // 自恢复：超过 1s 没成功解码 -> 重开文件 / 重挂 SD
  if (now - last_ok_frame_ms > 1000) {
    Serial.println("[WARN] no frame >1s, reopen video...");
    open_video_path(currentPath); // 尝试重开当前文件
    last_ok_frame_ms = now;
  }
}
