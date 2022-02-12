# Components

## Introduction

板子基于ESP32-WROVER-IE模块，ESP32-











## ESP32-WROVER-IE

| WROVER PIN No | WROVER PIN NAME                         | EMMC Slot 1                             | LyraT |                                                              |
| ------------- | --------------------------------------- | --------------------------------------- | ----- | ------------------------------------------------------------ |
| 1             | GND                                     |                                         |       |                                                              |
| 2             | 3V3                                     |                                         |       |                                                              |
| 3             | EN                                      | RESET                                   |       |                                                              |
| 4             | SENSOR_VP (GPIO36, ADC1_CH0, RTC_GPIO0  | (VOL+)                                  | REC   |                                                              |
| 5             | SENSOR_VN (GPIO39, ADC1_CH3, RTC_GPIO3) | (VOL-)                                  | MODE  |                                                              |
| 6             | IO34 (GPIO34, ADC1_CH6, RTC_GPIO4)      |                                         |       | available                                                    |
| 7             | IO35                                    | I2S_ASDOUT                              |       |                                                              |
| 8             | IO32                                    |                                         | SET   | available                                                    |
| 9             | IO33                                    |                                         | PLAY  | available                                                    |
| 10            | IO25                                    | I2S_LRCK                                |       |                                                              |
| 11            | IO26                                    | I2S_DSDIN                               |       |                                                              |
| 12            | IO27                                    |                                         | VOL+  | available                                                    |
| 13            | IO14                                    | MMC_CLK                                 |       |                                                              |
| 14            | IO12 (MTDI)                             | MMD_D2                                  |       | strapping pin, pull down for 3.3V VDD_SDIO, otherwise SDIO is 1.8V |
| 15            | GND                                     |                                         |       |                                                              |
| 16            | IO13                                    | MMC_D3                                  | VOL-  |                                                              |
| 23            | IO15 (MTDO)                             | MMC_CMD                                 |       |                                                              |
| 24            | IO2                                     | MMC_D0 \| PULL_DOWN for download boot   |       |                                                              |
| 25            | IO0                                     | I2S_MCLK \| PULL_DOWN for download boot |       |                                                              |
| 26            | IO4                                     | MMC_D1                                  |       |                                                              |
| 29            | IO5                                     | I2S_SCLK                                |       |                                                              |
| 30            | IO18                                    | I2C_SDA                                 |       |                                                              |
| 31            | IO19                                    | U1RXD, connect to C3 IO6, U1RXD         |       |                                                              |
| 33            | IO21                                    |                                         |       | available                                                    |
| 34            | RXD0                                    | DEBUG                                   |       |                                                              |
| 35            | TXD0                                    | DEBUG                                   |       |                                                              |
| 36            | IO22                                    | U1TXD, connect to C3 IO7, U1TXD         |       |                                                              |
| 37            | IO23                                    | I2C_SCL                                 |       |                                                              |
| 38            | GND                                     |                                         |       |                                                              |
| 39            | GND (9 Pads, Bottom)                    |                                         |       |                                                              |



https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdmmc_host.html

Slot 0 emmc is not possible for GPIO 5/6/7/8/9/10/11 and 16/17/18 required,  but 6/7/8/9/10/11/16/17 are all missing.

C3 UART1 is not documented anywhere, but is used in esp-at https://github.com/espressif/esp-at/blob/master/main/interface/uart/at_uart_task.c

https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sd_pullup_requirements.html
