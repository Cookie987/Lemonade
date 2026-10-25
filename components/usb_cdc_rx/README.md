# usb_cdc_rx —— 异步接收 usb_uart 的 CDC 数据

`usb_uart` 只提供 `UARTComponent` 通道，**没有"数据到了"的通知**,而且它有个坑:
IN 传输失败一次后就不再重试(`usb_uart.cpp:303-308` 只把 `input_started_` 置 false),
要靠 `read_array()` 才会重新启动(`usb_uart.cpp:211`)。没有消费者时没人调它 ——
一次瞬时错误(最典型的就是 light sleep 那一下)就让 RX 永久静音,只能拔插。

这个组件就是那个消费者:**每轮主循环非阻塞地把通道读干净**(没有 delay、没有阻塞等待),
拿到数据立即触发自动化。

## 它做两件事

1. **保活 RX**:空闲时每 250 ms 做一次零长度 `read_array()`,把失败的 IN 传输重新武装起来
   (零长度读不消耗数据,只是走 `read_array()` → `start_input()` 那条重启路径)。
   所以现在 light sleep 抖动、设备刚上电没准备好之类的瞬时错误,会自己恢复。
2. **异步派发数据**:
   * `on_data` —— 每收到一批字节触发一次,`x` 是 `std::string`(二进制安全,`x[i]` 取字节);
   * `on_line` —— 配了 `delimiter` 时按行触发,`x` 是一行文本(行尾的 `\r` 已去掉)。

## 用法

```yaml
usb_cdc_rx:
  id: usb_rx
  uart_id: usb_cdc_ch0      # usb_uart 的某个 channel
  delimiter: "\n"           # 可选，按行分帧；不想要行模式就不写
  max_line_length: 256      # 可选，等不到分隔符时最多攒这么多字节就当一行交出去
  on_data:
    - lambda: |-
        ESP_LOGI("usb_rx", "got %d bytes", (int) x.size());
  on_line:
    - text_sensor.template.publish:
        id: some_text_sensor
        state: !lambda "return x;"
```

## 注意

* ⚠ **不要把原始字节送进 text_sensor / 文本类实体。** 实测踩过：K5 协议是二进制的，
  `text_sensor.template.publish` 发出去后 API 客户端会报
  `String field had bad UTF-8` 并**断开连接**（proto 字符串必须是合法 UTF-8）。
  要结构化数据就用 `on_data`/`on_line` 的 lambda 自己格式化，或交给 Lua。
  原始数据的十六进制看 `usb_uart` 的 `debug: true` 就够了。
* 同一个 channel **只能有一个消费者**:本组件读走的数据不会留给别人(USB CDC 的数据也是
  一次性从环形缓冲区取走的)。要转发给别人就在 `on_data`/`on_line` 里转发。
* 该 channel 不能开 `usb_uart` 的 `dummy_receiver: true` —— 那个选项会在 USB 任务里
  直接把数据丢掉(不过队列、也不进环形缓冲区),本组件就永远收不到东西。
* 原始字节的十六进制在 `usb_uart` 的 `debug: true` 里已经有了(`<<< XX,XX,...`);
  想在 lambda 里自己打十六进制,用非分配版本:

  ```yaml
  on_data:
    - lambda: |-
        char buf[format_hex_pretty_size(64) + 1];
        size_t n = x.size() < 64 ? x.size() : 64;
        format_hex_pretty_to(buf, sizeof(buf), reinterpret_cast<const uint8_t *>(x.data()), n, ' ');
        ESP_LOGI("usb_rx", "%d bytes: %s", (int) x.size(), buf);
  ```

* 本组件不阻塞、不占用额外任务:所有处理都在 ESPHome 主循环里(`loop()` + `setup()`),
  收到数据到触发自动化最多差一轮主循环。
