#include <LilyGo_RGBPanel.h>
#include <SD_MMC.h>
#include "MjpegClass.h"

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

// MJPEG 解码临时缓冲（建议 >=128KB）
static uint8_t *mjpegBuf = nullptr;

// ------- 可选清零策略（帧首 MCU 才清一次）-------
#define CLEAR_ON_FIRST_MCU 0   // 0=不清，1=仅在每帧第一块MCU时清一次
static volatile bool first_mcu_of_frame = false;

// 将 MCU 块写入“当前目标帧缓冲”
static uint16_t *cur_fb = nullptr;
static int draw_to_fb(JPEGDRAW *p) {
  const uint16_t *src = (const uint16_t*)p->pPixels;  // RGB565 小端
  const int x = p->x, y = p->y, w = p->iWidth, h = p->iHeight;

#if CLEAR_ON_FIRST_MCU
  if (first_mcu_of_frame && p->x == 0 && p->y == 0) {
    // 仅每帧第一次调用 draw 回调时清一次，代价远小于每帧/每块都清
    memset(cur_fb, 0, W * H * sizeof(uint16_t));
    first_mcu_of_frame = false;
  }
#endif

  for (int r = 0; r < h; ++r) {
    memcpy(cur_fb + (y + r) * W + x, src + r * w, w * sizeof(uint16_t));
  }
  return 1;
}

// ----------------- VSYNC（默认关闭，确保必出画） -----------------
#define USE_VSYNC_WAIT 0
const int VSYNC_PIN = 41;
static inline void wait_vsync_edge(uint32_t timeout_us = 20000) {
#if USE_VSYNC_WAIT
  pinMode(VSYNC_PIN, INPUT);
  int prev = digitalRead(VSYNC_PIN);
  uint32_t t0 = micros();
  while (digitalRead(VSYNC_PIN) == prev) {
    if ((micros() - t0) > timeout_us) break;
  }
#endif
}

// ----------------- 同步原语 -----------------
static SemaphoreHandle_t semFrameReady;  // 解码好一帧 -> 通知显示任务
static SemaphoreHandle_t semFrameUsed;   // 显示完一帧 -> 通知解码继续

// ----------------- 打开视频 -----------------
static bool open_video() {
  if (mjpegFile) mjpegFile.close();
  mjpegFile = SD_MMC.open("/video.mjpeg");
  if (!mjpegFile) return false;
  mjpeg.setup(&mjpegFile, mjpegBuf, draw_to_fb, false, 0, 0, W, H);
  return true;
}

// ----------------- 显示任务（独立核并行推屏） -----------------
static uint32_t fps_counter = 0, fps_last_ms = 0;

static void displayTask(void *) {
  for (;;) {
    if (xSemaphoreTake(semFrameReady, portMAX_DELAY) == pdTRUE) {
      const int idx = ready_idx;
      if (idx >= 0) {
        wait_vsync_edge();                           // 默认关闭，不阻塞
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

  // 解码读帧缓冲（建议 >=128KB）
  mjpegBuf = (uint8_t*)ps_malloc(192 * 1024);   // 失败则退到 128KB
  if (!mjpegBuf) mjpegBuf = (uint8_t*)ps_malloc(128 * 1024);
  if (!mjpegBuf) {
    Serial.println("ps_malloc mjpegBuf failed!");
    while (1);
  }

  if (!open_video()) {
    Serial.println("/video.mjpeg not found");
    while (1);
  }

  // 信号量
  semFrameReady = xSemaphoreCreateBinary();
  semFrameUsed  = xSemaphoreCreateBinary();
  xSemaphoreGive(semFrameUsed); // 允许先解码

  // 创建显示任务，绑到 core0（loop 默认 core1）
  xTaskCreatePinnedToCore(displayTask, "disp", 6144, nullptr, 1, nullptr, 0);

  Serial.println("MJPEG player ready (pipelined, fastest)!");
}

// ======= 节奏控制 =======
// 0 = 极速模式（不节流，能多快播多快）
#define TARGET_FPS 0
static const uint32_t frame_interval = (TARGET_FPS > 0) ? (1000 / TARGET_FPS) : 0;
static uint32_t last_frame_tick = 0;
static uint32_t last_ok_frame_ms = 0;

void loop() {
  const uint32_t now = millis();

  // 仅当设置了固定FPS时才节流
  if (frame_interval && (now - last_frame_tick < frame_interval)) return;
  last_frame_tick = now;

  if (!mjpegFile.available()) {
    mjpegFile.seek(0);                                  // 循环播放
  }

  // 等上一帧推屏完再覆盖后备缓冲
  if (xSemaphoreTake(semFrameUsed, pdMS_TO_TICKS(100)) == pdTRUE) {
    const int next_idx = (draw_idx == 0) ? 1 : 0;
    cur_fb = fb[next_idx];

#if CLEAR_ON_FIRST_MCU
    first_mcu_of_frame = true;   // 标记“本帧首块MCU到来时清一次”
#endif

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
    if (!open_video()) {
      Serial.println("[WARN] reopen failed, remount SD...");
      SD_MMC.end();
      panel.uninstallSD();
      delay(10);
      panel.installSD();
      open_video();
    }
    last_ok_frame_ms = now;
  }
}
