# usb_mode —— ESP32-S3 单口 USB 角色切换（运行时）

Lemonade OS 只有一个 Type-C 口，它既能当 **设备**（USB 串口，日志输出到电脑）又能当
**主机**（读取插在同一个口上的 USB CDC 串口设备）。这个组件用一个 `switch` 实体在两种
角色之间**运行时切换，不需要重新烧写**。

## 为什么需要它

ESP32-S3 只有 **一个内部 USB FSLS PHY**，它被两个外设共用：

| 外设 | 用途 | ESPHome 里的使用者 |
| --- | --- | --- |
| USB-OTG（USB_WRAP） | 主机模式读 USB 设备 | `usb_host` / `usb_uart` |
| USB-Serial-JTAG（USJ） | 设备模式，输出日志 | `logger`（`hardware_uart: USB_SERIAL_JTAG`） |

两者**同一时刻只能有一个**拥有这个 PHY（Espressif 官方 FAQ 也是这么说的），所以：

* 直接用 `tinyusb`（设备） + `usb_host`（主机）是**配置期就被拒绝的**
  （`tinyusb/__init__.py` 里 `CONFLICTS_WITH = ["usb_host"]`，而 `usb_uart` 会
  `AUTO_LOAD` `usb_host`）。
* 也不能指望两个外设同时工作，硬件上做不到。

本组件不去装/卸任何一个协议栈，而是**只切换 PHY 的归属**：

* 主机模式：把内部 PHY 交给 USB-OTG，并配上主机必须有的 15k 下拉
  （等价于 IDF 的 `usb_wrap_hal_phy_set_external(false)` +
  `usb_phy_otg_set_mode(USB_OTG_MODE_HOST)`）。
* 设备模式：把 PHY 还给 USB-Serial-JTAG，日志立刻恢复输出到电脑
  （等价于 `usb_serial_jtag_driver_install()` 里的 PHY 配置）。

写的寄存器就是 IDF 自己驱动初始化时写的那两位：
`RTCCNTL.usb_conf.sw_hw_usb_phy_sel` / `sw_usb_phy_sel`。

## 用法

`packages/s3_touch/usb_mode.yaml`（已由 `touch3.2_canary.yaml` include）：

```yaml
logger:
  hardware_uart: USB_SERIAL_JTAG   # usb_mode 依赖这个（也是 S3 的默认值）

usb_host:
  max_transfer_requests: 16
  max_packet_size: 64

usb_uart:
  - type: CDC_ACM
    vid: 0x0000        # 0x0000/0x0000 = 匹配任意设备
    pid: 0x0000
    channels:
      - id: usb_cdc_ch0
        debug: true

switch:
  - platform: usb_mode
    id: usb_mode_switch
    name: "USB 主机模式"
```

开关语义（状态会存到 flash，重启后保持）：

* **OFF（默认）** = 设备模式：Type-C 是 USB-Serial-JTAG，电脑上是一个 COM 口，
  ESPHome 日志从这里出来。
* **ON** = 主机模式：Type-C 给 USB 主机用，`usb_uart` 的 `usb_cdc_ch0` 就是读到的
  CDC 串口通道（可以当 `uart_id` 给别的组件用）。

可选参数：

* `reboot_on_switch: true` —— 切换时保存状态并重启，而不是运行时切 PHY。
  如果现场发现运行时切换在某些板子上不稳，用它兜底（重启后由 `setup()` 按存下来的状态
  选择 PHY）。
* `allow_light_sleep: true` —— 允许主机模式下 light sleep。默认 `false`：主机模式会持有
  `ESP_PM_APB_FREQ_MAX` + `ESP_PM_NO_LIGHT_SLEEP`，因为 **USB 扛不住 light sleep**（见下）。

## 使用注意

1. **light sleep 会杀死 USB（这条最关键，实测踩过）**。本工程开了电源管理
   （`enable_light_sleep: true` / `idle_time_before_sleep: 3` / `power_down_peripherals: true`），
   芯片一进 light sleep：
   * 主机模式：挂起的 IN 传输立刻以 `USB_TRANSFER_STATUS_ERROR` 失败，设备还会掉线重枚举，
     甚至出现 `ENUM: CHECK_FULL_DEV_DESC FAILED`；
   * 设备模式：USB-Serial-JTAG 直接失联 —— 就是"关了 APB 锁 USB 就没反应"那个现象。

   所以 `usb_mode` 在**主机模式下自己持有** `ESP_PM_APB_FREQ_MAX` + `ESP_PM_NO_LIGHT_SLEEP`
   （`usb_mode.cpp` 的 `acquire_pm_locks_()`）：IDF 里锁会把 PM 模式下限抬到
   `PM_MODE_APB_MAX`（`pm_impl.c:230-243`），而模式选择在
   `s_mode_mask >= BIT(PM_MODE_APB_MIN)` 时不会选 `PM_MODE_LIGHT_SLEEP`（`pm_impl.c:561`）。
   切回设备模式时释放，省电逻辑照旧。IDF 自己的 `usb_host` 一把 PM 锁都不拿，这事只能我们做。
   *注意：界面上的 APB 开关（`switch_powermanage_lock_apb`）和这个锁是两回事 —— 主机模式下
   即使那个开关是 OFF，usb_mode 也持有自己的锁、设备不会睡。*

   设备模式那边 IDF 有官方的开关，同一份 package 里已经加上了：

   ```yaml
   esp32:
     framework:
       sdkconfig_options:
         CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION: y
   ```

   它对应的实现是 `usb_serial_jtag_connection_monitor.c`：检测到电脑插着就持有
   `ESP_PM_NO_LIGHT_SLEEP`，拔掉就释放 —— 也就是"USB 串口在用时不睡，不用时照睡"，
   正好治"关了 APB 锁 USB 就没反应"(默认 `n`，你们的 sdkconfig 里原来就是没开)。
2. **主机模式必须由板子给设备供 5V VBUS**。ESP32 当主机时不会自己发电，Type-C 上的
   USB 设备要有电才能枚举（自供电设备无所谓）。
3. **主机模式下这个口不可能再有日志**（PC 也是主机，插上去双方都不会有反应），
   要看日志就走 WiFi：ESPHome Dashboard / Home Assistant 的 API 日志，或者板子上的
   MQTT（本工程的 `logger.on_message` 已经把日志发到 `logs/<mac>`）。
   也可以引一根 UART0 调试线（GPIO43/44）当常驻日志通道。
4. 切换时电脑侧会看到 USB 设备**断开再重连**（几秒内重新枚举，COM 口号一般不变）。
5. `vid: 0x0000 / pid: 0x0000` 是通配。接上设备后日志里会出现
   `Device descriptor: vid XXXX pid YYYY`，确认后建议改成真实值，避免误配到别的设备。
6. **只支持 ESP32-S3**（P4/S31 的 USB PHY 结构不同，S2 没有 USB-Serial-JTAG），
   其它 variant 在配置校验阶段就会报错。

### 已知的 `usb_uart` 缺口（不是本组件的问题）

IN 传输失败一次后**不再重试**（`usb_uart.cpp:303-308` 只置 `input_started_ = false`，
等 `read_array()` 来重新启动）。没有消费者时 `read_array()` 永远不会被调用，于是
一次瞬时错误（例如上面那次 light sleep）就会让 RX 通道永久静音，只能拔插恢复。
解法二选一：给 `usb_uart` 打重试补丁，或者给它加一个消费者组件（消费者每轮读一次
就会自动重启传输）。

## 实现要点

`switch/usb_mode.cpp`：

* `get_setup_priority()` = `setup_priority::BUS - 1.0f`，即**在 `usb_host` 装好主机栈之后
  （BUS）、在 `usb_uart` 注册 client 之前（IO）**执行，这样无论启动时是哪种模式，
  client 注册都不受影响（不需要卸载/重装协议栈）。
* `setup()` 读取 flash 里存的状态并直接应用（不触发重启）。
* `write_state()` = 切 PHY + `publish_state()`，主机模式下不需要额外动作：
  主机栈一直是装着的，PHY 一回到它手里就会重新枚举设备。
