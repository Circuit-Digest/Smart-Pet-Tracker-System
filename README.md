# 🐾 Smart Pet Tracker System Using IoT



A real-time IoT-based pet tracking system built with the **GeoLinker GL868_ESP32** board. It uses GPS, GSM, geofencing, an E-paper display, and cloud connectivity to monitor your pet's location and send instant alerts when they leave a safe zone.



> **Disclaimer:** No animals were harmed during the development and testing of this project.


---



## 📖 Table of Contents



- [Overview](#overview)

- [How It Works](#how-it-works)

- [Components Required](#components-required)

- [Circuit Diagram](#circuit-diagram)

- [Hardware Assembly](#hardware-assembly)

- [3D Enclosure Design](#3d-enclosure-design)

- [Software Setup](#software-setup)

- [Code Explanation](#code-explanation)

- [Output & Demonstration](#output--demonstration)

- [Applications](#applications)

- [Troubleshooting](#troubleshooting)

- [FAQ](#faq)

- [Future Improvements](#future-improvements)

- [Related Projects](#related-projects)




---



## Overview



"Where did my pet go?" — If you have a pet, you've probably asked this question at least once. This project provides a practical solution by building a **smart pet tracking collar** that:



- 📍 Tracks your pet's real-time GPS location

- 🚧 Monitors multiple geofence zones (Home, Garden, Park, Restricted Area)

- 📱 Sends SMS alerts & voice calls when boundaries are crossed

- 📺 Displays pet status on an E-paper screen (retains info even without power)

- ☁️ Uploads location data to **CircuitDigest Cloud** for live map tracking



> **Note:** This is a proof-of-concept project intended for learning and experimentation. It should not be considered a replacement for commercially certified pet tracking products.



---



## How It Works



The system operates in three continuous stages:



### 1. System Initialization & Location Tracking

- On power-up, the GeoLinker board initializes **GPS**, **GSM/GPRS**, **E-paper display**, and **cloud services**.

- Previously saved geofence states are restored from flash memory to avoid false alerts after restart.

- The GPS module continuously acquires **latitude**, **longitude**, **speed**, and other location data.

- Current coordinates are compared against all predefined geofence zones using the **Haversine formula**.



### 2. Alert Generation & Safety Features

- **Zone Entry/Exit:** An SMS alert is sent with location, speed, battery %, and a Google Maps link.

- **Restricted Zone:** In addition to SMS, a **voice call** is placed to the caretaker for critical alerts.

- **GPS Loss:** If GPS signal is unavailable for multiple consecutive attempts, a GPS-loss alert is sent.

- **On-Demand Location:** The caretaker can send a `LOCATION` SMS command to receive the pet's latest coordinates.

- **E-paper Display:**

  - Normal: Shows pet name and healthy status.

  - Lost/Restricted Zone: Shows **"My Pet is Lost"** with the caretaker's phone number.

  - Retains content even when battery is fully drained.



### 3. Cloud Monitoring & Continuous Operation

- Periodically uploads GPS location, battery %, and signal strength to the **CircuitDigest Cloud**.

- Live map tracking via the cloud dashboard.

- Continuously checks for GPS updates, geofence events, incoming SMS commands, and display status.

- E-paper display refreshes only on status change to conserve power and extend display life.



---



## Components Required



| S.No | Component | Purpose | Alternatives |

|------|-----------|---------|-------------|

| 1 | **GeoLinker GL868_ESP32 Board** | Main board combining ESP32-S3 + SIM868 GSM modem for tracking | — |

| 2 | **IoT / Nano SIM Card** | Cellular connectivity (Airtel M2M IoT SIM recommended) | Any 2G-compatible SIM |

| 3 | **3.7V Li-Ion Battery** | Portable power supply | LiPo pouch battery |

| 4 | **USB-C Cable** | Programming, testing & charging | — |

| 5 | **E-Paper Display** | Displays messages; retains content without power | — |



---



## Circuit Diagram



The GeoLinker board integrates the **ESP32 microcontroller**, **GPS**, **GSM**, and **GPRS** modules on a single board, eliminating the need for multiple separate modules. Only the **E-paper display** needs to be externally connected via SPI.



A **3.7V lithium-ion battery** connects directly to the GeoLinker board for portable operation.



![Circuit Diagram](images/circuit-diagram.png) <!-- Replace with actual image path -->



---



## Hardware Assembly



The E-paper display and battery are connected to the GeoLinker board and arranged inside the 3D-printed enclosure. The compact layout allows all components to fit comfortably within the collar enclosure.



![Hardware Connection](images/hardware-connection.png) <!-- Replace with actual image path -->



---



## 3D Enclosure Design



The enclosure was designed in **Autodesk Fusion 360** and consists of two parts:



| Part | Description |

|------|-------------|

| **Base** | Houses the GeoLinker board, 3.7V battery, and internal wiring |

| **Lid** | Features a cutout for the E-paper display |



**Key Features:**

- Collar slots on both sides for mounting onto a standard pet collar

- Lightweight and durable 3D-printed construction

- Easy assembly with secure component placement



📁 **3D design files** are available in the [`/3D-Design`](3D-Design/) folder of this repository. You can modify them in Fusion 360 or use them as-is for printing.



![Enclosure Design](images/enclosure-design.png) <!-- Replace with actual image path -->



---



## Software Setup



### Prerequisites

- [Arduino IDE](https://www.arduino.cc/en/software) (v2.x recommended)

- GeoLinker board support package installed

- Required libraries (see code dependencies)



### Configuration



Update the following constants in the main code file before uploading:



```cpp

#define DEVICE_ID      "your_device_id"

#define API_KEY        "your_api_key"

#define ALERT_NUMBER   "+91XXXXXXXXXX"   // Caretaker's phone number

#define PET_NAME       "YourPetName"

```



### Upload Steps

1. Connect the GeoLinker board via USB-C.

2. Select the correct board and port in Arduino IDE.

3. Update the configuration constants above.

4. Configure your geofence zones (latitude, longitude, radius).

5. Upload the code to the board.



---



## Code Explanation



### Initialization



```cpp

void setup() {

    Serial.begin(115200);

    loadZoneStates();              // Restore saved geofence states from flash

    GeoLinker.begin(DEVICE_ID, API_KEY);

    GeoLinker.gpsOn();

    epdInit();                     // Initialize E-paper display

}

```



### GPS Acquisition & Geofence Checking



```cpp

bool hasLoc = getValidLocation(&gps);



if (hasLoc) {

    checkGeofences(&gps, hasLoc);

}



// Distance calculation using Haversine formula

double dist = haversineDistance(

    gps->latitude, gps->longitude,

    zones[i].latitude, zones[i].longitude

);

```



### Alert Generation



```cpp

static void sendGeofenceAlert(const char *zoneName, bool entered,

                              bool restricted, GPSData *gps, bool hasLoc) {

    char msg[256];

    buildAlertMessage(msg, sizeof(msg), entered ? "ENTERED" : "LEFT",

                      zoneName, gps, hasLoc);

    sendReliableSMS(ALERT_NUMBER, msg);



    // Schedule voice call for restricted zones

    if (restricted && !callInProgress && !pendingCall) {

        pendingCall = true;

        pendingCallTimer = millis();

    }

}

```



### E-paper Display Update



```cpp

static void epdUpdateIfNeeded() {

    if (petIsLost == displayShowsLost) return;  // No change needed



    if (petIsLost)

        epdShowLost();      // Shows "My Pet is Lost" + phone number

    else

        epdShowHealthy();   // Shows pet name + healthy status



    displayShowsLost = petIsLost;

}

```



### Main Loop



```cpp

void loop() {

    GeoLinker.update();

    checkIncomingSMS();                      // Handle "LOCATION" SMS commands



    bool hasLoc = getValidLocation(&gps);

    checkGeofences(&gps, hasLoc);



    // Cloud data upload at regular intervals

    if (!pendingCall && !callInProgress &&

        millis() - lastCloudPush >= CLOUD_SEND_INTERVAL * 1000UL) {

        lastCloudPush = millis();

        cloudPush(&gps, hasLoc);

    }

}

```



### Spoof/Test Mode



A **spoof code** is also available in this repository, allowing you to test the entire system (geofence alerts, SMS, calls, cloud upload) without physically moving — useful for development and debugging.



---



## Output & Demonstration



### Live Map Tracking

The cloud dashboard provides a live map showing the pet's real-time location and geofence zone status.



![Live Map](images/live-map.png) <!-- Replace with actual image path -->



### E-paper Display States



| Condition | Display |

|-----------|---------|

| Normal (inside safe zone) | Pet name + owner contact details |

| Restricted zone / Lost | ⚠️ "My Pet is Lost" + caretaker's phone number |



![Toby with Collar](images/toby-display-states.png) <!-- Replace with actual image path -->



### SMS & Call Alerts

- **SMS Alert:** Sent when the pet exits a safe zone — includes event details, location, and Google Maps link.

- **Voice Call:** Automatically placed when the pet enters a **restricted area** — ensures critical alerts are not missed.



![SMS and Call Alerts](images/sms-call-alerts.png) <!-- Replace with actual image path -->



---



## Applications



| Application | Description |

|-------------|-------------|

| 🐕 **Pet Theft Prevention** | Real-time alerts on unusual movement provide an additional layer of protection |

| 👶 **Child & Elderly Safety** | Track children or elderly individuals and receive alerts if they leave safe zones |

| 🚗 **Vehicle & Fleet Management** | Route monitoring, theft prevention, and fleet optimization |

| 🐄 **Livestock Monitoring** | Track livestock across large areas within designated boundaries |

| 📦 **Parcel & Delivery Tracking** | GPS-enabled package tracking with geofenced warehouse and delivery zones |



---



## Troubleshooting



| Problem | Cause | Solution |

|---------|-------|----------|

| GSM module fails to connect | SIM not inserted correctly, weak coverage, or inactive SIM | Check SIM placement, verify network availability, ensure active plan with balance |

| Voice call alerts not working | Network issues, insufficient balance, or incorrect config | Confirm GSM connectivity, enable calling services, verify emergency contact settings |

| E-paper display remains blank | Init failure, loose wiring, or power issues | Verify all SPI connections, check display power, ensure init code executes |

| Battery drains too fast | Continuous GPS, frequent GSM, no power-saving | Increase update intervals, optimize code, enable low-power modes |

| System freezes | Memory overflow, bugs, or module conflicts | Debug code, monitor memory usage, test modules independently |

| Whitelisted number not receiving alerts | Number format incorrect or whitelist not updated | Verify number with country code, check whitelist settings, restart device |



---



## FAQ



**Q: Can multiple safe zones be configured?**

> Yes. The system supports multiple geofenced areas that can be programmed based on your requirements.



**Q: What happens when the device moves outside the safe zone?**

> The system detects the boundary violation and automatically sends an SMS alert with location details to the registered contact number.



**Q: Why is an E-paper display used instead of a regular display?**

> E-paper displays consume very little power, retain content without power, and remain visible in bright sunlight — ideal for portable, battery-powered applications.



**Q: What is geofencing?**

> Geofencing creates a virtual boundary around a specific location. When the tracked object enters or exits this area, the system automatically generates alerts.



**Q: How are alerts sent to users?**

> Alerts are sent via SMS messages and voice calls through the GSM module, ensuring notifications can be received even without internet access.



---



## Future Improvements



- 📱 Mobile app integration for easier configuration and monitoring

- 🤖 AI-based analytics for behavior pattern detection

- 💓 Health monitoring sensors (body temperature, heart rate)

- 🔋 Extended battery life with low-power GNSS modules

- 📡 Wi-Fi / Bluetooth for indoor tracking

- 🏃 IMU sensor for activity and collar-removal detection

- 📊 Real-time dashboard visualization



---



## Related Projects



- [Arduino GPS Tracker using SIM800L and NEO-6M](https://circuitdigest.com/microcontroller-projects/arduino-gps-tracker-using-sim800l-and-neo-6m)

- [ESP32 GPS Tracker with SIM800L and NEO6M Module](https://circuitdigest.com/microcontroller-projects/esp32-gps-tracker-with-sim800l-and-neo6m-module)

- [Raspberry Pi Pico GPS Tracker](https://circuitdigest.com/microcontroller-projects/raspberry-pi-pico-gps-tracker)

- [ESP32 Interactive Voice Response System using GeoLinker](https://circuitdigest.com)

- [ESP32 GSM Calling Device using GeoLinker](https://circuitdigest.com)



---



## Repository Structure



```

Smart-Pet-Tracker-System/

├── Code/

│   ├── PetTracker/           # Main firmware code

│   └── SpoofTest/            # Test/spoof code for debugging

├── 3D-Design/

│   ├── Base.stl

│   ├── Lid.stl

│   └── Fusion360/            # Original Fusion 360 design files

├── images/                   # Documentation images

├── Circuit-Diagram/          # Circuit/schematic files

├── README.md

└── LICENSE

```



---



## License



This project is open-source. Feel free to modify and use it for your own projects.



---



## Credits



Developed by **[Circuit Digest](https://circuitdigest.com)** · Built with ❤️ for pets everywhere



📝 **Full Article:** [Pet Tracking System Using IoT – Circuit Digest](https://circuitdigest.com)



⭐ If you found this project helpful, please give it a star!

