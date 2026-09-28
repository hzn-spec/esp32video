# ESP32-S3 + SSD1306 OLED 视频播放器
#
# 独立于 ../esp32 那个 Wi-Fi LED 控制项目：
#   ../esp32        -> LED 网页控制 + OLED 显示
#   ../esp32-video  -> 电脑通过 Wi-Fi 推 1bpp 画面，OLED 当屏幕放视频
#
# 两个项目各自有独立的 build/ 和 sdkconfig，互不影响。

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-video)
