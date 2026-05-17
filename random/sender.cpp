#include <WiFi.h>
#include <esp_now.h>

#define TRIG_PIN 12
#define ECHO_PIN 13

// Sender Data
// === CHANGE THIS TO YOUR RECEIVER'S MAC ===
uint8_t receiverMAC[] = {0x70, 0x4B, 0xCA, 0x46, 0xD5, 0x50};

// Packed struct to prevent padding issues
typedef struct __attribute__((packed)) struct_message {
  uint32_t magic = 0xDEADBEEF; // validation
  uint32_t seq = 0;
  float distance;
  float timestamp;
} struct_message;

struct_message myData;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Delivery: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS ✅" : "FAILED ❌");
}

void setup() {
  Serial.begin(115200);
  delay(10000); // Helps avoid bootloader garbage

  Serial.println("\n=== SENDER STARTED ===");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  WiFi.mode(WIFI_STA);
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  delay(5000);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("Sender ready - sending every 2 seconds\n");
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0)
    return -1.0;

  float distance_cm = (duration * 0.0343) / 2.0;
  if (distance_cm < 2 || distance_cm > 400)
    return -1.0;

  return distance_cm;

  delay(60); //Give ultrasonic sensor time to settle before next reading
}

void loop() {
  float dist = getDistance();
  myData.seq++;
  myData.distance = (dist >= 0) ? dist : 0;
  myData.timestamp = millis() / 1000.0;

  Serial.printf("Sending: %.1f cm | Seq: %u\n",myData.distance, myData.seq);

  esp_err_t result =
      esp_now_send(receiverMAC, (uint8_t *)&myData, sizeof(myData));

  if (result == ESP_OK) {
    Serial.println("Sent successfully");
  } else {
    Serial.println("Send failed");
  }

  delay(2000);
}