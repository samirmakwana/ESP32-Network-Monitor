#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>

// ===================== CONFIG =====================
const char* ssid = "Dhunvor";
const char* password = "9819251784";

const char* mqtt_server = "192.168.29.2";
const int mqtt_port = 1883;
const char* mqtt_user = "mqttuser";
const char* mqtt_pass = "zigbee";
const char* mqtt_base_topic = "home/network_monitor";

#define CHECK_INTERVAL 15000           
#define SUCCESS_THRESHOLD 2
#define FAILURE_THRESHOLD 2
#define RSSI_INTERVAL 10000            

Preferences preferences;
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ===================== DEVICES =====================
struct Device {
  const char* name;
  const char* ip;
  uint16_t port;
  bool state;
  unsigned long lastStateChange;
  int consecutiveSuccesses;
  int consecutiveFailures;
  int totalChecks;
  int successfulChecks;
};

Device devices[] = {
  {"Router",       "192.168.29.1", 80, false, 0,0,0,0,0},
  {"Server",       "192.168.29.5", 80, false, 0,0,0,0,0},
  {"Desktop",      "192.168.29.94", 8080, false,0,0,0,0,0},
  {"Pihole",       "192.168.29.10", 80, false,0,0,0,0,0},
  {"Jellyfin",     "192.168.29.17", 8096, false,0,0,0,0,0},
  {"Proxmox",      "192.168.29.20", 8006, false,0,0,0,0,0},
  {"Realme",       "192.168.29.17", 80, false,0,0,0,0,0},
  {"Pioneer",      "192.168.29.20", 80, false,0,0,0,0,0},
  {"Pi",           "192.168.29.40", 80, false,0,0,0,0,0},
  {"Internet",     "1.1.1.1", 53, false,0,0,0,0,0}
};

const int DEVICE_COUNT = sizeof(devices)/sizeof(devices[0]);

// ===================== STATE =====================
unsigned long lastCheck = 0;
unsigned long lastRSSIUpdate = 0;
unsigned long bootTime = 0;
int totalChecksPerformed = 0;
int mqttMessagesPublished = 0;
int mqttReconnectCount = 0; // Track disconnections

// ===================== FUNCTION DECLARATIONS =====================
bool checkTCP(const char* ip, uint16_t port);
float getUptimePercentage(Device &d);
void mqttConnect();
void checkAllDevices();
void publishWiFiRSSI();

// ===================== WEB PAGES =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><title>Network Monitor</title>
<style>
  body { font-family: sans-serif; background: #1a1a1a; color: white; text-align: center; }
  .card { background: #2d2d2d; padding: 20px; margin: 10px; border-radius: 10px; display: inline-block; width: 300px; }
  .status-online { color: #4caf50; }
  .status-offline { color: #f44336; }
  .stats { font-size: 0.8em; color: #aaa; }
</style></head><body>
  <h1>🔍 Network Monitor</h1>
  <div id="info">
    <p>Monitor IP: <span id="ip">-</span> | RSSI: <span id="rssi">-</span></p>
    <p>MQTT Reconnects: <span id="reconnects" style="color:orange">-</span></p>
  </div>
  <div id="devices">Loading...</div>
  <script>
    function update() {
      fetch('/api/status').then(r => r.json()).then(data => {
        document.getElementById('ip').innerText = data.monitor_ip;
        document.getElementById('rssi').innerText = data.rssi + " dBm";
        document.getElementById('reconnects').innerText = data.mqtt_reconnects;
        let html = "";
        data.devices.forEach(d => {
          html += `<div class="card"><h3>${d.name}</h3>
                   <p class="${d.online ? 'status-online' : 'status-offline'}">${d.online ? '● ONLINE' : '○ OFFLINE'}</p>
                   <p class="stats">Uptime: ${d.uptime}% | Checks: ${d.checks}</p></div>`;
        });
        document.getElementById('devices').innerHTML = html;
      });
    }
    setInterval(update, 5000); update();
  </script>
</body></html>)rawliteral";

// ===================== WEB HANDLERS =====================
void handleIndex() { server.send_P(200, "text/html", INDEX_HTML); }

void handleAPI() {
  String json = "{";
  json += "\"monitor_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"mqtt_reconnects\":" + String(mqttReconnectCount) + ",";
  json += "\"devices\":[";
  for (int i=0; i<DEVICE_COUNT; i++){
    json += "{";
    json += "\"name\":\""+String(devices[i].name)+"\",";
    json += "\"online\":"+String(devices[i].state?"true":"false")+",";
    json += "\"uptime\":\""+String(getUptimePercentage(devices[i]),1)+"\",";
    json += "\"checks\":"+String(devices[i].totalChecks);
    json += "}";
    if(i<DEVICE_COUNT-1) json+=",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// ===================== CORE LOGIC =====================
bool checkTCP(const char* ip, uint16_t port) {
  WiFiClient client;
  client.setTimeout(2); // Short timeout for faster scanning
  bool result = client.connect(ip, port);
  client.stop();
  return result;
}

float getUptimePercentage(Device &d){
  if(d.totalChecks == 0) return 0.0;
  return (d.successfulChecks * 100.0) / d.totalChecks;
}

void mqttConnect(){
  if(!mqtt.connected()){
    Serial.print("Connecting to MQTT... ");
    // Unique ID is critical for ESP32-C6
    String clientId = "ESP32-C6" + String(random(0xffff), HEX);
    if(mqtt.connect(clientId.c_str(), mqtt_user, mqtt_pass)){
      Serial.println("CONNECTED");
      mqtt.publish(mqtt_base_topic, "Monitor Online");
    } else {
      mqttReconnectCount++;
      Serial.print("FAILED, rc=");
      Serial.println(mqtt.state());
    }
  }
}

void checkAllDevices(){
  for(int i=0; i<DEVICE_COUNT; i++){
    bool currentCheck = checkTCP(devices[i].ip, devices[i].port);
    devices[i].totalChecks++;
    if(currentCheck) devices[i].successfulChecks++;
    
    if(currentCheck){
      devices[i].consecutiveSuccesses++;
      devices[i].consecutiveFailures = 0;
      if(!devices[i].state && devices[i].consecutiveSuccesses >= SUCCESS_THRESHOLD){
        devices[i].state = true;
        if(mqtt.connected()) mqtt.publish((String(mqtt_base_topic)+"/"+devices[i].name).c_str(), "online");
      }
    } else {
      devices[i].consecutiveFailures++;
      devices[i].consecutiveSuccesses = 0;
      if(devices[i].state && devices[i].consecutiveFailures >= FAILURE_THRESHOLD){
        devices[i].state = false;
        if(mqtt.connected()) mqtt.publish((String(mqtt_base_topic)+"/"+devices[i].name).c_str(), "offline");
      }
    }
  }
}

void publishWiFiRSSI(){
  int32_t rssi = WiFi.RSSI();
  Serial.printf("Wi-Fi RSSI: %d dBm\n", rssi);
  if(mqtt.connected()){
    mqtt.publish((String(mqtt_base_topic)+"/rssi").c_str(), String(rssi).c_str());
  }
}

// ===================== SETUP =====================
void setup(){
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== XIAO ESP32-C6 NETWORK MONITOR ===");
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // STABILIZATION: Prevents radio power-down on weak signals
  WiFi.begin(ssid, password);

  while(WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  // FIXED: Proper IP format
  Serial.print("\n✓ Connected! IP: ");
  Serial.println(WiFi.localIP().toString());

  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setKeepAlive(60); // STABILIZATION: Better for high-latency/weak connections
  
  server.on("/", handleIndex);
  server.on("/api/status", handleAPI);
  server.begin();
  
  checkAllDevices();
}

void loop(){
  if(!mqtt.connected()) mqttConnect();
  mqtt.loop();

  if(millis() - lastCheck >= CHECK_INTERVAL){
    checkAllDevices();
    lastCheck = millis();
  }

  if(millis() - lastRSSIUpdate >= RSSI_INTERVAL){
    publishWiFiRSSI();
    lastRSSIUpdate = millis();
  }

  server.handleClient();
}