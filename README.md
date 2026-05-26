# 🌟 GlowBar: The Sound-Reactive LED Visualizer

**GlowBar** is a custom-engineered, handheld (or desktop) audio-reactive LED visualizer. Designed to be the ultimate maker-gift, it listens to the ambient audio in the room and translates it into buttery-smooth, mathematically complex visualizations on a retro-style dot matrix screen.

It features a one-button touch interface, 11 unique animations, a built-in battery, and completely wireless Over-The-Air (OTA) firmware updates.

[![GlowBar Showcase](https://img.youtube.com/vi/lLxH43aJjio/0.jpg)](https://youtu.be/lLxH43aJjio)
*(Click the image above to watch the full video breakdown, teardown, and flashing guide)*

---

## ✨ Features
* **11 Unique Visualizations:** Ranging from classic EQs and 3D wireframe terrains, to Conway's Game of Life and warp-speed starfields.
* **Smart Audio Processing:** Built with Fast Fourier Transform (FFT) and an Automatic Gain Control (AGC) algorithm to dynamically adjust to the volume of any room.
* **Invisible Touch Interface:** Entirely controlled through a capacitive touch sensor built into the casing.
* **Auto-Hotspot & Wi-Fi:** Automatically spins up a configuration hotspot if it can't find your router.
* **Battery Saver Logic:** The Wi-Fi chip automatically shuts down after 2 minutes to preserve battery life, leaving only the core graphics and audio engines running.
* **NFC Enabled:** Tap your phone to the top of the case to instantly open this GitHub repository!

---

## 🕹️ Controls & Navigation
The GlowBar relies on a single, intuitive touch sensor located on the side.

* **1 Tap:** Wake up from deep sleep (resumes the last used mode) OR cycle to the next visualizer mode.
* **Press & Hold (~1s):** Put the device into deep sleep / battery saving mode.
* **5 Taps:** Enter the **Settings Menu**.

### ⚙️ Inside the Settings Menu:
* **Press & Hold:** Ramps the brightness up or down (0 to 9).
* **1 Tap:** Reverses the dimming direction (if it was going up, it will now go down).
* **2 Taps:** Physically flips the screen 180° (perfect for upside-down mounting).
* *(After a few seconds of inactivity, the device will automatically save your settings and exit).*

---

## 🔌 Hardware Layout & Anatomy

**On the Case:**
* **Side:** Main ON/OFF power switch, Type-C charging port *(Safe to leave plugged in 24/7)*, and the primary Touch Interaction Button.
* **Back:** Wi-Fi control button, Hardware Reset button, a concealed Type-C port (for wired firmware flashing), and the I2S Microphone port.
* **Top:** NFC tag zone. 

**Under the Hood (Internals):**
* **Brain:** ESP32-C3 Super Mini microcontroller.
* **Display:** 4x MAX7219 LED Matrix modules (FC-16, 32x8 total).
* **Audio Input:** High-fidelity I2S digital microphone.
* **Power:** Rechargeable Li-Po battery.

---

## 🛜 Network & Wi-Fi Setup
Wi-Fi is *not* required for the audio visualizer to function. It is only used to push wireless firmware updates.

1. **Initial Boot:** If the device has never been connected to your Wi-Fi, the internal blue LED will blink, and it will broadcast its own network called **`GlowBar-Setup`**.
2. **Connect:** Connect to `GlowBar-Setup` using your phone or PC.
3. **Configure:** A captive portal (WiFiManager) will automatically pop up. Select your home Wi-Fi network and enter the password. 
4. **Reboot:** The device will reboot. The blue LED will turn solid, indicating it is successfully connected to your local network.

*Note: Every time you power the device on, the Wi-Fi stays awake for 2 minutes (indicated by a brief LED blink every second). After 2 minutes, Wi-Fi shuts off. You can force the Wi-Fi to stay on permanently by pressing and holding the Wi-Fi button on the back.*

---

## 🛠️ Flashing Custom Firmware (OTA)
Because the device supports Over-The-Air (OTA) updates, you never have to open the case or plug in a data cable to change the code. 

There are 3 ways to push an update:
1. **The Browser Method (Easiest):** Compile your code into a `firmware.bin` file. Ensure the GlowBar's Wi-Fi is awake, open a web browser, and navigate to `http://glowbar.local/update`. Drag and drop your file into the ElegantOTA interface!
2. **PlatformIO / VS Code:** Use the direct upload arrow in VS Code (you may need to specify the device's exact IP address in the `platformio.ini` if Windows struggles with `.local` hostnames).
3. **Arduino IDE:** The GlowBar will automatically show up as a discoverable network port in your Arduino IDE for direct flashing.

---

## 🔧 Teardown & Repair Guide
If you need to swap the battery or harvest the parts, the GlowBar is built to be modular and easy to open. **[Watch the Teardown Video Here](https://youtu.be/lLxH43aJjio?t=305)**.

1. **Remove the Matrices:** Using a small screwdriver or a flat butter knife, gently pry up the 4 LED matrix panels from side to side. *Be very careful not to bend the connecting pins!*
2. **Access the Base:** Once the LED caps are removed, you have full access to the display base, ESP32-C3 (screwed in), touch sensor, microphone, and battery.
3. **Swap the Battery:** Gently pry the battery out. **Crucial:** Always pry from the *right* side to avoid shorting the positive and negative terminals!
4. **Reassembly:** Ensure no internal wires are resting on the display support pegs. Slide the LED matrices back into place one by one (ensuring the printed text on the side of the modules faces the correct way), and press them firmly into their sockets. 

---

## 💡 Repurposing Ideas
Getting bored of the visualizer? The GlowBar is ultimately just a powerful ESP32-C3 attached to a microphone and a matrix screen! You can easily wipe the firmware and repurpose the hardware into:
* A Wi-Fi connected desktop ticker (crypto prices, weather, YouTube subscriber count).
* A Pomodoro productivity timer.
* A smart-home dashboard / MQTT notification display for Home Assistant.
* A standalone digital clock/calendar.
