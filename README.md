# 🎬 ESP32-S3 T-RGB MJPEG/GIF Player

This project designs a **website UI** for playing **user-uploaded GIFs** converted to `.mjpeg`.  
Media is played at a consistent **480x480 resolution** and **15 FPS**.

---

## ✨ Features
- 🖼️ Plays `.mjpeg` videos from SD card
- 📱 Control via built-in Wi-Fi web interface
- ⚡ Double framebuffer for smooth rendering
- 🔄 Automatic loop playback

---

## 📸 Screenshots
### Web UI Example
![Web UI Screenshot](docs/ui-example.png)

### Running on T-RGB Display
![Device Running](docs/device-example.jpg)

*(Put your images in a `docs/` folder in the repo, then reference them like above.)*

---

## 🚀 Usage
1. Flash the firmware to your **ESP32-S3 T-RGB board**.
2. Place your `.mjpeg` files on an SD card.
3. Connect to the Wi-Fi AP `ESP32-MJPEG` and open `http://192.168.4.1`.

---

## 📂 Repository Structure
