# Components

## Introduction

板子基于ESP32-WROVER-IE模块，ESP32-











## ESP32-WROVER-IE

| WROVER PIN No | WROVER PIN NAME                         | Connie            | LyraT                                                        | Comment                                                      |
| ------------- | --------------------------------------- | ----------------- | ------------------------------------------------------------ | ------------------------------------------------------------ |
| 1             | GND                                     |                   |                                                              |                                                              |
| 2             | 3V3                                     |                   |                                                              |                                                              |
| 3             | EN                                      | RESET             | RESET                                                        |                                                              |
| 4             | SENSOR_VP (GPIO36, ADC1_CH0, RTC_GPIO0  | POWER Button      | REC Button                                                   | input only, RTC wakeup                                       |
| 5             | SENSOR_VN (GPIO39, ADC1_CH3, RTC_GPIO3) | MODE Button       | MODE Button                                                  | input only                                                   |
| 6             | IO34 (GPIO34, ADC1_CH6, RTC_GPIO4)      | Factory Boot      | SD_DET (10k pull-up to 3V3, but not used in driver, SDMMC_SLOT_NO_CD) | input only                                                   |
| 7             | IO35                                    | I2S_ASDOUT        | I2S_ASDOUT                                                   |                                                              |
| 8             | IO32                                    | **Reset C3**      | SET Touch                                                    |                                                              |
| 9             | IO33                                    | PLAY Button / LED | PLAY Touch                                                   | On "power off", output low                                   |
| 10            | IO25                                    | I2S_LRCK          | I2S_LRCK                                                     |                                                              |
| 11            | IO26                                    | I2S_DSDIN         | I2S_DSDIN                                                    |                                                              |
| 12            | IO27                                    | VOL+ Button / LED | VOL+ Touch                                                   | on "power off", output low                                   |
| 13            | IO14                                    | MMC_CLK           | MMC_CLK (DIP 6 off) \| MTMS (DIF 6 on)                       |                                                              |
| 14            | IO12 (MTDI)                             | LED pull down     | MMC_D2 (DIP 1 on) \| MTDI (DIP 5 on) \| Aux Detect (DIP 7 on) | strapping pin, pull down for 3.3V VDD_SDIO, otherwise SDIO is 1.8V; to override this behavior and resolve the conflict, efuse must be written. |
| 15            | GND                                     |                   |                                                              |                                                              |
| 16            | IO13                                    | VOL- Button       | MMC_D3 (DIP 2 on) or VOL- (DIP 2 off, Touch)                 |                                                              |
| 23            | IO15 (MTDO)                             | MMC_CMD           | MMC_CMD                                                      |                                                              |
| 24            | IO2                                     | MMC_D0            | MMC_D0                                                       | PULL_DOWN for download boot                                  |
| 25            | IO0                                     | I2S_MCLK          | I2S_MCLK                                                     | PULL_DOWN for download boot                                  |
| 26            | IO4                                     | MMC_D1            | MMC_D1                                                       |                                                              |
| 29            | IO5                                     | I2S_SCLK          | I2S_SCK                                                      |                                                              |
| 30            | IO18                                    | I2C_SDA           | I2C_SDA                                                      |                                                              |
| 31            | IO19                                    | U1RXD             | Phone Jack Detection (pulled up to 3V3)                      | 1. defined by GPIO matrix (connect to C3 IO6, U1RXD, on EVT2)<br />2. weak pullup for maintaing voltage |
| 33            | IO21                                    | PA control        | PA control (pull down, 2 x 10k )                             | though different PA, should check voltage.                   |
| 34            | RXD0                                    | DEBUG             |                                                              |                                                              |
| 35            | TXD0                                    | DEBUG             |                                                              |                                                              |
| 36            | IO22                                    | U1TXD             | Green LED (pull down, 10k)                                   | 1. defined by GPIO matrix (connect to C3 IO7, U1TXD on EVT2)<br />2. weak pull-up for maintaining voltage and detecting board |
| 37            | IO23                                    | I2C_SCL           |                                                              |                                                              |
| 38            | GND                                     |                   |                                                              |                                                              |
| 39            | GND (9 Pads, Bottom)                    |                   |                                                              |                                                              |



https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdmmc_host.html

Slot 0 emmc is not possible for GPIO 5/6/7/8/9/10/11 and 16/17/18 required,  but 6/7/8/9/10/11/16/17 are all missing.

C3 UART1 is not documented anywhere, but is used in esp-at https://github.com/espressif/esp-at/blob/master/main/interface/uart/at_uart_task.c

https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sd_pullup_requirements.html



Logic OR https://item.szlcsc.com/10635.html SN74LVC1G32DBVR Using PLAY | Vol+ to drive 



https://esp32.com/viewtopic.php?t=2055



10kOhm and 1uF RC delay for Reset, mentioned in wrover-ie datasheet, https://www.espressif.com/sites/default/files/documentation/esp32-wrover-e_esp32-wrover-ie_datasheet_cn.pdf p16



https://esp32.com/viewtopic.php?t=2055

according to data published by igrr (https://github.com/igrr) in esp32 forum:



https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf

io mux, page 60





https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/efuse.html

is not disabled by efuse (espefusepy -p summary)



octavo systems' note on emmc routing

https://octavosystems.com/app_notes/designing-for-flexibility-around-emmc/



https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sd_pullup_requirements.html#strapping-conflicts-dat2