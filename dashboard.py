import serial
import serial.tools.list_ports
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
import sys
import time

# ─────────────────────────────────────────────
# CONFIG — change COM port if needed
# Run: python -m serial.tools.list_ports
# to find your ESP32 port
# ─────────────────────────────────────────────
BAUD_RATE = 115200
MAX_POINTS = 100  # How many data points to show on graph

# ─────────────────────────────────────────────
# Auto-detect ESP32 port
# ─────────────────────────────────────────────
def find_esp32_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "CP210" in p.description or "CH340" in p.description or "UART" in p.description:
            return p.device
    if ports:
        print(f"[WARNING] Could not auto-detect ESP32. Using first available: {ports[0].device}")
        return ports[0].device
    return None

port = find_esp32_port()
if not port:
    print("[ERROR] No serial port found. Connect your ESP32 and retry.")
    sys.exit(1)

print(f"[OK] Connecting to {port} at {BAUD_RATE} baud...")
try:
    ser = serial.Serial(port, BAUD_RATE, timeout=1)
    time.sleep(2)  # Wait for ESP32 to settle
    print("[OK] Connected!")
except Exception as e:
    print(f"[ERROR] Could not open port: {e}")
    sys.exit(1)

# ─────────────────────────────────────────────
# Data buffers
# ─────────────────────────────────────────────
scores     = deque([0] * MAX_POINTS, maxlen=MAX_POINTS)
freqs      = deque([0.0] * MAX_POINTS, maxlen=MAX_POINTS)
co2_deltas = deque([0] * MAX_POINTS, maxlen=MAX_POINTS)
timestamps = deque([0] * MAX_POINTS, maxlen=MAX_POINTS)
start_time = time.time()

latest = {
    "ai_conf": 0,
    "freq": 0.0,
    "co2": 0,
    "accel": 0.0,
    "human": False,
    "bat": 100,
    "pkt": 0,
    "class": 0
}

# ─────────────────────────────────────────────
# Parse incoming LoRa / serial line
# Format: GP,<AI_conf%>,<freq>,<co2delta>,<accel>,<human_flag>,<bat%>,<pktn>,<lifeClass>
# Also parse [SENSE] debug lines as fallback
# ─────────────────────────────────────────────
def parse_line(line):
    line = line.strip()
    if line.startswith("GP,"):
        try:
            parts = line.split(",")
            latest["ai_conf"] = int(parts[1])
            latest["freq"]   = float(parts[2])
            latest["co2"]    = int(parts[3])
            latest["accel"]  = float(parts[4])
            latest["human"]  = parts[5] == "1"
            latest["bat"]    = int(parts[6])
            latest["pkt"]    = int(parts[7])
            latest["class"]  = int(parts[8]) if len(parts) > 8 else 0  # ADD THIS
            return True
        except:
            return False
    return False
CLASS_LABELS = {0: "No life", 1: "HUMAN", 2: "Animal"}
# ─────────────────────────────────────────────
# Matplotlib figure setup
# ─────────────────────────────────────────────
fig, axes = plt.subplots(3, 1, figsize=(12, 8))
fig.patch.set_facecolor('#0d0d0d')
fig.suptitle('GroundPulse — Live Dashboard', color='white', fontsize=14, fontweight='bold')

for ax in axes:
    ax.set_facecolor('#1a1a1a')
    ax.tick_params(colors='white')
    ax.xaxis.label.set_color('white')
    ax.yaxis.label.set_color('white')
    for spine in ax.spines.values():
        spine.set_edgecolor('#444444')

ax_score, ax_freq, ax_co2 = axes

line_score, = ax_score.plot([], [], color='#00ff88', linewidth=2)
ax_score.set_ylim(0, 100)
ax_score.set_xlim(0, MAX_POINTS)
ax_score.set_ylabel(‘AI Confidence (%)’)
ax_score.axhline(y=60, color='red', linestyle='--', linewidth=1, alpha=0.7)
ax_score.text(2, 62, 'ALERT THRESHOLD', color='red', fontsize=7)

line_freq, = ax_freq.plot([], [], color='#00aaff', linewidth=2)
ax_freq.set_ylim(0, 5)
ax_freq.set_xlim(0, MAX_POINTS)
ax_freq.set_ylabel('Frequency (Hz)')
ax_freq.axhspan(0.8, 3.0, alpha=0.1, color='green')
ax_freq.text(2, 2.5, 'Heartbeat zone', color='#88ff88', fontsize=7)

line_co2, = ax_co2.plot([], [], color='#ffaa00', linewidth=2)
ax_co2.set_ylim(-10, 200)
ax_co2.set_xlim(0, MAX_POINTS)
ax_co2.set_ylabel('CO₂ Rise (ppm)')
ax_co2.axhline(y=27, color='orange', linestyle='--', linewidth=1, alpha=0.7)
ax_co2.text(2, 29, 'Human threshold', color='orange', fontsize=7)

status_text = fig.text(0.5, 0.01, '', ha='center', fontsize=12, fontweight='bold')

plt.tight_layout(rect=[0, 0.04, 1, 1])

# ─────────────────────────────────────────────
# Animation update function
# ─────────────────────────────────────────────
def update(frame):
    # Read all available lines from serial
    while ser.in_waiting:
        try:
            raw = ser.readline().decode('utf-8', errors='ignore')
            parse_line(raw)
        except:
            pass

    t = time.time() - start_time
    scores.append(latest[“ai_conf”])
    freqs.append(latest["freq"])
    co2_deltas.append(latest["co2"])
    timestamps.append(t)

    xs = list(range(MAX_POINTS))

    line_score.set_data(xs, list(scores))
    line_freq.set_data(xs, list(freqs))
    line_co2.set_data(xs, list(co2_deltas))

  cls_label = CLASS_LABELS.get(latest.get("class", 0), "No life")

    if latest["human"]:
        status_text.set_text(
            f'HUMAN DETECTED — AI Conf: {latest["ai_conf"]}%  |  '
            f'{latest["freq"]:.2f} Hz  |  CO2 +{latest["co2"]} ppm  |  '
            f'Class: {cls_label}  |  Bat: {latest["bat"]}%'
        )
        status_text.set_color('#ff4444')
        fig.patch.set_facecolor('#1a0000')

    elif latest.get("class", 0) == 2:
        status_text.set_text(
            f'ANIMAL DETECTED — AI Conf: {latest["ai_conf"]}%  |  '
            f'{latest["freq"]:.2f} Hz  |  Class: {cls_label}  |  '
            f'Bat: {latest["bat"]}%'
        )
        status_text.set_color('#ffaa00')
        fig.patch.set_facecolor('#1a1200')

    else:
        status_text.set_text(
            f'Scanning...  AI Conf: {latest["ai_conf"]}%  |  '
            f'{latest["freq"]:.2f} Hz  |  Class: {cls_label}  |  '
            f'Bat: {latest["bat"]}%  |  Pkt #{latest["pkt"]}'
        )
        status_text.set_color('#aaaaaa')
        fig.patch.set_facecolor('#0d0d0d')
        
    return line_score, line_freq, line_co2, status_text

# ─────────────────────────────────────────────
# Run
# ─────────────────────────────────────────────
ani = animation.FuncAnimation(fig, update, interval=250, blit=False)

print("[OK] Dashboard running. Close the window to stop.")
try:
    plt.show()
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print("[OK] Serial port closed.")
