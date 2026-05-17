#include <WiFi.h>
#include <esp_now.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// ============ CONFIG ============
#define MAX_DISTANCE_CM 10.0  // Tank height in cm (fullness calculation)
#define WIFI_SSID "ESP32-Garbage-Bin"
#define WIFI_PASSWORD "garbage123"

// ============ DATA STRUCTURES ============
typedef struct __attribute__((packed)) struct_message {
  uint32_t magic = 0xDEADBEEF;
  uint32_t seq;
  float distance;
  float timestamp;
} struct_message;

typedef struct {
  float fullness;
  uint64_t timestamp;
} ReadingRecord;

typedef struct {
  char mac_address[18];        // "AA:BB:CC:DD:EE:FF"
  float distance;              // latest distance in cm
  float fullness_percent;      // calculated fullness %
  uint32_t last_seq;           // last sequence number
  uint64_t last_update_time;   // millis() when last updated
  bool is_online;              // true if recently updated
  uint32_t total_readings;     // total readings received
  
  // Historical data - circular buffer
  ReadingRecord history[30];   // Last 30 readings
  int history_idx;             // Current index in circular buffer
  bool history_full;           // True if buffer is full
  
  // Statistics
  float avg_fullness;
  float min_fullness;
  float max_fullness;
  
  // Alert tracking
  uint32_t alert_count;        // Times it exceeded 80%
  uint64_t first_reading_time; // When first reading came in
  uint64_t last_reading_time;  // Most recent reading
} BinData;

// Store data for up to 5 bins
#define MAX_BINS 5
BinData bins[MAX_BINS];
int bin_count = 0;

// Create async web server on port 80
AsyncWebServer server(80);

struct_message receivedData;

// Global statistics tracking
uint32_t total_all_readings = 0;
uint64_t system_start_time = 0;

// ============ FORWARD DECLARATIONS ============
void updateBinStatistics(int bin_idx);

// ============ UTILITY FUNCTIONS ============
void macToString(const uint8_t *mac, char *str) {
  sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X", 
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int findBinByMAC(const char *mac) {
  for (int i = 0; i < bin_count; i++) {
    if (strcmp(bins[i].mac_address, mac) == 0) {
      return i;
    }
  }
  return -1;
}

float calculateFullness(float distance) {
  if (distance < 0) return 0.0;  // Invalid reading
  if (distance > MAX_DISTANCE_CM) distance = MAX_DISTANCE_CM;
  
  // Fullness = (MAX_DISTANCE - current_distance) / MAX_DISTANCE * 100
  float fullness = ((MAX_DISTANCE_CM - distance) / MAX_DISTANCE_CM) * 100.0;
  return constrain(fullness, 0.0, 100.0);
}

void addReadingToHistory(int bin_idx, float fullness) {
  BinData *bin = &bins[bin_idx];
  
  // Add to circular buffer
  bin->history[bin->history_idx].fullness = fullness;
  bin->history[bin->history_idx].timestamp = millis();
  
  bin->history_idx = (bin->history_idx + 1) % 30;
  if (bin->history_idx == 0) {
    bin->history_full = true;
  }
  
  // Update statistics
  updateBinStatistics(bin_idx);
}

void updateBinStatistics(int bin_idx) {
  BinData *bin = &bins[bin_idx];
  
  int count = bin->history_full ? 30 : bin->history_idx;
  if (count == 0) return;
  
  float sum = 0.0;
  float min_val = 100.0;
  float max_val = 0.0;
  
  for (int i = 0; i < count; i++) {
    float val = bin->history[i].fullness;
    sum += val;
    if (val < min_val) min_val = val;
    if (val > max_val) max_val = val;
  }
  
  bin->avg_fullness = sum / count;
  bin->min_fullness = min_val;
  bin->max_fullness = max_val;
}

String formatTime(uint64_t timestamp) {
  uint64_t now = millis();
  uint64_t diff = now - timestamp;
  
  if (diff < 1000) {
    return "just now";
  } else if (diff < 60000) {
    return String((int)(diff / 1000)) + "s ago";
  } else if (diff < 3600000) {
    return String((int)(diff / 60000)) + "m ago";
  } else {
    return String((int)(diff / 3600000)) + "h ago";
  }
}

// ============ ESP-NOW CALLBACK ============
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  Serial.println("\n=== DATA RECEIVED ===");

  if (len != sizeof(struct_message)) {
    Serial.printf("ERROR: Wrong size! Got %d bytes, expected %d\n", len, sizeof(struct_message));
    return;
  }

  memcpy(&receivedData, incomingData, sizeof(receivedData));

  if (receivedData.magic != 0xDEADBEEF) {
    Serial.println("ERROR: Invalid magic number - corrupted packet!");
    return;
  }

  // Convert MAC to string
  char mac_str[18];
  macToString(mac_addr, mac_str);

  // Find or create bin entry
  int bin_idx = findBinByMAC(mac_str);
  if (bin_idx == -1) {
    if (bin_count >= MAX_BINS) {
      Serial.println("ERROR: Max bins reached!");
      return;
    }
    bin_idx = bin_count;
    strcpy(bins[bin_idx].mac_address, mac_str);
    bins[bin_idx].total_readings = 0;
    bins[bin_idx].history_idx = 0;
    bins[bin_idx].history_full = false;
    bins[bin_idx].avg_fullness = 0.0;
    bins[bin_idx].min_fullness = 0.0;
    bins[bin_idx].max_fullness = 0.0;
    bins[bin_idx].alert_count = 0;
    bins[bin_idx].first_reading_time = millis();
    bins[bin_idx].last_reading_time = millis();
    bin_count++;
    Serial.printf("New bin registered: %s (Index: %d)\n", mac_str, bin_idx);
  }

  // Update bin data
  bins[bin_idx].distance = receivedData.distance;
  float fullness = calculateFullness(receivedData.distance);
  bins[bin_idx].fullness_percent = fullness;
  bins[bin_idx].last_seq = receivedData.seq;
  bins[bin_idx].last_update_time = millis();
  bins[bin_idx].last_reading_time = millis();
  bins[bin_idx].is_online = true;
  bins[bin_idx].total_readings++;
  total_all_readings++;
  
  // Track alerts
  if (fullness >= 80.0) {
    bins[bin_idx].alert_count++;
  }
  
  // Add to history
  addReadingToHistory(bin_idx, fullness);

  // Print to serial for debugging
  Serial.printf("Valid Packet | Seq: %u\n", receivedData.seq);
  Serial.printf("Distance    : %.1f cm\n", receivedData.distance);
  Serial.printf("Fullness    : %.1f %% (Avg: %.1f%%)\n", bins[bin_idx].fullness_percent, bins[bin_idx].avg_fullness);
  Serial.printf("From MAC    : %s\n", mac_str);
}

// ============ WEB SERVER ROUTES ============
void setupWebServer() {
  // Serve dashboard HTML
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Garbage Management | Professional Dashboard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        
        :root {
            --primary: #00d4ff;
            --secondary: #0099cc;
            --accent: #ff6b6b;
            --success: #51cf66;
            --warning: #ffd93d;
            --danger: #ff4757;
            --dark: #0d1117;
            --dark-2: #161b22;
            --light: #f0f6fc;
            --border: #30363d;
            --text: #c9d1d9;
            --text-muted: #8b949e;
        }
        
        body {
            font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
            background: linear-gradient(135deg, var(--dark) 0%, var(--dark-2) 100%);
            min-height: 100vh;
            padding: 20px;
            color: var(--text);
            overflow-x: hidden;
        }
        
        .container {
            max-width: 1600px;
            margin: 0 auto;
        }
        
        /* Header */
        .header {
            text-align: center;
            margin-bottom: 30px;
            padding: 30px 20px;
            border-bottom: 2px solid var(--border);
        }
        
        .header h1 {
            font-size: 2.5em;
            margin-bottom: 10px;
            background: linear-gradient(135deg, var(--primary), var(--accent));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
            font-weight: 800;
            letter-spacing: -1px;
        }
        
        .header-info {
            display: flex;
            justify-content: center;
            gap: 30px;
            flex-wrap: wrap;
            margin-top: 15px;
            font-size: 14px;
            color: var(--text-muted);
        }
        
        .status-badge {
            display: inline-block;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 12px;
            font-weight: 600;
        }
        
        .status-online { background: rgba(81, 207, 102, 0.2); color: var(--success); }
        .status-full { background: rgba(255, 71, 87, 0.2); color: var(--danger); }
        .status-warning { background: rgba(255, 217, 61, 0.2); color: var(--warning); }
        
        /* Section Title */
        .section-title {
            font-size: 1.5em;
            font-weight: 600;
            margin: 30px 0 20px 0;
            padding-bottom: 10px;
            border-bottom: 2px solid var(--border);
            color: var(--primary);
        }
        
        /* Bin Cards Grid */
        .bins-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
            gap: 20px;
            margin-bottom: 40px;
        }
        
        .bin-card {
            background: var(--dark-2);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 20px;
            transition: all 0.3s ease;
            position: relative;
            overflow: hidden;
        }
        
        .bin-card:hover {
            border-color: var(--primary);
            box-shadow: 0 0 20px rgba(0, 212, 255, 0.1);
        }
        
        .bin-card::before {
            content: '';
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
            height: 3px;
            background: linear-gradient(90deg, var(--primary), var(--accent));
        }
        
        .bin-id {
            font-size: 12px;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 8px;
        }
        
        .bin-name {
            font-size: 18px;
            font-weight: 700;
            margin-bottom: 15px;
            color: var(--primary);
        }
        
        .fullness-display {
            display: flex;
            align-items: baseline;
            gap: 8px;
            margin-bottom: 15px;
        }
        
        .fullness-value {
            font-size: 2.5em;
            font-weight: 800;
            color: var(--primary);
        }
        
        .fullness-percent {
            font-size: 1.2em;
            color: var(--text-muted);
        }
        
        .bin-details {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 12px;
            margin-bottom: 15px;
            padding-bottom: 15px;
            border-bottom: 1px solid var(--border);
        }
        
        .detail-item {
            font-size: 13px;
        }
        
        .detail-label {
            color: var(--text-muted);
            font-size: 11px;
            text-transform: uppercase;
            margin-bottom: 4px;
        }
        
        .detail-value {
            color: var(--text);
            font-weight: 600;
        }
        
        .bin-status {
            display: flex;
            align-items: center;
            gap: 8px;
            font-size: 12px;
        }
        
        .status-indicator {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            animation: pulse 2s infinite;
        }
        
        .status-indicator.online { background: var(--success); }
        .status-indicator.full { background: var(--danger); animation: pulse-danger 1s infinite; }
        .status-indicator.warning { background: var(--warning); animation: pulse-warning 1.5s infinite; }
        
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        
        @keyframes pulse-danger {
            0%, 100% { opacity: 1; transform: scale(1); }
            50% { opacity: 0.6; transform: scale(1.2); }
        }
        
        @keyframes pulse-warning {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.6; }
        }
        
        /* System Statistics Grid */
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 40px;
        }
        
        .stat-box {
            background: var(--dark-2);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 25px;
            text-align: center;
            transition: all 0.3s ease;
        }
        
        .stat-box:hover {
            border-color: var(--primary);
            transform: translateY(-4px);
        }
        
        .stat-label {
            color: var(--text-muted);
            font-size: 12px;
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 10px;
        }
        
        .stat-value {
            font-size: 2.2em;
            font-weight: 800;
            background: linear-gradient(135deg, var(--primary), var(--accent));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
            margin-bottom: 8px;
        }
        
        .stat-detail {
            font-size: 12px;
            color: var(--text-muted);
        }
        
        /* Per-Bin Statistics */
        .per-bin-stats {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
            gap: 20px;
            margin-bottom: 40px;
        }
        
        .bin-stats-card {
            background: var(--dark-2);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 20px;
        }
        
        .bin-stats-card h3 {
            color: var(--primary);
            font-size: 14px;
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 15px;
            padding-bottom: 10px;
            border-bottom: 1px solid var(--border);
        }
        
        .stats-row {
            display: flex;
            justify-content: space-between;
            padding: 12px 0;
            border-bottom: 1px solid rgba(48, 54, 61, 0.5);
            font-size: 13px;
        }
        
        .stats-row:last-child {
            border-bottom: none;
        }
        
        .stats-label {
            color: var(--text-muted);
        }
        
        .stats-value {
            color: var(--text);
            font-weight: 600;
            text-align: right;
        }
        
        .stats-value.success { color: var(--success); }
        .stats-value.warning { color: var(--warning); }
        .stats-value.danger { color: var(--danger); }
        
        /* Reading History Table */
        .history-section {
            background: var(--dark-2);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 40px;
            overflow-x: auto;
        }
        
        .history-section h3 {
            color: var(--primary);
            font-size: 14px;
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 15px;
        }
        
        .history-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 12px;
        }
        
        .history-table th {
            background: rgba(48, 54, 61, 0.5);
            padding: 12px;
            text-align: left;
            color: var(--text-muted);
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        
        .history-table td {
            padding: 12px;
            border-bottom: 1px solid rgba(48, 54, 61, 0.5);
            color: var(--text);
        }
        
        .history-table tr:hover {
            background: rgba(0, 212, 255, 0.05);
        }
        
        /* Footer */
        .footer {
            text-align: center;
            padding: 20px;
            color: var(--text-muted);
            font-size: 12px;
            border-top: 1px solid var(--border);
            margin-top: 40px;
        }
        
        .refresh-indicator {
            display: inline-block;
            padding: 8px 16px;
            background: rgba(0, 212, 255, 0.1);
            border: 1px solid rgba(0, 212, 255, 0.3);
            border-radius: 20px;
            margin-top: 10px;
        }
        
        .no-data {
            text-align: center;
            padding: 40px;
            color: var(--text-muted);
            font-size: 14px;
        }
    </style>
</head>
<body>
    <div class="container">
        <!-- Header -->
        <div class="header">
            <h1>🗑️ Smart Garbage Management System</h1>
            <div class="header-info">
                <div>
                    <span id="activeCount">0</span> of <span id="totalCount">0</span> 
                    <span class="status-badge status-online">BINS ONLINE</span>
                </div>
                <div>System Status: <span class="status-badge status-online">OPERATIONAL</span></div>
                <div>Last Update: <span id="lastUpdate">never</span></div>
            </div>
        </div>

        <!-- Live Bin Status Section -->
        <h2 class="section-title">Live Bin Status</h2>
        <div class="bins-grid" id="binsContainer">
            <div class="no-data">Waiting for bin data...</div>
        </div>

        <!-- System Statistics Section -->
        <h2 class="section-title">System Overview Statistics</h2>
        <div class="stats-grid">
            <div class="stat-box">
                <div class="stat-label">Total Readings</div>
                <div class="stat-value" id="totalReadings">0</div>
                <div class="stat-detail">From all bins combined</div>
            </div>
            <div class="stat-box">
                <div class="stat-label">System Uptime</div>
                <div class="stat-value" id="systemUptime">0m</div>
                <div class="stat-detail">Since initialization</div>
            </div>
            <div class="stat-box">
                <div class="stat-label">Average Fullness</div>
                <div class="stat-value" id="avgFullness">0%</div>
                <div class="stat-detail">All bins combined</div>
            </div>
            <div class="stat-box">
                <div class="stat-label">Total Alerts</div>
                <div class="stat-value" id="totalAlerts">0</div>
                <div class="stat-detail">Bins reached 80%+</div>
            </div>
        </div>

        <!-- Per-Bin Detailed Statistics -->
        <h2 class="section-title">Per-Bin Detailed Statistics</h2>
        <div class="per-bin-stats" id="perBinStats">
            <div class="no-data">No bins connected yet</div>
        </div>

        <!-- Reading History -->
        <h2 class="section-title">Latest Reading History (Last 30 readings)</h2>
        <div class="history-section">
            <div id="historyContainer" style="padding: 20px; text-align: center; color: var(--text-muted);">
                Connect a bin to view reading history
            </div>
        </div>

        <!-- Footer -->
        <div class="footer">
            <div>🔄 Auto-refreshing every 3 seconds</div>
            <div class="refresh-indicator">
                Last refresh: <span id="lastRefresh">never</span>
            </div>
        </div>
    </div>

    <script>
        const API_ENDPOINT = '/api/bins';
        const HISTORY_ENDPOINT = '/api/history';
        const REFRESH_INTERVAL = 3000;

        const appStartTime = Date.now();

        function getStatusColor(fullness) {
            if (fullness >= 80) return { color: 'danger', label: 'FULL', indicator: 'full' };
            if (fullness >= 50) return { color: 'warning', label: 'NEARLY FULL', indicator: 'warning' };
            return { color: 'success', label: 'OK', indicator: 'online' };
        }

        function formatTime(timestamp) {
            const now = Date.now();
            const diff = now - timestamp;
            const seconds = Math.floor(diff / 1000);
            
            if (seconds < 60) return seconds + 's ago';
            const minutes = Math.floor(seconds / 60);
            if (minutes < 60) return minutes + 'm ago';
            const hours = Math.floor(minutes / 60);
            return hours + 'h ago';
        }

        function formatUptime(uptime) {
            const hours = Math.floor(uptime / 3600);
            const minutes = Math.floor((uptime % 3600) / 60);
            
            if (hours > 0) return hours + 'h ' + minutes + 'm';
            return minutes + 'm';
        }

        function createBinCard(bin) {
            const status = getStatusColor(bin.fullness_percent);
            const shortMac = bin.mac_address.substring(12);
            
            return `
                <div class="bin-card">
                    <div class="bin-id">Bin ${shortMac}</div>
                    <div class="bin-name">MAC: ${bin.mac_address}</div>
                    
                    <div class="fullness-display">
                        <div class="fullness-value">${Math.round(bin.fullness_percent)}</div>
                        <div class="fullness-percent">%</div>
                    </div>
                    
                    <div class="bin-details">
                        <div class="detail-item">
                            <div class="detail-label">Distance</div>
                            <div class="detail-value">${bin.distance.toFixed(1)} cm</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">Readings</div>
                            <div class="detail-value">${bin.total_readings}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">Avg Fullness</div>
                            <div class="detail-value">${bin.avg_fullness.toFixed(1)}%</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">Last Update</div>
                            <div class="detail-value">${formatTime(bin.last_update_time)}</div>
                        </div>
                    </div>
                    
                    <div class="bin-status">
                        <div class="status-indicator ${status.indicator}"></div>
                        <span class="status-badge status-${status.color}">${status.label}</span>
                    </div>
                </div>
            `;
        }

        function createPerBinStats(bin) {
            const uptime = Math.floor((Date.now() - bin.first_reading_time) / 1000);
            const avgStatus = bin.avg_fullness >= 80 ? 'danger' : (bin.avg_fullness >= 50 ? 'warning' : 'success');
            
            return `
                <div class="bin-stats-card">
                    <h3>📊 ${bin.mac_address}</h3>
                    <div class="stats-row">
                        <span class="stats-label">Total Readings</span>
                        <span class="stats-value">${bin.total_readings}</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Current Fullness</span>
                        <span class="stats-value">${bin.fullness_percent.toFixed(1)}%</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Average Fullness</span>
                        <span class="stats-value ${avgStatus}">${bin.avg_fullness.toFixed(1)}%</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Min Fullness</span>
                        <span class="stats-value">${bin.min_fullness.toFixed(1)}%</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Max Fullness</span>
                        <span class="stats-value ${bin.max_fullness >= 80 ? 'danger' : ''}">${bin.max_fullness.toFixed(1)}%</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Alert Count (≥80%)</span>
                        <span class="stats-value ${bin.alert_count > 0 ? 'danger' : ''}">${bin.alert_count}</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Sequence Number</span>
                        <span class="stats-value">#${bin.last_seq}</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Time Since First Reading</span>
                        <span class="stats-value">${formatUptime(uptime)}</span>
                    </div>
                    <div class="stats-row">
                        <span class="stats-label">Last Reading</span>
                        <span class="stats-value">${formatTime(bin.last_reading_time)}</span>
                    </div>
                </div>
            `;
        }

        function createHistoryTable(bins) {
            if (bins.length === 0) {
                return '<div style="padding: 20px; text-align: center; color: var(--text-muted);">No bins connected</div>';
            }
            
            let html = '<table class="history-table"><thead><tr><th>Bin MAC</th><th>Current Fullness</th><th>Distance</th><th>Last Update</th><th>Total Readings</th></tr></thead><tbody>';
            
            for (const bin of bins) {
                html += `<tr>
                    <td>${bin.mac_address}</td>
                    <td>${bin.fullness_percent.toFixed(1)}%</td>
                    <td>${bin.distance.toFixed(1)} cm</td>
                    <td>${formatTime(bin.last_update_time)}</td>
                    <td>${bin.total_readings}</td>
                </tr>`;
            }
            
            html += '</tbody></table>';
            return html;
        }

        async function updateDashboard() {
            try {
                const response = await fetch(API_ENDPOINT);
                const data = await response.json();

                // Update header
                document.getElementById('activeCount').textContent = data.active_bins;
                document.getElementById('totalCount').textContent = data.total_bins;
                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
                document.getElementById('lastRefresh').textContent = new Date().toLocaleTimeString();

                // Render bins
                const container = document.getElementById('binsContainer');
                if (data.bins.length === 0) {
                    container.innerHTML = '<div class="no-data">Waiting for bin data...</div>';
                    document.getElementById('perBinStats').innerHTML = '<div class="no-data">No bins connected yet</div>';
                    return;
                }

                // Bin cards
                container.innerHTML = data.bins.map(bin => createBinCard(bin)).join('');

                // Per-bin stats
                document.getElementById('perBinStats').innerHTML = data.bins.map(bin => createPerBinStats(bin)).join('');

                // System statistics
                let totalReadings = 0;
                let totalFullness = 0;
                let totalAlerts = 0;

                for (const bin of data.bins) {
                    totalReadings += bin.total_readings;
                    totalFullness += bin.fullness_percent;
                    totalAlerts += bin.alert_count;
                }

                const avgFullness = data.bins.length > 0 ? (totalFullness / data.bins.length) : 0;
                const uptime = Math.floor((Date.now() - appStartTime) / 1000); // Seconds since epoch for demo

                document.getElementById('totalReadings').textContent = totalReadings;
                document.getElementById('avgFullness').textContent = avgFullness.toFixed(1) + '%';
                document.getElementById('totalAlerts').textContent = totalAlerts;
                document.getElementById('systemUptime').textContent = formatUptime(uptime);

                // History table
                document.getElementById('historyContainer').innerHTML = createHistoryTable(data.bins);

            } catch (error) {
                console.error('Failed to fetch data:', error);
                document.getElementById('binsContainer').innerHTML = '<div class="no-data">Failed to load data. Check connection.</div>';
            }
        }

        // Initial load and auto-refresh
        updateDashboard();
        setInterval(updateDashboard, REFRESH_INTERVAL);
    </script>
</body>
</html>
    )";
    request->send(200, "text/html", html);
  });

  // API endpoint for bin data (JSON)
  server.on("/api/bins", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    
    doc["total_bins"] = bin_count;
    doc["active_bins"] = bin_count;
    
    JsonArray bins_array = doc["bins"].to<JsonArray>();
    
    for (int i = 0; i < bin_count; i++) {
      JsonObject bin_obj = bins_array.add<JsonObject>();
      bin_obj["mac_address"] = bins[i].mac_address;
      bin_obj["distance"] = serialized(String(bins[i].distance, 1));
      bin_obj["fullness_percent"] = serialized(String(bins[i].fullness_percent, 1));
      bin_obj["last_seq"] = bins[i].last_seq;
      bin_obj["last_update_time"] = (uint64_t)bins[i].last_update_time;
      bin_obj["total_readings"] = bins[i].total_readings;
      bin_obj["avg_fullness"] = serialized(String(bins[i].avg_fullness, 1));
      bin_obj["min_fullness"] = serialized(String(bins[i].min_fullness, 1));
      bin_obj["max_fullness"] = serialized(String(bins[i].max_fullness, 1));
      bin_obj["alert_count"] = bins[i].alert_count;
      bin_obj["first_reading_time"] = (uint64_t)bins[i].first_reading_time;
      bin_obj["last_reading_time"] = (uint64_t)bins[i].last_reading_time;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API endpoint for historical data
  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("mac")) {
      request->send(400, "application/json", "{\"error\":\"Missing mac parameter\"}");
      return;
    }

    String mac = request->getParam("mac")->value();
    int bin_idx = findBinByMAC(mac.c_str());

    if (bin_idx == -1) {
      request->send(404, "application/json", "{\"error\":\"Bin not found\"}");
      return;
    }

    JsonDocument doc;
    JsonArray history_array = doc["history"].to<JsonArray>();

    BinData *bin = &bins[bin_idx];
    int count = bin->history_full ? 30 : bin->history_idx;

    if (bin->history_full) {
      // Start from next index (oldest reading)
      for (int i = 0; i < 30; i++) {
        int idx = (bin->history_idx + i) % 30;
        JsonObject record = history_array.add<JsonObject>();
        record["fullness"] = serialized(String(bin->history[idx].fullness, 1));
        record["timestamp"] = (uint64_t)bin->history[idx].timestamp;
      }
    } else {
      // Buffer not full yet, show from beginning
      for (int i = 0; i < count; i++) {
        JsonObject record = history_array.add<JsonObject>();
        record["fullness"] = serialized(String(bin->history[i].fullness, 1));
        record["timestamp"] = (uint64_t)bin->history[i].timestamp;
      }
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // Catch-all 404
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(10000);

  Serial.println("\n=== RECEIVER STARTED (with Enhanced Professional Dashboard) ===");
  delay(5000);

  // Initialize system time
  system_start_time = millis();

  // Initialize bins array
  memset(bins, 0, sizeof(bins));
  bin_count = 0;
  total_all_readings = 0;
  
  // Initialize each bin's history array
  for (int i = 0; i < MAX_BINS; i++) {
    bins[i].history_idx = 0;
    bins[i].history_full = false;
    bins[i].avg_fullness = 0.0;
    bins[i].min_fullness = 0.0;
    bins[i].max_fullness = 0.0;
    bins[i].alert_count = 0;
  }

  // WiFi setup - AP mode so you can access from laptop
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  IPAddress IP = WiFi.softAPIP();
  
  Serial.print("AP SSID: ");
  Serial.println(WIFI_SSID);
  Serial.print("AP Password: ");
  Serial.println(WIFI_PASSWORD);
  Serial.print("Access URL: http://");
  Serial.println(IP);
  Serial.println("\nConnect your laptop WiFi to the ESP32 AP above, then open:");
  Serial.println("http://192.168.4.1");

  // ESP-NOW setup
  WiFi.mode(WIFI_AP_STA);  // Both AP and STA modes
  delay(200);
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());
  delay(5000);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("ESP-NOW Receiver ready\n");

  // Setup web server
  setupWebServer();
  server.begin();
  Serial.println("Web Server started!");
}

void loop() {
  delay(100);
}
