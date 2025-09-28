/**
 * @file weight_i2c.ino
 * @brief TAL221ロードセル用の重量測定プログラム（M5Stack Core用）
 * Button A: オフセット設定
 * Button B: キャリブレーション
 * Button C: オフライン/オンライン切り替え
 */

#include <M5Stack.h>
#include "M5UnitWeightI2C.h"
#include "SPIFFS.h"
#include "Free_Fonts.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>

// 音階定義

// 基本設定
const float KNOWN_WEIGHT = 100.0; // キャリブレーション用の既知の重さ (g)
const char* CALIB_FILE = "/weight_calibration.txt";

// 音声設定
const int TONE_VOLUME = 3;      // 音量 (0-10)
const int BEEP_DURATION = 100;    // ビープ音の長さ (ms)
const int BEEP_INTERVAL = 50;     // ビープ音の間隔 (ms)

// 安定性検出のための設定
const int STABILITY_SAMPLES = 5;        // 安定性判断のためのサンプル数
const int STABILITY_INTERVAL = 200;      // サンプリング間隔(ms)
const float ZERO_THRESHOLD = 0.5;       // ゼロ判定のための閾値(g)
const int SAMPLES = 6 ;                 // 平均化のためのサンプル数

// 自動オフセット用の定数
const unsigned long AUTO_OFFSET_INTERVAL = 300000;  // 5分ごとに自動オフセット
const float AUTO_OFFSET_THRESHOLD = 1.2;          // この値以下なら自動オフセット実行
const unsigned long STABLE_TIME = 3000;           // 3秒間安定していること

// グローバル変数
unsigned long lastAutoOffset = 0;
unsigned long stableStartTime = 0;
bool isStableForOffset = false;

// 測定状態を表す列挙型
enum MeasurementState {
    STATE_READY,         // 測定準備完了
    STATE_MEASURING,     // 測定中
    STATE_STABLE,        // 測定値安定
    STATE_ZERO          // ゼロ状態
};



// 規格設定用の構造体（動的対応版）
struct GradeConfig {
    String name;           // 規格名（例："A", "6L", "特選"）
    int minWeight;         // 最小重量
    int maxWeight;         // 最大重量
    String soundFile;      // 音声ファイルパス
    uint16_t color;        // 背景色（RGB565形式）
};

// 製品設定用の構造体
struct ProductSettings {
    String productName;
    float minWeight;
    float maxWeight;
    float stabilityThreshold;
    std::vector<GradeConfig> grades;
};


// WAVファイル名の定義を拡張
const char* SYSTEM_WAV_FILES[] = {
    "/info.wav",   // システム情報、オフセット完了時
    "/zero.wav",   // ゼロ検出時
    "/error.wav"   // エラー時
};
// システムサウンド用のインデックス
enum SystemSound {
    SOUND_INFO,
    SOUND_ZERO,
    SOUND_ERROR
};

// グローバル製品設定変数
ProductSettings productSettings;

// 16進カラーコードをRGB565に変換する関数
uint16_t hexToRGB565(const String& hexColor) {
    if (hexColor.length() != 7 || hexColor[0] != '#') {
        return BLACK; // 無効な形式の場合は黒を返す
    }

    String hex = hexColor.substring(1); // '#'を除去
    long color = strtol(hex.c_str(), NULL, 16);

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // RGB888からRGB565に変換
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}



// 安定性検出用の構造体
struct WeightReading {
    float values[STABILITY_SAMPLES];
    int index;
    bool isFull;
    
    WeightReading() : index(0), isFull(false) {
        for (int i = 0; i < STABILITY_SAMPLES; i++) {
            values[i] = 0.0;
        }
    }
    
    void addReading(float value) {
        values[index] = value;
        index = (index + 1) % STABILITY_SAMPLES;
        if (index == 0) {
            isFull = true;
        }
    }
    
    bool isStable() {
        if (!isFull && index < 2) return false;
        
        float min_val = values[0];
        float max_val = values[0];
        int count = isFull ? STABILITY_SAMPLES : index;
        
        for (int i = 1; i < count; i++) {
            min_val = min(min_val, values[i]);
            max_val = max(max_val, values[i]);
        }
        
        return (max_val - min_val) <= productSettings.stabilityThreshold;
    }
    
    float getAverage() {
        float sum = 0;
        int count = isFull ? STABILITY_SAMPLES : index;
        if (count == 0) return 0;
        
        for (int i = 0; i < count; i++) {
            sum += values[i];
        }
        return sum / count;
    }
    
    void clear() {
        index = 0;
        isFull = false;
        for (int i = 0; i < STABILITY_SAMPLES; i++) {
            values[i] = 0.0;
        }
    }
};

// オーディオ再生タスク用の構造体
struct AudioMessage {
    const char* filename;
    bool play;
};

// グローバル変数
M5UnitWeightI2C weight_i2c;
WeightReading weightBuffer;
float calibration_factor = 1.0;   // キャリブレーション係数
float lastStableWeight = 0.0;
MeasurementState currentState = STATE_READY;
MeasurementState previousState = STATE_READY;
TFT_eSprite sprite = TFT_eSprite(&M5.Lcd);

AudioGeneratorWAV *wav = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

bool waitingForSpeakerReinit = false;
unsigned long speakerReinitTime = 0;
const unsigned long SPEAKER_REINIT_DELAY = 100; // 100ms待機

// マルチタスク用
TaskHandle_t audioTaskHandle = NULL;

// オーディオ再生用キュー
QueueHandle_t audioQueue;

// WAVファイル再生関数の修正
void playSystemSound(SystemSound sound) {
    const char* wavFile = SYSTEM_WAV_FILES[sound];
    if (wavFile != nullptr) {
        AudioMessage msg = {wavFile, true};
        // メッセージ送信（タイムアウト付き）
//        xQueueSend(audioQueue, &msg, pdMS_TO_TICKS(100));
    }
}
// オーディオ再生タスク
void audioTask(void* parameter) {
    AudioGeneratorWAV *wav_task = nullptr;
    AudioFileSourceSD *file_task = nullptr;
    AudioOutputI2S *out_task = new AudioOutputI2S(0, 1);  // M5Stack内蔵スピーカー用
    out_task->SetOutputModeMono(true);
    out_task->SetGain(0.8);

    AudioMessage msg;
    
    while(true) {
        // メッセージ待ち（ブロッキングモード）
        if (xQueueReceive(audioQueue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.play && msg.filename != nullptr) {
                // ファイルの存在確認
                if (!SD.exists(msg.filename)) {
                    continue;  // ファイルが存在しない場合はスキップ
                }

                // 既存の再生を停止
                if (wav_task != nullptr) {
                    wav_task->stop();
                    delete wav_task;
                    wav_task = nullptr;
                }
                if (file_task != nullptr) {
                    delete file_task;
                    file_task = nullptr;
                }

                // 新しい音声ファイルを再生
                file_task = new AudioFileSourceSD(msg.filename);
                wav_task = new AudioGeneratorWAV();
                
                if (wav_task->begin(file_task, out_task)) {
                    while(wav_task->isRunning()) {
                        if (!wav_task->loop()) {
                            wav_task->stop();
                            break;
                        }
                        vTaskDelay(1);  // 他のタスクに実行時間を与える
                    }
                }

                // クリーンアップ
                if (wav_task != nullptr) {
                    delete wav_task;
                    wav_task = nullptr;
                }
                if (file_task != nullptr) {
                    delete file_task;
                    file_task = nullptr;
                }
            }
        }
        vTaskDelay(1);  // タスクの実行間隔を調整
    }
}

// WAVファイルクラスの拡張（無音部分を追加）
class AudioFileSourceSDWithSilence : public AudioFileSourceSD {
public:
    AudioFileSourceSDWithSilence(const char* filename) : AudioFileSourceSD(filename) {
        // コンストラクタ
    }

    virtual bool open(const char* filename) {
        bool result = AudioFileSourceSD::open(filename);
        if (result) {
            // WAVヘッダーの後に無音部分を追加
            pos = 44;  // WAVヘッダーのサイズ
        }
        return result;
    }

    virtual uint32_t read(void *data, uint32_t len) {
        if (pos == 44) {
            // 先頭に短い無音部分を追加
            uint8_t* buffer = (uint8_t*)data;
            for (int i = 0; i < 100; i++) {
                buffer[i] = 0x80;  // 16bit PCMの中央値
            }
            pos += 100;
            return 100;
        }
        return AudioFileSourceSD::read(data, len);
    }

protected:
    size_t pos = 0;
};

// 規格に応じた音声再生（動的対応版）
void playGradeSound(const String& gradeName) {
    for (const auto& grade : productSettings.grades) {
        if (grade.name == gradeName) {
            const char* wavFile = grade.soundFile.c_str();
            AudioMessage msg = {wavFile, true};
            xQueueSend(audioQueue, &msg, 0);
            return;
        }
    }
}




// 規格に応じた背景色を返す関数（動的対応版）
uint16_t getGradeBackgroundColor(const String& gradeName) {
    for (const auto& grade : productSettings.grades) {
        if (grade.name == gradeName) {
            return grade.color;
        }
    }
    return BLACK;  // デフォルトは黒
}


// 規格を判定する関数（動的対応版）
String determineGrade(float weight) {
    // 最小重量未満は空文字列を返す
    if (weight < productSettings.minWeight) {
        return "";
    }

    for (const auto& grade : productSettings.grades) {
        if (weight >= grade.minWeight && weight <= grade.maxWeight) {
            return grade.name;
        }
    }
    return "ERR";  // エラー時
}


// SPIFFSからキャリブレーション係数を読み込む
void loadCalibrationFactor() {
    if (!SPIFFS.begin(true)) {
        M5.Lcd.println("SPIFFS Mount Failed");
        playSystemSound(SOUND_ERROR);  // エラー音に変更
        return;
    }

    if (SPIFFS.exists(CALIB_FILE)) {
        File file = SPIFFS.open(CALIB_FILE, "r");
        if (file) {
            String value = file.readStringUntil('\n');
            calibration_factor = value.toFloat();
            file.close();
            
            M5.Lcd.printf("Loaded cal: %0.4f\n", calibration_factor);
            delay(500);
        }
    }
}

// SPIFFSにキャリブレーション係数を保存
void saveCalibrationFactor() {
    File file = SPIFFS.open(CALIB_FILE, "w");
    if (file) {
        file.println(calibration_factor, 4);
        file.close();
        M5.Lcd.println("Calibration saved");
        playSystemSound(SOUND_INFO);  // 保存成功音に変更
        delay(1000);
    } else {
        M5.Lcd.println("Save failed");
        playSystemSound(SOUND_ERROR);  // エラー音に変更
        delay(1000);
    }
}

void setInitialOffset() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Set Offset");
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("Remove all weight");
    M5.Lcd.println("Please wait...");
    
    weight_i2c.setOffset();
    weightBuffer.clear();
    currentState = STATE_READY;
    previousState = STATE_READY;
    lastStableWeight = 0.0;
    
    M5.Lcd.println("Offset Complete!");
    playSystemSound(SOUND_INFO);  // オフセット完了時
    delay(1000);
}


void calibrate() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Calibrate");
    M5.Lcd.setTextSize(1);
    
    M5.Lcd.println("\nCalibration Steps:");
    M5.Lcd.println("1. Remove all weight");
    M5.Lcd.println("2. Wait for beep");
    M5.Lcd.printf("3. Place %0.1fg weight\n", KNOWN_WEIGHT);
    M5.Lcd.println("\nRecommended weight:");
    M5.Lcd.printf("100g - 300g best\n");
    M5.Lcd.printf("Current: %0.1fg\n", KNOWN_WEIGHT);
    
    delay(2000);
    
    M5.Lcd.println("\nMeasuring...");
    float measured = getAveragedWeight(20);
    calibration_factor = KNOWN_WEIGHT / measured;
    
    if (calibration_factor <= 0 || calibration_factor > 10.0) {
        M5.Lcd.println("Warning: Unusual cal value");
        M5.Lcd.println("Check weight placement");
        playSystemSound(SOUND_ERROR);
        delay(2000);
        return;
    }
    
    M5.Lcd.printf("Complete!\n");
    M5.Lcd.printf("Factor:%0.4f\n", calibration_factor);
    
    saveCalibrationFactor();
    playSystemSound(SOUND_INFO);
    
    weightBuffer.clear();
    currentState = STATE_READY;
    previousState = STATE_READY;
    lastStableWeight = 0.0;
    
    delay(2000);
}

float getAveragedWeight(int samples) {
    float sum = 0;
    
    // 最初の数回の読み取りを捨てる
    for (int i = 0; i < 3; i++) {
        weight_i2c.getWeight();
        delay(10);
    }
    
    // サンプリング
    for (int i = 0; i < samples; i++) {
        sum += weight_i2c.getWeight();
        delay(10);
    }
    
    return sum / samples;
}

float getAccurateWeight() {
    float rawWeight = getAveragedWeight(SAMPLES);
    float calibratedWeight = rawWeight * calibration_factor;
    
    // 1g未満は四捨五入
    calibratedWeight = round(calibratedWeight);
    
    // 0.5g未満は0に
    if (abs(calibratedWeight) < 0.5) {
        calibratedWeight = 0;
    }    
    return calibratedWeight;
}


void updateMeasurementState(float weight) {
    previousState = currentState;
    
    if (abs(weight) < ZERO_THRESHOLD) {
        currentState = STATE_ZERO;
    }
    else if (weightBuffer.isStable()) {
        if (currentState == STATE_MEASURING) {
            currentState = STATE_STABLE;
        }
    }
    else {
        currentState = STATE_MEASURING;
    }
    
    // 状態が変化した時のみ音声を再生
    if (currentState != previousState) {
        if (currentState == STATE_ZERO) {
            playSystemSound(SOUND_ZERO);  // ゼロ検出時
        }
    }
}

// GAS関連の設定
struct NetworkConfig {
    String ssid;
    String password;
    String gas_url;
    int device_id;
};

NetworkConfig networkConfig;
bool isOfflineMode = false;


// 設定ファイルを読み込む関数（エラー時処理中断版）
bool loadNetworkConfig() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Loading Config...");

    if (!SD.begin()) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("SD Card init failed");
        M5.Lcd.println("");
        M5.Lcd.println("Please check:");
        M5.Lcd.println("- SD card inserted");
        M5.Lcd.println("- SD card format");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    File configFile = SD.open("/config.json");
    if (!configFile) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("config.json not found");
        M5.Lcd.println("");
        M5.Lcd.println("Please check:");
        M5.Lcd.println("- config.json exists");
        M5.Lcd.println("- in SD card root");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("JSON parse failed");
        M5.Lcd.println("");
        M5.Lcd.printf("Error code: %s\n", error.c_str());
        M5.Lcd.println("");
        M5.Lcd.println("Check JSON syntax");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    // ネットワーク設定の読み込み
    if (!doc.containsKey("wifi") || !doc.containsKey("gas_url")) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("Missing network config");
        M5.Lcd.println("");
        M5.Lcd.println("Required fields:");
        M5.Lcd.println("- wifi");
        M5.Lcd.println("- gas_url");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    networkConfig.ssid = doc["wifi"]["ssid"].as<String>();
    networkConfig.password = doc["wifi"]["password"].as<String>();
    networkConfig.gas_url = doc["gas_url"].as<String>();
    networkConfig.device_id = doc["device_id"] | 1;

    // 製品設定の読み込み
    if (!doc.containsKey("product_settings")) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("Missing product_settings");
        M5.Lcd.println("");
        M5.Lcd.println("Required in config.json:");
        M5.Lcd.println("- product_settings");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    JsonObject productObj = doc["product_settings"];

    if (!productObj.containsKey("grades") || productObj["grades"].size() == 0) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("No grades defined");
        M5.Lcd.println("");
        M5.Lcd.println("At least one grade");
        M5.Lcd.println("must be defined in");
        M5.Lcd.println("product_settings");
        playSystemSound(SOUND_ERROR);
        while(1); // 処理中断
    }

    productSettings.productName = productObj["product_name"].as<String>();
    productSettings.minWeight = productObj["min_weight"] | 50.0;
    productSettings.maxWeight = productObj["max_weight"] | 1000.0;
    productSettings.stabilityThreshold = productObj["stability_threshold"] | 0.3;

    // 規格設定の読み込み
    productSettings.grades.clear();

    for (JsonObject grade : productObj["grades"].as<JsonArray>()) {
        if (!grade.containsKey("name") || !grade.containsKey("min_weight") ||
            !grade.containsKey("max_weight") || !grade.containsKey("sound_file") ||
            !grade.containsKey("color")) {
            M5.Lcd.fillScreen(RED);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setCursor(10, 10);
            M5.Lcd.println("ERROR:");
            M5.Lcd.println("Invalid grade config");
            M5.Lcd.println("");
            M5.Lcd.println("Each grade needs:");
            M5.Lcd.println("name, min_weight,");
            M5.Lcd.println("max_weight, sound_file,");
            M5.Lcd.println("color");
            playSystemSound(SOUND_ERROR);
            while(1); // 処理中断
        }

        GradeConfig gradeConfig;
        gradeConfig.name = grade["name"].as<String>();
        gradeConfig.minWeight = grade["min_weight"];
        gradeConfig.maxWeight = grade["max_weight"];
        gradeConfig.soundFile = grade["sound_file"].as<String>();

        String colorStr = grade["color"].as<String>();
        gradeConfig.color = hexToRGB565(colorStr);

        productSettings.grades.push_back(gradeConfig);
    }

    M5.Lcd.fillScreen(GREEN);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Config Loaded!");
    M5.Lcd.printf("Product: %s\n", productSettings.productName.c_str());
    M5.Lcd.printf("Grades: %d\n", productSettings.grades.size());
    playSystemSound(SOUND_INFO);
    delay(2000);

    return true;
}

// WiFi接続関数
void connectToWiFi() {
    if (isOfflineMode) return;
    
    WiFi.begin(networkConfig.ssid.c_str(), networkConfig.password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }
}

// GASにデータを送信する関数
void sendToGAS(const char* size, float weight) {
    if (isOfflineMode || WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(networkConfig.gas_url.c_str());
    http.addHeader("Content-Type", "application/json");

    // 現在時刻を取得（NTPは別途設定が必要）
    char timeString[20];
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        strftime(timeString, sizeof(timeString), "%Y/%m/%d %H:%M:%S", &timeinfo);
    } else {
        strcpy(timeString, "Time not set");
    }

    // JSONデータの作成
    StaticJsonDocument<200> doc;
    doc["size"] = size;
    doc["weight"] = weight;
    doc["timestamp"] = timeString;
    doc["device_id"] = networkConfig.device_id;

    String jsonString;
    serializeJson(doc, jsonString);

    int httpResponseCode = http.POST(jsonString);
    http.end();
}

// 自動オフセットのチェックと実行
void checkAndAutoOffset(float weight) {
    unsigned long currentTime = millis();
    
    // 重量が閾値以下で安定している場合
    if (abs(weight) < AUTO_OFFSET_THRESHOLD && weightBuffer.isStable()) {
        if (!isStableForOffset) {
            stableStartTime = currentTime;
            isStableForOffset = true;
        }
        
        // 一定時間安定していて、前回のオフセットから十分時間が経過
        if ((currentTime - stableStartTime) >= STABLE_TIME && 
            (currentTime - lastAutoOffset) >= AUTO_OFFSET_INTERVAL) {
            
            weight_i2c.setOffset();  // オフセットを実行
            lastAutoOffset = currentTime;
            
            // デバッグ用（必要に応じてコメントアウト）
            Serial.println("Auto offset executed");
        }
    } else {
        isStableForOffset = false;
    }
}

// 画面表示関数
void displayWeight(float weight) {
    static unsigned long lastUpdateTime = 0;
    const unsigned long UPDATE_INTERVAL = STABILITY_INTERVAL;
    
    unsigned long currentTime = millis();
    if (currentTime - lastUpdateTime >= UPDATE_INTERVAL) {
        weightBuffer.addReading(weight);
        updateMeasurementState(weight);
        
        sprite.fillSprite(BLACK);

        String grade = determineGrade(weight);
        // 最小重量未満の場合は何も表示しない
        if (weight >= productSettings.minWeight && currentState == STATE_STABLE) {
            if (previousState != STATE_STABLE) {
                playGradeSound(grade);
            }

            uint16_t bgColor = getGradeBackgroundColor(grade);
            sprite.fillRect(0, 0, 320, 180, bgColor);

            sprite.setFreeFont(FSSB24);
            sprite.setTextSize(4);
            sprite.setTextColor(WHITE, bgColor);
            int textWidth = sprite.textWidth(grade.c_str());
            int x = (320 - textWidth) / 2;
            sprite.setCursor(x, 150);
            sprite.print(grade.c_str());
            sprite.setTextFont(0);
        }
        
        // 重量表示（1g単位）
        sprite.setTextColor(WHITE, BLACK);
        sprite.setTextSize(3);
        char weightStr[20];
        sprintf(weightStr, "%dg", (int)weight);
        int textWidth = sprite.textWidth(weightStr);
        sprite.setCursor((320 - textWidth) / 2, 190);
        sprite.print(weightStr);
        
        // 状態表示
        sprite.setTextSize(2);
        sprite.setCursor(230, 200);
        if (weight < productSettings.minWeight) {
            // 最小重量未満の場合は特別な表示
            sprite.setTextColor(CYAN, BLACK);
            sprite.print("WAIT");
        } else {
            switch (currentState) {
                case STATE_MEASURING:
                    sprite.setTextColor(YELLOW, BLACK);
                    sprite.print("MEAS");
                    break;
                case STATE_STABLE:
                    sprite.setTextColor(GREEN, BLACK);
                    sprite.print("DONE");
                    break;
                case STATE_ZERO:
                    sprite.setTextColor(CYAN, BLACK);
                    sprite.print("ZERO");
                    break;
            }
        }
        
        // オーバーロード警告
        if (weight > productSettings.maxWeight) {
            sprite.setTextColor(RED, BLACK);
            sprite.setTextSize(3);
            textWidth = sprite.textWidth("OVER");
            sprite.setCursor((320 - textWidth) / 2, 140);
            sprite.print("OVER");
            
            static unsigned long lastWarningTime = 0;
            if (currentTime - lastWarningTime >= 1000) {
                playSystemSound(SOUND_ERROR);
                lastWarningTime = currentTime;
            }
        }
        

        // オフラインモード表示の追加（ボタンガイドの上）
        if (isOfflineMode) {
            sprite.fillRect(0, 195, 100, 25, RED);
            sprite.setTextColor(WHITE, RED);
            sprite.setTextSize(2);
            sprite.setCursor(10, 200);
            sprite.print("OFFLINE");
        }


        // ボタンガイド（変更なし）
        sprite.setTextColor(WHITE, BLACK);
        sprite.setTextSize(2);
        sprite.setCursor(15, 220);
        sprite.print("Offset");
        sprite.setCursor(130, 220);
        sprite.print("Calibrate");

        // デバイスID表示（最上位レイヤー）
        String deviceIdStr = String(networkConfig.device_id);
        int idWidth = sprite.textWidth(deviceIdStr.c_str()) + 8;
        int idHeight = 20;
        int idX = 320 - idWidth - 5;  // 右端から5px余白
        int idY = 5;  // 上端から5px余白

        // デバイスIDを黒文字で表示
        sprite.setTextColor(BLACK, WHITE);
        sprite.setTextSize(3);
        sprite.setCursor(idX + 4, idY + 6);
        sprite.print(deviceIdStr);

        sprite.pushSprite(0, 0);
        
        // 状態が安定してGASにまだ送信していない場合、データを送信
        static bool dataSent = false;
        if (currentState == STATE_STABLE && !dataSent && weight >= productSettings.minWeight) {
            String grade = determineGrade(weight);
            sendToGAS(grade.c_str(), weight);
            dataSent = true;
        } else if (currentState != STATE_STABLE) {
            dataSent = false;
        }


        lastUpdateTime = currentTime;
    }
}

void setup() {
     M5.begin();
    M5.Power.begin();
    
    // SDカードの初期化確認
    if (!SD.begin()) {
        M5.Lcd.println("SD Card Mount Failed");
        while (1);
    }

    // オーディオキューの作成（サイズを1に設定）
    audioQueue = xQueueCreate(1, sizeof(AudioMessage));
    
    // オーディオタスクの作成（優先度を上げる）
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        8192,
        NULL,
        2,  // 優先度を上げる（1→2）
        &audioTaskHandle,
        0    // Core 0で実行
    );

    // スプライトの初期化
    sprite.setColorDepth(8);
    sprite.createSprite(320, 240);
    sprite.setTextSize(2);

    // 起動画面表示
    sprite.fillSprite(BLACK);
    sprite.setCursor(10, 10);
    sprite.setTextColor(WHITE);
    sprite.println("Weight Sensor");
    sprite.setTextSize(1);
    sprite.println("\nBtn A: Offset");
    sprite.println("Btn B: Calibrate");
    sprite.pushSprite(0, 0);
    
    // Weight I2Cユニットの初期化
    while (!weight_i2c.begin(&Wire, 21, 22, DEVICE_DEFAULT_ADDR, 100000U)) {
        sprite.fillSprite(BLACK);
        sprite.println("weight i2c error");
        sprite.pushSprite(0, 0);
        playSystemSound(SOUND_ERROR); 
        delay(1000);
    }

    // 起動音を再生
    playSystemSound(SOUND_INFO);
    
    // 保存されたキャリブレーション係数を読み込む
    loadCalibrationFactor();

    // 設定ファイルの読み込み（エラー時は処理中断）
    loadNetworkConfig();

    // WiFi接続処理の追加
    connectToWiFi();

    // NTP設定
    configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");

    // 初期オフセットの設定
    delay(1000);
    setInitialOffset();
}

void loop() {
    M5.update();
    
    // WAV再生の処理
    if (wav != nullptr && wav->isRunning()) {
        if (!wav->loop()) {
            wav->stop();
            waitingForSpeakerReinit = true;
            speakerReinitTime = millis();
        }
    }

    // スピーカー再初期化の処理
    if (waitingForSpeakerReinit && (millis() - speakerReinitTime >= SPEAKER_REINIT_DELAY)) {
        M5.Speaker.begin();
        M5.Speaker.setVolume(TONE_VOLUME);
        waitingForSpeakerReinit = false;
    }

    // ボタンA（オフセット設定）
    if (M5.BtnA.wasPressed()) {
        setInitialOffset();
    }
    
    // ボタンB（キャリブレーション）
    if (M5.BtnB.wasPressed()) {
        calibrate();
    }

    // ボタンC（オフラインモード切り替え）
    if (M5.BtnC.wasPressed()) {
        isOfflineMode = !isOfflineMode;
        if (!isOfflineMode) {
            connectToWiFi(); // オンラインモードに戻す時にWiFi再接続
        }
    }

    float weight = getAccurateWeight();
    checkAndAutoOffset(weight);  // 自動オフセットのチェック
    displayWeight(weight);
    
    delay(50); // より短いディレイに変更
}
