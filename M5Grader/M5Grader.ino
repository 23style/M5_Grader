/**
 * @file M5Grader.ino
 * @brief TAL221ロードセル用の重量測定・規格分類プログラム（M5Stack Core用）
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

// 基本設定
const float KNOWN_WEIGHT = 100.0;
const char* CALIB_FILE = "/weight_calibration.txt";

// 音声設定
const int TONE_VOLUME = 3;

// 測定パラメータ（高速＋トリム平均で精度確保）
const int SAMPLES = 7;                    // 平均化サンプル数（min/max除去した中央5サンプル平均）
const int SAMPLE_INTERVAL_MS = 10;        // サンプル間隔
const int STABILITY_SAMPLES = 4;          // 安定性判定履歴数
const int STABILITY_INTERVAL = 100;       // 表示更新・安定判定周期(ms)
const float ZERO_THRESHOLD = 2.0;         // ゼロ判定閾値(g)
const int ZERO_DEBOUNCE_COUNT = 3;        // ZERO遷移に必要な連続near-zero回数（触れによる誤遷移防止）

// 確認ダイアログ
const unsigned long CONFIRM_TIMEOUT_MS = 10000;  // 10秒無操作でNo扱い

// ドリフト補正（適応的ゼロトラッキング）
const float SOFT_ZERO_ALPHA = 0.1;              // ゼロ追従EMA係数（0-1、小さいほど緩やか）
const float HW_OFFSET_TRIGGER = 3.0;            // ソフトゼロがこの値を超えたらハードオフセット
const unsigned long HW_OFFSET_STABLE_MS = 30000;// ゼロ付近安定でハードオフセット実行までの時間
const unsigned long HW_OFFSET_MIN_INTERVAL = 5000; // ハードオフセット最小間隔

// 測定状態
enum MeasurementState {
    STATE_MEASURING,
    STATE_STABLE,
    STATE_ZERO
};

// 規格設定
struct GradeConfig {
    String name;
    int minWeight;
    int maxWeight;
    String soundFile;
    uint16_t color;
};

// 製品設定
struct ProductSettings {
    String productName;
    float minWeight;
    float maxWeight;
    float stabilityThreshold;
    std::vector<GradeConfig> grades;
};

// ネットワーク設定
struct NetworkConfig {
    String ssid;
    String password;
    String gas_url;
    int device_id;
};

// 安定性検出バッファ
struct WeightReading {
    float values[STABILITY_SAMPLES];
    int index;
    bool isFull;

    WeightReading() : index(0), isFull(false) {
        for (int i = 0; i < STABILITY_SAMPLES; i++) values[i] = 0.0;
    }

    void addReading(float value) {
        values[index] = value;
        index = (index + 1) % STABILITY_SAMPLES;
        if (index == 0) isFull = true;
    }

    bool isStable(float threshold) {
        if (!isFull && index < 2) return false;
        float min_val = values[0];
        float max_val = values[0];
        int count = isFull ? STABILITY_SAMPLES : index;
        for (int i = 1; i < count; i++) {
            min_val = min(min_val, values[i]);
            max_val = max(max_val, values[i]);
        }
        return (max_val - min_val) <= threshold;
    }

    // バッファ内平均。ラッチ確定時、丸め境界での±1g誤差を排除するために使用
    float average() {
        int count = isFull ? STABILITY_SAMPLES : index;
        if (count == 0) return 0.0;
        float sum = 0.0;
        for (int i = 0; i < count; i++) sum += values[i];
        return sum / count;
    }

    void clear() {
        index = 0;
        isFull = false;
        for (int i = 0; i < STABILITY_SAMPLES; i++) values[i] = 0.0;
    }
};

// オーディオメッセージ
struct AudioMessage {
    const char* filename;
};

// ログファイル関連
const char* LOG_FILE = "/measurements.csv";               // 現在の記録先
const char* LOG_ARCHIVE_PREFIX = "/measurements_arc_";     // アーカイブファイル接頭辞
const char* LOG_ARCHIVE_SUFFIX = ".csv";
const char* LOG_RETRY_FILE = "/measurements_retry.csv";    // アップロード失敗行の再送用
const char* LOG_UPLOAD_PENDING_PREFIX = "/measurements_pending_"; // アップロード処理中の一時ファイル

// レコード件数しきい値
const int RECORD_WARN_YELLOW = 8000;   // Upload文字を黄色表示
const int RECORD_WARN_RED = 10000;     // Upload文字を赤表示＋自動ローテーション
const int BATCH_SIZE = 50;             // 1POSTあたりのレコード数

// グローバル変数
M5UnitWeightI2C weight_i2c;
WeightReading weightBuffer;
ProductSettings productSettings;
NetworkConfig networkConfig;
TFT_eSprite sprite = TFT_eSprite(&M5.Lcd);
TaskHandle_t audioTaskHandle = NULL;
QueueHandle_t audioQueue;

float calibration_factor = 1.0;
float softZeroOffset = 0.0;              // ソフトウェアゼロ補正値
unsigned long zeroStableStartTime = 0;   // ゼロ付近安定開始時刻
unsigned long lastHwOffsetTime = 0;      // 前回ハードオフセット時刻
bool isZeroStable = false;

MeasurementState currentState = STATE_ZERO;
bool wasStable = false;

// 記録機能とレコード管理
bool recordingEnabled = true;      // 起動時にconfig.jsonから読み込む
int pendingRecordCount = 0;        // 未アップロードの総レコード数（表示・警告用）
int rotationCounter = 0;           // 次のアーカイブ番号

// アップロード失敗時のデバッグ用（M5画面に表示）
int lastUploadHttpCode = 0;
String lastUploadResponse = "";

// ラッチ表示用（安定確定→ゼロ復帰までの結果保持）
String latchedGrade = "";
float latchedWeight = 0.0;

// 16進カラーコードをRGB565に変換
uint16_t hexToRGB565(const String& hexColor) {
    if (hexColor.length() != 7 || hexColor[0] != '#') return BLACK;
    long color = strtol(hexColor.substring(1).c_str(), NULL, 16);
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// 致命的エラー表示（処理中断）
void showFatalError(const char* title, const std::vector<const char*>& lines) {
    M5.Lcd.fillScreen(RED);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("ERROR:");
    M5.Lcd.println(title);
    M5.Lcd.println("");
    for (auto line : lines) M5.Lcd.println(line);
    while (1);
}

// オーディオタスク
void audioTask(void* parameter) {
    AudioGeneratorWAV *wav_task = nullptr;
    AudioFileSourceSD *file_task = nullptr;
    AudioOutputI2S *out_task = new AudioOutputI2S(0, 1);
    out_task->SetOutputModeMono(true);
    out_task->SetGain(0.8);

    AudioMessage msg;
    while (true) {
        if (xQueueReceive(audioQueue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.filename == nullptr || !SD.exists(msg.filename)) continue;

            if (wav_task != nullptr) { wav_task->stop(); delete wav_task; wav_task = nullptr; }
            if (file_task != nullptr) { delete file_task; file_task = nullptr; }

            file_task = new AudioFileSourceSD(msg.filename);
            wav_task = new AudioGeneratorWAV();

            if (wav_task->begin(file_task, out_task)) {
                while (wav_task->isRunning()) {
                    if (!wav_task->loop()) { wav_task->stop(); break; }
                    vTaskDelay(1);
                }
            }

            if (wav_task != nullptr) { delete wav_task; wav_task = nullptr; }
            if (file_task != nullptr) { delete file_task; file_task = nullptr; }
        }
        vTaskDelay(1);
    }
}

// 規格判定
String determineGrade(float weight) {
    if (weight < productSettings.minWeight) return "";
    for (const auto& grade : productSettings.grades) {
        if (weight >= grade.minWeight && weight <= grade.maxWeight) return grade.name;
    }
    return "ERR";
}

// 規格から背景色を取得
uint16_t getGradeBackgroundColor(const String& gradeName) {
    for (const auto& grade : productSettings.grades) {
        if (grade.name == gradeName) return grade.color;
    }
    return BLACK;
}

// 規格音声再生
void playGradeSound(const String& gradeName) {
    for (const auto& grade : productSettings.grades) {
        if (grade.name == gradeName) {
            AudioMessage msg = {grade.soundFile.c_str()};
            xQueueSend(audioQueue, &msg, 0);
            return;
        }
    }
}

// SPIFFSからキャリブレーション係数を読み込み
void loadCalibrationFactor() {
    if (!SPIFFS.begin(true)) {
        M5.Lcd.println("SPIFFS Mount Failed");
        return;
    }
    if (SPIFFS.exists(CALIB_FILE)) {
        File file = SPIFFS.open(CALIB_FILE, "r");
        if (file) {
            calibration_factor = file.readStringUntil('\n').toFloat();
            file.close();
            M5.Lcd.printf("Loaded cal: %0.4f\n", calibration_factor);
            delay(500);
        }
    }
}

// キャリブレーション係数を保存
void saveCalibrationFactor() {
    File file = SPIFFS.open(CALIB_FILE, "w");
    if (file) {
        file.println(calibration_factor, 4);
        file.close();
        M5.Lcd.println("Calibration saved");
        delay(1000);
    } else {
        M5.Lcd.println("Save failed");
        delay(1000);
    }
}

// 平均化した生重量を取得（キャリブレーション用：捨て読みあり）
float getStableRawWeight(int samples) {
    for (int i = 0; i < 3; i++) { weight_i2c.getWeight(); delay(10); }
    float sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += weight_i2c.getWeight();
        delay(10);
    }
    return sum / samples;
}

// SAMPLES個のサンプルを取り、min/maxを除いた中央値の平均（トリム平均）を返す
float readTrimmedMeanRaw() {
    float samples[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        samples[i] = weight_i2c.getWeight();
        if (i < SAMPLES - 1) delay(SAMPLE_INTERVAL_MS);
    }

    // min/max を検出
    float minV = samples[0], maxV = samples[0];
    int minIdx = 0, maxIdx = 0;
    for (int i = 1; i < SAMPLES; i++) {
        if (samples[i] < minV) { minV = samples[i]; minIdx = i; }
        if (samples[i] > maxV) { maxV = samples[i]; maxIdx = i; }
    }

    // min/max以外の平均
    float sum = 0;
    int count = 0;
    for (int i = 0; i < SAMPLES; i++) {
        if (i == minIdx || i == maxIdx) continue;
        sum += samples[i];
        count++;
    }
    return (count > 0) ? (sum / count) : samples[0];
}

// 状態リセット
void resetMeasurementState() {
    weightBuffer.clear();
    currentState = STATE_ZERO;
    wasStable = false;
    isZeroStable = false;
    softZeroOffset = 0.0;
    latchedGrade = "";
    latchedWeight = 0.0;
}

// オフセット実行本体（確認済み前提）
void executeOffset() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Set Offset");
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("Remove all weight");
    M5.Lcd.println("Please wait...");

    weight_i2c.setOffset();
    resetMeasurementState();
    lastHwOffsetTime = millis();

    M5.Lcd.println("Offset Complete!");
    delay(1000);
}

// オフセット設定（確認ダイアログ経由）
// A=Yes / B・C=No（全ダイアログで統一）
void setInitialOffset() {
    if (!confirmDialog("Set Offset?", "Reset the zero point?", 'A')) {
        showCancelled("Offset");
        return;
    }
    executeOffset();
}

// 全ボタンが離されるまで待機し、未処理イベントを消費
// ダイアログから抜けた直後にメインループが同じボタンを再検出する不具合を防ぐ
void drainButtonEvents() {
    delay(50);
    while (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
        M5.update();
        delay(10);
    }
    M5.update();
    (void)M5.BtnA.wasPressed();
    (void)M5.BtnB.wasPressed();
    (void)M5.BtnC.wasPressed();
}

// 汎用確認ダイアログ
// yesBtn: 'A', 'B', 'C' のいずれか（Yesとして扱うボタン）
// その他のボタン押下＆タイムアウトはNo扱い
// 戻り値: true=Yes, false=No
bool confirmDialog(const char* title, const char* message, char yesBtn) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(20, 20);
    M5.Lcd.println(title);

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(YELLOW, BLACK);
    M5.Lcd.setCursor(20, 80);
    M5.Lcd.println(message);

    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setCursor(20, 115);
    M5.Lcd.println("Default: No");
    M5.Lcd.setCursor(20, 140);
    M5.Lcd.printf("Auto cancel in %ds", CONFIRM_TIMEOUT_MS / 1000);

    // ボタンガイド（Yes=緑、No=白背景で強調）
    const char* labels[3] = {"A", "B", "C"};
    const int xPos[3] = {20, 130, 240};
    for (int i = 0; i < 3; i++) {
        bool isYes = (labels[i][0] == yesBtn);
        char text[16];
        snprintf(text, sizeof(text), "%s:%s", labels[i], isYes ? "Yes" : "No");
        if (isYes) {
            M5.Lcd.setTextColor(GREEN, BLACK);
            M5.Lcd.setCursor(xPos[i], 200);
        } else {
            M5.Lcd.fillRect(xPos[i] - 5, 195, 80, 30, WHITE);
            M5.Lcd.setTextColor(BLACK, WHITE);
            M5.Lcd.setCursor(xPos[i], 200);
        }
        M5.Lcd.setTextSize(2);
        M5.Lcd.print(text);
    }

    bool result = false;
    unsigned long start = millis();
    while (millis() - start < CONFIRM_TIMEOUT_MS) {
        M5.update();
        bool aPressed = M5.BtnA.wasPressed();
        bool bPressed = M5.BtnB.wasPressed();
        bool cPressed = M5.BtnC.wasPressed();
        if (yesBtn == 'A' && aPressed) { result = true; break; }
        if (yesBtn == 'B' && bPressed) { result = true; break; }
        if (yesBtn == 'C' && cPressed) { result = true; break; }
        if ((yesBtn != 'A' && aPressed) ||
            (yesBtn != 'B' && bPressed) ||
            (yesBtn != 'C' && cPressed)) {
            result = false; break;
        }
        delay(20);
    }
    drainButtonEvents();
    return result;
}

// キャンセル画面
void showCancelled(const char* label) {
    M5.Lcd.fillScreen(BLUE);
    M5.Lcd.setTextColor(WHITE, BLUE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(30, 100);
    M5.Lcd.println(label);
    M5.Lcd.setCursor(30, 130);
    M5.Lcd.println("Cancelled");
    delay(1000);
}

// メニュー画面
// A=Back / B=Down / C=Select、10秒無操作でBack
// 前方宣言（メニュー内から呼び出す）
void uploadLog();
void calibrate();
void saveRecordingFlag(bool enabled);

void showMenu() {
    const int MENU_ITEMS = 4;
    int selected = 0;
    unsigned long lastActivity = millis();
    bool needsRedraw = true;

    while (true) {
        if (millis() - lastActivity >= 10000) {
            drainButtonEvents();
            return;
        }

        if (needsRedraw) {
            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setTextColor(WHITE, BLACK);
            M5.Lcd.setTextSize(3);
            M5.Lcd.setCursor(100, 10);
            M5.Lcd.println("Menu");

            for (int i = 0; i < MENU_ITEMS; i++) {
                const char* label;
                switch (i) {
                    case 0: label = "Upload"; break;
                    case 1: label = recordingEnabled ? "Record: ON" : "Record: OFF"; break;
                    case 2: label = "Calibrate"; break;
                    case 3: label = "Back"; break;
                    default: label = "";
                }
                int y = 60 + i * 32;
                if (i == selected) {
                    M5.Lcd.fillRect(20, y - 3, 280, 28, WHITE);
                    M5.Lcd.setTextColor(BLACK, WHITE);
                } else {
                    M5.Lcd.fillRect(20, y - 3, 280, 28, BLACK);
                    M5.Lcd.setTextColor(WHITE, BLACK);
                }
                M5.Lcd.setTextSize(2);
                M5.Lcd.setCursor(30, y);
                M5.Lcd.print(label);
            }

            // ボタンガイド
            M5.Lcd.setTextColor(WHITE, BLACK);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setCursor(15, 220);
            M5.Lcd.print("Back");
            M5.Lcd.setCursor(135, 220);
            M5.Lcd.print("Down");
            M5.Lcd.setCursor(250, 220);
            M5.Lcd.print("Sel");
            needsRedraw = false;
        }

        M5.update();
        if (M5.BtnA.wasPressed()) {
            drainButtonEvents();
            return;
        }
        if (M5.BtnB.wasPressed()) {
            selected = (selected + 1) % MENU_ITEMS;
            lastActivity = millis();
            needsRedraw = true;
        }
        if (M5.BtnC.wasPressed()) {
            drainButtonEvents();
            lastActivity = millis();
            switch (selected) {
                case 0: uploadLog(); break;
                case 1: saveRecordingFlag(!recordingEnabled); break;
                case 2: calibrate(); break;
                case 3: return;
            }
            needsRedraw = true;
        }

        delay(20);
    }
}

// カウントダウン表示（3→2→1）
void countdownForCalibration() {
    for (int i = 3; i >= 1; i--) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextColor(WHITE, BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(30, 30);
        M5.Lcd.println("Remove all weight");
        M5.Lcd.setCursor(30, 60);
        M5.Lcd.println("Starting in...");

        M5.Lcd.setTextSize(8);
        M5.Lcd.setTextColor(YELLOW, BLACK);
        M5.Lcd.setCursor(130, 110);
        M5.Lcd.printf("%d", i);

        M5.Speaker.tone(1500, 100);
        delay(1000);
    }
}

// キャリブレーション実行本体
void executeCalibration() {
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
    float measured = getStableRawWeight(20);
    calibration_factor = KNOWN_WEIGHT / measured;

    if (calibration_factor <= 0 || calibration_factor > 10.0) {
        M5.Lcd.println("Warning: Unusual cal value");
        M5.Lcd.println("Check weight placement");
        delay(2000);
        return;
    }

    M5.Lcd.printf("Complete!\n");
    M5.Lcd.printf("Factor:%0.4f\n", calibration_factor);
    saveCalibrationFactor();
    resetMeasurementState();
    delay(2000);
}

// キャリブレーション（確認→カウントダウン→実行）
// A=Yes / B・C=No（全ダイアログで統一）
void calibrate() {
    if (!confirmDialog("Calibrate?", "Run calibration?", 'A')) {
        showCancelled("Calibration");
        return;
    }
    countdownForCalibration();
    executeCalibration();
}

// 測定状態更新
// STATE_STABLE に一度入ったら、STATE_ZERO（=秤から降ろす）になるまで保持する
// 触れによる一瞬の値落ちで誤遷移しないよう、ZERO判定は連続 ZERO_DEBOUNCE_COUNT 回でデバウンス
void updateMeasurementState(float weight) {
    static int zeroCount = 0;

    if (abs(weight) < ZERO_THRESHOLD) {
        zeroCount++;
        if (zeroCount >= ZERO_DEBOUNCE_COUNT) {
            currentState = STATE_ZERO;
        }
        // デバウンス期間中は状態を変えない（ラッチ中ならSTABLE維持）
    } else {
        zeroCount = 0;
        if (currentState == STATE_STABLE) {
            // ラッチ：STABLE中は物体を降ろすまで保持（微振動でMEASURINGに戻さない）
        } else if (weightBuffer.isStable(productSettings.stabilityThreshold)) {
            if (currentState == STATE_MEASURING) currentState = STATE_STABLE;
        } else {
            currentState = STATE_MEASURING;
        }
    }
}

// 適応的ゼロトラッキング（ドリフト補正）
void updateZeroTracking(float displayedWeight, float rawCalibratedWeight) {
    unsigned long now = millis();

    // ゼロ付近で安定している場合のみ追従
    bool nearZero = abs(displayedWeight) < ZERO_THRESHOLD;
    bool stable = weightBuffer.isStable(productSettings.stabilityThreshold);

    if (nearZero && stable) {
        // ソフトゼロを指数移動平均で追従（rawCalibrated がゼロに近づくよう補正値を調整）
        softZeroOffset += SOFT_ZERO_ALPHA * (rawCalibratedWeight - softZeroOffset);

        if (!isZeroStable) {
            zeroStableStartTime = now;
            isZeroStable = true;
        }

        bool overThreshold = abs(softZeroOffset) > HW_OFFSET_TRIGGER;
        bool longStable = (now - zeroStableStartTime) >= HW_OFFSET_STABLE_MS;
        bool intervalOk = (now - lastHwOffsetTime) >= HW_OFFSET_MIN_INTERVAL;

        if (intervalOk && (overThreshold || longStable)) {
            weight_i2c.setOffset();
            softZeroOffset = 0.0;
            lastHwOffsetTime = now;
            zeroStableStartTime = now;
        }
    } else {
        isZeroStable = false;
    }
}

// 前方宣言（loadNetworkConfig 内から呼び出す）
void showRecordOffAutoDisabled(const char* reason);

// 設定ファイル読み込み
void loadNetworkConfig() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Loading Config...");

    if (!SD.begin()) {
        showFatalError("SD Card init failed", {"Please check:", "- SD card inserted", "- SD card format"});
    }

    File configFile = SD.open("/config.json");
    if (!configFile) {
        showFatalError("config.json not found", {"Please check:", "- config.json exists", "- in SD card root"});
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
        while (1);
    }

    // ネットワーク設定不足時は Record OFF に自動切替（起動継続、後段で警告表示）
    // wifi/gas_url が欠けている場合、空文字で初期化しておく
    bool hasNetworkConfig = doc.containsKey("wifi") && doc.containsKey("gas_url");

    if (hasNetworkConfig) {
        networkConfig.ssid = doc["wifi"]["ssid"].as<String>();
        networkConfig.password = doc["wifi"]["password"].as<String>();
        networkConfig.gas_url = doc["gas_url"].as<String>();
    } else {
        networkConfig.ssid = "";
        networkConfig.password = "";
        networkConfig.gas_url = "";
    }
    networkConfig.device_id = doc["device_id"] | 1;

    // 記録ON/OFFフラグ（デフォルトON）。ネットワーク設定不足時は強制OFF
    recordingEnabled = doc["recording_enabled"] | true;
    if (!hasNetworkConfig) recordingEnabled = false;

    if (!doc.containsKey("product_settings")) {
        showFatalError("Missing product_settings", {"Required in config.json:", "- product_settings"});
    }

    JsonObject productObj = doc["product_settings"];
    if (!productObj.containsKey("grades") || productObj["grades"].size() == 0) {
        showFatalError("No grades defined", {"At least one grade", "must be defined in", "product_settings"});
    }

    productSettings.productName = productObj["product_name"].as<String>();
    productSettings.minWeight = productObj["min_weight"] | 50.0;
    productSettings.maxWeight = productObj["max_weight"] | 1000.0;
    productSettings.stabilityThreshold = productObj["stability_threshold"] | 0.3;
    productSettings.grades.clear();

    for (JsonObject grade : productObj["grades"].as<JsonArray>()) {
        if (!grade.containsKey("name") || !grade.containsKey("min_weight") ||
            !grade.containsKey("max_weight") || !grade.containsKey("sound_file") ||
            !grade.containsKey("color")) {
            showFatalError("Invalid grade config", {"Each grade needs:", "name, min_weight,", "max_weight, sound_file,", "color"});
        }

        GradeConfig gc;
        gc.name = grade["name"].as<String>();
        gc.minWeight = grade["min_weight"];
        gc.maxWeight = grade["max_weight"];
        gc.soundFile = grade["sound_file"].as<String>();
        gc.color = hexToRGB565(grade["color"].as<String>());
        productSettings.grades.push_back(gc);
    }

    M5.Lcd.fillScreen(GREEN);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Config Loaded!");
    M5.Lcd.printf("Product: %s\n", productSettings.productName.c_str());
    M5.Lcd.printf("Grades: %d\n", productSettings.grades.size());
    delay(2000);

    // ネットワーク設定不足時：Record OFF自動切替の警告
    if (!hasNetworkConfig) {
        showRecordOffAutoDisabled("Network config missing");
    }
}

// WiFi接続（アップロード時のみ使用）
// 成功時 true、失敗時 false（呼び出し側で結果表示）
bool connectToWiFi() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Connecting WiFi...");
    M5.Lcd.printf("SSID: %s\n", networkConfig.ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(networkConfig.ssid.c_str(), networkConfig.password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        M5.Lcd.print(".");
        delay(500);
        attempts++;
    }
    return (WiFi.status() == WL_CONNECTED);
}

// WiFi切断
void disconnectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// Record OFF自動切替の警告表示（3秒）
// 起動時のネットワーク設定不足／WiFi接続失敗で共通利用
void showRecordOffAutoDisabled(const char* reason) {
    M5.Lcd.fillScreen(RED);
    M5.Lcd.setTextColor(WHITE, RED);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("WARNING:");
    M5.Lcd.println(reason);
    M5.Lcd.println("");
    M5.Lcd.println("Record: OFF");
    M5.Lcd.println("(auto disabled)");
    delay(3000);
}

// 起動時のNTP同期試行
// 戻り値: WiFi接続に成功したら true、失敗したら false（呼び出し側でRecord OFFに切替）
// 設計書 4.3.3 に基づき、NTP同期失敗はBoot+Nmsフォールバックで継続、WiFi接続失敗はRecord OFFに切替
bool syncTimeAtBoot() {
    if (!connectToWiFi()) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        showRecordOffAutoDisabled("WiFi failed");
        return false;
    }

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Syncing time...");

    configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");

    struct tm timeinfo;
    unsigned long start = millis();
    bool synced = false;
    while (millis() - start < 5000) {
        if (getLocalTime(&timeinfo, 100)) {
            synced = true;
            break;
        }
    }

    if (synced) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &timeinfo);
        M5.Lcd.setCursor(10, 40);
        M5.Lcd.println("OK:");
        M5.Lcd.println(buf);
    } else {
        M5.Lcd.setCursor(10, 40);
        M5.Lcd.println("NTP failed");
    }
    delay(1200);

    disconnectWiFi();
    return true;
}

// 現在時刻を文字列化（NTP同期していれば実時刻、そうでなければ Boot+Nms 形式）
void formatTimestamp(char* buf, size_t bufSize) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        strftime(buf, bufSize, "%Y/%m/%d %H:%M:%S", &timeinfo);
    } else {
        snprintf(buf, bufSize, "Boot+%lums", millis());
    }
}

// あるファイルの行数（末尾改行なしにも耐える）を数える
int countLinesInFile(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    int count = 0;
    int lastChar = 0;
    while (f.available()) {
        int c = f.read();
        if (c == '\n') count++;
        lastChar = c;
    }
    f.close();
    if (lastChar != 0 && lastChar != '\n') count++;  // 最終行の改行なしを補正
    return count;
}

// あるパスがアップロード対象CSVファイルかを判定
// measurements.csv, measurements_arc_*.csv, measurements_retry.csv, measurements_pending_*.csv
bool isPendingLogFile(const String& fname) {
    if (fname == "measurements.csv") return true;
    if (fname == "measurements_retry.csv") return true;
    if (fname.startsWith("measurements_arc_") && fname.endsWith(".csv")) return true;
    if (fname.startsWith("measurements_pending_") && fname.endsWith(".csv")) return true;
    return false;
}

// SDカードルートを走査し、ペンディングファイルのパス一覧を返す
std::vector<String> listPendingLogFiles() {
    std::vector<String> result;
    File root = SD.open("/");
    if (!root) return result;
    File f = root.openNextFile();
    while (f) {
        String name = f.name();
        // ルート直下のパスは "/xxx" 形式で返ることがあるので接頭辞を除去
        int slash = name.lastIndexOf('/');
        String bare = (slash >= 0) ? name.substring(slash + 1) : name;
        if (!f.isDirectory() && isPendingLogFile(bare)) {
            result.push_back(String("/") + bare);
        }
        f = root.openNextFile();
    }
    root.close();
    return result;
}

// 全ペンディングファイルの合計行数を取得（起動時カウント用）
int countAllPendingRecords() {
    int total = 0;
    for (auto& p : listPendingLogFiles()) total += countLinesInFile(p.c_str());
    return total;
}

// 現在の measurements.csv をアーカイブにリネーム（ローテーション）
void rotateCurrentLog() {
    if (!SD.exists(LOG_FILE)) return;
    // 空き番号を探す
    for (int i = 0; i < 10000; i++) {
        int n = rotationCounter++;
        char arcPath[64];
        snprintf(arcPath, sizeof(arcPath), "%s%d%s", LOG_ARCHIVE_PREFIX, n, LOG_ARCHIVE_SUFFIX);
        if (!SD.exists(arcPath)) {
            SD.rename(LOG_FILE, arcPath);
            return;
        }
    }
}

// 起動時に既存アーカイブ番号の最大値を調べて rotationCounter を初期化
void initRotationCounter() {
    int maxN = -1;
    for (auto& p : listPendingLogFiles()) {
        // p 例: "/measurements_arc_3.csv"
        String prefix = LOG_ARCHIVE_PREFIX;
        if (p.startsWith(prefix)) {
            String tail = p.substring(prefix.length());
            int dot = tail.indexOf('.');
            if (dot > 0) {
                int n = tail.substring(0, dot).toInt();
                if (n > maxN) maxN = n;
            }
        }
    }
    rotationCounter = maxN + 1;
}

// Record ON切替時のWiFi失敗を赤画面で通知（Cボタンで閉じる／最大30秒）
void showRecordEnableWifiError() {
    M5.Lcd.fillScreen(RED);
    M5.Lcd.setTextColor(WHITE, RED);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("ERROR:");
    M5.Lcd.println("WiFi failed");
    M5.Lcd.println("");
    M5.Lcd.printf("SSID: %s\n", networkConfig.ssid.c_str());
    M5.Lcd.println("");
    M5.Lcd.println("Record stays OFF.");
    M5.Lcd.setCursor(10, 210);
    M5.Lcd.print("C: close");

    drainButtonEvents();
    unsigned long start = millis();
    while (millis() - start < 30000) {
        M5.update();
        if (M5.BtnC.wasPressed()) break;
        delay(20);
    }
    drainButtonEvents();
}

// config.json の recording_enabled フィールドを更新して書き戻す
// OFF→ON 切替時はWiFi接続+NTP同期を試行し、失敗時は OFF のまま維持（config.jsonも更新しない）
void saveRecordingFlag(bool enabled) {
    // OFF→ON 切替：WiFi接続+NTP同期を先に試みる
    if (enabled && !recordingEnabled) {
        if (!connectToWiFi()) {
            disconnectWiFi();
            showRecordEnableWifiError();
            return;  // recordingEnabled は false のまま、config.json も更新しない
        }

        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("Syncing time...");

        configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");
        struct tm timeinfo;
        unsigned long start = millis();
        while (millis() - start < 5000) {
            if (getLocalTime(&timeinfo, 100)) break;
        }
        disconnectWiFi();
    }

    File src = SD.open("/config.json", FILE_READ);
    if (!src) return;
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, src);
    src.close();
    if (err) return;

    doc["recording_enabled"] = enabled;

    // 一時ファイルに書いてからリネームで安全に置換
    const char* tmpPath = "/config.json.tmp";
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) return;
    serializeJsonPretty(doc, out);
    out.close();

    SD.remove("/config.json");
    SD.rename(tmpPath, "/config.json");

    recordingEnabled = enabled;
}

// 計量結果をSDカードのCSVファイルに追記
// 記録OFF時は何もしない。RECORD_WARN_RED 到達で自動ローテーション。
void appendToLog(const char* size, float weight) {
    if (!recordingEnabled) return;

    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) return;

    char timestamp[24];
    formatTimestamp(timestamp, sizeof(timestamp));

    f.printf("%s,%s,%.1f,%d\n", timestamp, size, weight, networkConfig.device_id);
    f.close();

    pendingRecordCount++;

    // 現在ファイルが赤しきい値の粒度に達したらローテーション
    // 現在ファイルの件数 = pendingRecordCount % RECORD_WARN_RED では厳密でないため、
    // 独立にカウント：ここではファイル行数で判断する（頻繁ではないので許容）
    if (pendingRecordCount > 0 && (pendingRecordCount % RECORD_WARN_RED) == 0) {
        rotateCurrentLog();
    }
}

// 複数レコードをまとめてPOST
// GAS側は { device_id, records: [{timestamp, size, weight}, ...] } を受ける想定
//
// リダイレクトは手動で処理する：
//  ESP32 HTTPClient は Google が返す ~450文字の Location URL への
//  再POSTに失敗して 400 を返すため、DISABLE_FOLLOW にして自分で
//  Location URL に GET し直す（Python の requests と同じ挙動）
bool postBatch(const std::vector<String>& lines) {
    if (lines.empty()) return true;

    // === JSON 組み立て ===
    DynamicJsonDocument doc(16384);
    doc["device_id"] = networkConfig.device_id;
    JsonArray records = doc.createNestedArray("records");
    for (auto& line : lines) {
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        if (c1 < 0 || c2 < 0 || c3 < 0) continue;
        JsonObject rec = records.createNestedObject();
        rec["timestamp"] = line.substring(0, c1);
        rec["size"] = line.substring(c1 + 1, c2);
        rec["weight"] = line.substring(c2 + 1, c3).toFloat();
    }
    String jsonString;
    serializeJson(doc, jsonString);

    // === 1st POST: /exec へ、リダイレクト追跡なし ===
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("Mozilla/5.0 (compatible; M5Stack-GASTest/1.0)");
    http.setTimeout(30000);

    http.begin(networkConfig.gas_url.c_str());
    http.addHeader("Content-Type", "application/json; charset=utf-8");

    const char* headerKeys[] = { "Location" };
    http.collectHeaders(headerKeys, 1);

    Serial.printf("[postBatch] POST %s\n", networkConfig.gas_url.c_str());
    Serial.printf("[postBatch] payload=%dB records=%d\n", jsonString.length(), (int)lines.size());

    int code1 = http.POST(jsonString);
    String location = (code1 >= 300 && code1 < 400) ? http.header("Location") : String();
    String body1 = http.getString();
    http.end();

    Serial.printf("[postBatch] 1st httpCode=%d\n", code1);
    if (location.length() > 0) {
        Serial.printf("[postBatch] Location(len=%d)\n", location.length());
    }

    // 直接200なら成功
    if (code1 >= 200 && code1 < 300) {
        Serial.printf("[postBatch] direct-OK response: %s\n", body1.c_str());
        return true;
    }

    // リダイレクトでない、または Location なし → 失敗
    if (location.length() == 0) {
        Serial.printf("[postBatch] no redirect, body: %s\n", body1.substring(0, 200).c_str());
        lastUploadHttpCode = code1;
        lastUploadResponse = body1.substring(0, 120);
        return false;
    }

    // === 2nd GET: Location URL へ（GAS 応答本体を取得）===
    // 注意：GASのリダイレクト先は GET で応答本文を返す（POSTボディはすでに1回目で処理済み）
    // 2回目は別ホスト（script.googleusercontent.com）なので、
    // WiFiClientSecure を明示的に指定して TLS 接続を確立
    WiFiClientSecure client2;
    client2.setInsecure();
    client2.setHandshakeTimeout(15);

    HTTPClient http2;
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http2.setUserAgent("Mozilla/5.0 (compatible; M5Stack-GASTest/1.0)");
    http2.setTimeout(30000);

    if (!http2.begin(client2, location.c_str())) {
        Serial.println("[postBatch] http2.begin failed");
        lastUploadHttpCode = -998;
        lastUploadResponse = "http2.begin failed";
        return false;
    }
    http2.addHeader("Accept", "*/*");

    int code2 = http2.GET();
    String body2 = http2.getString();
    http2.end();

    Serial.printf("[postBatch] 2nd httpCode=%d body: %s\n", code2, body2.substring(0, 200).c_str());

    // 2回目が失敗しても、1回目が 302 なら GAS はデータを受信済み。
    // 安全側に倒して成功扱いにする（応答本文で明示的な失敗が確認できた場合のみ失敗）
    if (code2 >= 200 && code2 < 300) {
        if (body2.indexOf("\"ok\":false") >= 0) {
            // GAS 側で例外発生
            Serial.println("[postBatch] GAS reported ok:false");
            lastUploadHttpCode = code2;
            lastUploadResponse = body2.substring(0, 120);
            return false;
        }
        return true;
    }

    // 2回目の GET に失敗したが 1回目が 302 で受理された。
    // データはGASに渡っている可能性が高いため成功扱い（重複送信のリスクより脱漏防止を優先）
    Serial.printf("[postBatch] 2nd failed (code=%d) but 1st was 302 -> assume delivered\n", code2);
    return true;
}

// アップロード進捗表示
void showUploadProgress(int done, int total, int errors) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(30, 20);
    M5.Lcd.println("Uploading");

    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(20, 90);
    M5.Lcd.printf("Sent : %d / %d", done, total);
    M5.Lcd.setCursor(20, 120);
    M5.Lcd.setTextColor(errors > 0 ? RED : WHITE, BLACK);
    M5.Lcd.printf("Error: %d", errors);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setCursor(20, 160);
    M5.Lcd.printf("Batch: %d/POST", BATCH_SIZE);
}

// アップロードメイン処理（同期実行、実行中は計量ブロック）
void executeUpload() {
    // デバッグ情報リセット
    lastUploadHttpCode = 0;
    lastUploadResponse = "";

    // 1) 現在の measurements.csv をアップロード対象に隔離（新規計量は新ファイルへ）
    if (SD.exists(LOG_FILE)) {
        char isolatedPath[64];
        snprintf(isolatedPath, sizeof(isolatedPath), "%s%lu.csv", LOG_UPLOAD_PENDING_PREFIX, millis());
        SD.rename(LOG_FILE, isolatedPath);
    }

    // 2) アップロード対象ファイル一覧を作成（現在の measurements.csv は隔離後なので除外）
    std::vector<String> targetFiles;
    for (auto& p : listPendingLogFiles()) {
        if (p != String(LOG_FILE)) targetFiles.push_back(p);
    }

    // 全件数カウント
    int total = 0;
    for (auto& p : targetFiles) total += countLinesInFile(p.c_str());

    if (total == 0) {
        M5.Lcd.fillScreen(BLUE);
        M5.Lcd.setTextColor(WHITE, BLUE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(30, 100);
        M5.Lcd.println("No data to upload");
        delay(1500);
        pendingRecordCount = 0;
        return;
    }

    // 3) WiFi 接続
    if (!connectToWiFi()) {
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("ERROR:");
        M5.Lcd.println("WiFi failed");
        M5.Lcd.println("");
        M5.Lcd.printf("SSID: %s\n", networkConfig.ssid.c_str());
        M5.Lcd.println("");
        M5.Lcd.println("Data kept");
        M5.Lcd.println("for retry.");
        delay(3000);
        disconnectWiFi();
        return;
    }

    // 4) NTP同期
    configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");

    // 5) 各ファイルを1行ずつ読み、BATCH_SIZE ごとに POST
    std::vector<String> allFailed;
    std::vector<String> batch;
    int done = 0, errors = 0;
    showUploadProgress(done, total, errors);

    for (auto& fpath : targetFiles) {
        File src = SD.open(fpath.c_str(), FILE_READ);
        if (!src) continue;

        while (src.available()) {
            String line = src.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            batch.push_back(line);

            if ((int)batch.size() >= BATCH_SIZE) {
                if (postBatch(batch)) {
                    done += batch.size();
                } else {
                    errors += batch.size();
                    for (auto& l : batch) allFailed.push_back(l);
                }
                batch.clear();
                showUploadProgress(done, total, errors);
            }
        }
        src.close();
        SD.remove(fpath.c_str());  // このファイルは全行処理済み（成功/失敗は allFailed に反映）
    }
    // 残バッチをflush
    if (!batch.empty()) {
        if (postBatch(batch)) done += batch.size();
        else { errors += batch.size(); for (auto& l : batch) allFailed.push_back(l); }
        batch.clear();
        showUploadProgress(done, total, errors);
    }

    // 6) 失敗行のみを retry ファイルに保存
    if (SD.exists(LOG_RETRY_FILE)) SD.remove(LOG_RETRY_FILE);
    if (!allFailed.empty()) {
        File out = SD.open(LOG_RETRY_FILE, FILE_WRITE);
        if (out) {
            for (auto& l : allFailed) { out.print(l); out.print('\n'); }
            out.close();
        }
    }

    // 7) WiFi 切断
    disconnectWiFi();

    // 8) カウンタ再計算
    pendingRecordCount = countAllPendingRecords();

    // 9) 完了画面（失敗時は httpCode とレスポンス先頭も表示してデバッグ支援）
    if (errors == 0) {
        M5.Lcd.fillScreen(GREEN);
        M5.Lcd.setTextColor(WHITE, GREEN);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(20, 60);
        M5.Lcd.println("Upload done");
        M5.Lcd.setCursor(20, 100);
        M5.Lcd.printf("OK    : %d", done);
        M5.Lcd.setCursor(20, 130);
        M5.Lcd.printf("Failed: %d", errors);
        delay(2500);
    } else {
        // 失敗時：デバッグ情報を表示。Cボタンで閉じる（最大30秒）
        M5.Lcd.fillScreen(RED);
        M5.Lcd.setTextColor(WHITE, RED);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 5);
        M5.Lcd.println("Upload Error");

        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 40);
        M5.Lcd.printf("OK  : %d", done);
        M5.Lcd.setCursor(10, 65);
        M5.Lcd.printf("NG  : %d", errors);
        M5.Lcd.setCursor(10, 90);
        M5.Lcd.printf("HTTP: %d", lastUploadHttpCode);

        // レスポンス本文（先頭 ~120 文字）を小さめフォントで
        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(10, 125);
        M5.Lcd.println("Response:");
        M5.Lcd.setCursor(10, 140);
        if (lastUploadResponse.length() > 0) {
            M5.Lcd.print(lastUploadResponse);
        } else {
            M5.Lcd.print("(empty)");
        }

        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(10, 210);
        M5.Lcd.print("C: close");

        drainButtonEvents();
        unsigned long start = millis();
        while (millis() - start < 30000) {
            M5.update();
            if (M5.BtnC.wasPressed()) break;
            delay(20);
        }
        drainButtonEvents();
    }
}

// Upload実行（メニューから呼び出し、確認ダイアログ付き）
void uploadLog() {
    if (!confirmDialog("Upload?", "Upload logged data?", 'A')) {
        showCancelled("Upload");
        return;
    }
    executeUpload();
    resetMeasurementState();
}

// 画面描画
// STATE_STABLE中はラッチされた結果（grade/weight）を表示
void renderDisplay(float liveWeight) {
    sprite.fillSprite(BLACK);

    bool showLatched = (currentState == STATE_STABLE && latchedGrade.length() > 0);
    float displayWeight = showLatched ? latchedWeight : liveWeight;

    if (showLatched) {
        uint16_t bgColor = getGradeBackgroundColor(latchedGrade);
        sprite.fillRect(0, 0, 320, 180, bgColor);

        sprite.setFreeFont(FSSB24);
        sprite.setTextSize(4);
        sprite.setTextColor(WHITE, bgColor);
        int textWidth = sprite.textWidth(latchedGrade.c_str());
        sprite.setCursor((320 - textWidth) / 2, 150);
        sprite.print(latchedGrade.c_str());
        sprite.setTextFont(0);
    }

    // 重量表示（ライブ値は float なのでゼロ近傍を丸めてから四捨五入）
    int displayInt = (int)round(displayWeight);
    if (abs(displayWeight) < ZERO_THRESHOLD) displayInt = 0;
    sprite.setTextColor(WHITE, BLACK);
    sprite.setTextSize(3);
    char weightStr[20];
    sprintf(weightStr, "%dg", displayInt);
    int tw = sprite.textWidth(weightStr);
    sprite.setCursor((320 - tw) / 2, 190);
    sprite.print(weightStr);

    // 状態表示
    sprite.setTextSize(2);
    sprite.setCursor(230, 200);
    if (displayWeight < productSettings.minWeight && currentState != STATE_STABLE) {
        sprite.setTextColor(CYAN, BLACK);
        sprite.print("WAIT");
    } else {
        switch (currentState) {
            case STATE_MEASURING: sprite.setTextColor(YELLOW, BLACK); sprite.print("MEAS"); break;
            case STATE_STABLE:    sprite.setTextColor(GREEN, BLACK);  sprite.print("DONE"); break;
            case STATE_ZERO:      sprite.setTextColor(CYAN, BLACK);   sprite.print("ZERO"); break;
        }
    }

    // オーバーロード警告
    if (displayWeight > productSettings.maxWeight) {
        sprite.setTextColor(RED, BLACK);
        sprite.setTextSize(3);
        int ow = sprite.textWidth("OVER");
        sprite.setCursor((320 - ow) / 2, 140);
        sprite.print("OVER");
    }

    // ボタンガイド（A: Offset / B: なし / C: Menu）
    // Menu 色分け：件数しきい値でユーザーに Upload タイミングを促す
    sprite.setTextColor(WHITE, BLACK);
    sprite.setTextSize(2);
    sprite.setCursor(15, 220);
    sprite.print("Offset");
    uint16_t menuColor = WHITE;
    if (pendingRecordCount >= RECORD_WARN_RED) menuColor = RED;
    else if (pendingRecordCount >= RECORD_WARN_YELLOW) menuColor = YELLOW;
    sprite.setTextColor(menuColor, BLACK);
    sprite.setCursor(230, 220);
    sprite.print("Menu");

    // 記録OFF時のインジケータ（画面下部左寄せ）
    if (!recordingEnabled) {
        sprite.fillRect(0, 195, 110, 22, RED);
        sprite.setTextColor(WHITE, RED);
        sprite.setTextSize(2);
        sprite.setCursor(5, 198);
        sprite.print("REC OFF");
    }

    // デバイスID（白背景を一回り大きく描画して視認性を確保）
    String deviceIdStr = String(networkConfig.device_id);
    sprite.setTextSize(3);
    int textW = sprite.textWidth(deviceIdStr.c_str());
    const int padX = 8;
    const int padY = 6;
    int boxW = textW + padX * 2;
    int boxH = 24 + padY * 2;   // size=3 の文字高さ ~24px
    int boxX = 320 - boxW - 5;
    int boxY = 5;
    sprite.fillRect(boxX, boxY, boxW, boxH, WHITE);
    sprite.setTextColor(BLACK, WHITE);
    sprite.setCursor(boxX + padX, boxY + padY);
    sprite.print(deviceIdStr);

    sprite.pushSprite(0, 0);
}

// 表示更新（周期実行）
void updateDisplay(float weight) {
    static unsigned long lastUpdateTime = 0;

    unsigned long now = millis();
    if (now - lastUpdateTime < STABILITY_INTERVAL) return;
    lastUpdateTime = now;

    weightBuffer.addReading(weight);
    updateMeasurementState(weight);

    // 安定判定に「入った」瞬間のみ、結果をラッチして音声再生・ログ追記を1回だけ実行
    // バッファ4サンプルの平均を四捨五入することで、丸め境界付近での±1g誤差を排除
    if (currentState == STATE_STABLE && !wasStable) {
        float stableAvg = weightBuffer.average();
        float finalWeight = round(stableAvg);
        String grade = determineGrade(finalWeight);
        if (finalWeight >= productSettings.minWeight) {
            latchedGrade = grade;
            latchedWeight = finalWeight;
            playGradeSound(latchedGrade);
            appendToLog(latchedGrade.c_str(), latchedWeight);
        }
    }
    wasStable = (currentState == STATE_STABLE);

    // ゼロ復帰時にラッチをクリア
    if (currentState == STATE_ZERO) {
        latchedGrade = "";
        latchedWeight = 0.0;
    }

    renderDisplay(weight);
}

void setup() {
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);  // アップロード時のデバッグ出力用

    M5.Speaker.begin();
    M5.Speaker.setVolume(TONE_VOLUME);

    if (!SD.begin()) {
        M5.Lcd.println("SD Card Mount Failed");
        while (1);
    }

    audioQueue = xQueueCreate(1, sizeof(AudioMessage));
    xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 2, &audioTaskHandle, 0);

    sprite.setColorDepth(8);
    sprite.createSprite(320, 240);
    sprite.setTextSize(2);

    sprite.fillSprite(BLACK);
    sprite.setCursor(10, 10);
    sprite.setTextColor(WHITE);
    sprite.println("Weight Sensor");
    sprite.setTextSize(1);
    sprite.println("\nBtn A: Offset");
    sprite.println("Btn C: Menu");
    sprite.pushSprite(0, 0);

    while (!weight_i2c.begin(&Wire, 21, 22, DEVICE_DEFAULT_ADDR, 100000U)) {
        sprite.fillSprite(BLACK);
        sprite.println("weight i2c error");
        sprite.pushSprite(0, 0);
        delay(1000);
    }

    loadCalibrationFactor();
    loadNetworkConfig();

    // 起動時NTP同期は記録モードON時のみ実行
    // WiFi接続失敗時はRecord OFFに自動切替（config.jsonは書き換えない）
    // NTP同期失敗時はBoot+Nmsフォールバックで継続（Record ONのまま）
    if (recordingEnabled) {
        if (!syncTimeAtBoot()) {
            recordingEnabled = false;
        }
    }

    // 未アップロードのレコード数を集計、ローテーションカウンタも初期化
    initRotationCounter();
    pendingRecordCount = countAllPendingRecords();

    delay(1000);
    executeOffset();  // 起動時は確認なしで初期オフセット実行
}

void loop() {
    M5.update();

    if (M5.BtnA.wasPressed()) setInitialOffset();
    // B ボタンは通常時は無反応（メニュー内でのみ使用）
    if (M5.BtnC.wasPressed()) showMenu();

    // トリム平均で外れ値除去した生値を取得。以降は float のまま扱い、
    // 安定判定・ラッチ・表示の直前で丸めることで境界付近の ±1g 誤差を排除する。
    float rawCalibrated = readTrimmedMeanRaw() * calibration_factor;
    float weight = rawCalibrated - softZeroOffset;

    updateDisplay(weight);
    updateZeroTracking(weight, rawCalibrated);
}
