/**
 * DLG LED — ESP32 · 64 NeoPixel · BLE · gói hiệu ứng NVS · bản đồ pixel
 *
 * A1 RR GG BB     màu toàn ma trận
 * A2 EE           hiệu ứng
 * A4 seq total …  chunk RGB (64×3 byte)
 * A5              hiển thị bản đồ pixel (tĩnh)
 * B0 / B1 / B2    đồng bộ gói hiệu ứng custom
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <esp_mac.h>

#define LED_PIN        48
#define NUM_PIXELS     64
#define NEO_BRIGHTNESS 255

#define SERVICE_UUID "00001f10-0000-1000-8000-00805f9b34fb"
#define CHAR_UUID    "00001f1f-0000-1000-8000-00805f9b34fb"

#define CMD_SET_COLOR    0xA1
#define CMD_SET_EFFECT   0xA2
#define CMD_PIXEL_CHUNK  0xA4
#define CMD_PIXEL_SHOW   0xA5
#define CMD_PACK_QUERY   0xB0
#define CMD_PACK_CHUNK   0xB1
#define CMD_PACK_COMMIT  0xB2
#define RSP_PACK_OK      0xC0
#define RSP_PACK_INFO    0xC1

#define CUSTOM_EFFECT_MIN 128
#define MAX_CUSTOM_FX     8
#define MAX_FRAMES        24
#define PACK_BUF_SIZE     2048
#define PIXEL_BUF_SIZE    (NUM_PIXELS * 3)

enum Effect : uint8_t {
  EFFECT_SOLID = 0, EFFECT_BLINK, EFFECT_FAST_BLINK, EFFECT_HEARTBEAT,
  EFFECT_BREATHE, EFFECT_WAVE, EFFECT_RAINBOW, EFFECT_AURORA, EFFECT_DISCO,
  EFFECT_POLICE, EFFECT_CANDLE, EFFECT_LIGHTNING, EFFECT_SOS, EFFECT_CHASE,
  EFFECT_OFF, EFFECT_COUNT
};

struct FxFrame { uint8_t r, g, b; uint16_t ms; };
struct CustomFx { uint8_t id; uint8_t frameCount; FxFrame frames[MAX_FRAMES]; };
struct EffectPack { uint16_t version; uint8_t count; CustomFx items[MAX_CUSTOM_FX]; };

struct SosStep { uint16_t ms; bool on; };
static const SosStep SOS_SEQ[] = {
  {120, true}, {120, false}, {120, true}, {120, false}, {120, true}, {360, false},
  {360, true}, {120, false}, {360, true}, {120, false}, {360, true}, {360, false},
  {120, true}, {120, false}, {120, true}, {120, false}, {120, true}, {900, false},
};
static const size_t SOS_LEN = sizeof(SOS_SEQ) / sizeof(SOS_SEQ[0]);

Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
BLECharacteristic *rxtxChar = nullptr;

EffectPack fxPack;
uint8_t packRxBuf[PACK_BUF_SIZE];
uint16_t packRxLen = 0;
uint8_t packRxTotal = 0;
bool packRxReady = false;

uint8_t pixelBuf[PIXEL_BUF_SIZE];
uint16_t pixelRxLen = 0;
uint8_t pixelRxTotal = 0;
bool pixelRxReady = false;
bool pixelMapActive = false;

uint8_t activeEffectId = EFFECT_SOLID;
uint8_t baseR = 245, baseG = 71, baseB = 31;
uint8_t effectHue = 0, breatheLevel = 0, wavePhase = 0, chasePos = 0;
int8_t breatheDir = 1;
bool blinkOn = true, policeRed = true, lightningFlash = false;
uint8_t heartbeatPhase = 0, sosIndex = 0, customFxIdx = 0, customFrameIdx = 0;
unsigned long lastAnimMs = 0, phaseEndMs = 0;

uint16_t readU16LE(const uint8_t *p) { return p[0] | (p[1] << 8); }

bool packIsValid(const uint8_t *data, size_t len, EffectPack *out) {
  if (len < 8 || data[0] != 'D' || data[1] != 'L' || data[2] != 'F' || data[3] != 'X') return false;
  EffectPack tmp = {};
  tmp.version = readU16LE(data + 4);
  tmp.count = data[6];
  if (tmp.count > MAX_CUSTOM_FX) return false;
  size_t off = 8;
  for (uint8_t i = 0; i < tmp.count; i++) {
    if (off + 2 > len) return false;
    uint8_t id = data[off++], fc = data[off++];
    if (id < CUSTOM_EFFECT_MIN || fc == 0 || fc > MAX_FRAMES) return false;
    if (off + fc * 6 > len) return false;
    tmp.items[i].id = id;
    tmp.items[i].frameCount = fc;
    for (uint8_t f = 0; f < fc; f++) {
      tmp.items[i].frames[f].r = data[off++];
      tmp.items[i].frames[f].g = data[off++];
      tmp.items[i].frames[f].b = data[off++];
      tmp.items[i].frames[f].ms = readU16LE(data + off);
      off += 2;
    }
  }
  return off == len && (*out = tmp, true);
}

void savePackToNvs(const uint8_t *data, size_t len) {
  prefs.begin("dlgled", false);
  prefs.putBytes("fxpack", data, len);
  prefs.end();
}

bool loadPackFromNvs() {
  prefs.begin("dlgled", true);
  size_t len = prefs.getBytesLength("fxpack");
  if (len == 0 || len > PACK_BUF_SIZE) { prefs.end(); return false; }
  uint8_t buf[PACK_BUF_SIZE];
  if (prefs.getBytes("fxpack", buf, len) != len) { prefs.end(); return false; }
  prefs.end();
  EffectPack parsed;
  if (!packIsValid(buf, len, &parsed)) return false;
  fxPack = parsed;
  return true;
}

int findCustomFxById(uint8_t id) {
  for (uint8_t i = 0; i < fxPack.count; i++)
    if (fxPack.items[i].id == id) return i;
  return -1;
}

void notifyPackInfo(uint8_t code) {
  if (!rxtxChar) return;
  uint8_t rsp[4] = { code, (uint8_t)(fxPack.version >> 8), (uint8_t)(fxPack.version & 0xFF), fxPack.count };
  rxtxChar->setValue(rsp, 4);
  rxtxChar->notify();
}

bool commitPackRx() {
  EffectPack parsed;
  if (!packIsValid(packRxBuf, packRxLen, &parsed)) return false;
  savePackToNvs(packRxBuf, packRxLen);
  fxPack = parsed;
  return true;
}

uint8_t scaleChannel(uint8_t c, uint8_t level) { return (uint16_t)c * level / 255; }

void stripShow() { strip.show(); }

void fillStrip(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = strip.Color(r, g, b);
  for (uint16_t i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, c);
  stripShow();
}

void fillStripScaled(uint8_t level) {
  uint8_t r = scaleChannel(baseR, level), g = scaleChannel(baseG, level), b = scaleChannel(baseB, level);
  fillStrip(r, g, b);
}

void showPixelMap() {
  for (uint16_t i = 0; i < NUM_PIXELS; i++) {
    uint16_t o = i * 3;
    strip.setPixelColor(i, strip.Color(pixelBuf[o], pixelBuf[o + 1], pixelBuf[o + 2]));
  }
  stripShow();
}

uint8_t triangleWave(uint8_t phase) { return (phase < 128) ? phase * 2 : (255 - phase) * 2; }

void resetEffectAnim() {
  lastAnimMs = millis();
  phaseEndMs = lastAnimMs;
  blinkOn = true;
  breatheLevel = 80;
  breatheDir = 1;
  wavePhase = 0;
  chasePos = 0;
  heartbeatPhase = 0;
  policeRed = true;
  sosIndex = 0;
  effectHue = 0;
  lightningFlash = false;
  customFrameIdx = 0;
  pixelMapActive = false;
}

void applySolid() {
  if (activeEffectId == EFFECT_OFF) fillStrip(0, 0, 0);
  else fillStrip(baseR, baseG, baseB);
}

void setActiveEffect(uint8_t id) {
  pixelMapActive = false;
  if (id < EFFECT_COUNT) activeEffectId = id;
  else if (id >= CUSTOM_EFFECT_MIN && findCustomFxById(id) >= 0) {
    activeEffectId = id;
    customFxIdx = findCustomFxById(id);
  } else activeEffectId = EFFECT_OFF;
  resetEffectAnim();
  if (activeEffectId == EFFECT_OFF) fillStrip(0, 0, 0);
  else if (activeEffectId == EFFECT_SOLID) applySolid();
  else if (activeEffectId >= CUSTOM_EFFECT_MIN) {
    const FxFrame &f = fxPack.items[customFxIdx].frames[0];
    fillStrip(f.r, f.g, f.b);
    phaseEndMs = millis() + f.ms;
  }
}

void runCustomEffect(unsigned long now) {
  int idx = findCustomFxById(activeEffectId);
  if (idx < 0) { fillStrip(0, 0, 0); return; }
  if (now < phaseEndMs) return;
  CustomFx &fx = fxPack.items[idx];
  customFrameIdx = (customFrameIdx + 1) % fx.frameCount;
  const FxFrame &f = fx.frames[customFrameIdx];
  fillStrip(f.r, f.g, f.b);
  phaseEndMs = now + f.ms;
}

void runRainbow(unsigned long now, uint16_t stepMs, bool spread) {
  if (now - lastAnimMs < stepMs) return;
  lastAnimMs = now;
  effectHue++;
  for (uint16_t i = 0; i < NUM_PIXELS; i++) {
    uint8_t h = spread ? effectHue + (uint16_t)i * 256 / NUM_PIXELS : effectHue;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(h * 256, 255, 200)));
  }
  stripShow();
}

void runChase(unsigned long now) {
  if (now - lastAnimMs < 80) return;
  lastAnimMs = now;
  strip.clear();
  for (uint8_t t = 0; t < 4; t++) {
    int p = (chasePos - t + NUM_PIXELS) % NUM_PIXELS;
    uint8_t lv = 255 - t * 55;
    strip.setPixelColor(p, strip.Color(scaleChannel(baseR, lv), scaleChannel(baseG, lv), scaleChannel(baseB, lv)));
  }
  stripShow();
  chasePos = (chasePos + 1) % NUM_PIXELS;
}

void runBuiltinEffect(unsigned long now) {
  switch ((Effect)activeEffectId) {
    case EFFECT_OFF: fillStrip(0, 0, 0); break;
    case EFFECT_SOLID: applySolid(); break;
    case EFFECT_BLINK:
      if (now - lastAnimMs >= 500) {
        lastAnimMs = now; blinkOn = !blinkOn;
        if (blinkOn) applySolid(); else fillStrip(0, 0, 0);
      }
      break;
    case EFFECT_FAST_BLINK:
      if (now - lastAnimMs >= 120) {
        lastAnimMs = now; blinkOn = !blinkOn;
        if (blinkOn) applySolid(); else fillStrip(0, 0, 0);
      }
      break;
    case EFFECT_HEARTBEAT:
      if (now >= phaseEndMs) {
        const uint16_t beats[] = {90, 90, 90, 650};
        if (heartbeatPhase % 2 == 0) applySolid(); else fillStrip(0, 0, 0);
        phaseEndMs = now + beats[heartbeatPhase];
        heartbeatPhase = (heartbeatPhase + 1) % 4;
      }
      break;
    case EFFECT_BREATHE:
      if (now - lastAnimMs >= 25) {
        lastAnimMs = now;
        breatheLevel += breatheDir * 4;
        if (breatheLevel >= 255) { breatheLevel = 255; breatheDir = -1; }
        if (breatheLevel <= 20) { breatheLevel = 20; breatheDir = 1; }
        fillStripScaled(breatheLevel);
      }
      break;
    case EFFECT_WAVE:
      if (now - lastAnimMs >= 20) {
        lastAnimMs = now;
        wavePhase += 6;
        fillStripScaled(30 + triangleWave(wavePhase) * 225 / 255);
      }
      break;
    case EFFECT_RAINBOW: runRainbow(now, 30, true); break;
    case EFFECT_AURORA:
      if (now - lastAnimMs >= 35) {
        lastAnimMs = now;
        effectHue += 2;
        for (uint16_t i = 0; i < NUM_PIXELS; i++) {
          uint8_t h = effectHue + i * 3;
          uint8_t sat = 180 + triangleWave(h) / 8;
          uint8_t val = 120 + triangleWave(h + 64) / 2;
          strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(h * 256, sat, val)));
        }
        stripShow();
      }
      break;
    case EFFECT_DISCO:
      if (now - lastAnimMs >= 160) {
        lastAnimMs = now;
        for (uint16_t i = 0; i < NUM_PIXELS; i++) {
          uint8_t h = random(0, 256);
          strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(h * 256, 255, 220)));
        }
        stripShow();
      }
      break;
    case EFFECT_POLICE:
      if (now - lastAnimMs >= 220) {
        lastAnimMs = now;
        policeRed = !policeRed;
        for (uint16_t i = 0; i < NUM_PIXELS; i++) {
          bool redSide = (i < NUM_PIXELS / 2) ? policeRed : !policeRed;
          strip.setPixelColor(i, redSide ? strip.Color(255, 0, 0) : strip.Color(0, 0, 255));
        }
        stripShow();
      }
      break;
    case EFFECT_CANDLE:
      if (now - lastAnimMs >= (uint16_t)(60 + random(0, 50))) {
        lastAnimMs = now;
        fillStripScaled(140 + random(0, 116));
      }
      break;
    case EFFECT_LIGHTNING:
      if (lightningFlash) {
        if (now >= phaseEndMs) { lightningFlash = false; fillStripScaled(8 + random(0, 20)); }
        break;
      }
      if (now - lastAnimMs >= (uint16_t)(50 + random(0, 90))) {
        lastAnimMs = now;
        if (random(0, 100) < 14) {
          fillStrip(255, 255, 255);
          lightningFlash = true;
          phaseEndMs = now + 60 + random(0, 140);
        } else fillStripScaled(8 + random(0, 20));
      }
      break;
    case EFFECT_SOS:
      if (now >= phaseEndMs) {
        const SosStep &step = SOS_SEQ[sosIndex];
        if (step.on) applySolid(); else fillStrip(0, 0, 0);
        phaseEndMs = now + step.ms;
        sosIndex = (sosIndex + 1) % SOS_LEN;
      }
      break;
    case EFFECT_CHASE: runChase(now); break;
    default: break;
  }
}

void runEffect(unsigned long now) {
  if (pixelMapActive) return;
  if (activeEffectId >= CUSTOM_EFFECT_MIN) runCustomEffect(now);
  else runBuiltinEffect(now);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer *) override { BLEDevice::startAdvertising(); }
};

class RxTxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String value = c->getValue();
    if (value.length() == 0) return;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(value.c_str());
    size_t len = value.length();

    if (len >= 4 && data[0] == CMD_SET_COLOR) {
      baseR = data[1]; baseG = data[2]; baseB = data[3];
      setActiveEffect(EFFECT_SOLID);
      return;
    }
    if (len >= 2 && data[0] == CMD_SET_EFFECT) {
      setActiveEffect(data[1]);
      return;
    }
    if (len >= 3 && data[0] == CMD_PIXEL_CHUNK) {
      uint8_t seq = data[1], total = data[2];
      if (seq == 0) { pixelRxLen = 0; pixelRxTotal = total; pixelRxReady = false; }
      if (total != pixelRxTotal || total == 0) return;
      size_t chunkLen = len - 3;
      if (pixelRxLen + chunkLen > PIXEL_BUF_SIZE) return;
      memcpy(pixelBuf + pixelRxLen, data + 3, chunkLen);
      pixelRxLen += chunkLen;
      if (seq + 1 == total) pixelRxReady = true;
      return;
    }
    if (len >= 1 && data[0] == CMD_PIXEL_SHOW) {
      if (pixelRxReady && pixelRxLen >= PIXEL_BUF_SIZE) {
        pixelMapActive = true;
        showPixelMap();
      }
      pixelRxReady = false;
      pixelRxLen = 0;
      return;
    }
    if (len >= 1 && data[0] == CMD_PACK_QUERY) {
      notifyPackInfo(RSP_PACK_INFO);
      return;
    }
    if (len >= 3 && data[0] == CMD_PACK_CHUNK) {
      uint8_t seq = data[1], total = data[2];
      if (seq == 0) { packRxLen = 0; packRxTotal = total; packRxReady = false; }
      if (total != packRxTotal || total == 0) return;
      size_t chunkLen = len - 3;
      if (packRxLen + chunkLen > PACK_BUF_SIZE) return;
      memcpy(packRxBuf + packRxLen, data + 3, chunkLen);
      packRxLen += chunkLen;
      if (seq + 1 == total) packRxReady = true;
      return;
    }
    if (len >= 1 && data[0] == CMD_PACK_COMMIT) {
      bool ok = packRxReady && commitPackRx();
      packRxReady = false;
      packRxLen = 0;
      if (ok) notifyPackInfo(RSP_PACK_OK);
    }
  }
};

String makeDeviceName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char name[20];
  snprintf(name, sizeof(name), "DLG-LED-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(name);
}

void setupBle() {
  String deviceName = makeDeviceName();
  BLEDevice::init(deviceName.c_str());
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *service = server->createService(SERVICE_UUID);
  rxtxChar = service->createCharacteristic(
    CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
    BLECharacteristic::PROPERTY_NOTIFY);
  rxtxChar->addDescriptor(new BLE2902());
  rxtxChar->setCallbacks(new RxTxCallbacks());
  service->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE: " + deviceName);
}

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(NEO_BRIGHTNESS);
  strip.clear();
  strip.show();
  randomSeed(esp_random());
  memset(pixelBuf, 0, sizeof(pixelBuf));
  fxPack.version = 0;
  fxPack.count = 0;
  loadPackFromNvs();
  applySolid();
  setupBle();
  Serial.printf("DLG LED %d pixels\n", NUM_PIXELS);
}

void loop() {
  runEffect(millis());
}
