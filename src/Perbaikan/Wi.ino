#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <driver/dac.h>
#include <driver/timer.h>
#include "WiThrottle.h"
#include "CommandStation.h"
#include "LocoAddress.h"

// ==================== KONFIGURASI ====================
#define DECODER_ADDRESS   3
#define WT_PORT_BASE      12090
const uint16_t WT_PORT = WT_PORT_BASE + DECODER_ADDRESS;

const char* ssid = "Rumah_Rifqi";
const char* password = "Komando45";

// Uncomment untuk mode JMRI
//#define USE_JMRI
#ifdef USE_JMRI
const IPAddress JMRI_IP(192, 168, 1, 100);
const uint16_t JMRI_PORT = 12090;
#endif

// ==================== PIN DEFINISI ====================
#define DAC1_PIN      25
#define DAC2_PIN      26
#define MOTOR_IN1_PIN 22
#define MOTOR_IN2_PIN 21
#define MOTOR_ENA_PIN 23
#define TRACK_POWER_PIN 4  // GPIO untuk mengontrol daya rel

// ==================== VARIABEL GLOBAL ====================
// Kontrol Motor
volatile bool motorEnabled = false;
volatile bool motorDirection = false; // false = forward, true = reverse
volatile uint8_t motorSpeed = 0;

// Kontrol Suara (dummy, jika tidak ada data audio)
volatile bool hornOn = false, sirenOn = false, sound1On = false;
volatile bool stasiunSoundOn = false, reversingSoundOn = false;

// Timer dan PWM
hw_timer_t *varTimer = nullptr;
hw_timer_t *fixTimer = nullptr;
portMUX_TYPE varMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE fixMux = portMUX_INITIALIZER_UNLOCKED;
const int ledcChannel = 0;
const int ledcFreq = 30000;  // 30 kHz
const int ledcResolution = 8;

// Server WiThrottle
WiThrottleServer withrottleServer(WT_PORT, DECODER_ADDRESS);

// ==================== DEKLARASI FUNGSI ====================
void IRAM_ATTR variablePlaybackTimer();
void IRAM_ATTR fixedPlaybackTimer();
void controlMotor();
void handleInternalControl(uint16_t addr, String actionVal, int source);
void setupTimers();
void setupPWM();
void testMotor();

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    
    // Inisialisasi pin
    pinMode(MOTOR_IN1_PIN, OUTPUT);
    pinMode(MOTOR_IN2_PIN, OUTPUT);
    pinMode(TRACK_POWER_PIN, OUTPUT);
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, LOW);
    digitalWrite(TRACK_POWER_PIN, HIGH); // Daya rel aktif
    
    // Inisialisasi WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    
    // Konfigurasi server WiThrottle
    #ifdef USE_JMRI
        Serial.println("Mode: JMRI Client");
        withrottleServer.setJMRIAddress(JMRI_IP, JMRI_PORT);
        withrottleServer.begin(WiThrottleServer::ConnectionMode::JMRI_CLIENT);
    #else
        Serial.println("Mode: Standalone");
        withrottleServer.begin(WiThrottleServer::ConnectionMode::STANDALONE);
    #endif
    
    // Set callback untuk kontrol internal
    withrottleServer.setLocoActionCallback(handleInternalControl);
    
    // Setup PWM dan timer
    setupPWM();
    setupTimers();
    
    // Tes motor (opsional)
    testMotor();
    
    Serial.println("Setup complete!");
}

void loop() {
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        Serial.printf("Motor: %s, Speed: %d, PWM: %d\n",
                     motorEnabled ? (motorDirection ? "REV" : "FWD") : "STOP",
                     motorSpeed,
                     ledcRead(ledcChannel));
        lastDebug = millis();
    }
    delay(10);
}

// ==================== FUNGSI UTAMA ====================
void handleInternalControl(uint16_t addr, String actionVal, int source) {
    // Perintah global (untuk daya rel)
    if (addr == 0) {
        if (actionVal.startsWith("P")) {
            bool power = (actionVal.charAt(1) == '1');
            digitalWrite(TRACK_POWER_PIN, power ? HIGH : LOW);
            Serial.printf("Track power: %s\n", power ? "ON" : "OFF");
        }
        return;
    }
    
    // Hanya proses untuk decoder ini
    if (addr != DECODER_ADDRESS) return;
    
    Serial.printf("Control: %s\n", actionVal.c_str());
    
    if (actionVal == "X") { // Emergency stop
        motorEnabled = false;
        motorSpeed = 0;
        Serial.println("EMERGENCY STOP");
    } 
    else if (actionVal == "Q") { // Stop
        motorEnabled = false;
        motorSpeed = 0;
        Serial.println("STOP");
    }
    else if (actionVal.startsWith("V")) { // Speed control
        motorSpeed = actionVal.substring(1).toInt();
        motorEnabled = (motorSpeed > 5);
        Serial.printf("Speed: %d\n", motorSpeed);
    }
    else if (actionVal.startsWith("R")) { // Direction
        motorDirection = (actionVal.substring(1).toInt() == 0);
        Serial.printf("Direction: %s\n", motorDirection ? "REV" : "FWD");
    }
    else if (actionVal.startsWith("F")) { // Function
        // Format: "F<number> <state>", e.g., "F1 1"
        int spacePos = actionVal.indexOf(' ');
        if (spacePos == -1) {
            Serial.println("Invalid function command");
            return;
        }
        
        int fn = actionVal.substring(1, spacePos).toInt();
        bool state = actionVal.substring(spacePos+1).toInt() > 0;
        
        Serial.printf("Function F%d: %s\n", fn, state ? "ON" : "OFF");
        
        switch (fn) {
            case 1: hornOn = state; break;
            case 2: sirenOn = state; break;
            case 3: sound1On = state; break;
            case 4: stasiunSoundOn = state; break;
            case 5: reversingSoundOn = state; break;
        }
    }
    
    controlMotor(); // Update motor
}

void controlMotor() {
    if (motorEnabled) {
        digitalWrite(MOTOR_IN1_PIN, !motorDirection);
        digitalWrite(MOTOR_IN2_PIN, motorDirection);
        uint8_t pwm = map(motorSpeed, 0, 126, 0, 255);
        ledcWrite(ledcChannel, pwm);
    } else {
        digitalWrite(MOTOR_IN1_PIN, LOW);
        digitalWrite(MOTOR_IN2_PIN, LOW);
        ledcWrite(ledcChannel, 0);
    }
}

void testMotor() {
    Serial.println("Testing motor...");
    
    // Forward
    Serial.println("Forward 50%");
    digitalWrite(MOTOR_IN1_PIN, HIGH);
    digitalWrite(MOTOR_IN2_PIN, LOW);
    ledcWrite(ledcChannel, 128);
    delay(2000);
    
    // Stop
    Serial.println("Stop");
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, LOW);
    ledcWrite(ledcChannel, 0);
    delay(1000);
    
    // Reverse
    Serial.println("Reverse 50%");
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, HIGH);
    ledcWrite(ledcChannel, 128);
    delay(2000);
    
    // Stop
    Serial.println("Stop");
    ledcWrite(ledcChannel, 0);
    delay(1000);
}

void setupTimers() {
    // Timer untuk suara mesin (variable rate)
    varTimer = timerBegin(0, 80, true); // Prescaler 80 (1 MHz)
    timerAttachInterrupt(varTimer, &variablePlaybackTimer, true);
    timerAlarmWrite(varTimer, 1000000 / 22050, true); // 22.05 kHz
    timerAlarmEnable(varTimer);
    
    // Timer untuk efek suara (fixed rate)
    fixTimer = timerBegin(1, 80, true);
    timerAttachInterrupt(fixTimer, &fixedPlaybackTimer, true);
    timerAlarmWrite(fixTimer, 1000000 / 22050, true); // 22.05 kHz
    timerAlarmEnable(fixTimer);
}

void setupPWM() {
    ledcSetup(ledcChannel, ledcFreq, ledcResolution);
    ledcAttachPin(MOTOR_ENA_PIN, ledcChannel);
    ledcWrite(ledcChannel, 0);
}

// ==================== INTERRUPT HANDLERS ====================
void IRAM_ATTR variablePlaybackTimer() {
    // Implementasi audio engine
}

void IRAM_ATTR fixedPlaybackTimer() {
    // Implementasi audio efek
}
