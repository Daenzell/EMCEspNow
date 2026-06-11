/*
 * main.cpp - ESP-NOW Slave Firmware for EMCFFBV2
 * Author: Daenzell
 * Modified by: [ebolzMagy]
 */

#include "EmcEspNow.h"
#include "driver/temperature_sensor.h"
#include "driver/gptimer.h"
#include "esp_sleep.h"
#include <vector>

// === SYSTEM LOOP CONFIGURATION ===
// Base scheduler = 10kHz
#define LOOP_BASE_HZ 10000

// Task divider
#define LOOP_1KHZ 10
#define LOOP_100HZ 100
#define LOOP_50HZ 200

// === TIMERS ===
gptimer_handle_t loopTimer = NULL;
// global tick count (increments every 100µs)
volatile uint64_t loopTicks = 0;

// === TASK HANDLERS ===
TaskHandle_t task1kHzHandle = NULL;
TaskHandle_t task100HzHandle = NULL;
TaskHandle_t task50HzHandle = NULL;

// === FUNCTION PROTOTYPES ===
void task1kHz(void *pvParameters);
void task100Hz(void *pvParameters);
void task50Hz(void *pvParameters);

// === TIMER ISR ===
static bool IRAM_ATTR onLoopTimer(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
  BaseType_t high_task_awoken = pdFALSE;

  loopTicks = loopTicks + 1; // Increment global tick count

  // 1kHz task (every 10 ticks)
  if (loopTicks % LOOP_1KHZ == 0 && task1kHzHandle != NULL)
  {
    vTaskNotifyGiveFromISR(task1kHzHandle, &high_task_awoken);
  }

  // 100Hz task (every 100 ticks)
  if (loopTicks % LOOP_100HZ == 0 && task100HzHandle != NULL)
  {
    vTaskNotifyGiveFromISR(task100HzHandle, &high_task_awoken);
  }

  // 50Hz task (every 200 ticks)
  if (loopTicks % LOOP_50HZ == 0 && task50HzHandle != NULL)
  {
    vTaskNotifyGiveFromISR(task50HzHandle, &high_task_awoken);
  }

  return (high_task_awoken == pdTRUE); // Return true if a higher priority task was awoken
}

// Instance of the ESP-NOW communication handler (slave mode)
EmcEspNow espNow;

// Temperature sensor handle and configuration
temperature_sensor_handle_t tempHandle = NULL;
temperature_sensor_config_t tempSensor = {
    .range_min = 20,
    .range_max = 50};

// Touch Sensor Threshold (tested on ESP32-S2)
const uint16_t touchThreshold = 8000;

// Timing variables
unsigned long slaveMillis = 0;
unsigned long ledMillis = 0;
unsigned long lastActivityMillis = 0;

// Temperature reading
float tempOut = 0.0f;

// Power management (optional)
const unsigned long INACTIVITY_TIMEOUT = 30000;
bool lowPowerMode = false;

// === Button Pin Configuration Vectors ===
// IMPORTANT: Do NOT include LED_BUILTIN in any of these vectors.
// It is reserved for status indication and must not be repurposed as an input or output pin.

std::vector<uint8_t> buttonsTouchpins = {1, 2, 3, 4};
std::vector<uint8_t> buttonsGndpins = {5, 6, 7, 8};
std::vector<uint8_t> buttonsVCCpins = {9, 10, 11, 12};
std::vector<uint8_t> buttonsColpins = {13, 14, 16, 17};
std::vector<uint8_t> buttonsRowpins = {18, 21, 33, 34};

// === Debounce Structure ===
struct ButtonState
{
  bool rawState;           // Raw read from pin/touch
  bool stableState;        // Debounced state
  bool lastStableState;    // Previous stable state (for edge detection)
  uint8_t debounceCounter; // Counter for debounce
  bool pressed;            // True for one cycle on press
  bool released;           // True for one cycle on release
};

// Vectors to store the state of each pin
std::vector<ButtonState> touchStates;
std::vector<ButtonState> gndStates;
std::vector<ButtonState> vccStates;
std::vector<ButtonState> colStates;

// Debounce threshold (how many times the reading should stabilize)
const uint8_t DEBOUNCE_THRESHOLD = 5;

// Debounce update function
void updateButtonState(ButtonState &state, bool raw)
{
  state.rawState = raw;
  if (state.rawState == state.stableState)
  {
    // Same as stable, reset counter
    state.debounceCounter = 0;
  }
  else
  {
    // Different, increment counter
    if (state.debounceCounter < DEBOUNCE_THRESHOLD)
    {
      state.debounceCounter++;
    }
    if (state.debounceCounter >= DEBOUNCE_THRESHOLD)
    {
      // Change stable state
      state.lastStableState = state.stableState;
      state.stableState = state.rawState;
      // Detect edges
      state.pressed = (state.stableState == true && state.lastStableState == false);
      state.released = (state.stableState == false && state.lastStableState == true);
      state.debounceCounter = 0;
    }
  }
}

// Function to initialize vector states
void initButtonStates()
{
  // Touch
  touchStates.resize(buttonsTouchpins.size());
  for (auto &s : touchStates)
  {
    s = ButtonState{false, false, false, 0, false, false};
  }
  // GND
  gndStates.resize(buttonsGndpins.size());
  for (auto &s : gndStates)
  {
    s = ButtonState{false, false, false, 0, false, false};
  }
  // VCC
  vccStates.resize(buttonsVCCpins.size());
  for (auto &s : vccStates)
  {
    s = ButtonState{false, false, false, 0, false, false};
  }
  // Columns (matrix)
  colStates.resize(buttonsColpins.size());
  for (auto &s : colStates)
  {
    s = ButtonState{false, false, false, 0, false, false};
  }
}

// Optional: Function to prepare wakeup sources for low power mode
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
  digitalWrite(LED_BUILTIN, LOW);
  prepareWakeupSources();

  // Disable temperature sensor to save power
  if (tempHandle)
    temperature_sensor_disable(tempHandle);

  // Disable WiFi and ESP-NOW
  espNow.end();

  // Configure deep sleep
  esp_deep_sleep_start();
}

// Function to check for button activity and reset inactivity timer
void checkButtonActivity()
{
  static uint8_t lastButtonState[sizeof(espNow.slaveSendData.button_data)] = {0};
  if (memcmp(lastButtonState, espNow.slaveSendData.button_data, sizeof(lastButtonState)))
  {
    lastActivityMillis = millis();
    memcpy(lastButtonState, espNow.slaveSendData.button_data, sizeof(lastButtonState));
    if (lowPowerMode)
    {
      lowPowerMode = false;
      Serial.println("Exiting low power mode");
    }
  }
}

// Global buffer and mutex for button data (if needed for cross-task access)
uint8_t debouncedBits[sizeof(slave_data_t::button_data)] = {0};
SemaphoreHandle_t dataMutex = NULL;

void setup()
{
  Serial.begin(115200);

  // Check wakeup reason
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  if (wakeupReason != ESP_SLEEP_WAKEUP_UNDEFINED)
  {
    // Woke from sleep - quick init
    lowPowerMode = false;
    lastActivityMillis = millis();
  }

  // Normal boot
  pinMode(LED_BUILTIN, OUTPUT);

  // Initialize ESP-NOW in SLAVE mode
  espNow.begin(false); // false = Slave

  // Initialize temperature sensor
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

  // Initialize debounce states
  initButtonStates();

  lastActivityMillis = millis();

  // GPTIMER Config
  gptimer_config_t timerConfig = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,
  };

  ESP_ERROR_CHECK(gptimer_new_timer(&timerConfig, &loopTimer));

  gptimer_alarm_config_t alarmConfig = {};

  alarmConfig.reload_count = 0;                  // No reload, we will manually reset the timer in the ISR
  alarmConfig.alarm_count = 100;                 // 100µs for 10kHz
  alarmConfig.flags.auto_reload_on_alarm = true; // Auto-reload for periodic interrupts

  ESP_ERROR_CHECK(gptimer_set_alarm_action(loopTimer, &alarmConfig));
  gptimer_event_callbacks_t cbs = {
      .on_alarm = onLoopTimer,
  };
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(loopTimer, &cbs, NULL));
  ESP_ERROR_CHECK(gptimer_enable(loopTimer));

  // Create Tasks
  xTaskCreatePinnedToCore(task1kHz, "Task1kHz", 4096, NULL, 4, &task1kHzHandle, 0);
  xTaskCreatePinnedToCore(task100Hz, "Task100Hz", 4096, NULL, 3, &task100HzHandle, 0);
  xTaskCreatePinnedToCore(task50Hz, "Task50Hz", 4096, NULL, 2, &task50HzHandle, 0);

  // Button mutex
  dataMutex = xSemaphoreCreateMutex();
  // === Start Timer ===
  ESP_ERROR_CHECK(gptimer_start(loopTimer));
}

void loop()
{
  // The main loop is empty because all work is done in tasks and timer ISR
  // You can put low-priority background tasks here if needed, but avoid blocking calls
  vTaskDelay(pdMS_TO_TICKS(1000)); // Sleep to reduce CPU usage, tasks will handle the work
}

// =========== 1kHz Task: Read Inputs, Update States ============

void task1kHz(void *pvParameters)
{
  while (true)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Wait for notification from timer ISR
                                             // 1kHz tasks can be placed here if needed

    // === Update all button states with debounce ===

    // Touch buttons (raw = touched > threshold)
    for (size_t i = 0; i < buttonsTouchpins.size(); i++)
    {
      bool touched = (touchRead(buttonsTouchpins[i]) > touchThreshold);
      updateButtonState(touchStates[i], touched);
    }

    // GND buttons (active low, so raw = !digitalRead)
    for (size_t i = 0; i < buttonsGndpins.size(); i++)
    {
      bool raw = !digitalRead(buttonsGndpins[i]);
      updateButtonState(gndStates[i], raw);
    }

    // VCC buttons (active high)
    for (size_t i = 0; i < buttonsVCCpins.size(); i++)
    {
      bool raw = digitalRead(buttonsVCCpins[i]);
      updateButtonState(vccStates[i], raw);
    }

    // Matrix scan with debounce
    for (uint8_t rowPin : buttonsRowpins)
    {
      digitalWrite(rowPin, LOW);
      for (size_t colIdx = 0; colIdx < buttonsColpins.size(); colIdx++)
      {
        bool raw = !digitalRead(buttonsColpins[colIdx]);
        updateButtonState(colStates[colIdx], raw);
      }
      digitalWrite(rowPin, HIGH);
    }
  }
}

// =========== 100Hz Task: Handle ESP-NOW Communication, Timeouts, Temperature etc. ============
void task100Hz(void *pvParameters)
{
  while (true)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Wait for notification from timer ISR
                                             // 100Hz tasks can be placed here if needed

    // Check if we should enter low power mode (optional, can be commented out if not needed)
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
    {
      ESP_ERROR_CHECK(temperature_sensor_get_celsius(tempHandle, &tempOut));
    }

    // ============ Prepare Button Data for ESP-NOW ============
    uint8_t tempBuffer[sizeof(debouncedBits)];
    memset(tempBuffer, 0, sizeof(tempBuffer));
    uint16_t totalBits = 0;
    auto writeBit = [&](bool bit)
    {
      if (totalBits >= sizeof(tempBuffer) * 8)
        return;
      uint8_t byteIdx = totalBits / 8;
      uint8_t bitIdx = totalBits % 8;
      bitWrite(tempBuffer[byteIdx], bitIdx, bit);
      totalBits++;
    };
    for (auto &s : touchStates)
      writeBit(s.stableState);
    for (auto &s : gndStates)
      writeBit(s.stableState);
    for (auto &s : vccStates)
      writeBit(s.stableState);
    for (auto &s : colStates)
      writeBit(s.stableState);

    // Copy debounced bits to global buffer (if needed for other tasks)
    if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
    memcpy(espNow.slaveSendData.button_data, tempBuffer, sizeof(espNow.slaveSendData.button_data));
    if (dataMutex) xSemaphoreGive(dataMutex);

    // Check for button activity to reset inactivity timer
    checkButtonActivity();

    // ============ ESP-NOW Transmission ============
    // Send updated button data if changed
    espNow.update(); // This will handle sending data to master if connected and also check for timeouts

    // ============ Status LED Behavior ============
    // LED_BUILTIN usage:
    // - ON  : Successfully connected to master 
    // - BLINK FAST : Over-temperature warning (≥ 80°C)
    // - OFF : Not connected or normal temperature

    if (espNow.isMasterConnected())
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
        digitalWrite(LED_BUILTIN, HIGH);
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

    // === Process commands from master (if any) ===
    if (memcmp(&espNow.masterCmdData, &espNow.lastmasterCmdData, sizeof(espNow.masterCmdData)) != 0)
    {
      Serial.printf("Main Id: %d | Sub Id: %d | Index1: %d | Index2: %d | Float: %f | Int: %d\n",
                    espNow.masterCmdData.mainId, espNow.masterCmdData.subId,
                    espNow.masterCmdData.index1, espNow.masterCmdData.index2,
                    espNow.masterCmdData.value, espNow.masterCmdData.valueInt);
      memcpy(&espNow.lastmasterCmdData, &espNow.masterCmdData, sizeof(espNow.masterCmdData));

      // Handle commands here based on mainId, subId, etc.
    }
  }
}

// =========== 50Hz Task: Optional for Additional Processing ============
void task50Hz(void *pvParameters)
{
  while (true)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Wait for notification from timer ISR
                                             // 50Hz tasks can be placed here if needed

    // This task can be used for additional processing that doesn't need to run as fast as 100Hz
    // For example, you could add more complex command handling, logging, or other features here.

    // ============ Debugging Output (Serial Monitor) ============
    static unsigned long debugMillis = 0;
    if (millis() - debugMillis >= 1000)
    {
      debugMillis = millis();
      Serial.printf("Temp: %.2f C | Connected to master: %s | Wifi Channel: %d\n",
                    tempOut, espNow.isMasterConnected() ? "YES" : "NO", espNow.getWiFiChannel());
      Serial.print("Button bits: ");
      for (int i = 0; i < sizeof(espNow.slaveSendData.button_data); i++)
      {
        Serial.printf("%02X ", espNow.slaveSendData.button_data[i]);
      }
      Serial.println();
    }
  }
}