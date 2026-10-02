#include <WiFi.h>
#include <PubSubClient.h>

// ===============================
// WiFi Configuration
// ===============================
const char* WIFI_SSID = "Ayush";
const char* WIFI_PASSWORD = "Ayush@5860";

// ===============================
// MQTT Configuration
// ===============================
const char* MQTT_SERVER = "88.222.213.221";
const int MQTT_PORT = 1883;

const char* MQTT_TOPIC = "smartauto/device/101";

// ===============================
// Relay Configuration
// ===============================
#define RELAY_PIN 26

// Your relay is working with this logic
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

WiFiClient espClient;
PubSubClient mqttClient(espClient);


// ===============================
// Connect WiFi
// ===============================
void connectWiFi()
{
  Serial.print("Connecting to WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}


// ===============================
// MQTT Message Received
// ===============================
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String message = "";

  for (unsigned int i = 0; i < length; i++)
  {
    message += (char)payload[i];
  }

  Serial.println();
  Serial.println("========== MQTT MESSAGE ==========");

  Serial.print("Topic   : ");
  Serial.println(topic);

  Serial.print("Message : ");
  Serial.println(message);

  // STATUS 1 = START
  if (message == "1")
  {
    digitalWrite(RELAY_PIN, RELAY_ON);

    Serial.println("STATUS 1 -> RELAY ON -> MOTOR START");
  }

  // STATUS 2 = STOP
  else if (message == "2")
  {
    digitalWrite(RELAY_PIN, RELAY_OFF);

    Serial.println("STATUS 2 -> RELAY OFF -> MOTOR STOP");
  }

  else
  {
    Serial.println("Unknown MQTT message");
  }

  Serial.println("==================================");
}


// ===============================
// Connect MQTT
// ===============================
void connectMQTT()
{
  while (!mqttClient.connected())
  {
    Serial.print("Connecting to MQTT...");

    String clientId = "ESP32_101_";
    clientId += String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str()))
    {
      Serial.println("Connected!");

      if (mqttClient.subscribe(MQTT_TOPIC))
      {
        Serial.print("Subscribed to: ");
        Serial.println(MQTT_TOPIC);
      }
      else
      {
        Serial.println("MQTT Subscribe Failed!");
      }
    }
    else
    {
      Serial.print("Failed, MQTT state = ");
      Serial.println(mqttClient.state());

      delay(5000);
    }
  }
}


// ===============================
// Setup
// ===============================
void setup()
{
  Serial.begin(115200);

  // Relay starts OFF
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32 SMART MOTOR CONTROLLER");
  Serial.println("==============================");

  // Connect WiFi
  connectWiFi();

  // Configure MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}


// ===============================
// Main Loop
// ===============================
void loop()
{
  // Reconnect WiFi if disconnected
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  // Reconnect MQTT if disconnected
  if (!mqttClient.connected())
  {
    connectMQTT();
  }

  mqttClient.loop();
}