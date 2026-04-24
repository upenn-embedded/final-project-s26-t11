#include <WiFi.h>
#include <WebServer.h>

const char* SSID     = "imungikar";
const char* PASSWORD = "ishan123";

#define GIMBAL_PIN 5 
#define BUTTON_PIN 6 
WebServer server(80);

volatile bool gimbal_enabled = true;
volatile bool button_changed = false;

void IRAM_ATTR handleButtonISR() {
    button_changed = true;
}

void applyGimbalState() {
    digitalWrite(GIMBAL_PIN, gimbal_enabled ? HIGH : LOW);
}

void handleToggle() {
    gimbal_enabled = !gimbal_enabled;
    applyGimbalState();
    String body = String("{\"gimbal_enabled\":") + (gimbal_enabled ? "true" : "false") + "}";
    server.send(200, "application/json", body);
}

void handleState() {
    String body = String("{\"gimbal_enabled\":") + (gimbal_enabled ? "true" : "false") + "}";
    server.send(200, "application/json", body);
}

void setup() {
    Serial.begin(115200);
    Serial.println("Begin Running!");

    pinMode(GIMBAL_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    gimbal_enabled = true;
    applyGimbalState();

    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonISR, FALLING);

    WiFi.begin(SSID, PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Set ESP32_URL in app.py to: http://" + WiFi.localIP().toString());

    server.on("/gimbal/toggle", HTTP_POST, handleToggle);
    server.on("/gimbal/state",  HTTP_GET,  handleState);
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();

    if (button_changed) {
        button_changed = false;
        static unsigned long last_press_ms = 0;
        unsigned long now = millis();
        if (now - last_press_ms > 200) {
            last_press_ms = now;
            gimbal_enabled = !gimbal_enabled;
            applyGimbalState();
            Serial.print("Physical button: gimbal ");
            Serial.println(gimbal_enabled ? "ENABLED" : "DISABLED");
        }
    }
}
