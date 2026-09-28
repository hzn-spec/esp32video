/* ESP32-S3 + SSD1306 OLED 视频播放器
 *
 * 思路：128x64 单色 = 每帧正好 1024 字节，ESP32 根本没有能力解码真视频，
 * 所以重活全交给电脑：电脑把视频缩放、抖动成 1bpp，再通过 TCP 推过来，
 * ESP32 只负责「收一帧 -> 写 I2C -> 再收下一帧」，这条路最省事也最快。
 *
 * 瓶颈完全在 I2C：1024 字节数据 + 控制字节，400kHz 下每帧约 23ms，
 * 所以帧率上限约 40fps，实测通常能到 30fps 上下。
 *
 * 通信协议（5 字节头 + 载荷）：
 *   [0] 0xA5  [1] 0x5A  魔数，用来发现失步
 *   [2] 类型   1=帧(1024 字节 1bpp)  2=文字(ASCII)
 *   [3..4] 载荷长度，小端 uint16
 */
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "oled.h"

static const char *TAG = "video";

#define WIFI_STARTED_BIT    BIT0
#define WIFI_CONNECTED_BIT  BIT1

#define MAGIC0        0xA5
#define MAGIC1        0x5A
#define MSG_FRAME     1
#define MSG_TEXT      2
#define MAX_PAYLOAD   OLED_FB_SIZE          /* 1024 */

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t  s_reconnect_sem;

static char s_ip[16] = "0.0.0.0";

/* 文字消息的落地缓冲。放静态区是为了不占任务栈（任务栈只有 4KB） */
static char s_text[MAX_PAYLOAD + 1];

/* ==================== 屏幕画面 ==================== */

static void draw_lines(const char *l0, const char *l1, const char *l2, const char *l3)
{
    oled_lock();
    oled_clear(0);
    if (l0) oled_center_text(0, l0);
    if (l1) oled_center_text(2, l1);
    if (l2) oled_center_text(4, l2);
    if (l3) oled_center_text(6, l3);
    oled_flush();
    oled_unlock();
}

/* 待机页：把 IP 和端口显示出来，方便电脑端填 --host */
static void show_idle_screen(void)
{
    char ip_line[24];
    char port_line[24];

    snprintf(ip_line, sizeof(ip_line), "IP %s", s_ip);
    snprintf(port_line, sizeof(port_line), "PORT %d", CONFIG_VIDEO_STREAM_PORT);

    draw_lines("VIDEO READY", ip_line, port_line, "waiting PC...");
}

/* ==================== Wi-Fi ==================== */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_events, WIFI_STARTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi 断开（原因码 %d），准备重连", disc->reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* 不在事件回调里 delay，交给 wifi_connect_task 慢慢重试 */
        xSemaphoreGive(s_reconnect_sem);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_events    = xEventGroupCreate();
    s_reconnect_sem  = xSemaphoreCreateBinary();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_OPEN,   /* 密码够长时 IDF 会自动提到 WPA2 */
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 关键：默认的省电模式要等 DTIM 才唤醒收包，测出来 ping 要 200ms 以上，
     * 推流会一顿一顿的。视频流必须关掉它。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "已关闭 Wi-Fi 省电模式（推流必须）");
}

/* 先连一次，之后每次掉线都等 1.5 秒重连，永远不放弃 */
static void wifi_connect_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_STARTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "连接 \"%s\" ...", CONFIG_WIFI_SSID);
    esp_wifi_connect();

    while (1) {
        xSemaphoreTake(s_reconnect_sem, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1500));
        draw_lines(NULL, "WiFi lost", "reconnecting", NULL);
        ESP_LOGI(TAG, "重连 \"%s\" ...", CONFIG_WIFI_SSID);
        esp_wifi_connect();
    }
}

/* ==================== TCP 收帧 ==================== */

/* 收满 len 字节才算成功；返回 0 成功，-1 连接断了 */
static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len) {
        int n = recv(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ESP_LOGW(TAG, "收数据超时，断开这个客户端");
            return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static void serve_client(int fd)
{
    uint8_t hdr[5];
    uint8_t *fb = oled_framebuffer();

    uint32_t frames = 0;
    int64_t  t0 = esp_timer_get_time();

    draw_lines(NULL, "STREAMING", NULL, NULL);

    while (1) {
        if (recv_all(fd, hdr, sizeof(hdr)) != 0) break;

        if (hdr[0] != MAGIC0 || hdr[1] != MAGIC1) {
            ESP_LOGW(TAG, "帧头对不上 (0x%02X 0x%02X)，断开重连", hdr[0], hdr[1]);
            break;
        }

        uint16_t len = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
        if (len > MAX_PAYLOAD) {
            ESP_LOGE(TAG, "载荷长度 %u 超过上限 %d，断开", len, MAX_PAYLOAD);
            break;
        }

        if (hdr[2] == MSG_FRAME && len == OLED_FB_SIZE) {
            /* 直接收进显存，省掉一次 memcpy */
            if (recv_all(fd, fb, OLED_FB_SIZE) != 0) break;
            oled_flush();                   /* 唯一的重活：400kHz 下约 23ms */
            frames++;
        } else if (hdr[2] == MSG_TEXT) {
            if (recv_all(fd, s_text, len) != 0) break;
            s_text[len] = '\0';
            draw_lines(NULL, NULL, s_text, NULL);
        } else {
            if (recv_all(fd, fb, len) != 0) break;   /* 未知类型：丢掉 */
        }

        int64_t dt = esp_timer_get_time() - t0;
        if (dt >= 5000000) {
            ESP_LOGI(TAG, "实测 %.1f fps", frames * 1000000.0 / (double)dt);
            frames = 0;
            t0 = esp_timer_get_time();
        }
    }

    ESP_LOGI(TAG, "客户端已断开");
}

static void stream_server_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() 失败");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(CONFIG_VIDEO_STREAM_PORT),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() 失败，端口 %d 可能被占用", CONFIG_VIDEO_STREAM_PORT);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() 失败");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP 服务已启动，端口 %d，等电脑推流", CONFIG_VIDEO_STREAM_PORT);
    show_idle_screen();

    while (1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char peer_ip[16] = "?";
        inet_ntoa_r(peer.sin_addr, peer_ip, sizeof(peer_ip));
        ESP_LOGI(TAG, "电脑 %s 已连接", peer_ip);

        /* 小包别攒着，攒了就是一帧的延迟 */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* 电脑如果半路跑了，别让 recv 永远挂着 */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        serve_client(fd);
        close(fd);
        show_idle_screen();
    }
}

/* ==================== 入口 ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 OLED 视频播放器启动");

    oled_config_t ocfg = {
        .sda_gpio   = CONFIG_VIDEO_SDA_GPIO,
        .scl_gpio   = CONFIG_VIDEO_SCL_GPIO,
        .scl_hz     = CONFIG_VIDEO_I2C_HZ,
        .allow_swap = CONFIG_VIDEO_ALLOW_SWAP,
    };
    if (oled_init(&ocfg) == ESP_OK) {
        draw_lines("ESP32-S3 VIDEO", "boot ok", NULL, NULL);
    } else {
        ESP_LOGE(TAG, "OLED 没起来，后面收帧也会照常跑，但你看不到画面");
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    xTaskCreate(wifi_connect_task,  "wifi_conn", 4096, NULL, 5, NULL);
    xTaskCreate(stream_server_task, "stream",    4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "启动完成，等电脑连 %s:%d", s_ip, CONFIG_VIDEO_STREAM_PORT);
}
