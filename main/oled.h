/* SSD1306 128x64 I2C OLED 驱动（视频项目精简版）
 *
 * 显存格式就是 SSD1306 的原生页格式：每字节代表 8 个纵向像素，
 * bit0 在最上方、bit7 在最下方，共 1024 字节。电脑端按同样格式打包即可。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)                 /* 8 页 */
#define OLED_FB_SIZE    (OLED_WIDTH * OLED_HEIGHT / 8)    /* 1024 字节 */

typedef struct {
    int      sda_gpio;
    int      scl_gpio;
    uint32_t scl_hz;
    bool     allow_swap;    /* 找不到屏幕时把 SDA/SCL 对调再试一次 */
} oled_config_t;

/* 建 I2C 总线、扫描地址、初始化 SSD1306 面板 */
esp_err_t oled_init(const oled_config_t *cfg);

/* 初始化完成后才知道实际用的引脚和地址 */
int     oled_sda_gpio(void);
int     oled_scl_gpio(void);
uint8_t oled_i2c_addr(void);

/* 1bpp 显存，可直接往里写整帧画面（1024 字节） */
uint8_t *oled_framebuffer(void);

/* 把显存刷到屏幕。内部会加锁，不会和别的任务抢 I2C */
void oled_flush(void);

/* 下面是纯显存操作，不碰 I2C。
 * 要「先清屏、再写字、最后刷新」这种连续动作时，请用 oled_lock()/oled_unlock()
 * 包起来，否则可能被视频流任务插进来画花。 */
void oled_clear(int on);
void oled_text(int x, int page, const char *s);
void oled_center_text(int page, const char *s);

void oled_lock(void);
void oled_unlock(void);
