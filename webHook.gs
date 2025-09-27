// Scriptを変更したら、必ずデプロイする事。
// 新しいデプロイを選択すると、URLが変更になる。
// デプロイを編集してバージョンだけ上げることでURLを固定できる
function doPost(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName("Sheet1");
    if (!sheet) {
      return ContentService.createTextOutput("Sheet not found").setMimeType(ContentService.MimeType.TEXT);
    }
    
    // 受信したデータの内容をログに出力
//    Logger.log("Raw postData: " + e.postData.contents);
    var data = JSON.parse(e.postData.contents);
//    Logger.log("Parsed data:");
//    Logger.log(data);
    
    // データの各フィールドを確認
//    Logger.log("Size: " + data.size);
//    Logger.log("Weight: " + data.weight);

//    Logger.log("Timestamp: " + data.timestamp);
//    Logger.log("Device ID: " + data.device_id);
    
    // 実際に書き込もうとしている配列の内容を確認
    var rowToWrite = [data.timestamp,data.size, data.weight, data.device_id];
    Logger.log("Row to write: " + rowToWrite);
    
    sheet.appendRow(rowToWrite);
    
    return ContentService.createTextOutput("Success").setMimeType(ContentService.MimeType.TEXT);
  } catch (err) {
    Logger.log("Error: " + err.toString());
    Logger.log("Stack: " + err.stack);
    return ContentService.createTextOutput("Error: " + err.message).setMimeType(ContentService.MimeType.TEXT);
  }
}