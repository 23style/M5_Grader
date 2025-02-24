# M5Stack 柿選果システム

M5Stackを使用した柿の自動選果システム。重量測定、音声フィードバック、クラウドデータロギング機能を実装しています。

## 概要

IoTベースの柿選果機を M5Stack で実装したプロジェクトです。リアルタイムの重量測定、音声フィードバック、Google Sheets を介したデータロギングが特徴です。

### 主要機能

- TAL221ロードセルによるリアルタイム重量測定
- 重量に基づく自動サイズ分類
- 音声フィードバック
- Google Sheetsへのデータロギング
- オフラインモード対応
- 自動ゼロ点調整
- デバイスID によるマルチデバイス対応

### ハードウェア要件

- M5Stack Core
- TAL221ロードセル（HX711搭載）
- SDカード（設定・音声ファイル用）
- キャリブレーション用分銅（推奨: 100g）

## 技術アーキテクチャ

### システム構成

```
├── M5Stack Core
│   ├── 重量センサ (TAL221 + HX711)
│   ├── SDカード
│   │   ├── config.json
│   │   └── wav/
│   └── SPIFFS (キャリブレーションデータ)
├── Google Apps Script
└── Google Sheets
```

### 重量による等級分類

```
6L: 350g以上
5L: 320-349g
4L: 290-319g
3L: 260-289g
2L: 220-259g
L:  190-219g
M:  160-189g
S:  130-159g
2S: 100-129g
3S: 50-99g
```

## セットアップ手順

### 1. ハードウェアセットアップ

1. TAL221ロードセルをM5StackにI2C接続（SDA: 21, SCL: 22）
2. 必要なファイルを格納したSDカードを挿入

### 2. SDカード構成

```
/
├── config.json
└── wav/
    ├── 6L.wav
    ├── 5L.wav
    ├── ...
    ├── info.wav
    ├── zero.wav
    └── error.wav
```

### 3. 設定ファイル

`config.json` の構成:

```json
{
  "wifi": {
    "ssid": "your_ssid",
    "password": "your_password"
  },
  "gas_url": "https://script.google.com/macros/s/.../exec",
  "device_id": 1
}
```

### 4. Google Apps Script設定

1. Google Sheetsを新規作成
2. スクリプトエディタを開く（拡張機能 > Apps Script）
3. `webHook.gs` の内容をエディタにコピー
4. Webアプリケーションとしてデプロイ:
   - 実行者: `自分`
   - アクセス権限: `全員（匿名ユーザーを含む）`
5. デプロイメントURLを `config.json` にコピー

## 使用方法

### ボタン操作

- **Aボタン**: ゼロリセット（風袋引き）
  - 基準重量をゼロにリセット
  - 測定セッション開始前に使用
  
- **Bボタン**: キャリブレーション
  - 100g分銅が必要
  - キャリブレーション係数はSPIFFSに保存
  
- **Cボタン**: オフラインモード切替
  - クラウドデータロギングを無効化
  - テストやネットワーク未接続時に使用

### ディスプレイインターフェース

```
+-----------------+
| サイズ (6L-3S)   |
| 重量 (g)        |
| ステータス       |
|   MEAS/DONE/    |
|   ZERO/WAIT     |
+-----------------+
```

### 高度な機能

#### 自動ゼロ調整
- 条件: 1.2g未満の重量が3秒間継続
- 5分間隔で条件を満たせば実行

#### 安定性検出
- サンプル数: 5
- 閾値: 0.3g
- サンプリング間隔: 200ms

## 開発情報

### 主要パラメータ

```cpp
const float MAX_WEIGHT = 1000.0;  // 最大重量 (g)
const float STABILITY_THRESHOLD = 0.3;  // 安定性閾値 (g)
const int STABILITY_SAMPLES = 5;  // 安定性判定用サンプル数
const int STABILITY_INTERVAL = 200;  // サンプリング間隔 (ms)
```

### プロジェクト構成

```
├── kaki_weight-M5basic_Speaking.ino  // メインプログラム
├── webHook.gs                        // Google Apps Script
├── config.json                       // 設定ファイル
└── README.md                         // ドキュメント
```

## 開発への貢献

Pull Requestを歓迎します。大きな変更を行う場合は、まずIssueを作成して変更内容についての議論をお願いします。

#### 重量測定の精度向上

- 測定環境の温度を一定に保つ
- 定期的なキャリブレーション実施
- 振動の少ない設置場所の選択

## ライセンス

[MIT](LICENSE)
