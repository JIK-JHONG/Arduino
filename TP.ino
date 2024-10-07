#include <U8g2lib.h>
#include <Wire.h>
#include "max6675.h"
#include <WiFi.h>
#include <WebServer.h>

// 溫度感測器的腳位
int thermocoupleSO = 0;
int thermocoupleCS = 1;
int thermocoupleCLK = 2;

// 記錄矩陣大小
const int MAX_LOG = 100 ;

// LED 定義
#define LED_ID_ACT 3
#define LED_ID_ERR 4
#define FLASH_RATE 5  // 确保已定义值
#define IOT_ID "ESP32-C3"


MAX6675 ktc(thermocoupleCLK, thermocoupleCS, thermocoupleSO);

// IIC OLED 初始化
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, 6, 5, U8X8_PIN_NONE);

// Wi-Fi 資訊
char const SSID[] = "YOUR_SSID";
char const PSWD[] = "YOUR_PSWD";

// WebServer 初始化
WebServer server(80);



const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;  // 台灣時區 UTC+8
const int daylightOffset_sec = 0;

// 儲存時間資料
unsigned long previousMillis = 0;
int seconds = 0;
float data_log[MAX_LOG];
int data_log_count = 0;

void init_data_log() {
  for (int i = 0; i < MAX_LOG; i++) {
    data_log[i] = 0.0;
  }
}

void update_data_log(float new_data) {
  // 若數據記錄已滿，將最前面的數據刪除
  if (data_log_count >= MAX_LOG) {
    // 將數據向前移動一個位置
    for (int i = 0; i < MAX_LOG-1; i++) {
      data_log[i] = data_log[i + 1];
    }
    data_log[MAX_LOG-1] = new_data;  // 新數據放入最後一個位置
  } else {
    data_log[data_log_count] = new_data;  // 新數據放入當前記錄位置
    data_log_count++;
  }
}

void setup(void) {
  init_data_log();

  // OLED 和溫度感測器初始化
  u8g2.setContrast(250);
  u8g2.begin();

  // 設置 LED 輸出
  pinMode(LED_ID_ACT, OUTPUT);
  pinMode(LED_ID_ERR, OUTPUT);

  // 初始化 Wi-Fi 連接
  WiFi.begin(SSID, PSWD);
  
  // 連接 Wi-Fi，嘗試連接直到成功為止
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    digitalWrite(LED_ID_ERR, HIGH);  // 若無法連接，LED_ID_ERR 恆亮
    digitalWrite(LED_ID_ACT, LOW);   // 若無法連接，LED_ID_ACT 關閉
  }
  
  // 當連接成功，LED_ID_ACT 恆亮，LED_ID_ERR 關閉
  digitalWrite(LED_ID_ACT, HIGH);
  digitalWrite(LED_ID_ERR, LOW);

  // 啟動 WebServer，並且定義回應的路由
  server.on("/", handleRoot);
  server.begin();
}

// 取得當前時間的函數
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "無法取得時間";  // 若無法取得時間，返回 N.A.
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// 當網頁被請求時，回傳溫度資訊
void handleRoot() {
  float tempC = ktc.readCelsius();
  String message = "<html>";
  message += "<head>";
  message += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' /><meta http-equiv='refresh' content='" + String(FLASH_RATE) + "'><title>溫度監控模組 ESP32</title>";
  message += "<script> function drawGraph() { var canvas = document.getElementById('tempCanvas');var ctx = canvas.getContext('2d');ctx.clearRect(0, 0, canvas.width, canvas.height);ctx.beginPath();ctx.moveTo(10, 58);";  // 起始位置
  // 繪製圖形數據
  for (int i = 0; i < data_log_count; i++) {
    message += "ctx.lineTo(" + String(map(i, 0, 99, 10, 120)) + ", " + String(map(data_log[i], 0, 100, 58, 10)) + ");";
  }
  message += "ctx.strokeStyle = 'blue';ctx.stroke();}</script>";
  
  message += "</head>";  
  message += "<body onload='drawGraph()'>";
  message += "<div style='width:100%;line-height:6vh; font-size:4vh; font-weight:bold;'>環境溫度監控模組 - " + String(IOT_ID) + "</div>";
  
  // 顯示溫度
  if (tempC == -1.0) {
    message += "<div id='TMP_DATA' style='width:100%;line-height:4vh; font-size:2.4vh;'>Temperature: N.A.</div>";
  } else {
    message += "<div id='TMP_DATA' style='width:100%; line-height:4vh; font-size:2.4vh;'>Temperature: " + String(tempC) + " °C</div>";
    update_data_log(tempC);  // 更新數據日誌
  }
  
  // 顯示目前時間
  message += "<div id='TMP_TIME' style='width:100%; line-height:3vh; font-size:1.8vh;'>Current Time: " + getCurrentTime() + "</div>";
  message += "<div style='width:100%; height:3vh; line-height:3vh;'></div>";
  message += "<div style='width:100%; line-height:3vh; font-size:1.6vh;'>系統將會於每 " + String(FLASH_RATE) + " 秒 更新一次</div>";


  // 顯示畫布
  message += "<canvas id='tempCanvas'  style='margin:auto;width:auto;min-width:40vh;height:auto;border:0.1vh solid rgba(180,180,180,1);background:rgba(240,240,240,1);'></canvas>";

  // 顯示數據日誌
  message += "<div style='width:40vh;margin-top:5vh;border-bottom:0.1vh solid rgba(180,180,180,1);font-size:2vh;line-height:3.6vh;'><span style='width:10vw;'>ID</span> Temperature(oC)</div>";
  message += "<div style='width:40vh;height:40vh;overflow-y:auto;overflow-x:hidden;border:0.1vh solid rgba(180,180,180,1);'>";
  
  for (int i = 0; i < data_log_count - 1; i++) {
    int log_set = data_log_count - 1 - i ; 
    message += "<div style='width:100%; line-height:3vh; font-size:1.8vh;border-bottom:0.1vh dashed rgba(220,220,220,1);'><span style='width:10vw;'>[" + String(log_set) + "]</span> " + String(data_log[log_set]) + "</div>";
  }

  message += "</div>";
  message += "</body></html>";
  server.send(200, "text/html", message);

  // 控制傳輸時 LED_ID_ERR 閃爍
  digitalWrite(LED_ID_ERR, HIGH);
  delay(100);  // 短暫閃爍
  digitalWrite(LED_ID_ERR, LOW);
}

void loop(void) {
  unsigned long currentMillis = millis();

  // 每秒更新一次時間
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;
    seconds = (seconds + 1) % 60;  // 秒數從 0 到 59，60 則重新計算
  }

  // 讀取溫度值
  float tempC = ktc.readCelsius();

  // 更新 OLED 顯示
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_ncenB10_tr);

    // 顯示溫度或 "N.A." 若溫度感測器未插入
    char buffer_tmp[20];
    if (tempC == -1.0) {
      sprintf(buffer_tmp, "Tp = N.A.");
    } else {
      sprintf(buffer_tmp, "Tp = %.2f oC", tempC);
    }
    u8g2.drawStr(10, 24, buffer_tmp);

    // 顯示秒數
    char buffer[10];
    sprintf(buffer, "Ts = %02d s", seconds);
    u8g2.drawStr(30, 48, buffer);

  } while (u8g2.nextPage());

  // 處理 WebServer 的請求
  server.handleClient();
}