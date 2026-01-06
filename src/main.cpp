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
enum LedMode
{
  MODE_FLOW,    // 流水灯
  MODE_COLORFUL // 彩色闪光灯
};

LedMode currentMode = MODE_FLOW;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

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
    StaticJsonDocument<128> doc;
    doc["event"] = "connected";
    doc["clientId"] = static_cast<uint32_t>(client->id());
    sendJson(client, doc);
    return;
  }

  if (type == WS_EVT_DISCONNECT)
  {
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

    StaticJsonDocument<512> doc;
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
    const char *text = doc["msg"] | "";

    // 通过串口打印收到的JSON数据
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
          reply["value"] = value;
          Serial.print("Screen control: ");
          Serial.println(value ? "ON" : "OFF");

          // 控制 OLED 显示屏
          if (value)
          {
            display.oled_command(SH110X_DISPLAYON);
          }
          else
          {
            display.oled_command(SH110X_DISPLAYOFF);
          }
        }
        else if (strcmp(target, "ac_output") == 0)
        {
          bool value = doc["value"] | false;
          reply["value"] = value;
          Serial.print("AC output control: ");
          Serial.println(value ? "ON" : "OFF");
          // 写入GPIO控制继电器
          digitalWrite(AC_RELAY_PIN, value ? HIGH : LOW);
        }
        else if (strcmp(target, "brightness") == 0)
        {
          int value = doc["value"] | 50;
          reply["value"] = value;
          Serial.print("Brightness: ");
          Serial.println(value);

          // 设置 FastLED 亮度 (0-100 转换为 0-255)
          uint8_t brightness = map(value, 0, 100, 0, 255);
          FastLED.setBrightness(brightness);
          FastLED.show();
        }
        else if (strcmp(target, "led_mode") == 0)
        {
          int mode = doc["value"] | 0;
          if (mode == 0)
          {
            currentMode = MODE_FLOW;
            reply["value"] = "flow";
            Serial.println("LED Mode: FLOW");
          }
          else if (mode == 1)
          {
            currentMode = MODE_COLORFUL;
            reply["value"] = "colorful";
            Serial.println("LED Mode: COLORFUL");
          }
          else
          {
            reply["ok"] = false;
            reply["error"] = "invalid_mode";
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
      // 其他命令保留 msg 字段
      reply["msg"] = text;
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
  FastLED.setBrightness(255); // 先用最大亮度测试

  // Set all LEDs to green
  Serial.println("Setting LEDs to green...");
  for (int i = 0; i < LED_COUNT; i++)
  {
    leds[i] = CRGB::Green;
  }
  FastLED.show(); // Display the colors
  Serial.println("LED strip initialized!");
}

void loop()
{
  // LED Effects
  static unsigned long lastUpdate = 0;
  static unsigned long lastOLEDUpdate = 0; // For OLED refresh
  static uint8_t flowPosition = 0;
  static uint8_t colorIndex = 0;

  // Update OLED display every 500ms to reduce I2C traffic
  if (millis() - lastOLEDUpdate > 500)
  {
    lastOLEDUpdate = millis();
    updateOLEDDisplay();
  }

  if (currentMode == MODE_FLOW)
  {
    // 流水灯效果
    if (millis() - lastUpdate > 50) // Update every 50ms
    {
      lastUpdate = millis();

      // Clear all LEDs
      FastLED.clear();

      // Light up 5 consecutive LEDs with a gradient
      for (int i = 0; i < 5; i++)
      {
        int pos = (flowPosition + i) % LED_COUNT;
        uint8_t brightness = 255 - (i * 50);    // Gradient tail
        leds[pos] = CHSV(160, 255, brightness); // Cyan color
      }

      FastLED.show();
      flowPosition = (flowPosition + 1) % LED_COUNT;
    }
  }
  else if (currentMode == MODE_COLORFUL)
  {
    // 彩色闪光灯效果
    if (millis() - lastUpdate > 200) // Update every 200ms
    {
      lastUpdate = millis();

      // Random colorful flashing
      for (int i = 0; i < LED_COUNT; i++)
      {
        // Use different hues for each LED
        uint8_t hue = (colorIndex + (i * 10)) % 255;
        leds[i] = CHSV(hue, 255, 255);
      }

      FastLED.show();
      colorIndex += 20; // Shift color pattern
    }
  }

  // Clean up disconnected WebSocket clients periodically
  ws.cleanupClients();

  // UART Parsing
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

          StaticJsonDocument<512> doc;
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