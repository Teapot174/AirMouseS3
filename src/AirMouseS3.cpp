#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include "bitmap.h"


namespace {

constexpr char kDeviceName[] = "AirStick";
constexpr char kManufacturerName[] = "Teapot";

constexpr uint16_t kBgColor = 0x0000;
constexpr uint16_t kFgColor = 0xFFFF;
constexpr uint16_t kAccentColor = 0x2CDE;
constexpr uint32_t kBootSplashMs = 3000;
constexpr uint32_t kUiRefreshMs = 33;
constexpr uint32_t kBatterySyncMs = 30000;
constexpr uint32_t kLevelingReleaseSettleMs = 150;
constexpr int kCalibrationMinSamples = 25;
constexpr float kCalibrationStableDps = 8.0f;
constexpr float kGyroDeadzoneDps = 0.0f;
constexpr float kPointerGain = 9.5f;
constexpr float kHorizontalGain = 16.0f;
constexpr float kMaxStep = 40.0f;

enum class UiState : uint8_t {
  Boot,
  WaitingForPair,
  Connected,
};

struct MouseReport {
  uint8_t buttons;
  int8_t x;
  int8_t y;
  int8_t wheel;
} __attribute__((packed));

struct CalibrationState {
  bool active = false;
  int samples = 0;
  float sumX = 0.0f;
  float sumY = 0.0f;
  float sumZ = 0.0f;
};

static const uint8_t kMouseReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Const,Array,Abs)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

UiState currentState = UiState::Boot;
bool screenDirty = true;
bool bleConnected = false;
bool bleInitialized = false;
bool motionEnabled = true;
bool leftPressed = false;
bool rightPressed = false;
bool levelingHeld = false;
bool levelingLatch = false;

uint32_t bootStartedAt = 0;
uint32_t lastUiRefreshAt = 0;
uint32_t lastBatterySyncAt = 0;
uint32_t lastImuAt = 0;
uint32_t suppressMotionUntil = 0;

float gyroBiasX = 0.0f;
float gyroBiasY = 0.0f;
float gyroBiasZ = 0.0f;
float pointerAccumulatorX = 0.0f;
float pointerAccumulatorY = 0.0f;
float aimAngleX = 0.0f;
float aimAngleY = 0.0f;
float aimCursorX = 0.0f;
float aimCursorY = 0.0f;
float gyroViewX = 0.0f;
float gyroViewY = 0.0f;
float gyroViewZ = 0.0f;

CalibrationState calibration;

NimBLEServer* bleServer = nullptr;
NimBLEHIDDevice* hidDevice = nullptr;
NimBLECharacteristic* inputReport = nullptr;

int readBatteryLevelSafe() {
  const int level = M5.Power.getBatteryLevel();
  if (level < 0) {
    return 0;
  }
  if (level > 100) {
    return 100;
  }
  return level;
}

bool isChargingSafe() {
  return M5.Power.getVBUSVoltage() > 4000;
}

float clampf(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

int8_t clampMouseAxis(float value) {
  const float clamped = clampf(value, -127.0f, 127.0f);
  return static_cast<int8_t>(clamped);
}

float applyDeadzone(float value, float deadzone) {
  if (std::fabs(value) < deadzone) {
    return 0.0f;
  }
  return value;
}

void clearScreen() {
  M5.Display.fillScreen(kBgColor);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextWrap(false);
}

void drawBatteryStatus() {
  const bool charging = isChargingSafe();
  const unsigned char* batteryImage = charging ? image_battery_charging_bits : image_battery_bits;
  const int batteryLevel = readBatteryLevelSafe();

  M5.Display.setTextFont(1);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(kFgColor, kBgColor);
  M5.Display.fillRect(84, 0, 50, 18, kBgColor);
  if (!charging) {
    const String batteryText = String(batteryLevel);
    int textX = 117;
    if (batteryLevel >= 100) {
      textX = 110;
    } else if (batteryLevel >= 10) {
      textX = 114;
    }
    M5.Display.drawString(batteryText, textX, 6);
  }
  M5.Display.drawBitmap(106, 2, batteryImage, 24, 16, kFgColor);
}

void drawBootScreen() {
  clearScreen();
  M5.Display.drawBitmap(1, 90, image_Teapot_bits, 134, 135, kFgColor);
  M5.Display.setTextColor(kFgColor);
  M5.Display.setTextFont(1);
  M5.Display.setTextSize(3);
  M5.Display.drawString("AIR", 42, 15);
  M5.Display.drawString("Mouse", 24, 45);
  M5.Display.setTextColor(kAccentColor);
  M5.Display.drawString("S3", 51, 76);
  M5.Display.setTextColor(kFgColor);
  M5.Display.setTextSize(2);
  M5.Display.drawString("by Teapot", 15, 215);
}

void drawWaitingScreen() {
  clearScreen();
  M5.Display.drawBitmap(1, 50, image_Teapot_bits, 134, 135, kFgColor);
  M5.Display.setTextColor(kFgColor);
  M5.Display.setTextFont(1);
  M5.Display.setTextSize(2);
  M5.Display.drawString("AirStick", 21, 28);
  M5.Display.drawString("Waiting", 27, 175);
  M5.Display.drawString("for pair...", 3, 195);
}

void drawConnectedScreen() {
  clearScreen();
  M5.Display.drawBitmap(1, 60, image_Teapot_bits, 134, 135, kAccentColor);
  M5.Display.setTextColor(kFgColor);
  M5.Display.setTextFont(1);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Connected", 15, 35);
  M5.Display.drawString("X:", 12, 180);
  M5.Display.drawString("Y:", 12, 200);
  M5.Display.drawString("Z:", 12, 220);
  if (calibration.active) {
    M5.Display.setTextSize(1);
    M5.Display.drawString("*Calibration*", 29, 60);
    M5.Display.setTextSize(2);
  }
  M5.Display.setTextFont(1);
  M5.Display.setTextSize(2);
  M5.Display.drawRightString(String(static_cast<int>(gyroViewX)), 128, 180);
  M5.Display.drawRightString(String(static_cast<int>(gyroViewY)), 128, 200);
  M5.Display.drawRightString(String(static_cast<int>(gyroViewZ)), 128, 220);
  drawBatteryStatus();
}

void renderUi() {
  switch (currentState) {
    case UiState::Boot:
      drawBootScreen();
      break;
    case UiState::WaitingForPair:
      drawWaitingScreen();
      break;
    case UiState::Connected:
      drawConnectedScreen();
      break;
  }
  screenDirty = false;
  lastUiRefreshAt = millis();
}

void syncBatteryLevel() {
  if (hidDevice == nullptr) {
    return;
  }
  hidDevice->setBatteryLevel(static_cast<uint8_t>(readBatteryLevelSafe()));
  lastBatterySyncAt = millis();
}

void sendMouseReport(int8_t x, int8_t y, uint8_t buttons) {
  if (!bleConnected || inputReport == nullptr) {
    return;
  }

  MouseReport report{
      .buttons = buttons,
      .x = x,
      .y = y,
      .wheel = 0,
  };
  inputReport->setValue(reinterpret_cast<uint8_t*>(&report), sizeof(report));
  inputReport->notify();
}

void sendButtonState() {
  const uint8_t buttons = (leftPressed ? 0x01 : 0x00) | (rightPressed ? 0x02 : 0x00);
  sendMouseReport(0, 0, buttons);
}

void resetPointerState() {
  aimAngleX = 0.0f;
  aimAngleY = 0.0f;
  aimCursorX = 0.0f;
  aimCursorY = 0.0f;
  pointerAccumulatorX = 0.0f;
  pointerAccumulatorY = 0.0f;
  lastImuAt = 0;
}

void startCalibration() {
  calibration.active = true;
  calibration.samples = 0;
  calibration.sumX = 0.0f;
  calibration.sumY = 0.0f;
  calibration.sumZ = 0.0f;
  screenDirty = true;
}

void finishCalibration() {
  if (calibration.samples >= kCalibrationMinSamples) {
    gyroBiasX = calibration.sumX / calibration.samples;
    gyroBiasY = calibration.sumY / calibration.samples;
    gyroBiasZ = calibration.sumZ / calibration.samples;
  }
  calibration.active = false;
  screenDirty = true;
}

void updateCalibrationFromImu(const m5::imu_data_t& imuData) {
  if (!calibration.active) {
    return;
  }

  const float gyroMagnitude = std::fabs(imuData.gyro.x) + std::fabs(imuData.gyro.y) + std::fabs(imuData.gyro.z);
  if (gyroMagnitude > kCalibrationStableDps) {
    calibration.samples = 0;
    calibration.sumX = 0.0f;
    calibration.sumY = 0.0f;
    calibration.sumZ = 0.0f;
    return;
  }

  calibration.sumX += imuData.gyro.x;
  calibration.sumY += imuData.gyro.y;
  calibration.sumZ += imuData.gyro.z;
  calibration.samples += 1;
}

void updateLevelingFromImu() {
  if (!levelingHeld) {
    return;
  }

  if (!M5.Imu.update()) {
    return;
  }

  const auto imuData = M5.Imu.getImuData();
  gyroViewX = imuData.gyro.z - gyroBiasZ;
  gyroViewY = imuData.gyro.y - gyroBiasY;
  gyroViewZ = imuData.gyro.z - gyroBiasZ;
  updateCalibrationFromImu(imuData);
  lastImuAt = 0;
}

void updateButtons() {
  const bool btnA = M5.BtnA.isPressed();

  if (btnA != leftPressed) {
    leftPressed = btnA;
    sendButtonState();
    screenDirty = true;
  }
}

void updatePointerFromImu() {
  const uint32_t now = millis();

  if (levelingHeld) {
    lastImuAt = 0;
    return;
  }

  if (now < suppressMotionUntil) {
    lastImuAt = 0;
    return;
  }

  if (!M5.Imu.update()) {
    return;
  }

  const auto imuData = M5.Imu.getImuData();
  gyroViewX = imuData.gyro.z - gyroBiasZ;
  gyroViewY = imuData.gyro.y - gyroBiasY;
  gyroViewZ = imuData.gyro.z - gyroBiasZ;

  if (!motionEnabled || !bleConnected) {
    lastImuAt = now;
    return;
  }

  if (lastImuAt == 0) {
    lastImuAt = now;
    return;
  }

  float dt = static_cast<float>(now - lastImuAt) / 1000.0f;
  lastImuAt = now;
  dt = clampf(dt, 0.001f, 0.03f);

  const float gyroHorizontal = applyDeadzone(imuData.gyro.z - gyroBiasZ, kGyroDeadzoneDps);
  const float gyroVertical = applyDeadzone(imuData.gyro.y - gyroBiasY, kGyroDeadzoneDps);

  aimAngleX += -gyroHorizontal * dt;
  aimAngleY += gyroVertical * dt;

  const float nextAimCursorX = aimAngleX * kHorizontalGain;
  const float nextAimCursorY = aimAngleY * kPointerGain;

  pointerAccumulatorX += nextAimCursorX - aimCursorX;
  pointerAccumulatorY += nextAimCursorY - aimCursorY;

  aimCursorX = nextAimCursorX;
  aimCursorY = nextAimCursorY;

  pointerAccumulatorX = clampf(pointerAccumulatorX, -kMaxStep, kMaxStep);
  pointerAccumulatorY = clampf(pointerAccumulatorY, -kMaxStep, kMaxStep);

  const int8_t dx = clampMouseAxis(std::round(pointerAccumulatorX));
  const int8_t dy = clampMouseAxis(std::round(pointerAccumulatorY));
  if (dx == 0 && dy == 0) {
    return;
  }

  pointerAccumulatorX -= dx;
  pointerAccumulatorY -= dy;

  const uint8_t buttons = (leftPressed ? 0x01 : 0x00) | (rightPressed ? 0x02 : 0x00);
  sendMouseReport(dx, dy, buttons);
  screenDirty = true;
}

class AirMouseServerCallbacks final : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    currentState = UiState::Connected;
    aimAngleX = 0.0f;
    aimAngleY = 0.0f;
    aimCursorX = 0.0f;
    aimCursorY = 0.0f;
    pointerAccumulatorX = 0.0f;
    pointerAccumulatorY = 0.0f;
    server->updateConnParams(connInfo.getConnHandle(), 6, 6, 0, 30);
    syncBatteryLevel();
    screenDirty = true;
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int) override {
    bleConnected = false;
    currentState = UiState::WaitingForPair;
    leftPressed = false;
    rightPressed = false;
    aimAngleX = 0.0f;
    aimAngleY = 0.0f;
    aimCursorX = 0.0f;
    aimCursorY = 0.0f;
    pointerAccumulatorX = 0.0f;
    pointerAccumulatorY = 0.0f;
    NimBLEDevice::getAdvertising()->start();
    screenDirty = true;
  }
};

void startAdvertising() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(kDeviceName);
  advertising->addServiceUUID(hidDevice->getHidService()->getUUID());
  advertising->enableScanResponse(true);
  advertising->start();
}

void initBleMouse() {
  if (bleInitialized) {
    return;
  }

  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new AirMouseServerCallbacks());

  hidDevice = new NimBLEHIDDevice(bleServer);
  inputReport = hidDevice->getInputReport(1);

  hidDevice->setManufacturer(kManufacturerName);
  hidDevice->setPnp(0x02, 0x303A, 0x4001, 0x0100);
  hidDevice->setHidInfo(0x00, 0x01);
  hidDevice->setReportMap(const_cast<uint8_t*>(kMouseReportMap), sizeof(kMouseReportMap));
  hidDevice->startServices();
  syncBatteryLevel();

  startAdvertising();
  bleInitialized = true;
}

}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  pinMode(12, INPUT_PULLUP);

  M5.Display.setRotation(2);
  M5.Display.fillScreen(kBgColor);
  M5.Display.setTextColor(kFgColor, kBgColor);

  bootStartedAt = millis();
  lastUiRefreshAt = bootStartedAt;
  lastBatterySyncAt = bootStartedAt;

  renderUi();
}

void loop() {
  M5.update();
  levelingHeld = (digitalRead(12) == LOW);

  if (levelingHeld && !levelingLatch) {
    levelingLatch = true;
    startCalibration();
    resetPointerState();
    suppressMotionUntil = 0xFFFFFFFFu;
    screenDirty = true;
  } else if (!levelingHeld && levelingLatch) {
    levelingLatch = false;
    finishCalibration();
    resetPointerState();
    suppressMotionUntil = millis() + kLevelingReleaseSettleMs;
    screenDirty = true;
  }

  if (currentState == UiState::Boot && millis() - bootStartedAt >= kBootSplashMs) {
    initBleMouse();
    currentState = bleConnected ? UiState::Connected : UiState::WaitingForPair;
    screenDirty = true;
  }

  updateButtons();
  updateLevelingFromImu();
  updatePointerFromImu();

  if (millis() - lastBatterySyncAt >= kBatterySyncMs) {
    syncBatteryLevel();
    screenDirty = true;
  }

  if (screenDirty && millis() - lastUiRefreshAt >= kUiRefreshMs) {
    renderUi();
  }

  delay(1);
}
