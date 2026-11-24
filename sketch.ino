#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===== WiFi + EMQX =====
const char* ssid         = "Wokwi-GUEST";
const char* wifiPassword = "";
const char* mqttHost     = "vd2a1e71.ala.us-east-1.emqxsl.com";
const int   mqttPort     = 8883;
const char* mqttUser     = "smartparking";
const char* mqttPassword = "1234group";

// ===== Topics =====
const char* T_PLACES = "parking22/places";
const char* T_ENTREE = "parking22/entree";
const char* T_CMD    = "parking22/barriere/cmd";

// ===== Pins =====
#define TRIG1 12
#define ECHO1 13
#define TRIG2 14
#define ECHO2 27
#define TRIG3 26
#define ECHO3 25
#define PIR_PIN   33
#define SERVO_PIN 18
#define LED_GREEN 19
#define LED_RED   23
#define BUZZER    15

#define SEUIL 15                  // cm : < 15 = occupe
#define OUVERTURE_MS 5000         // la barriere reste ouverte 5 s
const int TOTAL_PLACES = 3;

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo barriere;

String dernierJson = "";
unsigned long lastPublish = 0;
unsigned long closeAt = 0;        // != 0 : barriere ouverte, fermeture prevue
int lastPir = LOW;

float mesurerDistance(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duree = pulseIn(echo, HIGH, 30000);
  if (duree == 0) return 999;
  return duree * 0.0343 / 2;
}

// ===== Sons (non bloquants) =====
void bipOuverture() { tone(BUZZER, 1500, 300); }   // aigu : la barriere monte
void bipFermeture() { tone(BUZZER, 800, 400);  }   // grave : la barriere descend
void sonRefus()     { tone(BUZZER, 400, 1500); }   // long : parking complet

void ouvrirBarriere() {
  if (closeAt == 0) {                  // bip seulement si elle etait fermee
    barriere.write(90);
    bipOuverture();
    Serial.println("Barriere: OUVERTE");
  }
  closeAt = millis() + OUVERTURE_MS;   // (re)demarre les 5 s
}

void fermerBarriere() {
  barriere.write(0);
  closeAt = 0;
  bipFermeture();
  Serial.println("Barriere: FERMEE");
}

// ===== Commandes recues de Node-RED / dashboard =====
void onMessage(char* topic, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.println("Commande recue: " + cmd);

  if (cmd == "open") {
    ouvrirBarriere();
  } else if (cmd == "close") {
    if (closeAt != 0) fermerBarriere();
  } else if (cmd == "refuse") {
    barriere.write(0);                 // reste fermee
    sonRefus();
    Serial.println("Acces REFUSE: parking complet");
  }
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, wifiPassword, 6);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Connected!");
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to EMQX...");
    if (mqtt.connect("SmartParking-ESP32", mqttUser, mqttPassword)) {
      Serial.println("Connected!");
      mqtt.subscribe(T_CMD);
    } else {
      Serial.print("Failed: "); Serial.println(mqtt.state());
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  ESP32PWM::allocateTimer(0);
  barriere.setPeriodHertz(50);
  barriere.attach(SERVO_PIN, 500, 2400);
  barriere.write(0);                   // barriere fermee au demarrage

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("SMART PARKING");
  lcd.setCursor(0, 1); lcd.print("Connexion...");

  connectWiFi();
  secureClient.setInsecure();          // TLS (certificat CA a ajouter plus tard)
  mqtt.setServer(mqttHost, mqttPort);
  mqtt.setCallback(onMessage);
  connectMQTT();

  lcd.clear();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  // --- Fermeture automatique apres 5 s ---
  if (closeAt != 0 && millis() > closeAt) fermerBarriere();

  // --- Lecture des 3 places ---
  bool o1 = mesurerDistance(TRIG1, ECHO1) < SEUIL; delay(30);
  bool o2 = mesurerDistance(TRIG2, ECHO2) < SEUIL; delay(30);
  bool o3 = mesurerDistance(TRIG3, ECHO3) < SEUIL;

  int available = TOTAL_PLACES - (o1 + o2 + o3);

  String json = "{\"p1\":\"" + String(o1 ? "occupe" : "libre") +
                "\",\"p2\":\"" + String(o2 ? "occupe" : "libre") +
                "\",\"p3\":\"" + String(o3 ? "occupe" : "libre") + "\"}";

  // publier au changement, et toutes les 10 s (heartbeat)
  if (json != dernierJson || millis() - lastPublish > 10000) {
    mqtt.publish(T_PLACES, json.c_str());
    Serial.println("Publie: " + json);
    dernierJson = json;
    lastPublish = millis();
  }

  // --- LCD + LEDs ---
  lcd.setCursor(0, 0);
  lcd.print("Libres: "); lcd.print(available); lcd.print("/3       ");
  lcd.setCursor(0, 1);
  lcd.print(available > 0 ? "Parking DISPON. " : "Parking COMPLET ");
  digitalWrite(LED_GREEN, available > 0 ? HIGH : LOW);
  digitalWrite(LED_RED,   available > 0 ? LOW  : HIGH);

  // --- PIR : voiture a l'entree (front montant LOW -> HIGH) ---
  // ignoree si la barriere est deja ouverte (une seule voiture a la fois)
  int pir = digitalRead(PIR_PIN);
  if (pir == HIGH && lastPir == LOW && closeAt == 0) {
    mqtt.publish(T_ENTREE, "{\"vehicule\":true}");
    Serial.println("Voiture a l'entree");
  }
  lastPir = pir;

  delay(100);
}