#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// WiFi AP credentials
constexpr const char *kApSsid = "户外移动电源";
constexpr const char *kApPassword = "portablepower123";

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

    StaticJsonDocument<512> reply;
    reply["event"] = "response";
    reply["cmd"] = cmd;
    reply["msg"] = text;
    reply["from"] = static_cast<uint32_t>(client->id());
    reply["ok"] = true;

    if (strcmp(cmd, "ping") == 0)
    {
      reply["pong"] = millis();
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

  ws.onEvent(handleWebSocketEvent);
  server.addHandler(&ws);

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request)
                    { request->send(404, "text/plain", "Not found"); });

  server.begin();
}

void loop()
{
  // Clean up disconnected WebSocket clients periodically
  ws.cleanupClients();
}