#include <WiFi.h>
#include <WebServer.h>

const char* SSID     = "imungikar";
const char* PASSWORD = "ishan123";

#define LED_PIN 5

WebServer server(80);

void handleLedOn() {
    digitalWrite(LED_PIN, HIGH);
    server.send(200, "text/plain", "OK");
}

void handleLedOff() {
    digitalWrite(LED_PIN, LOW);
    server.send(200, "text/plain", "OK");
}

void setup() {
    Serial.begin(115200);
    Serial.println("Begin Running!");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

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

    server.on("/led/on",  HTTP_POST, handleLedOn);
    server.on("/led/off", HTTP_POST, handleLedOff);
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();
}
