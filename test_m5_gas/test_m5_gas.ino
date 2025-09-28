/**
 * @file test_m5_gas.ino
 * @brief GAS connection test program for M5Stack
 * Send test data to GAS endpoint by pressing center button
 */

#include <M5Stack.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <WiFiClientSecure.h>

// Configuration structure
struct TestConfig {
    String wifi_ssid;
    String wifi_password;
    String gas_url;
    int device_id;
    std::vector<String> grade_names;
    std::vector<int> grade_weights;
};

TestConfig testConfig;
bool wifiConnected = false;

// Load configuration file
bool loadTestConfig() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.println("Loading Config...");

    if (!SD.begin()) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("SD Card init failed");
        return false;
    }

    File configFile = SD.open("/config.json");
    if (!configFile) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("config.json not found");
        return false;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("JSON parse failed");
        M5.Lcd.println(error.c_str());
        return false;
    }

    // Load WiFi settings
    testConfig.wifi_ssid = doc["wifi"]["ssid"].as<String>();
    testConfig.wifi_password = doc["wifi"]["password"].as<String>();
    testConfig.gas_url = doc["gas_url"].as<String>();
    testConfig.device_id = doc["device_id"] | 1;

    // Load grade data
    testConfig.grade_names.clear();
    testConfig.grade_weights.clear();

    if (doc.containsKey("product_settings") && doc["product_settings"].containsKey("grades")) {
        for (JsonObject grade : doc["product_settings"]["grades"].as<JsonArray>()) {
            String gradeName = grade["name"].as<String>();
            int minWeight = grade["min_weight"];
            int maxWeight = grade["max_weight"];
            int avgWeight = (minWeight + maxWeight) / 2;

            testConfig.grade_names.push_back(gradeName);
            testConfig.grade_weights.push_back(avgWeight);
        }
    }

    M5.Lcd.fillScreen(GREEN);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Config Loaded!");
    M5.Lcd.printf("SSID: %s\n", testConfig.wifi_ssid.c_str());
    M5.Lcd.printf("Device ID: %d\n", testConfig.device_id);
    M5.Lcd.printf("Grades: %d\n", testConfig.grade_names.size());
    delay(2000);

    return true;
}

// WiFi connection
bool connectWiFi() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.println("Connecting WiFi...");
    M5.Lcd.printf("SSID: %s\n", testConfig.wifi_ssid.c_str());

    WiFi.begin(testConfig.wifi_ssid.c_str(), testConfig.wifi_password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        M5.Lcd.print(".");
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        M5.Lcd.fillScreen(GREEN);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("WiFi Connected!");
        M5.Lcd.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        delay(2000);
        return true;
    } else {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("WiFi Connection Failed!");
        delay(2000);
        return false;
    }
}

// Generate random test data
void generateTestData(String& grade, int& weight, String& timestamp) {
    // Random grade selection
    if (testConfig.grade_names.size() > 0) {
        int randomIndex = random(0, testConfig.grade_names.size());
        grade = testConfig.grade_names[randomIndex];
        weight = testConfig.grade_weights[randomIndex] + random(-10, 11); // +/- 10g range
    } else {
        grade = "TEST";
        weight = 100;
    }

    // Get current time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%Y/%m/%d %H:%M:%S", &timeinfo);
        timestamp = String(timeStr);
    } else {
        timestamp = "Time not set";
    }
}

// Send test data to GAS (with 302 error countermeasures)
int sendTestDataToGAS(const String& grade, int weight, const String& timestamp) {
    if (WiFi.status() != WL_CONNECTED) {
        return -999; // WiFi not connected
    }

    HTTPClient http;

    // 302 error countermeasure: Enable redirect following
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    // 302 error countermeasure: Set User-Agent
    http.setUserAgent("Mozilla/5.0 (compatible; M5Stack-GASTest/1.0)");

    // Timeout setting
    http.setTimeout(15000); // 15 seconds

    // Disable SSL certificate verification if needed
    // replaced: http.setInsecure();
  WiFiClientSecure client;
  client.setInsecure();

    http.begin(testConfig.gas_url.c_str());

    // 400 error countermeasure 1: Minimize headers and add charset
    http.addHeader("Content-Type", "application/json; charset=utf-8");
    http.addHeader("Accept", "*/*");
    // Removed Accept-Encoding to avoid gzip compression issues
    // http.addHeader("Accept-Encoding", "gzip, deflate");
    http.addHeader("Connection", "keep-alive");

    // 400 error countermeasure 2: Increase JSON document size
    StaticJsonDocument<300> doc;
    doc["size"] = grade;
    doc["weight"] = weight;
    doc["timestamp"] = timestamp;
    doc["device_id"] = testConfig.device_id;

    String jsonString;
    serializeJson(doc, jsonString);

    // 400 error countermeasure 3: Enhanced debug output
    Serial.println("=== DEBUG: GAS Request Info ===");
    Serial.printf("Target URL: %s\n", testConfig.gas_url.c_str());
    Serial.printf("JSON Size: %d bytes\n", jsonString.length());
    Serial.printf("JSON Content: %s\n", jsonString.c_str());
    Serial.printf("Content-Type: application/json; charset=utf-8\n");
    Serial.println("==============================");

    // POST request
    int httpResponseCode = http.POST(jsonString);

    // Enhanced debug output for response
    Serial.printf("=== RESPONSE ===\n");
    Serial.printf("HTTP Response Code: %d\n", httpResponseCode);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("Response Body: %s\n", response.c_str());
    }
    Serial.println("================");

    http.end();
    return httpResponseCode;
}

// Display result
void displayResult(int responseCode, const String& grade, int weight) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);

    if (responseCode == 200) {
        M5.Lcd.fillScreen(GREEN);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.println("SUCCESS!");
        M5.Lcd.printf("Code: %d\n", responseCode);
    } else if (responseCode == 302) {
        M5.Lcd.fillScreen(YELLOW);
        M5.Lcd.setTextColor(BLACK);
        M5.Lcd.println("REDIRECT!");
        M5.Lcd.printf("Code: %d\n", responseCode);
        M5.Lcd.println("Should be handled");
        M5.Lcd.println("automatically");
    } else {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.println("FAILED!");
        M5.Lcd.printf("Code: %d\n", responseCode);
    }

    M5.Lcd.println("");
    M5.Lcd.println("Sent Data:");
    M5.Lcd.printf("Grade: %s\n", grade.c_str());
    M5.Lcd.printf("Weight: %dg\n", weight);
    M5.Lcd.printf("Device: %d\n", testConfig.device_id);

    delay(3000);
}

// Display main screen
void displayMainScreen() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE);

    M5.Lcd.setCursor(60, 20);
    M5.Lcd.println("GAS Test Tool");

    M5.Lcd.setCursor(10, 60);
    if (wifiConnected) {
        M5.Lcd.setTextColor(GREEN);
        M5.Lcd.println("WiFi: Connected");
    } else {
        M5.Lcd.setTextColor(RED);
        M5.Lcd.println("WiFi: Disconnected");
    }

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(10, 90);
    M5.Lcd.printf("Device ID: %d", testConfig.device_id);

    M5.Lcd.setCursor(10, 120);
    M5.Lcd.printf("Grades: %d", testConfig.grade_names.size());

    M5.Lcd.setCursor(10, 160);
    M5.Lcd.println("Press [B] to send test data");

    M5.Lcd.setCursor(10, 210);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.println("[A]Reconnect [B]Send [C]Info");
}

void setup() {
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.println("GAS Test Starting...");

    // Initialize random seed
    randomSeed(analogRead(0));

    // Load configuration
    if (!loadTestConfig()) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 100);
        M5.Lcd.println("Setup Failed!");
        while(1) delay(1000);
    }

    // Connect WiFi
    wifiConnected = connectWiFi();

    // NTP setup
    if (wifiConnected) {
        configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");
        delay(2000); // Wait for NTP sync
    }

    displayMainScreen();
}

void loop() {
    M5.update();

    // Button A: WiFi reconnection
    if (M5.BtnA.wasPressed()) {
        wifiConnected = connectWiFi();
        displayMainScreen();
    }

    // Button B: Send test data (main function)
    if (M5.BtnB.wasPressed()) {
        if (!wifiConnected) {
            M5.Lcd.fillScreen(RED);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(10, 100);
            M5.Lcd.println("WiFi not connected!");
            delay(2000);
            displayMainScreen();
            return;
        }

        M5.Lcd.fillScreen(BLUE);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 100);
        M5.Lcd.println("Sending test data...");

        String grade, timestamp;
        int weight;
        generateTestData(grade, weight, timestamp);

        int responseCode = sendTestDataToGAS(grade, weight, timestamp);
        displayResult(responseCode, grade, weight);
        displayMainScreen();
    }

    // Button C: Information display
    if (M5.BtnC.wasPressed()) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("Configuration Info:");
        M5.Lcd.println("");
        M5.Lcd.printf("SSID: %s\n", testConfig.wifi_ssid.c_str());
        M5.Lcd.println("GAS URL:");
        M5.Lcd.printf("%s\n", testConfig.gas_url.c_str());
        M5.Lcd.printf("Device ID: %d\n", testConfig.device_id);
        M5.Lcd.println("");
        M5.Lcd.println("Available Grades:");
        for (int i = 0; i < testConfig.grade_names.size() && i < 8; i++) {
            M5.Lcd.printf("%s (%dg)\n", testConfig.grade_names[i].c_str(), testConfig.grade_weights[i]);
        }

        M5.Lcd.setCursor(10, 210);
        M5.Lcd.println("Press any button to return");

        // Wait for button press
        while (!M5.BtnA.wasPressed() && !M5.BtnB.wasPressed() && !M5.BtnC.wasPressed()) {
            M5.update();
            delay(50);
        }

        displayMainScreen();
    }

    delay(50);
}