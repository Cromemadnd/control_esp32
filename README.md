# 功能需求

- RGB **寻址**灯条, 使用 Adafruit NeoPixel 库, 需要一个pwm口
- 与stm32串口通信, 数据[稳压反馈, 市电电压, 温度, 电池电压, 母线电流], 使用USART1, PA9,PA10
- 中继器控制 , 一个GPIO引脚即可, 继电器接线接NO和COM口


# 引脚连接

串口通信 stm32->esp32
B10 -> D16
B11 -> D17