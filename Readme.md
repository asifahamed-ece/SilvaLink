# 🌲 SilvaLink

**2nd Prize Winner (Hardware Category) - Makeathon 7.0 @ Sri Venkateswara College of Engineering (SVCE)**

SilvaLink is a decentralized, offline-first communication system designed to resolve the communication blackout issue in remote forest reserves. By seamlessly bridging an offline-first mobile application with a custom-engineered hardware mesh network, we turn any ordinary smartphone into a powerful SOS beacon.

---

## ⚠️ The Problem
Remote forests and trekking zones suffer from a severe lack of reliable communication due to poor cellular coverage and strict restrictions on traditional radio systems. This creates dangerous blind spots, making it incredibly difficult to coordinate rescues and ensure public safety during emergencies like missing persons or medical crises. Current offline solutions are prohibitively expensive, infrastructure-dependent, and not user-friendly for the general public.

## 💡 Our Solution
SilvaLink provides a vital off-grid communication lifeline with minimal environmental impact. We utilize a hardware mesh network of ESP32 and LoRa-powered stationary nodes to transmit critical alerts without needing a single cell tower or internet connection.

### 🔥 Key Features
* **Zero-Network Smartphone Integration:** Establishes a local link with the stationary node via BLE or Local Wi-Fi.
* **Ultra-Compressed SOS Payloads:** Intercepts output and compresses it into a 3-byte hex payload, reducing Time-on-Air (ToA) to under 60 milliseconds and extending battery life.
* **Forward Error Correction (FEC):** Implements a lightweight Hamming Code algorithm to locally correct single-bit errors flipped by forest interference, preventing costly re-transmissions.
* **Adaptive Signal Penetration (Dynamic ADR):** Nodes automatically monitor neighbor RSSI and shift LoRa spreading factors (e.g., from SF7 to SF12) to trade speed for maximum physical penetration power through thick brush.

---

## 🛠️ Hardware Architecture

Each stationary mesh node is designed as a highly scalable edge unit. 

* **Microcontroller:** ESP32 Development Board
* **Transceiver:** LoRa SX1278 (433MHz Sub-GHz)
* **Environmental Sensor:** BME280 (Temperature, Humidity, Pressure)
* **Power Supply:** 1x 18650 Li-ion Battery (2500mAh) with TP4056 Module & Boost Converter
* **Energy Harvesting:** 5V Mini Solar Panel (Trickle Charger)

*Note: The 433MHz band is specifically chosen as its longer wavelength (~69 cm) allows radio waves to diffract around tree trunks, improving the link budget by up to 30-40 dB over a 1km stretch.*

---

## 🚀 How It Works (Data Flow)

1. **Local Sync:** The trekker's smartphone pairs with a local stationary node via BLE/Wi-Fi.
2. **Triggering SOS:** The user presses the emergency button in the SilvaLink Flutter app.
3. **Data Handoff:** The payload is compressed and transmitted locally to the ESP32 Node.
4. **Mesh Hopping:** The node encrypts and broadcasts the 3-byte payload via long-range LoRa (node-to-node).
5. **Dashboard Alert:** The basecamp admin node receives the message, identifies the origin node ID, and pinpoints the location for rescue.

---

## 💻 Firmware Setup & Installation

This project's firmware is built for the ESP32. 

1. Clone this repository:
   ```bash
   git clone [https://github.com/asifahamed-ece/SilvaLink.git](https://github.com/asifahamed-ece/SilvaLink.git)
