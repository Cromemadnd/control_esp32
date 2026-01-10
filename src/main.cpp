#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_GFX.h>
#include <Wire.h>

// WiFi AP credentials
constexpr const char *kApSsid = "户外移动电源";
constexpr const char *kApPassword = "portablepower123";

// FastLED Strip Configuration
#define LED_PIN 12        // LED 数据信号线接 GPIO 12
#define LED_COUNT 55      // LED 灯珠数量，根据实际灯条修改
#define LED_BRIGHTNESS 50 // 初始亮度 (0-255)
#define AC_RELAY_PIN 14   // 交流输出继电器控制引脚

CRGB leds[LED_COUNT];

// OLED Display Configuration (0.96" SH1106/SSD1306-compatible on I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1       // No reset pin
#define SCREEN_ADDRESS 0x3C // I2C address for the OLED

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Data variables from STM32
struct SensorData
{
  float voltage = 0.0f;     // 稳压反馈电压
  float ac_voltage = 0.0f;  // 市电电压
  float temperature = 0.0f; // 环境温度
  float battery = 0.0f;     // 储备电池电量
  float current = 0.0f;     // 母线电流
} sensorData;

// LED Mode Enumeration
enum LedMode
{
  MODE_SOLID = 0,      // 纯色常亮
  MODE_BREATHING = 1,  // 呼吸灯
  MODE_SOS = 2,        // SOS闪光
  MODE_FAST_FLASH = 3, // 快速闪光
  MODE_FLOW = 4        // 流水灯
};

// LED Strip State Structure
struct LedStripState
{
  LedMode mode = MODE_SOLID;
  uint8_t brightness = 50; // 0-100%
  uint32_t colors[8];      // 8个颜色值，未使用的设为0xFFFFFFFF
  uint8_t colorCount = 1;  // 实际使用的颜色数量
};

LedStripState ledState;

// System Control State
struct SystemState
{
  bool screenEnabled = true;
  bool acOutputEnabled = false;
  uint8_t ledBrightness = 50;
  LedMode ledMode = MODE_SOLID;
  uint32_t ledColors[8] = {0xFF0000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}; // Red as default
  uint8_t ledColorCount = 1;
};

SystemState systemState;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// 持久化存储相关函数
const char *STATE_FILE = "/system_state.json";

// 从SPIFFS读取系统状态
void loadSystemState()
{
  if (SPIFFS.exists(STATE_FILE))
  {
    Serial.println("Loading system state from SPIFFS...");
    File file = SPIFFS.open(STATE_FILE, "r");
    if (file)
    {
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, file);
      file.close();

      if (!err)
      {
        systemState.screenEnabled = doc["screenEnabled"] | true;
        systemState.acOutputEnabled = doc["acOutputEnabled"] | false;
        systemState.ledBrightness = doc["ledBrightness"] | 50;
        systemState.ledMode = (LedMode)(doc["ledMode"] | 0);
        systemState.ledColorCount = doc["ledColorCount"] | 1;

        // 读取颜色数组
        JsonArray colorArray = doc["ledColors"];
        if (colorArray)
        {
          for (uint8_t i = 0; i < 8; i++)
          {
            systemState.ledColors[i] = colorArray[i] | 0xFF0000;
          }
        }

        Serial.print("System state loaded: brightness=");
        Serial.print(systemState.ledBrightness);
        Serial.print(", mode=");
        Serial.print(systemState.ledMode);
        Serial.print(", colors=");
        Serial.println(systemState.ledColorCount);
      }
      else
      {
        Serial.print("JSON parse error: ");
        Serial.println(err.c_str());
      }
    }
    else
    {
      Serial.println("Failed to open state file");
    }
  }
  else
  {
    Serial.println("No state file found, using default settings");
  }
}

// 保存系统状态到SPIFFS
void saveSystemState()
{
  StaticJsonDocument<512> doc;
  doc["screenEnabled"] = systemState.screenEnabled;
  doc["acOutputEnabled"] = systemState.acOutputEnabled;
  doc["ledBrightness"] = systemState.ledBrightness;
  doc["ledMode"] = systemState.ledMode;
  doc["ledColorCount"] = systemState.ledColorCount;

  // 保存颜色数组
  JsonArray colorArray = doc.createNestedArray("ledColors");
  for (uint8_t i = 0; i < 8; i++)
  {
    colorArray.add(systemState.ledColors[i]);
  }

  File file = SPIFFS.open(STATE_FILE, "w");
  if (file)
  {
    size_t bytesWritten = serializeJson(doc, file);
    file.close();
    Serial.print("System state saved: ");
    Serial.print(bytesWritten);
    Serial.println(" bytes");
  }
  else
  {
    Serial.println("Failed to open state file for writing");
  }
}

void sendJson(AsyncWebSocketClient *client, const JsonDocument &doc)
{
  String payload;
  serializeJson(doc, payload);
  client->text(payload);
}

void broadcastJson(const JsonDocument &doc)
{
  String payload;
  serializeJson(doc, payload);
  ws.textAll(payload);
}

// 创建系统状态同步消息
void sendSystemStateSyncJson(AsyncWebSocketClient *client)
{
  StaticJsonDocument<512> doc;
  doc["event"] = "state_sync";
  doc["screen"] = systemState.screenEnabled;
  doc["ac_output"] = systemState.acOutputEnabled;
  doc["brightness"] = systemState.ledBrightness;
  doc["led_mode"] = (int)systemState.ledMode;
  doc["color_count"] = systemState.ledColorCount;

  JsonArray colors = doc.createNestedArray("colors");
  for (uint8_t i = 0; i < 8; i++)
  {
    colors.add(systemState.ledColors[i]);
  }

  sendJson(client, doc);
}

// 根据电池电量和颜色数组计算当前应显示的颜色
uint32_t interpolateColor(float batteryPercent)
{
  if (systemState.ledColorCount == 0)
    return 0xFF0000; // 默认红色

  if (systemState.ledColorCount == 1)
    return systemState.ledColors[0];

  // 将电池百分比分段到颜色数组
  // 例如：3个颜色对应 100%, 50%, 0%
  float normalizedBattery = batteryPercent / 100.0f;
  float segmentSize = 1.0f / (systemState.ledColorCount - 1);
  int lowIdx = (int)(normalizedBattery / segmentSize);
  int highIdx = lowIdx + 1;

  if (lowIdx >= systemState.ledColorCount - 1)
    return systemState.ledColors[systemState.ledColorCount - 1];

  if (lowIdx < 0)
    lowIdx = 0;
  if (highIdx >= systemState.ledColorCount)
    highIdx = systemState.ledColorCount - 1;

  // 计算插值系数
  float localPercent = (normalizedBattery - lowIdx * segmentSize) / segmentSize;

  // 解析RGB颜色
  uint32_t lowColor = systemState.ledColors[lowIdx];
  uint32_t highColor = systemState.ledColors[highIdx];

  uint8_t lowR = (lowColor >> 16) & 0xFF;
  uint8_t lowG = (lowColor >> 8) & 0xFF;
  uint8_t lowB = lowColor & 0xFF;

  uint8_t highR = (highColor >> 16) & 0xFF;
  uint8_t highG = (highColor >> 8) & 0xFF;
  uint8_t highB = highColor & 0xFF;

  // 线性插值
  uint8_t r = lowR + (highR - lowR) * localPercent;
  uint8_t g = lowG + (highG - lowG) * localPercent;
  uint8_t b = lowB + (highB - lowB) * localPercent;

  return (r << 16) | (g << 8) | b;
}

// 更新LED显示（根据当前模式）
void updateLedDisplay()
{
  uint8_t brightness = map(systemState.ledBrightness, 0, 100, 0, 255);
  FastLED.setBrightness(brightness);

  uint32_t currentColor = interpolateColor(sensorData.battery * 100.0f);

  switch (systemState.ledMode)
  {
  case MODE_SOLID:
  {
    // 纯色常亮
    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;
    for (int i = 0; i < LED_COUNT; i++)
    {
      leds[i] = CRGB(r, g, b);
    }
    FastLED.show();
    break;
  }
  case MODE_BREATHING:
  {
    // 呼吸灯效果
    static unsigned long lastBreath = 0;
    static uint8_t breathBrightness = 0;
    static int8_t breathDirection = 1;

    if (millis() - lastBreath > 30) // 每30ms更新一次
    {
      lastBreath = millis();

      uint8_t r = (currentColor >> 16) & 0xFF;
      uint8_t g = (currentColor >> 8) & 0xFF;
      uint8_t b = currentColor & 0xFF;

      for (int i = 0; i < LED_COUNT; i++)
      {
        leds[i] = CRGB(r, g, b);
        leds[i] %= (breathBrightness); // 调节亮度
      }

      breathBrightness += breathDirection * 5;
      if (breathBrightness >= 255)
      {
        breathBrightness = 255;
        breathDirection = -1;
      }
      else if (breathBrightness <= 50)
      {
        breathBrightness = 50;
        breathDirection = 1;
      }

      FastLED.show();
    }
    break;
  }
  case MODE_SOS:
  {
    // SOS闪光 (三短0.1s 三长0.3s 三短0.1s)
    static unsigned long sosCycle = 0;
    static unsigned long sosStart = 0;
    bool sosLedOn = false;

    if (millis() - sosStart > 2400) // 2.4秒一个周期
    {
      sosStart = millis();
      sosCycle = 0;
    }

    unsigned long elapsed = millis() - sosStart;

    // S (三个短闪 0.1s 间隔)
    if ((elapsed < 100) || (elapsed >= 200 && elapsed < 300) || (elapsed >= 400 && elapsed < 500))
      sosLedOn = true;
    // O (三个长闪 0.3s 间隔)
    else if ((elapsed >= 600 && elapsed < 900) || (elapsed >= 1100 && elapsed < 1400) || (elapsed >= 1600 && elapsed < 1900))
      sosLedOn = true;
    // S (三个短闪 0.1s 间隔)
    else if ((elapsed >= 2000 && elapsed < 2100) || (elapsed >= 2200 && elapsed < 2300))
      sosLedOn = true;

    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;

    for (int i = 0; i < LED_COUNT; i++)
    {
      leds[i] = sosLedOn ? CRGB(r, g, b) : CRGB::Black;
    }
    FastLED.show();
    break;
  }
  case MODE_FAST_FLASH:
  {
    // 快速闪光 (周期0.2s)
    static unsigned long lastFlash = 0;
    static bool flashOn = true;

    if (millis() - lastFlash > 100) // 每100ms切换
    {
      lastFlash = millis();
      flashOn = !flashOn;
    }

    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;

    for (int i = 0; i < LED_COUNT; i++)
    {
      leds[i] = flashOn ? CRGB(r, g, b) : CRGB::Black;
    }
    FastLED.show();
    break;
  }
  case MODE_FLOW:
  {
    // 流水灯效果
    static unsigned long lastFlow = 0;
    static uint8_t flowPosition = 0;

    if (millis() - lastFlow > 50) // 每50ms移动一步
    {
      lastFlow = millis();

      FastLED.clear();

      uint8_t r = (currentColor >> 16) & 0xFF;
      uint8_t g = (currentColor >> 8) & 0xFF;
      uint8_t b = currentColor & 0xFF;

      // 显示5个LED的流水效果
      for (int i = 0; i < 5; i++)
      {
        int pos = (flowPosition + i) % LED_COUNT;
        uint8_t brightness = 255 - (i * 50);
        leds[pos] = CRGB(r, g, b);
        leds[pos] %= brightness;
      }

      FastLED.show();
      flowPosition = (flowPosition + 1) % LED_COUNT;
    }
    break;
  }
  }
}

// OLED Display Update Function
void updateOLEDDisplay()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(false); // avoid wrap-induced shifts

  // Title (short to avoid wrap)
  display.setCursor(0, 0);
  display.println("Power Monitor");

  // Line spacing 10px
  display.setCursor(0, 10);
  display.print("Batt: ");
  display.print(sensorData.battery, 1);
  display.println(" %");

  display.setCursor(0, 20);
  display.print("AC: ");
  display.print(sensorData.ac_voltage, 1);
  display.println(" V");

  display.setCursor(0, 30);
  display.print("DC: ");
  display.print(sensorData.voltage, 2);
  display.println(" V");

  display.setCursor(0, 40);
  display.print("Temp: ");
  display.print(sensorData.temperature, 1);
  display.println(" C");

  display.setCursor(0, 50);
  display.print("I-Bus: ");
  display.print(sensorData.current, 2);
  display.println(" A");

  display.display();
}

void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data,
                          size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.print("WebSocket client connected, ID: ");
    Serial.println(client->id());

    // 向新连接的客户端发送当前系统状态
    sendSystemStateSyncJson(client);
    return;
  }

  if (type == WS_EVT_DISCONNECT)
  {
    Serial.print("WebSocket client disconnected, ID: ");
    Serial.println(client->id());
    return;
  }

  if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
    if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT))
    {
      return;
    }

    String msg;
    msg.reserve(len);
    for (size_t i = 0; i < len; ++i)
    {
      msg += static_cast<char>(data[i]);
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err)
    {
      StaticJsonDocument<128> errorDoc;
      errorDoc["error"] = "invalid_json";
      errorDoc["detail"] = err.c_str();
      sendJson(client, errorDoc);
      return;
    }

    const char *cmd = doc["cmd"] | "echo";
    bool broadcast = doc["broadcast"] | false;

    Serial.print("Received command: ");
    Serial.println(cmd);

    StaticJsonDocument<512> reply;
    reply["event"] = "response";
    reply["cmd"] = cmd;
    reply["from"] = static_cast<uint32_t>(client->id());
    reply["ok"] = true;

    if (strcmp(cmd, "ping") == 0)
    {
      reply["pong"] = millis();
    }
    else if (strcmp(cmd, "control") == 0)
    {
      // 处理控制命令
      const char *target = doc["target"];
      if (target)
      {
        reply["target"] = target;

        if (strcmp(target, "screen") == 0)
        {
          bool value = doc["value"] | false;
          systemState.screenEnabled = value;
          reply["value"] = value;
          Serial.print("Screen control: ");
          Serial.println(value ? "ON" : "OFF");

          if (value)
          {
            display.oled_command(SH110X_DISPLAYON);
          }
          else
          {
            display.oled_command(SH110X_DISPLAYOFF);
          }

          saveSystemState(); // 保存状态
        }
        else if (strcmp(target, "ac_output") == 0)
        {
          bool value = doc["value"] | false;
          systemState.acOutputEnabled = value;
          reply["value"] = value;
          Serial.print("AC output control: ");
          Serial.println(value ? "ON" : "OFF");
          digitalWrite(AC_RELAY_PIN, value ? HIGH : LOW);

          saveSystemState(); // 保存状态
        }
        else if (strcmp(target, "brightness") == 0)
        {
          int value = doc["value"] | 50;
          value = constrain(value, 0, 100);
          systemState.ledBrightness = value;
          reply["value"] = value;
          Serial.print("Brightness: ");
          Serial.println(value);
          updateLedDisplay(); // 立即更新LED显示

          saveSystemState(); // 保存状态
        }
        else if (strcmp(target, "led_mode") == 0)
        {
          int mode = doc["value"] | 0;
          if (mode >= 0 && mode <= 4)
          {
            systemState.ledMode = (LedMode)mode;
            reply["value"] = mode;
            Serial.print("LED Mode: ");
            Serial.println(mode);

            saveSystemState(); // 保存状态
          }
          else
          {
            reply["ok"] = false;
            reply["error"] = "invalid_mode";
          }
        }
        else if (strcmp(target, "colors") == 0)
        {
          // 设置灯条颜色数组
          JsonArray colorArray = doc["colors"];
          if (colorArray)
          {
            systemState.ledColorCount = min((uint8_t)colorArray.size(), (uint8_t)8);
            for (uint8_t i = 0; i < systemState.ledColorCount; i++)
            {
              systemState.ledColors[i] = colorArray[i] | 0xFF0000;
            }
            // 填充未使用的颜色为0xFFFFFFFF
            for (uint8_t i = systemState.ledColorCount; i < 8; i++)
            {
              systemState.ledColors[i] = 0xFFFFFFFF;
            }

            JsonArray replyColors = reply.createNestedArray("colors");
            for (uint8_t i = 0; i < 8; i++)
            {
              replyColors.add(systemState.ledColors[i]);
            }
            reply["color_count"] = systemState.ledColorCount;
            Serial.print("Colors updated, count: ");
            Serial.println(systemState.ledColorCount);
            updateLedDisplay(); // 立即更新LED显示

            saveSystemState(); // 保存状态
          }
          else
          {
            reply["ok"] = false;
            reply["error"] = "missing_colors";
          }
        }
        else
        {
          reply["ok"] = false;
          reply["error"] = "unknown_target";
        }
      }
      else
      {
        reply["ok"] = false;
        reply["error"] = "missing_target";
      }
    }
    else
    {
      reply["ok"] = false;
      reply["error"] = "unknown_command";
    }

    if (broadcast)
    {
      broadcastJson(reply);
    }
    else
    {
      sendJson(client, reply);
    }
  }
}

void setup()
{
  Serial.begin(9600);
  delay(2000);

  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed");
  }

  // 加载保存的系统状态
  loadSystemState();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // Setup AC Relay Pin
  pinMode(AC_RELAY_PIN, OUTPUT);
  digitalWrite(AC_RELAY_PIN, LOW); // 默认关闭交流输出

  ws.onEvent(handleWebSocketEvent);
  server.addHandler(&ws);

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request)
                    { request->send(404, "text/plain", "Not found"); });

  server.begin();

  // Initialize Serial2 for STM32 Communication
  // RX: 16, TX: 17, 115200 bps
  Serial2.begin(115200, SERIAL_8N1, 16, 17);

  // Initialize I2C and OLED Display
  Serial.println("Initializing OLED display...");
  Wire.begin();          // Explicitly init I2C (SDA=21, SCL=22 by default)
  Wire.setClock(400000); // Faster I2C for smoother refresh
  if (!display.begin(SCREEN_ADDRESS, true))
  {
    Serial.println("SSD1306 allocation failed");
    // Continue anyway, OLED failure won't stop other operations
  }
  else
  {
    display.setRotation(0);
    display.setTextWrap(false);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println("Power Monitor");
    display.println("Init...");
    display.display();
    Serial.println("OLED initialized successfully!");
  }

  // Initialize FastLED Strip
  Serial.println("Initializing LED strip...");
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(LED_BRIGHTNESS);

  // Set all LEDs to initial state
  Serial.println("Setting LEDs to initial state...");
  updateLedDisplay();
  Serial.println("LED strip initialized!");
}

void loop()
{
  // LED Effects and OLED Updates
  static unsigned long lastOLEDUpdate = 0;

  // Update OLED display every 500ms to reduce I2C traffic
  if (millis() - lastOLEDUpdate > 500)
  {
    lastOLEDUpdate = millis();
    updateOLEDDisplay();
  }

  // Update LED display based on current mode
  updateLedDisplay();

  // Clean up disconnected WebSocket clients periodically
  ws.cleanupClients();

  // UART Parsing for sensor data from STM32
  static uint8_t rx_buffer[32];
  static int rx_idx = 0;

  while (Serial2.available())
  {
    uint8_t c = Serial2.read();

    if (rx_idx == 0)
    {
      if (c == 0xAA)
      {
        rx_buffer[rx_idx++] = c;
      }
    }
    else if (rx_idx == 1)
    {
      if (c == 0x55)
        rx_buffer[rx_idx++] = c;
      else
        rx_idx = 0; // Reset if header invalid
    }
    else
    {
      rx_buffer[rx_idx++] = c;

      // Full Packet Received: 2 Header + 20 Data + 1 Checksum = 23 bytes
      if (rx_idx >= 23)
      {
        // Validation
        uint8_t checksum = 0;
        // Data is from index 2 to 21 (20 bytes)
        for (int i = 2; i < 22; i++)
        {
          checksum += rx_buffer[i];
        }

        if (checksum == rx_buffer[22])
        {
          // Parse
          float *values = (float *)&rx_buffer[2];

          // Update sensor data struct
          sensorData.voltage = values[0];
          sensorData.ac_voltage = values[1];
          sensorData.temperature = values[2];
          sensorData.battery = values[3];
          sensorData.current = values[4];

          StaticJsonDocument<256> doc;
          doc["voltage"] = sensorData.voltage;
          doc["ac_voltage"] = sensorData.ac_voltage;
          doc["temperature"] = sensorData.temperature;
          doc["battery"] = sensorData.battery;
          doc["current"] = sensorData.current;

          // Broadcast to Web UI
          broadcastJson(doc);
        }

        rx_idx = 0; // Reset for next packet
      }
    }
  }
}