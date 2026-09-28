/* SSD1306 128x64 I2C OLED 驱动
 *
 * 硬件抽象用的是 ESP-IDF 5.4 自带的官方驱动栈：
 *   i2c_master(新驱动) -> esp_lcd_panel_io_i2c -> esp_lcd_panel_ssd1306
 * 参数与官方示例 examples/peripherals/lcd/i2c_oled 完全一致（DC 位在第 6 位）。
 * 之所以不自己写初始化序列，是因为这套组合已经在同一块板子上实测跑通。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"

#include "oled.h"
#include "font6x8.h"

static const char *TAG = "oled";

static i2c_master_bus_handle_t s_bus   = NULL;
static esp_lcd_panel_handle_t  s_panel = NULL;
static SemaphoreHandle_t       s_lock  = NULL;
static uint8_t s_addr = 0;
static int     s_sda  = -1;
static int     s_scl  = -1;
static uint8_t s_fb[OLED_FB_SIZE];

/* ---------------- 对外查询 ---------------- */

int     oled_sda_gpio(void) { return s_sda; }
int     oled_scl_gpio(void) { return s_scl; }
uint8_t oled_i2c_addr(void) { return s_addr; }
uint8_t *oled_framebuffer(void) { return s_fb; }

/* ---------------- 锁 ---------------- */

void oled_lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void oled_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

/* ---------------- 显存绘图 ---------------- */

void oled_clear(int on)
{
    memset(s_fb, on ? 0xFF : 0x00, sizeof(s_fb));
}

void oled_text(int x, int page, const char *s)
{
    int y0 = page * 8;

    for (; *s; s++, x += FONT6X8_ADVANCE) {
        char c = *s;
        if (c < FONT6X8_FIRST || c > FONT6X8_LAST) c = '?';
        const uint8_t *glyph = FONT6X8[c - FONT6X8_FIRST];

        for (int col = 0; col < FONT6X8_COLS; col++) {
            int px = x + col;
            if (px < 0 || px >= OLED_WIDTH) continue;

            for (int row = 0; row < 8; row++) {
                if (!(glyph[col] & (1u << row))) continue;
                int py = y0 + row;
                if (py < 0 || py >= OLED_HEIGHT) continue;
                s_fb[(py >> 3) * OLED_WIDTH + px] |= (uint8_t)(1u << (py & 7));
            }
        }
    }
}

void oled_center_text(int page, const char *s)
{
    int x = (OLED_WIDTH - (int)strlen(s) * FONT6X8_ADVANCE) / 2;
    oled_text(x < 0 ? 0 : x, page, s);
}

/* ---------------- 刷屏 ---------------- */

void oled_flush(void)
{
    if (!s_panel) return;

    oled_lock();
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, s_fb);
    oled_unlock();
}

/* ---------------- 初始化 ---------------- */

/* 用给定的一组引脚建总线并扫描；找到屏幕就记住它，否则拆掉总线 */
static bool probe_pins(const oled_config_t *cfg, int sda, int scl)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .flags.enable_internal_pullup = true,
    };

    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "SDA=GPIO%d SCL=GPIO%d：建立 I2C 总线失败", sda, scl);
        s_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "尝试 SDA=GPIO%d SCL=GPIO%d ...", sda, scl);

    int first_found = 0;
    int ssd1306 = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 50) != ESP_OK) continue;
        ESP_LOGI(TAG, "  I2C 设备在线: 0x%02X", a);
        if (!first_found) first_found = a;
        if (a == 0x3C || a == 0x3D) { ssd1306 = a; break; }
    }

    int addr = ssd1306 ? ssd1306 : first_found;
    if (!addr) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return false;
    }

    s_addr = (uint8_t)addr;
    s_sda  = sda;
    s_scl  = scl;
    ESP_LOGI(TAG, "使用地址 0x%02X（SDA=GPIO%d, SCL=GPIO%d）", s_addr, sda, scl);
    (void)cfg;
    return true;
}

esp_err_t oled_init(const oled_config_t *cfg)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* 1) 找屏幕：先按配置的接法，找不到再对调 SDA/SCL 试一次 */
    if (!probe_pins(cfg, cfg->sda_gpio, cfg->scl_gpio) &&
        !(cfg->allow_swap && probe_pins(cfg, cfg->scl_gpio, cfg->sda_gpio))) {
        ESP_LOGE(TAG, "没找到 SSD1306！检查接线：VCC->3V3, GND->GND, "
                      "SCL->GPIO%d, SDA->GPIO%d",
                 cfg->scl_gpio, cfg->sda_gpio);
        return ESP_ERR_NOT_FOUND;
    }

    /* 2) 装 panel IO。SSD1306 的控制字节：0x00=命令，0x40=数据 */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = s_addr,
        .scl_speed_hz = cfg->scl_hz,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_err_t ret = esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 panel IO 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3) 装 SSD1306 面板驱动（4 针模块没有 RST 脚） */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };
    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = OLED_HEIGHT };
    panel_cfg.vendor_config = &ssd_cfg;

    ret = esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 SSD1306 面板失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    oled_clear(0);
    oled_flush();

    ESP_LOGI(TAG, "SSD1306 就绪：%dx%d，I2C %u Hz", OLED_WIDTH, OLED_HEIGHT,
             (unsigned)cfg->scl_hz);
    return ESP_OK;
}
