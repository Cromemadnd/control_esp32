<!-- # 功能需求

- RGB **寻址**灯条, 使用 Adafruit NeoPixel 库, 需要一个pwm口
- 与stm32串口通信, 数据[稳压反馈, 市电电压, 温度, 电池电压, 母线电流], 使用USART1, PA9,PA10
- 中继器控制 , 一个GPIO引脚即可, 继电器接线接NO和COM口 -->


# 引脚连接

串口通信 

stm32->esp32
B10 -> D16
B11 -> D17

继电器（5V供电）控制
D14 -> VIN

OLED（3.3V供电）控制
D21 -> SDA
D22 -> SCL

55灯灯带（5V供电）控制
D12 -> DIN


# 语音模块部分

- 开灯: 0xFF, 0x1F, 0x34, 0xAA
- 关灯: 0xFF, 0x1F, 0x34, 0xBB

RX引脚: D4

# TODO
- 语音模块链接
- 通过灯带显示电量的逻辑