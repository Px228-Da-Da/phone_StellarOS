# Паспорт устройства

| Параметр | Значение | Источник |
|---|---|---|
| Кодовое имя | **merlinnfc** | `ro.product.device` |
| Модель | M2003J15SC / M2003J15SG | `ro.product.model` |
| Маркетинг | Redmi Note 9 NFC (Global) | `ro.boot.hwc` = Global |
| Платформа | **mt6768** | `ro.board.platform` |
| SoC | MediaTek Helio G85 | |
| CPU | 2x Cortex-A75 @2.0 + 6x Cortex-A55 @1.8 | ARMv8-A |
| RAM | 4 ГБ LPDDR4X | |
| Storage | 128 ГБ **eMMC** (`mmcblk0`, 53 раздела) | `/dev/block/by-name` |
| Экран | 1080x2340, IPS, MIPI DSI | |
| Прошивка | MIUI V13.0.2.0.SJOMIXM, Android 12 | `ro.build.version.incremental` |
| Ядро Linux | 4.14.186 | |
| Загрузчик | **РАЗБЛОКИРОВАН** 04.09.2026 | `fastboot getvar unlocked` = yes |
| OEM unlock | разрешён в настройках | `sys.oem_unlock_allowed` = 1 |
| Verified boot | отключён после разлочки | `fastboot getvar secure` = no |

## Карта регистров — снята с живого устройства

Имена узлов в `/proc/device-tree` содержат базовые адреса, поэтому
угадывать ничего не пришлось. Полный список: `device-info/dt-nodes.txt`.

| Блок | Адрес | Узел |
|---|---|---|
| UART0 | `0x11002000` | `serial@11002000` |
| UART1 | `0x11003000` | `serial@11003000` |
| GIC distributor | `0x0C000000` | `gic500@0c000000` — **GIC-500, то есть GICv3** |
| GIC CPU | `0x0C400000` | `gic_cpu@0c400000` |
| DISP_OVL0 | `0x1400B000` | `disp_ovl0@1400b000` |
| DISP_RDMA0 | `0x1400D000` | `disp_rdma0@1400d000` |
| DISP_PWM | `0x1100E000` | `disp_pwm@1100e000` |
| I2C0..I2C8 | `0x11007000`+ | `i2c0@11007000` ... |
| USB | `0x11200000` | `usb0@11200000` |

Две поправки к первоначальным предположениям:
- DISP_OVL0 оказался по `0x1400B000`, а не `0x14008000`.
- Контроллер прерываний — **GICv3**, значит драйвер пишем через системные
  регистры `ICC_*`, а не через MMIO-интерфейс GICv2.

## Карта периферии целиком

Снято с работающего Android через `sysfs`, чтобы больше не возвращаться:
привязка драйверов там видна без root, в отличие от значений свойств
device tree, которые закрыты SELinux.

### Ввод и вывод

| Устройство | Где | Драйвер Android |
|---|---|---|
| **Тачскрин Novatek** | SPI0 `0x1100A000`, cs 0, 8 МГц | `NVT-ts` |
| Отпечаток пальца | SPI2 `0x11012000` | `goodix_fp` |
| Защищённый элемент | SPI5 `0x11015000` | `p61` |
| Вибромотор | `odm:vibrator@0` | — |
| Кнопки | `kpd` (клавиатурный контроллер) | — |

### Экран

| Блок | Адрес |
|---|---|
| mmsys_config | `0x14000000` |
| disp_mutex0 | `0x14001000` |
| **disp_ovl0** | `0x1400B000` |
| disp_ovl0_2l | `0x1400C000` |
| **disp_rdma0** | `0x1400D000` |
| disp_wdma0 | `0x1400E000` |
| color / ccorr / aal / gamma / dither | `0x1400F000` … `0x14013000` |
| **dsi0** | `0x14014000` |
| disp_rsz0 | `0x14015000` |
| Подсветка (PWM) | `0x1100E000` |
| Подсветка (I2C) | `lm3697@36` на I2C0, драйвер `ti-lmu` |
| Питание панели | `lcd_bias@3e` на I2C0, драйвер `ocp2131_bias` |

### GPIO и прерывания от выводов

| Блок | Адрес | Зачем |
|---|---|---|
| **gpio** | `0x10005000` (4 КБ) | направление, чтение и запись выводов |
| io_cfg × 8 | `0x10002000`…`0x10002E00`, по `0x200` | подтяжки и сила тока |
| **apirq (EINT)** | `0x1000B000` (4 КБ) | прерывания от выводов, в том числе от тача |

Выводов 186 (`gpio-ranges` до `0xba`), pinctrl — `mediatek,mt6768-pinctrl`.

### Звук

| Блок | Где |
|---|---|
| AFE (звуковой тракт) | `0x11220000`, `mediatek,mt6768-sound`, INTID 201 |
| Память AFE | `0x11221000` |
| **Усилитель динамика** | SPI3 `0x11013000`, Cirrus `cs35l41` |
| Второй усилитель | `speaker_amp@34` на I2C7 |
| Звук по Bluetooth | `0x18050000`, `mtk-btcvsd-snd` |

Микрофоны и наушники идут через тот же AFE: у MediaTek аналоговая часть
живёт в PMIC, отдельного кодека на шине нет.

### Связь

| Что | Где | Драйвер |
|---|---|---|
| **Wi-Fi** | `0x18000000` | `wlan` |
| **Bluetooth** | `0x1100C000` (BTIF) | `mtk_btif` |
| **Модем** | интерфейсы `ccmni0`…`ccmni20` | CCCI |
| NFC | `nxp@28` на I2C3 | `nq-nci` |

Wi-Fi и Bluetooth встроены в сам SoC, отдельной микросхемы нет. Обе
подсистемы закрыты и требуют загрузки прошивки — для нашей ОС это
практически недостижимо, о чём и предупреждает README.

### Питание и заряд

| Что | Где |
|---|---|
| Зарядное | `smb1351-charger@55` на I2C7 |
| USB Type-C | `usb_type_c@60` и `fusb303@21` на I2C7 |
| Дополнительный PMIC | `subpmic_pmu@34` на I2C5 |
| Классы в sysfs | `ac`, `battery`, `bms`, `charger`, `main`, `parallel`, `usb` |

### Накопитель и USB

| Что | Где |
|---|---|
| eMMC (msdc0) | `0x11230000`, top `0x11CD0000` |
| SD (msdc1) | `0x11240000`, top `0x11C90000` |
| USB | `0x11200000`, контроллер `musb-hdrc` |

### Датчики

Все идут через сопроцессор SCP, а не напрямую по шине:
`accel_hub_pl`, `gyro_hub`, `mag_hub`, `alsps_hub_pl` (свет и приближение),
управляет ими `sensor_hub_pl`. Плюс два термодатчика платы.

### Камеры

I2C2 и I2C4: `camera_main@20`, `camera_main_two@5a`, `camera_sub@6c`,
`camera_main_three@5a`, их EEPROM по адресам `0x50`/`0x51` и приводы
автофокуса `0x0c`/`0x18`. Драйверы `kd_camera_hw`, `CAM_CAL_*`, `MAINAF`.

## Тачскрин — Novatek на SPI, а не Goodix на I2C

Это стоило отдельного расследования, потому что дерево устройства описывает
**оба** варианта: Xiaomi ставила в эту модель разные панели.

| Что | Значение | Откуда |
|---|---|---|
| Контроллер | **Novatek NT36xxx** | `driver -> NVT-ts` в sysfs |
| Шина | **SPI0**, `0x1100A000` | `spi0.0` |
| Выбор кристалла | 0 | `reg = <0>` |
| Частота | 8 МГц | `spi-max-frequency = <0x7a1200>` |
| GPIO сброса | 92 | `novatek,reset-gpio` |
| GPIO прерывания | 1 | `novatek,irq-gpio` |
| Прерывание SPI0 | SPI 138 → INTID 170 | `interrupts = <0 0x8a 8>` |

Узел `i2c0@11007000/cap_touch@5d` в дереве есть и даже виден в
`/sys/bus/i2c/devices/0-005d`, **но драйвера к нему не привязано** — это
описание для другой панели, а не для нашей. Слепое сканирование шины I2C
на нём и спотыкалось.

Как это выяснилось (пригодится для любой другой периферии): значения свойств
в `/proc/device-tree` закрыты SELinux, но **привязку драйверов видно**:

```
ls -l /sys/bus/spi/devices/spi0.0     -> driver = NVT-ts
ls -l /sys/bus/i2c/devices/0-005d     -> драйвера нет
```

А сами значения свойств берутся из раздела `dtbo` официального ROM:
распаковать `images/dtbo.img`, разобрать контейнер Android DT table и
расшифровать через `dtc`. Ни root, ни обхода защиты для этого не нужно.

## Где лежит адрес фреймбуфера

LK передаёт его прямо в DTB, в узле `/chosen`:
`atag,videolfb-fb_base_h` и `atag,videolfb-fb_base_l` (плюс `vramSize`,
`lcmname`, `islcm_inited`). Прочитать их из-под Android нельзя — SELinux
не пускает пользователя `shell`. Но наше ядро работает в EL1 без SELinux
и прочитает их свободно.

## Откуда брать техническую информацию

MediaTek не публикует datasheet на MT6769. **Единственный достоверный источник адресов
регистров и параметров периферии — официальные исходники ядра от Xiaomi:**

- Ядро merlin: https://github.com/MiCode/Xiaomi_Kernel_OpenSource — ветка `merlin-*-opensource`
- Что искать: `arch/arm64/boot/dts/mediatek/mt6768.dtsi`, `mt6769.dtsi`, `merlin.dts`
- Драйверы: `drivers/misc/mediatek/` (дисплей, тач, i2c, pmic)

Родственные SoC с той же периферией: MT6765 / MT6768 / MT6769 — одно семейство,
адреса регистров почти полностью совпадают. Полезен код mainline-порта:
https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/mediatek (mt6765.dtsi)

## Статус проверки адресов

Всё, что помечено `TODO-VERIFY` в `kernel/src/soc/mt6769.h`, взято по аналогии
с MT6765/MT6768 и **должно быть сверено с DTS от merlin** перед прошивкой.
