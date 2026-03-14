# 🚨 Ground Pulse v1.0
A low-cost, ground‑penetrating life detection system for disaster rescue  
**Cost: ~₹3,000 | Detection Time: <10 seconds | Open Source**

---

## 🧭 Problem
South Asia faces **4,700+ landslide deaths every year**, yet professional life‑detection equipment costs **₹7–10 Lakh**, requires wired geophones, and is **inaccessible to 80% of disaster‑hit regions**.  
Rural first responders arrive **blind**, relying only on manual digging.

Ground Pulse aims to solve this technological gap.

---

## ✅ Our Solution
A **handheld, ₹3,000 device** that detects buried survivors by combining:

- **Seismic sensing** (heartbeats & breathing vibrations)
- **CO₂ metabolic detection**
- **Real‑time FFT analysis on ESP32**
- **Wireless LoRa transmission**
- **Solar‑powered field deployment**

---

## ⚙️ How It Works
### **1. Seismic Detection (0.1–5 Hz)**
- Piezo LDT0‑028K senses micro‑vibrations  
- ADXL345 provides redundant motion feedback

### **2. CO₂ Confirmation**
- MH‑Z19B detects exhaled CO₂ (>27 ppm threshold)

### **3. Dual‑Core ESP32 Processing**
- **Core 0:** FFT processing (filters 0.1–5 Hz biological band)  
- **Core 1:** LoRa SX1276 wireless transmission

### **4. Output**
- Confidence score (0–100%) on OLED SSD1306  
- Wireless data stream to laptop receiver  
- Detection within **10 seconds**

---

## 🧩 Components & Cost Breakdown

| Component | Cost |
|----------|------|
| ESP32 Dev Kit V1 | ₹300 |
| Piezo LDT0‑028K | ₹400 |
| MH‑Z19B CO₂ Sensor | ₹1,500 |
| LoRa SX1276 Ra‑02 | ₹400 |
| OLED SSD1306 | ₹200 |
| Supporting Components | ₹300 |
| **Total** | **~₹2,200–₹3,000** |

---

## 📂 Repository Contents
