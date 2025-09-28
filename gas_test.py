#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GAS URL テストプログラム
M5Graderから送信されるJSONデータ形式でGoogle Apps Scriptをテストします。
"""

import requests
import json
import datetime
import random
import time

# GAS URL
GAS_URL = "https://script.google.com/macros/s/AKfycbxijBpEFM94MmymSEX4Wi-vTZHkiYJ790xguj3y2q1w3N_CDW65DDs06q97zpeR0WSr/exec"

# テスト用の規格データ
TEST_GRADES = ["6L", "5L", "4L", "3L", "2L", "L", "M", "S", "2S", "3S"]
TEST_WEIGHTS = [380, 340, 310, 280, 240, 200, 170, 140, 110, 80]

def create_test_data(device_id=1):
    """
    テスト用の測定データを生成
    M5Graderから送信される形式と同じJSONを作成
    """
    # ランダムな規格と重量を選択
    grade_index = random.randint(0, len(TEST_GRADES) - 1)
    grade = TEST_GRADES[grade_index]
    base_weight = TEST_WEIGHTS[grade_index]
    # 基準重量から±10gの範囲でランダム化
    weight = base_weight + random.randint(-10, 10)

    # 現在時刻
    timestamp = datetime.datetime.now().strftime("%Y/%m/%d %H:%M:%S")

    # M5Graderと同じJSON形式
    data = {
        "size": grade,
        "weight": weight,
        "timestamp": timestamp,
        "device_id": device_id
    }

    return data

def send_to_gas(data):
    """
    GASにJSONデータを送信
    """
    headers = {
        "Content-Type": "application/json"
    }

    try:
        print(f"送信データ: {json.dumps(data, ensure_ascii=False)}")

        response = requests.post(GAS_URL,
                               json=data,
                               headers=headers,
                               timeout=10)

        print(f"レスポンスコード: {response.status_code}")
        print(f"レスポンス内容: {response.text}")

        if response.status_code == 200:
            print("✅ 送信成功")
            return True
        else:
            print(f"❌ 送信失敗: HTTP {response.status_code}")
            return False

    except requests.exceptions.Timeout:
        print("❌ タイムアウトエラー")
        return False
    except requests.exceptions.ConnectionError:
        print("❌ 接続エラー")
        return False
    except Exception as e:
        print(f"❌ エラー: {e}")
        return False

def test_single_send():
    """
    単発送信テスト
    """
    print("=== 単発送信テスト ===")
    data = create_test_data()
    result = send_to_gas(data)
    print()
    return result

def test_multiple_send(count=5, interval=2):
    """
    複数回送信テスト
    """
    print(f"=== 複数回送信テスト ({count}回) ===")
    success_count = 0

    for i in range(count):
        print(f"\n--- {i+1}/{count}回目 ---")
        data = create_test_data()

        if send_to_gas(data):
            success_count += 1

        if i < count - 1:  # 最後以外は待機
            print(f"{interval}秒待機...")
            time.sleep(interval)

    print(f"\n結果: {success_count}/{count}回成功")
    return success_count == count

def test_device_ids():
    """
    複数デバイスIDテスト
    """
    print("=== 複数デバイスIDテスト ===")
    device_ids = [1, 2, 3]
    success_count = 0

    for device_id in device_ids:
        print(f"\n--- デバイスID: {device_id} ---")
        data = create_test_data(device_id)

        if send_to_gas(data):
            success_count += 1

        time.sleep(1)

    print(f"\n結果: {success_count}/{len(device_ids)}台成功")
    return success_count == len(device_ids)

def main():
    """
    メイン関数
    """
    print("GAS URL テストプログラム")
    print(f"テスト対象URL: {GAS_URL}")
    print("=" * 50)

    while True:
        print("\n実行するテストを選択してください:")
        print("1. 単発送信テスト")
        print("2. 複数回送信テスト (5回)")
        print("3. 複数デバイスIDテスト")
        print("4. カスタムテスト")
        print("0. 終了")

        try:
            choice = input("\n選択 (0-4): ").strip()

            if choice == "0":
                print("テスト終了")
                break
            elif choice == "1":
                test_single_send()
            elif choice == "2":
                test_multiple_send()
            elif choice == "3":
                test_device_ids()
            elif choice == "4":
                # カスタムテスト
                try:
                    count = int(input("送信回数 (1-10): "))
                    if 1 <= count <= 10:
                        interval = float(input("送信間隔(秒) (0.5-10): "))
                        if 0.5 <= interval <= 10:
                            test_multiple_send(count, interval)
                        else:
                            print("間隔は0.5-10秒で入力してください")
                    else:
                        print("回数は1-10で入力してください")
                except ValueError:
                    print("数値を入力してください")
            else:
                print("無効な選択です")

        except KeyboardInterrupt:
            print("\n\nテスト中断")
            break
        except Exception as e:
            print(f"エラー: {e}")

if __name__ == "__main__":
    main()