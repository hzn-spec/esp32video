# ESP32-S3 OLED 视频播放器

把电脑上的视频（或摄像头）实时推到 ESP32-S3 上那块 0.96 寸 SSD1306 单色屏上播放。

在一块 128×64、1 位色的屏幕上放视频，难点不在解码，而在**怎么把数据尽量快地送进屏幕**。
这个项目把重活全交给电脑，ESP32 只负责收帧和写 I2C。

```
电脑：读视频 -> 缩放成 128x64 -> Floyd-Steinberg 抖动成 1bpp -> TCP 推送
ESP32：收 1024 字节 -> 写 I2C -> 再收下一帧
```

## 实测数据

| 项目 | 结果 |
|---|---|
| 稳定帧率 | **30.0 fps** |
| 帧率上限 | **39.4 fps**（I2C 400kHz 的物理极限，每帧约 23ms） |
| 单帧数据 | 1024 字节 |
| 端到端延迟 | 单帧以内 |

瓶颈完全在 I2C 总线上：把目标帧率设成 60，ESP32 依然只报 39.4 fps，
说明 Wi-Fi 侧还远远没跑满。

## 硬件

- **ESP32-S3-DevKitC-1 N16R8**（16MB Flash / 8MB PSRAM）
- **0.96 寸 SSD1306 OLED**，I2C 4 针，地址 `0x3C`

| OLED 模块 | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | GPIO5 |
| SDA | GPIO4 |

接反了也不要紧：固件找不到屏幕时会自动把 SDA/SCL 对调再试一次，并把结果打进日志。

> 换引脚或换板子：`idf.py menuconfig` → **Video Player Configuration**。
> 换 Flash 容量：改 `sdkconfig.defaults` 里的 `CONFIG_ESPTOOLPY_FLASHSIZE_*`。

## 快速开始

### 1. 编译烧录

需要 **ESP-IDF v5.4**（5.2 以上应该都能用，未测）。

```powershell
# 先导入 ESP-IDF 环境（IDF 安装器生成的 PowerShell 快捷方式，或 export.ps1）
cd esp32-video

idf.py set-target esp32s3
idf.py menuconfig     # Video Player Configuration -> 填你的 Wi-Fi
idf.py -p COM6 flash monitor
```

> 不跑 menuconfig 也能编译，但用的是 `Kconfig.projbuild` 里的占位符
> `your-wifi-ssid`，连不上网。

开机后 OLED 会显示自己的 IP 和端口，这就是电脑要连的地址：

```
     VIDEO READY
   IP 10.144.179.174
      PORT 8888
   waiting PC...
```

### 2. 电脑端推流

电脑必须连到**同一个** Wi-Fi（同一个网段）。OLED 上显示哪个 IP，`--host` 就填哪个。

```powershell
pip install opencv-python pillow numpy

cd pc
python send_video.py --host 10.144.179.174 --source test            # 自检画面
python send_video.py --host 10.144.179.174 --source movie.mp4 --loop
python send_video.py --host 10.144.179.174 --source 0               # 摄像头
python send_video.py --host 10.144.179.174 --source "text:hello"    # 只发一行字
```

仓库里带了一段 12 秒的测试素材 `pc/test_clip.mp4`，可以直接拿来验证。
想自己生成：

```bash
ffmpeg -f lavfi -i "testsrc2=duration=12:size=426x240:rate=25" \
       -pix_fmt yuv420p -c:v libx264 -preset veryfast test_clip.mp4
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--fps 30` | 目标帧率。超过 I2C 上限会被自动压回来，不会出错 |
| `--no-dither` | 关掉抖动，纯二值化（更快，但暗部会糊成一片） |
| `--invert` | 黑白反转（浅色背景的视频用得上） |
| `--no-autocontrast` | 关掉逐帧自动对比度 |

## 通信协议

5 字节头 + 载荷，跑在裸 TCP 上：

| 偏移 | 内容 |
|---|---|
| 0–1 | 魔数 `A5 5A`，用来发现失步 |
| 2 | 类型：`1` = 帧，`2` = 文字（ASCII） |
| 3–4 | 载荷长度，小端 uint16 |

帧载荷固定 1024 字节，就是 SSD1306 的原生页格式：
每字节表示 8 个纵向像素，bit0 在最上方、bit7 在最下方，共 8 页 × 128 列。

想用别的语言写发送端，照着这个格式打包就行。

## 目录结构

```
esp32-video/
├── CMakeLists.txt
├── sdkconfig.defaults      干净克隆时的默认硬件配置
├── main/
│   ├── Kconfig.projbuild   Wi-Fi / 引脚 / I2C 频率
│   ├── video_main.c        Wi-Fi + TCP 收帧 + 主循环
│   ├── oled.c / oled.h     SSD1306 驱动
│   └── font6x8.h           6x8 点阵字库
└── pc/
    ├── send_video.py       电脑端发送程序
    └── test_clip.mp4       测试素材
```

`build/` 和 `sdkconfig` 都被 `.gitignore` 忽略了——
它们体积大（约 138MB / 70KB），而且是本机生成物。

## 踩过的坑

开发过程中真实踩到的四个，都记在这里省得你重踩。

### 1. PIL 的 `'1'` 模式转 numpy 得到的是 `bool` 数组，不是 0/255

最坑的一个。写成这样：

```python
bits = (np.asarray(img, dtype=np.uint8) > 127)   # 结果全是 0
```

`True` 先被转成 `1`，再和 127 比，永远为假。表现是
**帧率正常、链路正常、屏幕全黑**，而且一行报错都没有，极难定位。

必须写：

```python
bits = (np.asarray(img) != 0)
```

`send_video.py` 启动时会先打包一个棋盘格自检（应恰好 4096 个亮点），
专门用来挡住这类「静默失败」。

### 2. Wi-Fi 省电模式必须关掉

ESP-IDF 默认的 `WIFI_PS_MIN_MODEM` 要等 DTIM 才唤醒收包，
实测 ping 要 200ms 以上，推流会一顿一顿。加一行就好了：

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

### 3. 扫描 / 连接不能写在 Wi-Fi 事件回调里

在里面做阻塞扫描会把事件循环卡死。本项目的做法是回调只置一个事件位，
真正的扫描和连接交给独立任务做。

### 4. 抖动比直接二值化好看太多

`Image.convert("1")` 默认走 Floyd–Steinberg。单色小屏上，
照片类画面有没有抖动是天壤之别，暗场景尤其明显。

### 5. `sdkconfig.defaults` 里不能写非 ASCII 字符

这个坑是发布仓库时才发现的，而且很隐蔽。

`sdkconfig.defaults` 由 kconfgen 解析，而 kconfgen 用的是**系统区域编码**。
在中文 Windows（GBK）上，文件里任何一个中文注释都会让构建在配置阶段就挂掉，
根本进不到编译：

```
UnicodeDecodeError: 'gbk' codec can't decode byte 0xae in position 24
CMake Error at tools/cmake/kconfig.cmake:209 (message):
  Failed to run kconfgen
```

**Kconfig 解析链上的文件（`sdkconfig.defaults`、`Kconfig`、`Kconfig.projbuild`）
请保持纯 ASCII。** README 这类文档不受影响，随便写中文。

## 疑难杂症

### 编译报 internal compiler error

如果碰到这种，不是你的代码问题：

```
esp_lcd_panel_rgb.c:686:1: internal compiler error: Segmentation fault
```

先关掉 ccache 再试，一般就好了：

```powershell
$env:CCACHE_DISABLE = "1"
idf.py build
```

ccache 在 Windows + xtensa-gcc 上偶发会触发编译器崩溃，与项目代码无关。

## 想再快一点

瓶颈是 I2C 时钟。`VIDEO_I2C_HZ` 从 `400000` 提到 `1000000`，
理论上能把上限从 39 提到 70 fps 左右。

**但不是所有 SSD1306 模块都能跑满 1MHz**，跑不动会花屏，改之前先试。

## 许可

未指定许可证。如果要让别人自由使用，建议补一个 MIT。
