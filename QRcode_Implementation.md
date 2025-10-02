# 📱 Single QR Code to Connect to IoT devices and Open Web App

## 📖 Overview
This project describes how to onboard users into a **IoT devices** with just a single QR code.  
By scanning the code, a user’s device connects to an ESP32-based Wi-Fi hotspot and automatically loads a **captive portal** web app hosted locally on the ESP32.  

This approach avoids installing extra apps, works offline, and makes onboarding simple for IoT devices.  

---

## 🛠️ System Requirements

### ESP32 (Sensor Side)
- Configure ESP32 as a Wi-Fi Access Point (AP)  
- Run a DNS hijack server to intercept all DNS lookups  
- Host a lightweight HTTP web server  
- Implement captive portal logic to redirect all traffic to the local web page  

### Host PC (QR Code Generation Side)
- Access to the repository for QR code generation  
- Use the [QRCoder](https://github.com/codebude/QRCoder) C# library  
- Logic to generate QR codes in Wi-Fi format  
- (Optional) Provide a **fallback QR code** linking to a cloud-hosted help page  

---

## ⚙️ Implementation Steps

### **Step 1 – Generate QR Code for Wi-Fi**
Use `QRCoder` in C# to encode the ESP32 SSID, password, and encryption type.  
This QR code triggers devices to connect directly to the ESP32 AP.  

**Example Wi-Fi QR string:**
```text
WIFI: T:WPA;S:ESP_Sensor_1234;P:yourpassword;;
```

**C# QR generation example with QRCoder:**
```csharp
using QRCoder;

string wifiString = "WIFI:T:WPA;S:ESP_Sensor_1234;P:yourpassword;;";
QRCodeGenerator qrGenerator = new QRCodeGenerator();
QRCodeData qrCodeData = qrGenerator.CreateQrCode(wifiString, QRCodeGenerator.ECCLevel.Q);
QRCode qrCode = new QRCode(qrCodeData);

using (Bitmap qrCodeImage = qrCode.GetGraphic(20))
{
    qrCodeImage.Save("wifi_qr.png", ImageFormat.Png);
}
```

---

### **Step 2 – ESP32 Wi-Fi Hotspot**
Set up the ESP32 as an Access Point (AP):  

```cpp
WiFi.softAP("ESP_Sensor_1234", "yourpassword");
```

Users scan the QR code and connect automatically.  

---

### **Step 3 – DNS Interceptor**
The ESP32 runs a DNS server that replies to **any** domain with its own IP.  
This tricks devices into believing they must display a captive portal.  

```cpp
dnsServer.start(53, "*", WiFi.softAPIP());
```

---

### **Step 4 – Web Server**
Host a simple web server to serve the **homepage**, sensor data, and settings UI.  

```cpp
server.on("/", []() {
    server.send(200, "text/html", "<h1>Welcome to the ESP Sensor</h1>");
});
```

---

### **Step 5 – Captive Portal Redirect**
Redirect **all unknown routes** to the homepage.  
Ensures the user always lands on the correct page.  

```cpp
server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting...");
});
```

---

## 📦 Types of QR Codes for Onboarding

1. **Wi-Fi QR Code**  
   - Encodes SSID, password, and encryption type  
   - ✅ Convenient, direct connection  
   - ❌ Credentials are visible if shared  

2. **URL QR Code**  
   - Opens a specific web page (local or remote)  
   - ✅ Simple and universal  
   - ❌ Requires internet if hosted in the cloud  

3. **Multi-Action QR Code**  
   - Combines Wi-Fi + URL + App install info  
   - ✅ One-scan onboarding flow  
   - ❌ Limited platform support  

4. **Dynamic QR Code**  
   - Points to a server that can redirect to updated links  
   - ✅ Updateable without reprinting codes  
   - ❌ Requires backend infrastructure  

5. **Fallback QR Code**  
   - Backup QR pointing to a help page or cloud app  
   - ✅ Increases reliability  
   - ❌ Adds complexity  

---

## ✅ Pros & ❌ Cons of Single QR Code Approach

**Pros**
- Simple and user-friendly (one scan → connected + redirected)  
- No dedicated app required  
- Works completely offline  
- ESP32 captive portal ensures smooth UX  

**Cons**
- Captive portals behave differently on **iOS vs Android**  
- Some devices block automatic captive portal opening  
- Hardcoded Wi-Fi credentials reduce security  
- Only local access unless internet bridging is added  

---

## 💡 Suggestions & Improvements

- 🔒 Add HTTPS support (self-signed certs possible, but browsers may warn)  
- 🔑 Implement user authentication (tokens, session handling)  
- 🌐 Provide a **cloud fallback QR code** for support pages  
- 📝 Add logging/troubleshooting interface on ESP32  
- 🔗 Integrate with Zigbee or BLE for hybrid onboarding  
- ⏳ Use **short-lived Wi-Fi credentials** (tokens) for security  
- 📱 Test across iOS/Android captive portal quirks  

---

## 🚀 Future Enhancements

- Multi-device mesh networking (ESP-NOW / Zigbee)  
- Cloud integration for remote monitoring  
- QR codes that embed temporary access tokens  
- Use **BLE advertising** as an alternative onboarding channel  

---

## 🏁 Conclusion
Using a **single QR code** for Wi-Fi onboarding with an ESP32 captive portal is a lightweight, user-friendly approach for IoT devices.  

It reduces onboarding friction, requires no app installation, and works fully offline.  
Future improvements should focus on **security, redundancy, and cross-platform compatibility**.  

---
