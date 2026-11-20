// Handheld BLE / Wi-Fi signal finder for the XIAO ESP32S3. Streams RSSI to laptop/finder.py over UDP.
// BOOT: short press toggles BLE/Wi-Fi, hold 1 s tracks strongest signal. Needs NimBLE-Arduino 2.x.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NimBLEDevice.h>
#include <algorithm>

const char *WIFI_SSID = "YOUR_HOTSPOT_NAME";   // laptop Mobile Hotspot, 2.4 GHz band
const char *WIFI_PASS = "YOUR_HOTSPOT_PASSWORD";
const char *LAPTOP_IP = "10.56.226.130";         // Windows Mobile Hotspot address (auto-updated when the app says hello)

const uint16_t REPORT_PORT = 4210;   // laptop listens here
const uint16_t COMMAND_PORT = 4211;  // ESP listens here
const int BUTTON_PIN = 0;            // XIAO BOOT button (LOW when pressed)
const int LED_PIN = LED_BUILTIN;

const float RSSI_SMOOTHING = 0.3f;         // 0..1, higher = faster but noisier
const uint32_t REPORT_EVERY_MS = 300;
const uint32_t FORGET_AFTER_MS = 20000;
const uint32_t WIFI_FULL_SCAN_MS = 4000;   // Wi-Fi mode, nothing tracked: scan all channels
const uint32_t WIFI_TRACK_SCAN_MS = 400;   // Wi-Fi mode, tracking: rescan just that channel
const int MAX_DEVICES = 80;
const int MAX_REPORTED = 30;

enum Mode : uint8_t { MODE_BLE, MODE_WIFI };
volatile Mode mode = MODE_BLE;

struct Device {
  bool used;
  char id[24];      // "ble:aa:bb:.." or "wifi:aa:bb:.." (Wi-Fi = router/hotspot BSSID)
  char name[33];    // BLE name or Wi-Fi network name (SSID)
  int16_t company;  // BLE manufacturer ID, -1 if none
  uint8_t channel;  // Wi-Fi channel
  float rssi;       // smoothed dBm
  uint32_t lastSeen;
};

Device devices[MAX_DEVICES];
SemaphoreHandle_t tableLock;   // BLE callbacks run in a different task than loop()
char targetId[24] = "";

WiFiUDP udp;
IPAddress laptopIp;

Device *findOrCreate(const char *id) {  // caller holds tableLock
  int freeSlot = -1, oldest = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].used && strcmp(devices[i].id, id) == 0) return &devices[i];
    if (!devices[i].used && freeSlot < 0) freeSlot = i;
    if (devices[i].lastSeen < devices[oldest].lastSeen) oldest = i;
  }
  Device &d = devices[freeSlot >= 0 ? freeSlot : oldest];
  memset(&d, 0, sizeof(d));
  d.used = true;
  strlcpy(d.id, id, sizeof(d.id));
  d.company = -1;
  d.rssi = NAN;
  return &d;
}

void updateRssi(Device *d, int sample) {  // exponential moving average
  d->rssi = std::isnan(d->rssi) ? sample : d->rssi + RSSI_SMOOTHING * (sample - d->rssi);
  d->lastSeen = millis();
}

void clearTable() {
  xSemaphoreTake(tableLock, portMAX_DELAY);
  for (int i = 0; i < MAX_DEVICES; i++) devices[i].used = false;
  xSemaphoreGive(tableLock);
}

class ScanCallbacks : public NimBLEScanCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice *adv) override {
    if (mode != MODE_BLE) return;
    char id[24];
    snprintf(id, sizeof(id), "ble:%s", adv->getAddress().toString().c_str());
    std::string name = adv->haveName() ? adv->getName() : std::string();
    int company = -1;
    if (adv->haveManufacturerData()) {
      std::string m = adv->getManufacturerData();
      if (m.size() >= 2) company = (uint8_t)m[0] | ((uint8_t)m[1] << 8);  // Bluetooth SIG company ID
    }
    if (xSemaphoreTake(tableLock, pdMS_TO_TICKS(5)) != pdTRUE) return;
    Device *d = findOrCreate(id);
    if (!name.empty()) strlcpy(d->name, name.c_str(), sizeof(d->name));
    if (company >= 0) d->company = company;
    updateRssi(d, adv->getRSSI());
    xSemaphoreGive(tableLock);
  }
} scanCallbacks;

void startBleScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // every packet, so RSSI updates constantly
  scan->setActiveScan(true);                     // ask for scan responses (often hold the name)
  scan->setInterval(100);
  scan->setWindow(60);                           // leave radio time for Wi-Fi reporting
  scan->setDuplicateFilter(0);
  scan->setMaxResults(0);
  scan->start(0, false, true);
}

uint32_t lastWifiScan = 0;
bool wifiScanRunning = false;

int targetChannel() {
  if (strncmp(targetId, "wifi:", 5) != 0) return 0;
  int ch = 0;
  xSemaphoreTake(tableLock, portMAX_DELAY);
  for (int i = 0; i < MAX_DEVICES; i++)
    if (devices[i].used && strcmp(devices[i].id, targetId) == 0) ch = devices[i].channel;
  xSemaphoreGive(tableLock);
  return ch;
}

void serviceWifiScan() {
  if (wifiScanRunning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    wifiScanRunning = false;
    lastWifiScan = millis();
    if (n <= 0) { WiFi.scanDelete(); return; }  // failed or nothing: try again next period
    xSemaphoreTake(tableLock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
      char id[24];
      snprintf(id, sizeof(id), "wifi:%s", WiFi.BSSIDstr(i).c_str());
      Device *d = findOrCreate(id);
      strlcpy(d->name, WiFi.SSID(i).c_str(), sizeof(d->name));
      d->channel = WiFi.channel(i);
      updateRssi(d, WiFi.RSSI(i));
    }
    xSemaphoreGive(tableLock);
    WiFi.scanDelete();
    return;
  }
  if (mode != MODE_WIFI) return;
  int ch = targetChannel();
  if (millis() - lastWifiScan < (ch ? WIFI_TRACK_SCAN_MS : WIFI_FULL_SCAN_MS)) return;
  // tracking: scan target channel only, otherwise sweep all 13
  WiFi.scanNetworks(true, true, false, ch ? 150 : 180, ch);
  wifiScanRunning = true;
}

void setMode(Mode m) {
  if (m == mode) return;
  mode = m;
  targetId[0] = '\0';
  clearTable();
  if (m == MODE_BLE) startBleScan();
  else NimBLEDevice::getScan()->stop();  // free the radio for Wi-Fi scanning
  lastWifiScan = 0;
  Serial.printf("Mode: %s\n", m == MODE_BLE ? "Bluetooth" : "Wi-Fi");
}

void trackStrongest() {
  int best = -1;
  xSemaphoreTake(tableLock, portMAX_DELAY);
  for (int i = 0; i < MAX_DEVICES; i++)
    if (devices[i].used && millis() - devices[i].lastSeen < 3000 && (best < 0 || devices[i].rssi > devices[best].rssi))
      best = i;
  if (best >= 0) strlcpy(targetId, devices[best].id, sizeof(targetId));
  xSemaphoreGive(tableLock);
  Serial.printf("Tracking strongest: %s\n", best >= 0 ? targetId : "(nothing heard)");
}

uint32_t lastReport = 0;

void sendReport() {
  if (WiFi.status() != WL_CONNECTED) return;
  int order[MAX_DEVICES], n = 0;
  uint32_t now = millis();
  xSemaphoreTake(tableLock, portMAX_DELAY);
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!devices[i].used) continue;
    if (now - devices[i].lastSeen > FORGET_AFTER_MS) { devices[i].used = false; continue; }
    order[n++] = i;
  }
  std::sort(order, order + n, [](int a, int b) { return devices[a].rssi > devices[b].rssi; });

  String json;
  json.reserve(3000);
  json = "{\"mode\":\"";
  json += mode == MODE_BLE ? "ble" : "wifi";
  json += "\",\"target\":\"";
  json += targetId;
  json += "\",\"dev\":[";
  int sent = 0;
  for (int k = 0; k < n; k++) {
    Device &d = devices[order[k]];
    bool isTarget = strcmp(d.id, targetId) == 0;
    if (sent >= MAX_REPORTED && !isTarget) continue;
    char name[70];
    size_t j = 0;
    for (size_t c = 0; d.name[c] && j < sizeof(name) - 2; c++) {  // escape for JSON
      char ch = d.name[c];
      if (ch == '"' || ch == '\\') name[j++] = '\\';
      if ((uint8_t)ch >= 0x20) name[j++] = ch;
    }
    name[j] = '\0';
    char item[160];
    // [id, name, rssi, company, channel, ms since heard]
    snprintf(item, sizeof(item), "%s[\"%s\",\"%s\",%.1f,%d,%d,%lu]", sent ? "," : "", d.id, name, d.rssi,
             d.company, d.channel, (unsigned long)(now - d.lastSeen));
    json += item;
    sent++;
  }
  xSemaphoreGive(tableLock);
  json += "]}";
  udp.beginPacket(laptopIp, REPORT_PORT);
  udp.print(json);
  udp.endPacket();
}

// Commands from the laptop: "hello", "mode ble", "mode wifi", "track <id>", "track"
void serviceCommands() {
  int len = udp.parsePacket();
  if (len <= 0) return;
  char buf[64];
  int r = udp.read(buf, sizeof(buf) - 1);
  buf[r > 0 ? r : 0] = '\0';
  laptopIp = udp.remoteIP();  // reply to whoever is talking to us
  if (strcmp(buf, "mode ble") == 0) setMode(MODE_BLE);
  else if (strcmp(buf, "mode wifi") == 0) setMode(MODE_WIFI);
  else if (strncmp(buf, "track", 5) == 0) {
    const char *id = buf[5] == ' ' ? buf + 6 : "";
    strlcpy(targetId, id, sizeof(targetId));
  }
}

uint32_t pressedAt = 0;
bool wasPressed = false, longDone = false;

void serviceButton() {
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (pressed && !wasPressed) { pressedAt = now; longDone = false; }
  if (pressed && !longDone && now - pressedAt > 1000) {  // long press
    longDone = true;
    if (targetId[0]) { targetId[0] = '\0'; Serial.println("Stopped tracking"); }
    else trackStrongest();
  }
  if (!pressed && wasPressed && !longDone && now - pressedAt > 40)  // short press (40 ms debounce)
    setMode(mode == MODE_BLE ? MODE_WIFI : MODE_BLE);
  wasPressed = pressed;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, HIGH);
  tableLock = xSemaphoreCreateMutex();
  laptopIp.fromString(LAPTOP_IP);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // non-blocking; keeps retrying in the background
  udp.begin(COMMAND_PORT);

  NimBLEDevice::init("");
  startBleScan();
  Serial.println("Signal Finder ready (Bluetooth mode). Short-press BOOT = switch mode, long-press = track strongest.");
}

bool wasConnected = false;
void loop() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    digitalWrite(LED_PIN, connected ? LOW : HIGH);  // LED on = connected
    if (connected) Serial.printf("Joined hotspot, my IP %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("Laptop hotspot not reachable - retrying");
  }
  serviceButton();
  if (connected) serviceCommands();
  serviceWifiScan();
  if (millis() - lastReport >= REPORT_EVERY_MS) {
    lastReport = millis();
    sendReport();
  }
  delay(2);
}
