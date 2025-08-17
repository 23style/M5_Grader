# M5Stack Persimmon Sorting Machine Program

This repository provides a program for a persimmon sorting machine using M5Stack.
The program integrates Wi-Fi connectivity, weight measurement, and audio output to improve the efficiency of fruit sorting operations.

## Features

- **Wi-Fi Connectivity**  
  Connects to the internet using SSID and password specified in `config.json`.

- **Google Apps Script Integration**  
  Records measurement results in Google Spreadsheet. Can switch to offline mode when needed.

- **Device Identification**  
  Uses `device_id` in `config.json` to individually identify multiple devices.

- **Weight Measurement and Sorting Process**  
  Measures persimmon weight using weight sensors and performs sorting based on configured criteria.

- **Audio Output**  
  Provides audio feedback for measurement results and sorting status to notify users of operation status.

## Google Apps Script (GAS) Setup

Configure Google Apps Script (GAS) to process data sent from M5Stack and save it to a spreadsheet.

### 1. Creating Google Apps Script

1. **Create a Google Spreadsheet** and create a sheet named "Sheet1".
2. **Open Google Apps Script (GAS) Editor**
   - Open the spreadsheet and select "Extensions" → "Apps Script" from the menu.
3. **Add Script**
   - Copy the contents of `webHook.gs` and paste it into the Apps Script editor.

### 2. GAS Deployment

1. Select "Deploy" → "New deployment"
2. Select "Web app" for "Select type"
3. Set "Execute as" to "Me"
4. Change "Who has access" to "Anyone"
5. Copy the "Web app URL" displayed after deployment and set it in the `gas_url` field of `config.json`

### 3. GAS Processing Details

This script records data sent from M5Stack to Google Spreadsheet. The basic operation is as follows:

1. The `doPost(e)` function receives HTTP requests
2. Parses JSON data and saves it to the spreadsheet
3. Returns an HTTP response when processing is complete

The detailed code of the script is described in `webHook.gs`.

## Installation on M5Stack

1. **SD Card Preparation**  
   Place config.json in the root directory of the SD card.
   Place the wav folder in the root directory of the SD card.
   Place wav files in the wav folder.

2. **Wi-Fi Configuration**  
   Open the `config.json` file in the root directory and configure Wi-Fi connection information, Google Apps Script endpoint URL, and `device_id`.  
   Example:
   ```json
   {
     "wifi": {
       "ssid": "yourSSID",
       "password": "password"
     },
     "gas_url": "https://script.google.com/macros/..../exec",
     "device_id": 1
   }
   ```

## Usage and Functions

1. **Device Startup**  
   When you turn on the M5Stack, it automatically connects to Wi-Fi and starts communication based on the configured network information.

2. **Weight Measurement and Sorting Process**  
   Measures persimmon weight using the connected weight sensor and performs sorting according to configured criteria.

3. **Audio Output Feedback**  
   Measurement results and sorting status are notified to users through audio output.

4. **Data Transmission**  
   Measurement data is sent to the Google Apps Script endpoint and saved to the spreadsheet.

5. **Offline Mode**  
   If you don't want to record measurement data, you can toggle offline mode with the C button.

## Button Functions

**A Button - Zero Reset (Tare Function)**  
Resets the tare (container weight) during measurement to accurately measure persimmon weight.

**B Button - Calibration Function (Required on First Use)**  
Performs calibration using a 100g weight to improve measurement accuracy. Settings are saved to the M5Stack.

**C Button - Offline Mode Toggle**  
Switches between online and offline modes for data recording.

## License

This project is released under the MIT License. Please refer to the [LICENSE](LICENSE) file for details.

## Contact

If you have questions or improvement suggestions, please create an Issue on GitHub or send a pull request.