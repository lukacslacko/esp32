Build with `idf.py build`, flash with `idf.py flash`.

`sdkconfig` specifies hardware version < 3.0, since the device is hardware chip version 1.0.

For using the JoyIt BMP280, connect GND to GND; VCC, CSB, SDO to VCC; SDA to 7, SCL to 8.

## KY-023 Joystick Wiring

If you are using the KY-023 Joystick module with the Joystick test app, please wire it as follows:

* **+5V**: Connect to **3.3V** on the ESP32-P4. Do **not** connect to 5V to avoid damaging the GPIO pins!
* **GND**: Connect to **GND**
* **VRx**: Connect to **GPIO 20** (ADC1 Channel 4)
* **VRy**: Connect to **GPIO 21** (ADC1 Channel 5)
* **SW**: Connect to **GPIO 22** (configured with an internal pull-up in the software)

## Notes App
The ESP32-P4 Launchpad feature includes a "Quick Notes" app on the home menu!
* Tap `+ New Note` to create a new canvas.
* You can smoothly write across the screen using your finger to draw vectors.
* Press `Done` at the top right to save and view your note as a thumbnail.
* Tap any note thumbnail to reopen it and continue drawing your masterpiece.
* **Long press** any note thumbnail to bring up the delete dialog to toss it.