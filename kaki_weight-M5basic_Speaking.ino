/**
 * @file weight_i2c.ino
 * @brief TAL221ロードセル用の重量測定プログラム（M5Stack Core用）
 * Button A: オフセット設定
 * Button B: キャリブレーション
 * Button C: 未使用
 * 音声フィードバック機能付き
 */

#include <M5Stack.h>
#include "M5UnitWeightI2C.h"
#include "SPIFFS.h"
#include "Free_Fonts.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// 音階定義

// 基本設定
const float MAX_WEIGHT = 1000.0;   // 最大計測重量 (g)
const float KNOWN_WEIGHT = 100.0; // キャリブレーション用の既知の重さ (g)
const char* CALIB_FILE = "/weight_calibration.txt"; 

// 音声設定
const int TONE_VOLUME = 3;      // 音量 (0-10)
const int BEEP_DURATION = 100;    // ビープ音の長さ (ms)
const int BEEP_INTERVAL = 50;     // ビープ音の間隔 (ms)

// 安定性検出のための設定
const int STABILITY_SAMPLES = 5;        // 安定性判断のためのサンプル数
const float STABILITY_THRESHOLD = 0.3;   // 安定判定のための閾値(g)
const int STABILITY_INTERVAL = 200;      // サンプリング間隔(ms)
const float ZERO_THRESHOLD = 0.5;       // ゼロ判定のための閾値(g)
const int SAMPLES = 6 ;                 // 平均化のためのサンプル数

// 測定状態を表す列挙型
enum MeasurementState {
    STATE_READY,         // 測定準備完了
    STATE_MEASURING,     // 測定中
    STATE_STABLE,        // 測定値安定
    STATE_ZERO          // ゼロ状態
};

// 柿のサイズ判定用の構造体
struct SizeRange {
    const char* size;
    int start;
    int end;
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


// サイズ範囲の定義
const SizeRange SIZE_RANGES[] = {
    {"6L", 350, 999},  // 350g以上は全て6L
    {"5L", 320, 349},
    {"4L", 290, 319},
    {"3L", 260, 289},
    {"2L", 220, 259},
    {"L",  190, 219},
    {"M",  160, 189},
    {"S",  130, 159},
    {"2S", 100, 129},
    {"3S",   50,  99}
};
const int SIZE_RANGES_COUNT = sizeof(SIZE_RANGES) / sizeof(SIZE_RANGES[0]);

// サイズごとの音声ファイル名を定義
const char* SIZE_WAV_FILES[] = {
    "/6L.wav",
    "/5L.wav",
    "/4L.wav",
    "/3L.wav",
    "/2L.wav",
    "/L.wav",
    "/M.wav",
    "/S.wav",
    "/2S.wav",
    "/3S.wav"
};


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
        
        return (max_val - min_val) <= STABILITY_THRESHOLD;
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

// サイズに応じた音声再生（メインループから呼び出し）
void playWeightSound(const char* size) {
    const char* wavFile = nullptr;
    
    // サイズに対応する音声ファイルを特定
    for (int i = 0; i < SIZE_RANGES_COUNT; i++) {
        if (strcmp(size, SIZE_RANGES[i].size) == 0) {
            wavFile = SIZE_WAV_FILES[i];
            break;
        }
    }
    
    if (wavFile != nullptr) {
        AudioMessage msg = {wavFile, true};
        xQueueSend(audioQueue, &msg, 0);
    }
}



// サイズに応じた背景色を返す関数
uint16_t getSizeBackgroundColor(const char* size) {
    if (strcmp(size, "6L") == 0) return 0xF800;      // 赤
    if (strcmp(size, "5L") == 0) return 0xF81F;      // マゼンタ
    if (strcmp(size, "4L") == 0) return 0x780F;      // 紫
    if (strcmp(size, "3L") == 0) return 0x001F;      // 青
    if (strcmp(size, "2L") == 0) return 0x07FF;      // シアン
    if (strcmp(size, "L") == 0)  return 0x07E0;      // 緑
    if (strcmp(size, "M") == 0)  return 0xFFE0;      // 黄
    if (strcmp(size, "S") == 0)  return 0xFBE0;      // オレンジ
    if (strcmp(size, "2S") == 0) return 0xC618;      // グレー
    if (strcmp(size, "3S") == 0) return 0x8410;      // ダークグレー
    return BLACK;
}

// サイズを判定する関数
const char* determineSize(float weight) {
    // 50g未満は空文字列を返す
    if (weight < 50.0) {
        return "";
    }
    
    for (int i = 0; i < SIZE_RANGES_COUNT; i++) {
        if (weight >= SIZE_RANGES[i].start && weight <= SIZE_RANGES[i].end) {
            return SIZE_RANGES[i].size;
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

// 画面表示関数
void displayWeight(float weight) {
    static unsigned long lastUpdateTime = 0;
    const unsigned long UPDATE_INTERVAL = STABILITY_INTERVAL;
    
    unsigned long currentTime = millis();
    if (currentTime - lastUpdateTime >= UPDATE_INTERVAL) {
        weightBuffer.addReading(weight);
        updateMeasurementState(weight);
        
        sprite.fillSprite(BLACK);
        
        const char* size = determineSize(weight);
        // 50g未満の場合は何も表示しない
        if (weight >= 50.0 && currentState == STATE_STABLE) {
            if (previousState != STATE_STABLE) {
                playWeightSound(size);
            }
            
            uint16_t bgColor = getSizeBackgroundColor(size);
            sprite.fillRect(0, 0, 320, 180, bgColor);

            sprite.setFreeFont(FSSB24);
            sprite.setTextSize(4);
            sprite.setTextColor(WHITE, bgColor);
            
            int textWidth = sprite.textWidth(size);
            int x = (320 - textWidth) / 2;
            sprite.setCursor(x, 150);
            sprite.print(size);
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
        if (weight < 50.0) {
            // 50g未満の場合は特別な表示
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
        
        // オーバーロード警告（変更なし）
        if (weight > MAX_WEIGHT) {
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
        
        // ボタンガイド（変更なし）
        sprite.setTextColor(WHITE, BLACK);
        sprite.setTextSize(2);
        sprite.setCursor(15, 220);
        sprite.print("Offset");
        sprite.setCursor(130, 220);
        sprite.print("Calibrate");
        
        sprite.pushSprite(0, 0);
        
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
    
    float weight = getAccurateWeight();
    displayWeight(weight);
    
    delay(50); // より短いディレイに変更
}
