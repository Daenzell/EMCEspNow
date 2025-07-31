
#include "EmcEspNow.h"
#include "driver/temperature_sensor.h"
#include "esp_sleep.h"

// Instance of the ESP-NOW communication handler (slave mode)
EmcEspNow espNow;

// Temperature sensor handle and configuration
temperature_sensor_handle_t tempHandle = NULL;
temperature_sensor_config_t tempSensor = {
    .range_min = 20, // Minimum temperature range in Celsius
    .range_max = 50  // Maximum temperature range in Celsius
};

// Touch Sensor Treshold (tested on ESP32-S2)
const uint16_t touchThreshold = 8000; // change to higher value if too sensitive

// Timing variables for status LED and periodic tasks
unsigned long slaveMillis = 0;
unsigned long ledMillis = 0;
unsigned long lastActivityMillis = 0; // Track last button activity

// Temperature reading (in Celsius) from internal sensor
float tempOut = 0.0f;

// Power management settings
const unsigned long INACTIVITY_TIMEOUT = 30000; // 30 seconds of inactivity before sleep
bool lowPowerMode = false;

// === Button Pin Configuration Vectors ===
// IMPORTANT: Do NOT include LED_BUILTIN in any of these vectors.
// It is reserved for status indication and must not be repurposed as an input or output pin.

// Touch sensor pins
std::vector<uint8_t> buttonsTouchpins = {1, 2, 3, 4};

// Digital buttons connected to GND, use INPUT_PULLUP
std::vector<uint8_t> buttonsGndpins = {5, 6, 7, 8};

// Digital buttons connected to VCC, use INPUT_PULLDOWN
std::vector<uint8_t> buttonsVCCpins = {9, 10, 11, 12};

// Column pins for matrix buttons, read as inputs with pull-ups
std::vector<uint8_t> buttonsColpins = {13, 14, 16, 17};

// Row pins for matrix buttons, driven low during scan
std::vector<uint8_t> buttonsRowpins = {18, 21, 33, 34};

// Function to prepare wakeup sources
void prepareWakeupSources()
{
  // Configure touch pins as wakeup sources
  for (uint8_t pin : buttonsTouchpins)
  {
    esp_sleep_enable_touchpad_wakeup();
    touchAttachInterrupt(pin, []() {}, touchThreshold);
  }

  // Configure digital buttons as wakeup sources
  for (uint8_t pin : buttonsGndpins)
  {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, LOW);
  }

  for (uint8_t pin : buttonsVCCpins)
  {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, HIGH);
  }

  // Note: Matrix buttons are more complex to wake from - consider using only direct buttons for wakeup
}

// Function to enter low power mode
void enterLowPowerMode()
{
  Serial.println("Entering low power mode...");
  digitalWrite(LED_BUILTIN, LOW); // Turn off LED

  // Prepare wakeup sources
  prepareWakeupSources();

  // Disable temperature sensor
  if (tempHandle)
  {
    temperature_sensor_disable(tempHandle);
  }

  // Disable WiFi and ESP-NOW
  espNow.end();

  // Configure deep sleep
  esp_deep_sleep_start();
}

void setup()
{
  Serial.begin(115200);

  // Check wakeup reason
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  if (wakeupReason != ESP_SLEEP_WAKEUP_UNDEFINED)
  {
    // We woke from sleep - quick initialization
    lowPowerMode = false;
    lastActivityMillis = millis();

    // Minimal reinitialization
    pinMode(LED_BUILTIN, OUTPUT);

    // Reinitialize ESP-NOW in Slave mode
    espNow.begin(false); // false = Slave

    return;
  }

  // Configure built-in LED for status indication
  pinMode(LED_BUILTIN, OUTPUT);

  // Initialize ESP-NOW in Slave mode
  espNow.begin(false); // false = Slave

  // Initialize internal temperature sensor
  ESP_ERROR_CHECK(temperature_sensor_install(&tempSensor, &tempHandle));
  ESP_ERROR_CHECK(temperature_sensor_enable(tempHandle));

  // Configure input pins for GND-driven buttons
  for (uint8_t pin : buttonsGndpins)
  {
    // WARNING: Do NOT include LED_BUILTIN in this vector to avoid LED malfunction.
    pinMode(pin, INPUT_PULLUP);
  }

  // Configure input pins for VCC-driven buttons
  for (uint8_t pin : buttonsVCCpins)
  {
    // WARNING: Do NOT include LED_BUILTIN in this vector to avoid LED malfunction.
    pinMode(pin, INPUT_PULLDOWN);
  }

  // Configure row pins as outputs for button matrix scanning
  for (uint8_t pin : buttonsRowpins)
  {
    // WARNING: Do NOT include LED_BUILTIN in this vector to avoid LED malfunction.
    pinMode(pin, OUTPUT);
  }

  // Configure column pins as inputs with pull-up for button matrix
  for (uint8_t pin : buttonsColpins)
  {
    // WARNING: Do NOT include LED_BUILTIN in this vector to avoid LED malfunction.
    pinMode(pin, INPUT_PULLUP);
  }

  lastActivityMillis = millis();
}

void checkButtonActivity()
{
  static uint8_t lastButtonState[sizeof(espNow.slaveSendData.button_data)] = {0};

  if (memcmp(lastButtonState, espNow.slaveSendData.button_data, sizeof(lastButtonState)))
  {
    // Button state changed - update last activity time
    lastActivityMillis = millis();
    memcpy(lastButtonState, espNow.slaveSendData.button_data, sizeof(lastButtonState));

    if (lowPowerMode)
    {
      // Exiting low power mode
      lowPowerMode = false;
      Serial.println("Exiting low power mode");
    }
  }
}

void loop()
{
  // Check if we should enter low power mode
  if (!lowPowerMode && (millis() - lastActivityMillis > INACTIVITY_TIMEOUT))
  {
    lowPowerMode = true;
    enterLowPowerMode();
    return; // This line won't be reached if deep sleep is entered
  }

  // ============ Temperature Reading ============
  // Read the internal temperature sensor in Celsius
  // Only read temperature if not in low power mode
  if (!lowPowerMode)
    ESP_ERROR_CHECK(temperature_sensor_get_celsius(tempHandle, &tempOut));

  // ============ Prepare Button Data ============
  // Clear previous button states
  memset(espNow.slaveSendData.button_data, 0, sizeof(espNow.slaveSendData.button_data));

  uint16_t totalBits = 0;

  // Lambda function to encode button states into bit-packed array
  auto writeBits = [&](const std::vector<uint8_t> &pins, bool invert)
  {
    for (uint8_t pin : pins)
    {
      if (totalBits >= sizeof(espNow.slaveSendData.button_data) * 8)
        return; // Prevent overflow

      uint8_t byteIndex = totalBits / 8;
      uint8_t bitIndex = totalBits % 8;
      bool state = invert ? !digitalRead(pin) : digitalRead(pin);
      bitWrite(espNow.slaveSendData.button_data[byteIndex], bitIndex, state);
      totalBits++;
    }
  };

  // Read TOUCH sensor
  for (uint8_t pin : buttonsTouchpins)
  {
    bool touched = (touchRead(pin) > touchThreshold);
    uint8_t byteIndex = totalBits / 8;
    uint8_t bitIndex = totalBits % 8;
    bitWrite(espNow.slaveSendData.button_data[byteIndex], bitIndex, touched);
    totalBits++;
  }

  // // Read TOUCH sensor
  // for (uint8_t pin : buttonsTouchpins)
  // {
  //   bool touched = (touchRead(pin) > touchThreshold);
  //   if (touched)
  //   {
  //     uint8_t byteIndex = totalBits / 8;
  //     uint8_t bitIndex = totalBits % 8;
  //     bitWrite(espNow.slaveSendData.button_data[byteIndex], bitIndex, 1);
  //     totalBits++;
  //   }
  //   else
  //   {
  //     // Optional: Explicitly set bit to 0 if needed
  //     uint8_t byteIndex = totalBits / 8;
  //     uint8_t bitIndex = totalBits % 8;
  //     bitWrite(espNow.slaveSendData.button_data[byteIndex], bitIndex, 0);
  //     totalBits++;
  //   }
  // }

  // Read GND-referenced buttons (active-low)
  writeBits(buttonsGndpins, true);

  // Read VCC-referenced buttons (active-high)
  writeBits(buttonsVCCpins, false);

  // Matrix scan: iterate through row pins and read columns
  for (uint8_t rowPin : buttonsRowpins)
  {
    digitalWrite(rowPin, LOW);       // Enable current row
    writeBits(buttonsColpins, true); // Read columns (active-low)
    digitalWrite(rowPin, HIGH);      // Disable row again

    if (totalBits >= sizeof(espNow.slaveSendData.button_data) * 8)
      break; // Stop if buffer is full
  }

  // Check for button activity
  checkButtonActivity();

  // ============ ESP-NOW Transmission ============
  // Send updated button data if changed
  espNow.update();

  // ============ Status LED Behavior ============
  // LED_BUILTIN usage:
  // - ON  : Successfully connected to master (peer list size ≥ 2)
  // - BLINK FAST : Over-temperature warning (≥ 80°C)
  // - OFF : Not connected

  if (espNow.peers.size() >= 2)
  {
    if (tempOut >= 80.0)
    {
      // Critical temperature (blink every 50ms)
      if (millis() - ledMillis > 50)
      {
        ledMillis = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      }
    }
    else if (tempOut >= 70.0)
    {
      // Over-temperature warning (blink every 100ms)
      if (millis() - ledMillis > 100)
      {
        ledMillis = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      }
    }
    else
    {
      digitalWrite(LED_BUILTIN, HIGH); // Connected and safe
    }
  }
  else
  {
    // Blink slowly when searching for master (e.g., 500ms interval)
    if (millis() - ledMillis > 500)
    {
      ledMillis = millis();
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
  }

  // Example processing data from master
  if (memcmp(&espNow.masterCmdData, &espNow.lastmasterCmdData, sizeof(espNow.masterCmdData)) != 0)
  { // new data

    Serial.printf("Main Id: %d | Sub Id: %d | Index1: %d | Index2: %d | Float: %f | Int: %d \n",
                  espNow.masterCmdData.mainId, espNow.masterCmdData.subId, espNow.masterCmdData.index1, espNow.masterCmdData.index2, espNow.masterCmdData.value, espNow.masterCmdData.valueInt);

    memcpy(&espNow.lastmasterCmdData, &espNow.masterCmdData, sizeof(espNow.masterCmdData));

    // Example action based on master data
    // digitalWrite(LED_PIN, espNow.espnowRecvData.button_data[0]);

    // Reset received data after processing
    // memset(&espNow.espnowRecvData, 0, sizeof(espNow.espnowRecvData));
  }

  // ============ Debugging Output (Serial Monitor) ============
  // Print temperature and button bit data every second
  static unsigned long debugMillis = 0;
  if (millis() - debugMillis >= 1000)
  {
    debugMillis = millis();

    // Touch sensor debugging
    // for (uint8_t pin : buttonsTouchpins)
    // {
    //   Serial.printf("Touch pin %d: %d\n", pin, touchRead(pin));
    // }

    Serial.printf("Temp: %.2f C | Peers: %d\n", tempOut, espNow.peers.size());
    Serial.print("Button bits: ");
    for (int i = 0; i < sizeof(espNow.slaveSendData.button_data); i++)
    {
      Serial.printf("%02X ", espNow.slaveSendData.button_data[i]);
    }
    Serial.println();
  }
}
