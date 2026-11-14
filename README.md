# DIY-Macro-Keyboard

An open-source, customizable macro keyboard and Stream Deck alternative built from scratch for around €30. Powered by a Raspberry Pi Pico, it features 12 mechanical macro keys, a dedicated page navigation button, 5 OLED displays for dynamic key labels, and a standalone HTML web configurator that stores configuration files on the internal MicroSD card of the keyboard.

Watch the full project video on YouTube: *Link coming soon*


## Description

Commercial macro controllers like the Stream Deck are great productivity tools, but they come at a high price. This project replicates and expands upon their core functionality at a fraction of the cost, using mechanical keyboard switches and a multi-display layout.

Instead of hardcoding shortcuts into the firmware, every key, label, and page layout is configured through an easy to use browser-based graphical interface and saved onto a MicroSD card.

### Key Features
- **12 + 1 Key Layout:** 12 mechanical macro keys arranged in a 4x3 grid, paired with 1 dedicated page navigation key.
- **Dynamic OLED Labels:** 4 OLED displays mounted above the key columns show the real-time function of each key, while a 5th dedicated OLED displays the active page name.
- **Unlimited & Nested Pages:** Organize commands into multiple application-specific pages (e.g., macOS, DaVinci Resolve, Fusion) and create sub-pages nested inside other pages.
- **Versatile Key Actions:** Assign keyboard shortcuts, multimedia controls (e.g., volume up/down), MIDI commands, automated text strings, or internal system actions (such as standby mode or jumping to a specific page).
- **Delayed Secondary Actions:** Configure any key to trigger a second action automatically after a custom time delay.
- **No-Code HTML Configurator:** Set up all keys, labels, delays, and pages via a local HTML graphical interface and export the configuration file directly to a MicroSD card.
- **Multi-Profile Support:** Store multiple configuration files on the MicroSD card and switch between them directly from the keyboard.
- **Intuitive Page Navigation:** Press the page key to cycle through pages, open a quick-select page menu, or hold it down to load a different configuration file from the MicroSD card.

### Hardware Overview
- **Microcontroller:** Raspberry Pi Pico (RP2040), chosen for its low cost, ample memory, and native USB support using the TinyUSB library to emulate a standard USB HID keyboard and MIDI device.
- **Displays:** 5 SSD1306 monochrome I2C OLED displays connected through a PCA9548A I2C multiplexer to overcome their shared fixed I2C address.
- **Storage:** SPI MicroSD card reader module for loading user configuration files.
- **Controls:** 13 mechanical keyboard switches wired with a common copper GND bus, fitted with green keycaps for the macro grid and a grey keycap for the page key.
- **Enclosure:** Two-part 3D-printed PLA enclosure designed in Fusion, consisting of a front panel and an angled base, along with custom internal 3D-printed brackets for mounting the Pico, SD reader, and multiplexer.


## Repository Contents

- **`/firmware`:** Source code for the Raspberry Pi Pico (built with TinyUSB) that reads configuration files from the MicroSD card, updates the OLED displays, and sends USB HID/MIDI commands.
- **`/3d-models`:** 3D-printable STL/CAD files for the front panel, angled base, internal Pico/SD card holder, and I2C multiplexer spacer.
- **`/configurator`:** Standalone HTML/CSS/JavaScript graphical interface for creating keymaps and exporting configuration files to the MicroSD card.
- **`/schematic`:** Diagram of the wiring between the Raspberry Pi Pico, keyboard switches and displays.



## Bill of Materials (BOM)

### Primary Components
- Raspberry Pi Pico
- 5x Monochrome SSD1306 I2C OLED Displays
- I2C Multiplexer Breakout Board PCA9548A
- SPI MicroSD Card Reader Module (5V)
- 13x Mechanical Keyboard Switches
- 13x Keycaps (12x Green, 1x Grey)
- MicroSD Card

### Hardware & Fasteners
- 3D-Printed Enclosure & Internal Mounts
- 5 M3 Heat-Set Brass Threaded Inserts
- 4 M3x14 mm Countersunk Screws
- 1 M3x8 mm Screw
- M3 Nut
- Solid Copper Wire (for common GND rail) and Hookup Wire


## License

This project is licensed under the MIT License - feel free to build, modify, and adapt it for your personal or community projects.
