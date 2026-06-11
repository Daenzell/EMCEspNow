# EMCFFBV2 Slave Firmware

ESP‑NOW based slave device for the EMCFFBV2 firmware.  
Reads multiple button inputs (touch, GND‑referenced, VCC‑referenced, matrix), internal temperature, and sends the button states to a master device via ESP‑NOW.  
Includes debouncing, inactivity timer, deep sleep, and a deterministic task scheduler (1 kHz sampling, 100 Hz ESP‑NOW updates, 50 Hz debug output).

---

## Features

- **ESP‑NOW communication** (slave mode)  
- **Debounced button inputs**  
  - Touch sensors (4 pins)  
  - Digital buttons to GND (active low, internal pull‑up)  
  - Digital buttons to VCC (active high, internal pull‑down)  
  - Matrix keypad (rows driven, columns read)  
- **Internal temperature sensor** (ESP32‑S2)  
- **LED status indication**  
  - Solid ON – connected to master, normal temperature  
  - Blinking fast (50 ms) – overtemperature ≥80 °C  
  - Blinking (100 ms) – overtemperature ≥70 °C  
  - Slow blink (500 ms) – no master connection  
- **Inactivity timeout & deep sleep** (30 seconds, configurable)  
  - Wake‑up sources: touch pins, digital buttons  
- **Deterministic FreeRTOS tasks** driven by a GPTimer (10 kHz base)  
  - 1 kHz: read all inputs & update debounce states  
  - 100 Hz: pack data, run ESP‑NOW update, handle LED and commands  
  - 50 Hz: debug serial output  

---

## Hardware Requirements

- ESP32‑S2 (tested) or any ESP32 with touch sensor support  
- Buttons / keypad connected according to the pin mapping below  
- Optional: temperature sensor (internal)  

### Pin Mapping (Default)

| Type                | Pins                     |
|---------------------|--------------------------|
| Touch sensors       | 1, 2, 3, 4               |
| GND‑referenced      | 5, 6, 7, 8               |
| VCC‑referenced      | 9, 10, 11, 12            |
| Matrix columns      | 13, 14, 16, 17 (pull‑up) |
| Matrix rows         | 18, 21, 33, 34 (output)  |
| Built‑in LED        | `LED_BUILTIN`            |

*You can change the pin vectors in `main.cpp`.*

---

## Software Dependencies

- [Espressif IoT Development Framework (ESP‑IDF)](https://github.com/espressif/esp-idf) or Arduino‑ESP32 (tested with PlatformIO)  
- Custom library `EmcEspNow.h` / `EmcEspNow.cpp` (provided separately)  

The code uses standard Arduino functions (`pinMode`, `digitalWrite`, `touchRead`, `millis`) and ESP‑IDF components (`driver/temperature_sensor`, `driver/gptimer`, `esp_sleep`).

---

## How to Use

1. **Prepare the Master Device**  
   The slave communicates with a master running the **EMCFFBV2** firmware.  
   - Download the latest firmware for ESP32‑S2/S3 from the [official releases page](https://github.com/ebolzMagy/EMCFFBV2/releases).  
   - Flash the master device using the utility provided in the release `.zip` file.

2. **Configure the Slave Firmware**  
   Open `EmcEspNow.h` and adjust the following parameters to match your hardware and the master’s network settings:  
   - **Wi-Fi Channel:** `ESPNOW_WIFI_CHANNEL` – must be the same on both devices (default `6`).  
   - **Touch threshold:** `touchThreshold` (default `8000`).  
   - **Button pins:** Change `buttonsTouchpins`, `buttonsGndpins`, etc., according to your wiring.  
   - **Inactivity timeout:** `INACTIVITY_TIMEOUT` (milliseconds, default `30000`).  
   - **Disable deep sleep:** Comment out the inactivity check inside `task100Hz()` if needed.

3. **Upload the Firmware** to your ESP32‑S2 board.

4. **Monitor Serial Output** (baud rate `115200`) – you will see temperature, connection status, and button bits every second.

5. **Observe the LED**  
   - Slow blink (500 ms) → scanning for master.  
   - Solid ON → connected, normal temperature.  
   - Blinking (100 ms) → over‑temperature warning (≥70 °C).  
   - Fast blink (50 ms) → critical temperature (≥80 °C).

6. **Inactivity & Deep Sleep**  
   - If no button state change occurs for `INACTIVITY_TIMEOUT` (default 30 seconds), the device enters deep sleep.  
   - Press any configured touch or digital button to wake it up.

---

## Code Structure

- `setup()`: initialises pins, sensors, ESP‑NOW, debounce states, GPTimer, and FreeRTOS tasks.  
- `loop()`: empty – all work is done in tasks.  

### Tasks

| Task      | Frequency | Responsibility                                           |
|-----------|-----------|----------------------------------------------------------|
| `task1kHz`| 1 kHz     | Read touch, GND, VCC, and matrix pins; update debounce. |
| `task100Hz`| 100 Hz    | Pack stable button states, call `espNow.update()`, handle LED and master commands. |
| `task50Hz` | 50 Hz     | Print debug info once per second.                       |

### Debounce

- Each button has a `ButtonState` struct with a counter.  
- `DEBOUNCE_THRESHOLD = 5` → state changes only after 5 consecutive identical readings (5 ms at 1 kHz).  

### Deep Sleep

- `checkButtonActivity()` compares the current button data with the previous one.  
- If no change for `INACTIVITY_TIMEOUT`, `enterLowPowerMode()` disables sensor, ends ESP‑NOW, and starts deep sleep.  
- Wake‑up sources: all touch pins (via `touchAttachInterrupt`) and all direct digital buttons (EXT0 wake‑up).

---

## Customisation

- **Change debounce time** – modify `DEBOUNCE_THRESHOLD` (value = milliseconds at 1 kHz sampling).  
- **Add more buttons** – enlarge the button state vectors and update the packing order in `task100Hz`.  
- **Disable deep sleep** – comment out the inactivity check inside `task100Hz`.  
- **Change Wi‑Fi channel** – edit `ESPNOW_WIFI_CHANNEL` in `EmcEspNow.h`.

---

## Troubleshooting

| Symptom                      | Possible cause & fix                                 |
|------------------------------|-------------------------------------------------------|
| No connection to master      | Check channel, broadcast MAC, and that master is running. |
| Button states not updating   | Verify pin mappings and wiring (pull‑up/pull‑down).   |
| Device does not wake from sleep | Ensure touch or digital wake‑up sources are correctly configured in `prepareWakeupSources()`. |
| LED stays off                | Check `LED_BUILTIN` definition (usually GPIO 15 on S2). |

---

## License

---

## Credits

- Original ESP‑NOW wrapper: `EmcEspNow` by daenzell  
- Slave firmware modifications by ebolzMagy
