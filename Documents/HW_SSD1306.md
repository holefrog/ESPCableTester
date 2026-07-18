
# I2C SSD1306 OLED

## 接线 (Connection)
| PIN  | RPi | 描述 (Description)        |
|------|-----|-------------------------|
| 5V   | 1   | 3.3V 电源                |
| GND  | 9   | 接地 (Ground)           |
| SDA  | 7   | I2C 数据线 (Data input) |
| SCL  | 29  | I2C 时钟线 (Clock input)|

> 注意：请确保电源和地线连接正确，否则屏幕可能无法正常工作。

---

## I2C 总线使用情况 (I2C Bus)
- I2C-0：被系统占用  
- I2C-1：被 WM8960 占用  

---

## 启用 GPIO 模拟 I2C (Dtoverlay)
编辑配置文件：

```bash
sudo nano /boot/firmware/config.txt
```

在文件末尾添加：

```txt
# Enable GPIO simulated I2C for SSD1306 OLED
dtoverlay=i2c-gpio,bus=3,i2c_gpio_sda=4,i2c_gpio_scl=5
```

保存并退出后，重启 Raspberry Pi：

```bash
sudo reboot
```

---

## 检查 I2C 总线 (Check)
列出 I2C 设备：

```bash
ls /dev/i2c-*
```

示例输出：

```
/dev/i2c-1  /dev/i2c-3
```

扫描 I2C 设备地址：

```bash
i2cdetect -y 3
```

示例输出：

```
sudo: unable to resolve host rpibedroom: System error
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:          -- -- -- -- -- -- -- -- -- -- -- -- -- 
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
30: -- -- -- -- -- -- -- -- -- -- -- -- 3c -- -- -- 
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
```

> 注：地址 `0x3c` 即 SSD1306 OLED 显示屏。
