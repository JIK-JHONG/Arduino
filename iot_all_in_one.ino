#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <string.h>
#include "max6675.h"

// LED 定義
#define LED_ID_ACT 3
#define LED_ID_ERR 4
// 更新頻率預設 定義
#define FLASH_RATE 5
#define IOT_ID "ESP32-C3"
//  WIFI帳密
#define JIK_HOME_AC_SSID "YOUR_SSID"
#define JIK_HOME_AC_PSWD "YOUR_PSWD"
//  儲存大小
#define MAX_LOG_SIZE 1000

struct DataSet
{
    char str_time[30]; // 使用 char 陣列而不是 String
    float temp;
};

struct SignalStrength
{
    String level;
    int bars; // 信號條數量（1-4）
};


// 溫度感測器的腳位
int thermocoupleSO = 0;
int thermocoupleCS = 1;
int thermocoupleCLK = 2;

// 記錄矩陣大小
const int MAX_LOG = MAX_LOG_SIZE;

// OLED 狀態變數
bool oledOn = true; // OLED 初始狀態為開啟

bool ledOn = true;

// 溫度 時間 紀錄格式
static char timeStringBuff[30];  // 使用 static 確保返回的指針有效
static time_t baseTime;          // 儲存初始的標準時間
static unsigned long baseMillis; // 儲存初始的毫秒數
static int initialized = 0;      // 是否已經初始化的標記

unsigned long log_save_timer = 0;  // 紀錄間隔 (  log_save_timer > diff_save_timer*1000 >> save)
unsigned long diff_save_timer = FLASH_RATE; // 實際紀錄間隔 預設 5 sec

// 時間同步
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600; // 台灣時區 UTC+8
unsigned long init_Millis = 0;
unsigned long sys_days = 0;
const int daylightOffset_sec = 0;

// 儲存時間資料
unsigned long previousMillis = 0;
int seconds = 0;    //  OLED 秒數從 0 到 59，60 則重新計算
int total_count = 0;    //  整體計數器，作為判斷是否已經滿紀錄上限，啟動FIFO
int data_log_count = 0; //  當前資料總數目
struct DataSet time_log[MAX_LOG]; // 声明一个结构体数组

//  溫度計初始化
MAX6675 ktc(thermocoupleCLK, thermocoupleCS, thermocoupleSO);

// IIC OLED 初始化
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, 6, 5, U8X8_PIN_NONE);

// Wi-Fi 資訊
char const SSID[] = JIK_HOME_AC_SSID;
char const PSWD[] = JIK_HOME_AC_PSWD;


// WebServer 初始化
WebServer server(80);


// 根據 RSSI 值獲取信號強度等級
SignalStrength getSignalStrengthLevel(int rssi)
{
    SignalStrength signal;
    if (rssi >= -50)
    {
        signal.level = "Excellent";
        signal.bars = 4;
    }
    else if (rssi >= -70)
    {
        signal.level = "Good";
        signal.bars = 3;
    }
    else if (rssi >= -80)
    {
        signal.level = "Fair";
        signal.bars = 2;
    }
    else
    {
        signal.level = "Poor";
        signal.bars = 1;
    }
    return signal;
}

char *getCurrentTime()
{
    if (!initialized)
    {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo))
        {
            strncpy(timeStringBuff, "無法取得時間", sizeof(timeStringBuff));
            return timeStringBuff; // 返回 N.A.
        }

        // 獲取當前時間和毫秒
        baseTime = mktime(&timeinfo);
        baseMillis = millis();
        initialized = 1; // 標記為已初始化
    }

    // 計算經過的時間
    unsigned long elapsedMillis = millis() - baseMillis;
    time_t currentTime = baseTime + elapsedMillis / 1000; // 將毫秒轉換為秒

    // 將當前時間轉換為字符串
    struct tm *currentTimeInfo = localtime(&currentTime);
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", currentTimeInfo);

    return timeStringBuff;
}
String formatElapsedTime(unsigned long elapsed, unsigned long days)
{
    unsigned long secondsTotal = elapsed / 1000;
    // unsigned long days = secondsTotal / 86400;
    unsigned long hours = (secondsTotal % 86400) / 3600;
    unsigned long minutes = (secondsTotal % 3600) / 60;
    unsigned long seconds = secondsTotal % 60;

    String formattedTime = "";
    if (days > 0)
    {
        formattedTime += String(days) + " days ";
    }
    if (hours > 0 || days > 0)
    {
        formattedTime += String(hours) + " hours ";
    }
    formattedTime += String(minutes) + " minutes ";
    formattedTime += String(seconds) + " seconds";

    return formattedTime;
}

void init_data_log()
{
    for (int i = 0; i < MAX_LOG; i++)
    {
        time_log[i].temp = 0.0;
    }
}


void update_data_log(float new_data)
{
    // 若數據記錄已滿，將最前面的數據刪除
    if (data_log_count >= MAX_LOG)
    {
        // 將數據向前移動一個位置
        for (int i = 0; i < MAX_LOG - 1; i++)
        {
            time_log[i].temp = time_log[i + 1].temp;
            strncpy(time_log[i].str_time, time_log[i + 1].str_time, sizeof(time_log[i].str_time));
        }
        time_log[MAX_LOG - 1].temp = new_data; // 新數據放入最後一個位置
        strncpy(time_log[MAX_LOG - 1].str_time, getCurrentTime(), sizeof(time_log[MAX_LOG - 1].str_time));
    }
    else
    {
        time_log[data_log_count].temp = new_data; // 新數據放入當前記錄位置
        strncpy(time_log[data_log_count].str_time, getCurrentTime(), sizeof(time_log[data_log_count].str_time));
        data_log_count++;
    }
    total_count++;
}


// 開啟 OLED
void turnOledOn()
{
    oledOn = true;
    u8g2.setPowerSave(false); // 退出省電模式，開啟 OLED
}

// 關閉 OLED
void turnOledOff()
{
    oledOn = false;
    u8g2.setPowerSave(true); // 進入省電模式，關閉 OLED
}

// 當網頁被請求時，回傳溫度資訊
void handleRoot()
{
    // float tempC = ktc.readCelsius();
    String message = "<html>";
    message += "<head>";
    message += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' /><title>Temperature Monitoring - " + String(IOT_ID) + "</title>";
    // message += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' /><meta http-equiv='refresh' content='" + String(FLASH_RATE) + "'><title>溫度監控模組 ESP32</title>";
    message += "<style>";
    message += ".clear_left{clear:left}.clear_right{clear:right}body{font-family:Arial,sans-serif;background-color:#f0f0f0;margin:0;padding:0}@media only screen and (min-width:1080px){#sys_msg_bks,.function_bar{top:0;left:0;-webkit-backdrop-filter:blur(20px);position:fixed}.function_bar>.wifi_box>.wifi_0,.function_bar>.wifi_box>.wifi_1,.function_bar>.wifi_box>.wifi_2,.function_bar>.wifi_box>.wifi_3{transition:.35s;width:5px;background:#b4b4b4;bottom:0}.div_select_box>.div_select>.func_title,.function_bar>.title,.log_msg_data .id,.log_msg_data .time,.log_msg_data .title,.log_msg_title .id,.log_msg_title .time,.log_msg_title .title{white-space:nowrap;text-overflow:ellipsis;overflow:hidden}#sys_msg_bks,.function_bar>.icon_func{display:none}.main_info{width:calc(90% - 40px);margin:20px auto;background-color:#fff;padding:50px 20px 20px;box-shadow:0 0 10px rgba(0,0,0,.1)}.function_bar{font-size:14px;width:90%;line-height:30px;padding:10px 5%;transition:.35s;backdrop-filter:blur(20px);background:rgba(255,255,255,.8);border-bottom:1px solid #b4b4b4;z-index:2}.function_bar>.title{font-size:18px;min-width:100px;max-width:200px;border-radius:5px;line-height:30px;text-align:left;color:#404040;font-weight:700;margin-right:20px;float:left}.div_select_box>.div_func,.div_select_box>.div_func_reset{margin-left:10px;margin-right:10px;border-radius:5px;text-align:center;color:#fff;cursor:pointer}.function_bar>.wifi_box{width:50px;height:20px;margin-bottom:5px;margin-top:5px;float:left;position:relative}.div_select_box>.div_func,.div_select_box>.div_func_reset,.div_select_box>.div_select{float:right;font-size:14px;line-height:30px}.function_bar>.wifi_box>.wifi_0{position:absolute;height:2px;left:0}.function_bar>.wifi_box>.wifi_1{position:absolute;height:5px;left:7px}.function_bar>.wifi_box>.wifi_2{position:absolute;height:10px;left:14px}.function_bar>.wifi_box>.wifi_3{position:absolute;height:15px;left:21px}.function_bar>.wifi_box>.wifi_4{position:absolute;width:5px;height:20px;background:#b4b4b4;left:28px;bottom:0;transition:.35s}#sys_msg_box>.btn_box>.btn:hover,.div_select_box>.div_func:hover{background:#074c7e;transition:.35s}.function_bar>.wifi_box>.wifi_0.active,.function_bar>.wifi_box>.wifi_1.active,.function_bar>.wifi_box>.wifi_2.active,.function_bar>.wifi_box>.wifi_3.active,.function_bar>.wifi_box>.wifi_4.active{background:#1176bc;transition:.35s}.div_select_box{position:fixed;top:10px;right:5%;width:auto;height:30px;line-height:30px;font-size:14px;z-index:2}.div_select_box>.div_func{min-width:120px;height:30px;background:#1176bc;transition:.35s}.div_select_box>.div_func_reset{min-width:100px;height:30px;background:#787878;transition:.35s}.div_select_box>.div_func_reset:hover{transition:.35s;background:#1176bc}.div_select_box>.div_select{min-width:150px;height:30px}.div_select_box>.div_select>.func_title{min-width:50px;padding-right:10px;padding-left:10px;height:30px;line-height:30px;font-size:14px;text-align:center;float:left}.div_select_box>.div_select>select{border:none;border-bottom:1px solid #b4b4b4;width:100px;height:30px;text-align:center;float:left;background:0 0}.div_select_box>.div_select>.switch_box{position:relative;border:1px solid #b4b4b4;border-radius:5px;width:78px;height:20px;margin:4px 20px;text-align:center;float:left;cursor:pointer;background:#dcdcdc;transition:.35s;overflow:hidden}.div_select_box>.div_select>.switch_box.active{transition:.35s;background:#e6e6e6}.div_select_box>.div_select>.switch_box>.switch{position:absolute;top:0;line-height:20px;font-size:12px;left:39px;width:39px;height:20px;border-radius:5px;background:#b8b8b8;transition:.35s;color:#fff}.div_select_box>.div_select>.switch_box>.switch.active{transition:.35s;left:0;background:#1176bc}.function_header{position:relative;font-size:14px;width:100%;line-height:30px;margin-top:10px;margin-bottom:10px}.function_header>.title{font-size:18px;width:100px;border-radius:5px;line-height:30px;text-align:left;color:#404040}.function_header>.div_select{position:absolute;top:0;right:0;width:260px;height:30px;line-height:30px;font-size:14px}.function_header>.div_select>.func_title{width:120px;padding-right:20px;height:30px;line-height:30px;font-size:14px;text-align:right;float:left}.function_header>.div_select>select{border:none;border-bottom:1px solid #b4b4b4;width:120px;height:30px;text-align:center;float:left}.header_sub_title>.key,.header_sub_title_time>.key{margin-left:20px;margin-right:20px;border-radius:5px;color:#fff;text-align:center;margin-bottom:10px;float:left;font-weight:700}.header_title{font-size:24px;font-weight:700;margin-bottom:10px;line-height:30px}.header_note,.header_sub_title,.header_sub_title_time{width:50%;font-size:16px;margin-bottom:5px;line-height:30px;float:left}.header_sub_title>.key{width:200px;background-color:#5f9ea0}.header_sub_title>.key.ave{background-color:#d2691e}.header_sub_title>.key.up{background-color:brown}.header_sub_title>.key.low{background-color:#1e90ff}.header_sub_title>.key.ip{background-color:#000}.header_sub_title>.value{width:100px;font-weight:700;text-align:center;margin-bottom:10px;float:left}.header_sub_title_time>.key{width:200px;background-color:#696969}.header_sub_title_time>.value{width:250px;font-weight:700;text-align:left;margin-bottom:10px;float:left}.header_sub_title>.unit{width:50px;font-weight:700;margin-bottom:10px;float:left}.canvas_box{width:100%;height:400px;margin:20px 0;position:relative}#tempCanvas{width:100%;height:100%;display:block;border:1px solid #b4b4b4;background-color:#f0f0f0}.log_msg{width:100%;margin-top:20px}.log_msg_title{display:flex;border-bottom:2px double #b4b4b4;padding-bottom:5px;margin-bottom:10px}.log_msg_data .id,.log_msg_title .id{width:100px}.log_msg_title .title{width:40%;font-weight:700}.log_msg_title .time{width:calc(100% - 100px - 40%);font-weight:700}.log_msg_data_box{max-height:200px;overflow-y:auto;border:1px solid #b4b4b4;padding:10px;background-color:#fafafa}.log_msg_data{display:flex;padding:5px 0;border-bottom:1px solid #d4d4d4}.log_msg_data .title{width:40%}.log_msg_data .time{width:calc(100% - 100px - 40%)}.log_msg_data:last-child{border-bottom:none}#sys_msg_bks{width:100%;height:100%;background:rgba(0,0,0,.5);transition:.35s;backdrop-filter:blur(20px);z-index:1}#sys_msg_bks.active{transition:.35s;display:block}#sys_msg_box{position:fixed;width:500px;height:400px;margin-left:calc(49.5% - 250px);margin-right:calc(49.5% - 250px);padding:.5%;top:200%;background:rgba(255,255,255,.86);transition:.35s;-webkit-backdrop-filter:blur(20px);backdrop-filter:blur(20px);border-radius:5px;border:.2vh solid #1176bc;z-index:3}#sys_msg_box.active{top:calc(50% - 200px);transition:.35s}#sys_msg_box>.title{width:98%;margin-left:1%;margin-right:1%;line-height:40px;font-size:18px;text-align:center;font-weight:700;border-bottom:.1vh solid #b4b4b4}#sys_msg_box>.content{width:96%;height:270px;padding-top:15px;padding-bottom:15px;margin-left:2%;margin-right:2%;font-size:14px;line-height:30px;overflow-y:auto;overflow-x:hidden}#sys_msg_box>.btn_box{width:96%;height:50px;margin:5px 2%}#sys_msg_box>.btn_box>.btn{width:96%;height:50px;margin-left:2%;background:rgba(17,118,188);color:#fff;font-size:16px;line-height:50px;text-align:center;border-radius:5px;cursor:pointer;transition:.35s}}@media only screen and (max-width:1080px){#sys_msg_bks,.function_bar{top:0;left:0;-webkit-backdrop-filter:blur(20px)}.function_bar>.wifi_box>.wifi_0,.function_bar>.wifi_box>.wifi_1,.function_bar>.wifi_box>.wifi_2,.function_bar>.wifi_box>.wifi_3,.function_bar>.wifi_box>.wifi_4{width:.5vh;background:#b4b4b4;bottom:0;transition:.35s}.header_sub_title>.key,.log_msg_data .id,.log_msg_data .time,.log_msg_data .title,.log_msg_title .id,.log_msg_title .time,.log_msg_title .title{overflow:hidden;white-space:nowrap;text-overflow:ellipsis}.main_info{position:relative;width:96%;margin:1% auto;background-color:#fff;padding:7vh 1% 1%;box-shadow:0 0 .2vh rgba(0,0,0,.1)}.function_bar{position:fixed;background:rgba(255,255,255,.8);font-size:1.8vh;width:95%;padding-left:2.5%;padding-right:2.5%;backdrop-filter:blur(20px);border-bottom:.1vh solid #b4b4b4;z-index:3}.function_bar>.title{font-size:1.8vh;width:calc(100% - 8vh);line-height:5vh;text-align:left;color:#404040;font-weight:700;float:left}.function_bar>.wifi_box{width:5vh;height:3vh;margin-bottom:1vh;margin-top:1vh;float:left;position:relative}.function_bar>.wifi_box>.wifi_0{position:absolute;height:.2vh;left:0}.function_bar>.wifi_box>.wifi_1{position:absolute;height:.7vh;left:.7vh}.function_bar>.wifi_box>.wifi_2{position:absolute;height:1.3vh;left:1.4vh}.function_bar>.wifi_box>.wifi_3{position:absolute;height:1.8vh;left:2.1vh}.function_bar>.wifi_box>.wifi_4{position:absolute;height:2.3vh;left:2.8vh}.function_bar>.wifi_box>.wifi_0.active,.function_bar>.wifi_box>.wifi_1.active,.function_bar>.wifi_box>.wifi_2.active,.function_bar>.wifi_box>.wifi_3.active,.function_bar>.wifi_box>.wifi_4.active{background:#1176bc;transition:.35s}.function_bar>.icon_func{position:absolute;top:0;right:0;width:5vh;height:5vh;margin-left:2.5vh;margin-right:.5vh}.div_select_box>.div_func,.div_select_box>.div_func_reset{width:50%;font-size:1.6vh;text-align:center;padding-left:1.5%;padding-right:1.5%;margin:1vh 1%;border-radius:.5vh;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;color:#fff;height:3vh;cursor:pointer;transition:.35s}.function_bar>.icon_func>.icon{width:4vh;height:4vh;margin:.5vh;border-radius:.5vh;font-size:4vh;line-height:4vh;text-align:center;vertical-align:middle;transition:.35s}.function_bar>.icon_func>.icon.active{background:#787878;color:#fff;transition:.35s}.div_select_box{position:fixed;top:-100vh;left:0;width:calc(98%);min-height:3vh;line-height:3vh;font-size:1.8vh;padding:.5vh 1% 1.5vh;background:rgba(245,245,245,.95);transition:.35s;box-shadow:0 1vh 221vh -.5vh transparent;z-index:-5}.div_select_box.active{top:5vh;transition:.35s;box-shadow:0 1vh 2vh -.5vh rgba(0,0,0,.5);z-index:1}.div_select_box>.div_func{line-height:3vh;background:#1176bc}.div_select_box>.div_func:hover{transition:.35s;background:#074c7e}.div_select_box>.div_func_reset{line-height:3vh;background:#787878}.div_select_box>.div_func_reset:hover{transition:.35s;background:#1176bc}.div_select_box>.div_select{width:100%;min-height:3vh;line-height:3vh;font-size:1.8vh;position:relative;padding-top:1vh;padding-bottom:1vh}.div_select_box>.div_select>.func_title{width:calc(100% - 50% - 2vh);padding-left:1vh;padding-right:1vh;min-height:3vh;line-height:3vh;font-size:1.8vh;text-align:left;float:left}.div_select_box>.div_select>select{border:none;border-bottom:.1vh solid #b4b4b4;width:45%;margin-left:2.5%;margin-right:2.5%;font-size:1.8vh;height:3vh;text-align:center;float:left;background:0 0}.div_select_box>.div_select>.switch_box{position:relative;border:.1vh solid #b4b4b4;border-radius:.5vh;width:15.8vh;margin-left:calc(25% - 8vh);margin-right:calc(25% - 8vh);height:2.8vh;text-align:center;float:left;cursor:pointer;background:#dcdcdc;transition:.35s}.div_select_box>.div_select>.switch_box.active{transition:.35s;background:#e6e6e6}.div_select_box>.div_select>.switch_box>.switch{position:absolute;top:0;line-height:2.8vh;font-size:1.4vh;left:7.9vh;width:7.9vh;height:2.8vh;border-radius:.5vh;background:#b8b8b8;transition:.35s;color:#fff}.div_select_box>.div_select>.switch_box>.switch.active{transition:.35s;left:0;background:#1176bc}.function_header{font-size:1.8vh;width:100%;min-height:3vh;margin-top:1vh;margin-bottom:1vh}.function_header>.title{font-size:1.8vh;width:30vh;line-height:3vh;text-align:left;color:#404040}.function_header>.div_select{width:calc(100% - 4vh);margin-left:2vh;min-height:3vh;line-height:3vh;font-size:1.8vh;position:relative;display:block;margin-bottom:1vh}.function_header>.div_select>.func_title{width:calc(100% - 50% - 2vh);padding-right:2vh;min-height:3vh;line-height:3vh;font-size:1.8vh;text-align:left;float:left}.function_header>.div_select>select{border:none;border-bottom:.1vh solid #b4b4b4;width:50%;font-size:1.8vh;height:3vh;text-align:center;float:left}.header_sub_title>.key,.header_sub_title>.value,.header_sub_title_time>.key,.header_sub_title_time>.value{text-align:center;margin-bottom:1vh;float:left;font-weight:700}.header_sub_title>.key,.header_sub_title_time>.key{margin-left:2vh;margin-right:2vh;border-radius:.5vh;color:#fff}.header_title{font-size:2.4vh;font-weight:700;margin-bottom:1vh;line-height:3vh}.header_note,.header_sub_title,.header_sub_title_time{width:100%;font-size:2vh;margin-bottom:1vh;line-height:3vh;float:left}.header_sub_title>.key{width:calc(50% - 4vh);background-color:#5f9ea0}.header_sub_title>.key.ave{background-color:#d2691e}.header_sub_title>.key.up{background-color:brown}.header_sub_title>.key.low{background-color:#1e90ff}.header_sub_title>.key.ip{background-color:#000}.header_sub_title>.value{width:30%}.header_sub_title_time>.key{width:calc(50% - 4vh);background-color:#696969}.header_sub_title_time>.value{width:50%}.header_sub_title>.unit{width:20%;font-weight:700;margin-bottom:1vh;float:left}.canvas_box{width:100%;height:30vh;margin:auto;position:relative;display:block}.log_msg_data,.log_msg_title{display:flex;line-height:3vh}#tempCanvas{width:100%;height:100%;display:block;border:.1vh solid #b4b4b4;background-color:#f0f0f0}.log_msg{width:100%;margin-top:2vh}.log_msg_title{font-size:2vh;border-bottom:.4vh double #b4b4b4;padding-bottom:.5vh;margin-bottom:1vh}.log_msg_data .id,.log_msg_title .id{width:10vw}.log_msg_title .title{font-size:1.8vh;line-height:3vh;width:40%;font-weight:700}.log_msg_title .time{font-size:1.8vh;line-height:3vh;width:calc(100% - 10vw - 40%);font-weight:700}.log_msg_data_box{max-height:40vh;overflow-y:auto;border:.1vh solid #b4b4b4;padding:1vh;background-color:#fafafa}.log_msg_data{font-size:1.8vh;padding:.5vh 0;border-bottom:.1vh solid #d4d4d4}.log_msg_data .title{width:40%}.log_msg_data .time{width:calc(100% - 10vw - 40%)}.log_msg_data:last-child{border-bottom:none}#sys_msg_bks{position:fixed;width:100%;height:100%;background:rgba(0,0,0,.5);transition:.35s;backdrop-filter:blur(20px);z-index:1;display:none}#sys_msg_bks.active{transition:.35s;display:block}#sys_msg_box{position:fixed;width:calc(96% - .4vh);height:40vh;margin-left:1.5%;margin-right:1.5%;padding:.5%;top:200%;background:rgba(255,255,255,.86);transition:.35s;-webkit-backdrop-filter:blur(20px);backdrop-filter:blur(20px);border-radius:.5vh;border:.2vh solid #1176bc;z-index:3}#sys_msg_box.active{top:calc(50% - 20vh);transition:.35s}#sys_msg_box>.title{width:98%;margin-left:1%;margin-right:1%;line-height:4vh;font-size:2.2vh;text-align:center;font-weight:700;border-bottom:.1vh solid #b4b4b4}#sys_msg_box>.content{width:96%;height:27vh;padding-top:1.5vh;padding-bottom:1.5vh;margin-left:2%;margin-right:2%;font-size:1.8vh;line-height:3vh;overflow-y:auto;overflow-x:hidden}#sys_msg_box>.btn_box{width:96%;height:5vh;margin:.5vh 2%}#sys_msg_box>.btn_box>.btn{width:96%;height:5vh;margin-left:2%;background:rgba(17,118,188);color:#fff;font-size:1.8vh;line-height:5vh;text-align:center;border-radius:.5vh;transition:.35s;cursor:pointer}#sys_msg_box>.btn_box>.btn:hover{background:#074c7e;transition:.35s}}";
    message += "</style>";
    message += "</head>";
    message += "<body onload='draw()'>";
    message += "<div id='sys_msg_bks'></div> <div id='sys_msg_box'> <div id='msg_box_title' class='title'></div> <div id='msg_box_context' class='content'></div> <div class='btn_box'> <div class='btn' onclick='bks_text()'>Close</div> </div> </div>";
    message += "<div class='main_info'>";
    message += "<div class='function_bar'> <div class='title'>Controll Pannel</div> <div class='wifi_box' id='wifi_box' title=''> <div id='wifi_0' class='wifi_0'></div> <div id='wifi_1' class='wifi_1'></div> <div id='wifi_2' class='wifi_2'></div> <div id='wifi_3' class='wifi_3'></div> <div id='wifi_4' class='wifi_4'></div> </div> <div class='icon_func'> <div id='navi_icon' class='icon' onclick='show_navi()'>≡</div> </div> <div class='clear_left'></div> </div> <div id='div_select_box' class='div_select_box'> <div onClick='download_csv()' class='div_func'> Download CSV </div><div onClick='reset_log()' class='div_func_reset'> Reset Log </div> <div class='div_select'> <div class='func_title'>Refresh Rate</div> <select id='refresh_rate' onchange='change_status_manager(3);'> <option value='5'>5 sec</option> <option value='10'>10 sec</option> <option value='30'>30 sec</option> <option value='60'>60 sec</option> </select> <div class='clear_left'></div> </div> <div class='div_select'> <div class='func_title'>Module</div> <select id='module_select' onchange='change_status_manager(0);'> <option value='ESP32_00'>ESP32_00</option> </select> <div class='clear_left'></div> </div> <div class='div_select'> <div class='func_title'>LCD</div> <div class='switch_box' id='switch_option_box' onclick='lcd_switch();'> <div id='switch_option' class='switch'>OFF</div> <input type='hidden' id='lcd_setting' value='0'> </div> <div class='clear_left'></div> </div> <div style='display:none;' class='div_select'> <div class='func_title'>LED signal</div> <div class='switch_box' id='switch_option_box_led' onclick='led_switch();'> <div id='switch_option_led' class='switch'>OFF</div> <input type='hidden' id='led_setting' value='0'> </div> <div class='clear_left'></div> </div> <div class='clear_right'></div> </div>";
    message += "<div class='header_title'>Environment Temperature Monitoring Module - " + String(IOT_ID) + "</div>";
    message += "<div class='header_sub_title'> <div class='key'> Temperature </div> <div id='TMP_DATA' class='value'>";
    // 顯示溫度
    // if (tempC == -1.0)
    // {
    //     // message += "N.A.";
    // }
    // else
    // {
    //     // message += String(tempC);
    //     // update_data_log(tempC);  // 更新數據日誌
    // }
    
    message += " - ";
    message += "</div> <div class='unit'> °C </div> <div class='clear_left'></div></div>";
    message += "<div class='header_sub_title'> <div class='key ave'> Average Temperature </div> <div id='TMP_DATA_AVE' class='value'>";
    // // 顯示溫度
    // if (tempC == -1.0) {
    //   message += "N.A.";
    // } else {
    //   message += String(tempC);

    // }
    message += " - ";
    message += "</div> <div class='unit'> °C </div> <div class='clear_left'></div> </div>";
    message += "<div class='header_sub_title'> <div class='key up'> Max. Temperature </div> <div id='TMP_DATA_MAX' class='value'> - </div> <div class='unit'> °C </div> <div class='clear_left'></div> </div>";
    message += "<div class='header_sub_title'> <div class='key low'> Min. Temperature </div> <div id='TMP_DATA_MIN' class='value'> - </div> <div class='unit'> °C </div> <div class='clear_left'></div> </div>";
    message += "<div class='header_sub_title_time'> <div class='key'> Last Update Time </div> <div id='TMP_TIME' class='value'>";
    // 顯示目前時間
    // message += getCurrentTime();
    message += "</div> <div class='clear_left'></div> </div>";
    message += "<div class='header_sub_title_time'> <div class='key ip'> System Active  </div> <div id='TMP_WORKS' class='value' style='text-align:center;'>";
    // message += formatElapsedTime(elapsed_Millis,sys_days);
    message += "</div> <div class='clear_left'></div> </div>";
    message += "<div class='clear_left'></div>";
    message += "<div class='header_note'>The page will automatically refresh every <span id='fresh_span'>"+String(FLASH_RATE)+"</span> sec.</div>";
    // 顯示畫布
    message += "<div class='canvas_box'> <canvas id='tempCanvas'></canvas> </div><div class='clear_left'></div>";
    message += "<div class='function_header'> <div class='title'>Log</div> </div>";
    // 顯示數據日誌
    message += "<div class='log_msg'> <div class='log_msg_title'> <div class='id'>ID</div><div class='time'>Time Stamp</div> <div class='title'>Temperature (°C)</div> </div>";
    message += "<div class='log_msg_data_box' id='logData'> </div>";
    message += "</div>";
    message += "<script>";
    // message += "let points = new Array();";
    // message += "let TimeStamp_Log = new Array();";
    // for (int i = 0; i < data_log_count - 1; i++) {
    //   message += "points.push({ x: "+String(i)+", y: "+String(time_log[i].temp)+" });";
    // }
    // for (int i = 0; i < data_log_count - 1; i++) {
    //   message += "TimeStamp_Log.push('"+String(time_log[i].str_time)+"');";
    // }
    message += "let points = new Array(); let TimeStamp_Log = new Array();";
    message += " const canvas = document.getElementById('tempCanvas'); const ctx = canvas.getContext('2d'); function calcMeanValue(points_ref) { let Y_mean = 0; let tmp = 0; for (let i = 0; i < points_ref.length; i++) { tmp += points_ref[i].y; } Y_mean = tmp / points_ref.length; return Y_mean; } function drawGrid(ctx, width, height, gridX, gridY, color, majorInterval) { ctx.beginPath(); ctx.strokeStyle = color; ctx.lineWidth = 1; for (let x = 0; x <= width; x += gridX) { ctx.moveTo(x, 0); ctx.lineTo(x, height); } for (let y = 0; y <= height; y += gridY) { ctx.moveTo(0, y); ctx.lineTo(width, y); } ctx.stroke(); } function drawAxes(ctx, width, height, color) { ctx.beginPath(); ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.moveTo(0, height / 2); ctx.lineTo(width, height / 2); ctx.moveTo(0, 0); ctx.lineTo(0, height); ctx.stroke(); } function drawTickMarks(ctx, width, height, gridX, gridY, color, Y_upper, Y_lower) { const fontSize = Math.max(16, Math.round(height / 40)); ctx.font = `${fontSize}px Arial`; ctx.fillStyle = color; ctx.textAlign = 'center'; ctx.textBaseline = 'top'; const dataIntervalX = 100; const labelIntervalX = dataIntervalX * (width / 1000); for (let i = 1; i < 10; i++) { const x = i * (width / 10); const label = (i * dataIntervalX).toString();  ctx.fillText(label, x, height - 20); } ctx.textAlign = 'right'; ctx.textBaseline = 'middle'; const numYLabels = 10; for (let i = 0; i <= numYLabels; i++) { const value = Y_lower + (i * (Y_upper - Y_lower)) / numYLabels; const y = height - (i * height) / numYLabels; ctx.fillText(value.toFixed(1), -5, y); } } function drawCenterOfY(ctx, width, height, y_mean, color) { ctx.beginPath(); ctx.strokeStyle = color; ctx.lineWidth = 2; const y_position = height / 2; ctx.moveTo(0, y_position); ctx.lineTo(width, y_position); ctx.stroke(); } function drawLineChart(ctx, points, scaleX, scaleY, Y_mean, rectHeight) { ctx.beginPath(); if (points.length > 0) { const firstPoint = points[0]; ctx.moveTo(firstPoint.x * scaleX, rectHeight / 2 - (firstPoint.y - Y_mean) * scaleY); for (let i = 1; i < points.length; i++) { ctx.lineTo(points[i].x * scaleX, rectHeight / 2 - (points[i].y - Y_mean) * scaleY); } } ctx.strokeStyle = 'blue'; ctx.lineWidth = 2; ctx.stroke(); } function dynamic_Y(points_ref) { let default_boundary_torr = 10; let boundary_torr = 0; let tmp_max = points_ref[0].y; let tmp_min = tmp_max; for (let i = 1; i < points_ref.length; i++) { if (tmp_max < points_ref[i].y) { tmp_max = points_ref[i].y; } if (tmp_min >= points_ref[i].y) { tmp_min = points_ref[i].y; } } boundary_torr = (tmp_max - tmp_min); if (boundary_torr > default_boundary_torr) { return boundary_torr; } else { return default_boundary_torr; } } function calcMinMax(points_ref) { let tmp_max = points_ref[0].y; let tmp_min = tmp_max; for (let i = 1; i < points_ref.length; i++) { if (tmp_max < points_ref[i].y) { tmp_max = points_ref[i].y; } if (tmp_min >= points_ref[i].y) { tmp_min = points_ref[i].y; } } var output = new Array(); output[0] = Math.round(tmp_max * 100) / 100; output[1] = Math.round(tmp_min * 100) / 100; return output; } function draw() { const dpr = window.devicePixelRatio || 1; const rect = canvas.getBoundingClientRect(); canvas.width = rect.width * dpr; canvas.height = rect.height * dpr; ctx.scale(dpr, dpr); ctx.clearRect(0, 0, rect.width, rect.height); const Y_mean = calcMeanValue(points); const torrent_Y = dynamic_Y(points); const Y_upper_boundary = Y_mean + torrent_Y; const Y_lower_boundary = Y_mean - torrent_Y; const gridX = rect.width / 10; const gridY = rect.height / 10; drawGrid(ctx, rect.width, rect.height, gridX, gridY, '#e0e0e0', gridX * 10); drawAxes(ctx, rect.width, rect.height, '#000000'); drawTickMarks(ctx, rect.width, rect.height, gridX, gridY, '#000000', Y_upper_boundary, Y_lower_boundary); drawCenterOfY(ctx, rect.width, rect.height, Y_mean, '#ff0000'); const originalWidth = 1000; const originalHeight = torrent_Y * 2; const scaleX = rect.width / originalWidth; const scaleY = rect.height / originalHeight; drawLineChart(ctx, points, scaleX, scaleY, Y_mean, rect.height); max_min_tmp = calcMinMax(points); document.getElementById('TMP_DATA_AVE').innerHTML = Y_mean.toFixed(2); document.getElementById('TMP_DATA_MIN').innerHTML = max_min_tmp[1].toFixed(2); document.getElementById('TMP_DATA_MAX').innerHTML = max_min_tmp[0].toFixed(2); } function updateTemperatureDisplay() {  const logData = document.getElementById('logData'); var tmp_log = ''; for (var i=0;i<points.length;i++){ currentTemp = (points[i].y).toFixed(2); TimeStamp = (TimeStamp_Log[i]);tmp_log = tmp_log + `<div class='log_msg_data'><div class='id'>[${i}]</div><div class='time'>${TimeStamp}</div><div class='title'>${currentTemp}</div></div>`; } logData.innerHTML = tmp_log; logData.scrollTop = logData.scrollHeight;}";
    message += " window.onload = () => { updateTemperatureDisplay(); draw(); }; window.addEventListener('resize', () => { ctx.setTransform(1, 0, 0, 1, 0, 0); draw(); });";
    // message += "console.log(points);";
    // message += "console.log(TimeStamp_Log);";
    // message += "console.log(torrent_Y);";
    message += "function change_status_manager(func_id) { let func_set = ''; let code = ''; if (func_id == 1) { func_set = 'oled'; code = document.getElementById('lcd_setting').value; setOLED(code); } else if (func_id == 2) { func_set = 'led'; code = document.getElementById('led_setting').value; } else if (func_id == 3) { func_set = 'freq'; code = document.getElementById('refresh_rate').value; document.getElementById('fresh_span').innerHTML = code; clearInterval(system_update); system_update = setInterval(other_request, code * 1000); } else if (func_id == 9) { func_set = 'update'; } else if (func_id == 0) { func_set = 'modules'; code = document.getElementById('module_select').value; } } async function setOLED(value) { const url = `${getBaseURL()}/api/oled/${value}`; console.log(url); try { const response = await fetch(url, { method: 'GET', headers: { 'Content-Type': 'application/json' } }); const data = await response.json(); console.log('OLED回傳的資料:', data); value = parseInt(value); console.log(value); if (value) { document.getElementById('lcd_setting').value = 1; document.getElementById('switch_option').innerHTML = 'ON'; document.getElementById('switch_option').classList.add('active'); document.getElementById('switch_option_box').classList.add('active'); } else { document.getElementById('lcd_setting').value = 0; document.getElementById('switch_option').innerHTML = 'OFF'; document.getElementById('switch_option').classList.remove('active'); document.getElementById('switch_option_box').classList.remove('active'); } } catch (error) { console.error('發生錯誤:', error); } } async function fetchOLEDStatus() { const url = `${getBaseURL()}/api/oled/status`; try { let oled_status = 0; const response = await fetch(url); const data = await response.json(); console.log('OLED狀態碼：' + data.status); console.log(data); if (data.status == 'ON') { oled_status = 1; } console.log(oled_status); if (oled_status) { document.getElementById('lcd_setting').value = 1; document.getElementById('switch_option').innerHTML = 'ON'; document.getElementById('switch_option').classList.add('active'); document.getElementById('switch_option_box').classList.add('active'); } else { document.getElementById('lcd_setting').value = 0; document.getElementById('switch_option').innerHTML = 'OFF'; document.getElementById('switch_option').classList.remove('active'); document.getElementById('switch_option_box').classList.remove('active'); } } catch (error) { console.error('取得 OLED 狀態時發生錯誤:', error); } } async function fetchTime() { const url = `${getBaseURL()}/api/time`; try { const response = await fetch(url); const data = await response.json(); console.log('現在時間：'+data.time); document.getElementById('TMP_TIME').innerHTML = data.time; } catch (error) { console.error('取得系統時間發生錯誤:', error); document.getElementById('TMP_TIME').innerHTML = 'Time service error'; } } async function fetchPowerOn() { const url = `${getBaseURL()}/api/powerOn`; try { const response = await fetch(url); const data = await response.json(); console.log('運作時間：'+data.uptime); console.log(data); document.getElementById('TMP_WORKS').innerHTML = data.uptime; } catch (error) { console.error('取得系統運作時間發生錯誤:', error); document.getElementById('TMP_WORKS').innerHTML = 'Unknown Status'; } } async function fetchTemperature() { const url = `${getBaseURL()}/api/temperature`; try { const response = await fetch(url); const data = await response.json(); console.log('單一溫度：'+data.temperature); document.getElementById('TMP_DATA').innerHTML = parseFloat(data.temperature).toFixed(2); } catch (error) { console.error('取得溫度時發生錯誤:', error); document.getElementById('TMP_DATA').innerHTML = 'N.A.'; } } async function fetchTemperatureLog() { const url = `${getBaseURL()}/api/temperature/log`; try { const response = await fetch(url); const data = await response.json(); console.log('溫度紀錄：'+data); console.log(data); console.log(data.log.length); console.log(data.log[0].temperature); console.log(data.log[0].timeStamp); points = new Array(); TimeStamp_Log = new Array(); for (var i = 0; i < data.log.length; i++) { points.push({ x: i, y: parseFloat(data.log[i].temperature) }); TimeStamp_Log.push(data.log[i].timeStamp); } updateTemperatureDisplay(); draw(); console.log(points); console.log(TimeStamp_Log); } catch (error) { console.error('取得溫度日誌時發生錯誤:', error); } } async function fetchWIFIStatus() { const url = `${getBaseURL()}/api/wifi/status`; type = 'controlled'; try { const response = await fetch(url); const data = await response.json(); wifi_icon_controller(type, data.bars); document.getElementById('wifi_box').title = '[' + data.rssi + ' db]' + data.level; } catch (error) { console.error('取得 WIFI 狀態時發生錯誤:', error); document.getElementById('wifi_box').title = 'disconnected'; wifi_icon_controller(type, -1); } }";
    message += "function download_csv() { let row_title = ['temp', 'timestamp']; let csvContent = 'data:text/csv;charset=utf-8,'; csvContent += row_title.join('', '') + '\\r\\n'; for (var i = 0; i < points.length; i++) { let row = TimeStamp_Log[i] + ',' + points[i].y.toFixed(2); csvContent += row + '\\r\\n'; } var encodedUri = encodeURI(csvContent); var link = document.createElement('a'); link.setAttribute('href', encodedUri); link.setAttribute('download', 'ESP32.csv'); document.body.appendChild(link); link.click(); } function show_navi() { var status = document.getElementById('div_select_box').classList.contains('active'); if (status) { document.getElementById('div_select_box').classList.remove('active'); document.getElementById('navi_icon').innerHTML = '≡'; } else { document.getElementById('div_select_box').classList.add('active'); document.getElementById('navi_icon').innerHTML = '⨉'; } } function lcd_switch() { var option = document.getElementById('switch_option').classList.contains('active'); console.log('OLED --> '+option); console.log(option); if (option) { document.getElementById('lcd_setting').value = 0; document.getElementById('switch_option').classList.remove('active'); document.getElementById('switch_option_box').classList.remove('active'); document.getElementById('switch_option').innerHTML = 'OFF'; } else { document.getElementById('lcd_setting').value = 1; document.getElementById('switch_option').classList.add('active'); document.getElementById('switch_option_box').classList.add('active'); document.getElementById('switch_option').innerHTML = 'ON'; } change_status_manager(1); } function led_switch() { var option = document.getElementById('switch_option_led').classList.contains('active'); console.log(option); if (option) { document.getElementById('led_setting').value = 0; document.getElementById('switch_option_led').classList.remove('active'); document.getElementById('switch_option_box_led').classList.remove('active'); document.getElementById('switch_option_led').innerHTML = 'OFF'; } else { document.getElementById('led_setting').value = 1; document.getElementById('switch_option_led').classList.add('active'); document.getElementById('switch_option_box_led').classList.add('active'); document.getElementById('switch_option_led').innerHTML = 'ON'; } change_status_manager(2); } function bks_text(title, msg) { var option = document.getElementById('sys_msg_bks').classList.contains('active'); if (option) { document.getElementById('sys_msg_bks').classList.remove('active'); document.getElementById('sys_msg_box').classList.remove('active'); document.getElementById('msg_box_title').innerHTML = title; document.getElementById('msg_box_context').innerHTML = msg; } else { document.getElementById('sys_msg_bks').classList.add('active'); document.getElementById('sys_msg_box').classList.add('active'); document.getElementById('msg_box_title').innerHTML = title; document.getElementById('msg_box_context').innerHTML = msg; } } function wifi_signal_icon(range) { wifi_icon_controller('init', 0); wifi_icon_controller('controlled', range); } function wifi_icon_controller(type, range) { if (type == 'init') { for (var i = 0; i < 5; i++) { document.getElementById('wifi_' + i).classList.remove('active'); } } else if (type == 'controlled') { for (var i = 0; i < range + 1; i++) { document.getElementById('wifi_' + i).classList.add('active'); } } } function getBaseURL(callback) { return window.location.origin; }";
    message += "async function other_request(func_id) { if (func_id == 'WIFI') { await fetchWIFIStatus(); } else if (func_id == 'TMP') { await fetchTemperature(); } else if (func_id == 'TMP_LOG') { await fetchTemperatureLog(); } else if (func_id == 'SYS_TIME') { await fetchTime(); } else if (func_id == 'OLED') { await fetchPowerOn(); } else if (func_id == 'POWER_ON') { await fetchPowerOn(); } else if (func_id == 'RESET') { reset_log();} else if (func_id == 'INIT') { await fetchTime(); await fetchTemperature(); await fetchTemperatureLog(); await fetchOLEDStatus(); await fetchWIFIStatus(); await fetchPowerOn(); } else { await fetchTime(); await fetchTemperature(); await fetchTemperatureLog(); await fetchWIFIStatus(); await fetchPowerOn(); } } ";
    message += "let system_update = setInterval(other_request, document.getElementById('refresh_rate').value * 1000); other_request('INIT');";
    message += "async function reset_log(){ const url = `${getBaseURL()}/api/temperature/reset`; try { let oled_status = 0; const response = await fetch(url); const data = await response.json(); console.log('清除狀態碼：' + data.status); console.log(data); if(data.status){ points = new Array(); TimeStamp_Log = new Array(); updateTemperatureDisplay(); draw(); }else{ } } catch (error) { console.error('清除狀態時發生錯誤:', error); } }";
    message += "</script>";

    message += "</body></html>";
    server.send(200, "text/html", message);

    // 控制傳輸時 LED_ID_ERR 閃爍
    digitalWrite(LED_ID_ERR, HIGH);
    delay(100); // 短暫閃爍
    digitalWrite(LED_ID_ERR, LOW);
}

// 處理 GET /api/temperature/sampling 請求
void handleGetTemperatureSampling()
{
    DynamicJsonDocument doc(100);
    doc["sampling_freq"] = diff_save_timer;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}
// 處理 GET /api/temperature/sampling 請求
void handleGetTemperatureSamplingFreq()
{
    String uri = server.uri();

    if (uri.startsWith("/api/temperature/sampling/"))
    {
        String valueStr = uri.substring(strlen("/api/temperature/sampling/"));
        diff_save_timer = valueStr.toInt();
        DynamicJsonDocument doc(100);
        char status[100]; // 確保有足夠空間來存儲字串
        sprintf(status, "Sampling Freq = %d sec.", diff_save_timer);
        doc["status"] = status;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    }
    else
    {
        // 處理其他未匹配的路由
        server.send(404, "application/json", "{\"error\":\"Not Found\"}");
    }
}

// Handler：處理 PUT /api/led 請求
void handleSetLED()
{
    if (server.method() != HTTP_PUT)
    {
        server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        return;
    }

    if (server.hasArg("plain"))
    {
        String body = server.arg("plain");

        // 使用 ArduinoJson 解析 JSON
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, body);
        if (error)
        {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        const char *led = doc["led"];
        if (led)
        {
            if (strcmp(led, "on") == 0)
            {
                // digitalWrite(LED_ID_ACT, HIGH);
                // digitalWrite(LED_ID_ERR, HIGH);
                ledOn = true;
                server.send(200, "application/json", "{\"status\":\"LED is ON\"}");
                return;
            }
            if (strcmp(led, "off") == 0)
            {
                digitalWrite(LED_ID_ACT, LOW);
                digitalWrite(LED_ID_ERR, LOW);
                ledOn = false;
                server.send(200, "application/json", "{\"status\":\"LED is OFF\"}");
                return;
            }
        }
    }
    server.send(400, "application/json", "{\"error\":\"Bad Request\"}");
}



// Handler：處理 GET /api/led/status 請求
void handleGetLedStatus()
{
    DynamicJsonDocument doc(100);
    if (digitalRead(LED_ID_ACT) == HIGH)
    {
        doc["status"] = "ON";
    }
    else
    {
        doc["status"] = "OFF";
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// Handler：處理 GET /api/oled/status 請求
void handleGetOledStatus()
{
    DynamicJsonDocument doc(100);
    doc["status"] = oledOn ? "ON" : "OFF";

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// Handler：處理動態路由 /api/oled/<value>
void handleOledControl()
{
    String uri = server.uri();

    if (uri.startsWith("/api/oled/"))
    {
        String valueStr = uri.substring(strlen("/api/oled/"));
        int value = valueStr.toInt();

        DynamicJsonDocument doc(100);

        if (value == 1)
        {
            oledOn = true;
            // 實作 OLED 開啟代碼
            doc["status"] = "OLED is ON";
        }
        else if (value == 0)
        {
            oledOn = false;
            // 實作 OLED 關閉代碼
            doc["status"] = "OLED is OFF";
        }
        else
        {
            doc["error"] = "Invalid OLED value. Use 0 or 1.";
            String response;
            serializeJson(doc, response);
            server.send(400, "application/json", response);
            return;
        }

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    }
    else
    {
        // 處理其他未匹配的路由
        server.send(404, "application/json", "{\"error\":\"Not Found\"}");
    }
}

// Handler：處理 GET /api/time 請求
void handleGetTime()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        server.send(500, "application/json", "{\"error\":\"Failed to obtain time\"}");
        return;
    }
    // char timeStringBuff[50] = getCurrentTime();
    char* timeStringBuff = getCurrentTime();
    String timeStr = String(timeStringBuff);
    DynamicJsonDocument doc(100);
    doc["time"] = timeStr;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}
// Handler：處理 GET /api/powerOn 請求
void handleGetPowerOn()
{
    unsigned long uptimeMillis = millis();
    unsigned long seconds = uptimeMillis / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    String uptimeStr = "";
    if (days > 0)
        uptimeStr += String(days) + "d ";
    if (hours % 24 > 0)
        uptimeStr += String(hours % 24) + "h ";
    if (minutes % 60 > 0)
        uptimeStr += String(minutes % 60) + "m ";
    uptimeStr += String(seconds % 60) + "s";

    DynamicJsonDocument doc(100);
    doc["uptime"] = uptimeStr;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}
// Handler：處理 GET /api/temperature 請求
void handleGetTemperature()
{
    float tempC = ktc.readCelsius();
    DynamicJsonDocument doc(200);

    if (tempC == -1.0)
    {
        doc["temperature"] = "N.A.";
    }
    else
    {
        doc["temperature"] = tempC;
        update_data_log(tempC);
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}
// Handler：處理 GET /api/temperature/log 請求
void handleGetTemperatureLog()
{
    DynamicJsonDocument doc(200 * MAX_LOG); // 根據需要調整大小
    JsonArray logArray = doc.createNestedArray("log");
    for (int i = 0; i < data_log_count; i++)
    {
        JsonObject entry = logArray.createNestedObject();
        entry["temperature"] = String(time_log[i].temp);   // 將溫度資料添加到物件中
        entry["timeStamp"] = String(time_log[i].str_time); // 將時間戳資料添加到物件中
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// Handler：處理 GET /api/wifi/status 請求
void handleGetWifiStatus()
{
    int32_t rssi = WiFi.RSSI();
    SignalStrength signal = getSignalStrengthLevel(rssi);

    DynamicJsonDocument doc(200);
    doc["rssi"] = rssi;
    doc["level"] = signal.level;
    doc["bars"] = signal.bars;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// Handler：處理 GET /api/temperature/reset 請求
void handleResetLog()
{


    init_data_log();
    float tmp_res = 0.0 ;
    for (int i=0;i<data_log_count;i++){
      tmp_res += time_log[i].temp ;
    }
    DynamicJsonDocument doc(200);
    if (tmp_res == 0.0){
      doc["status"] = 1;
      total_count = 0 ;
      data_log_count = 0 ;
    }else{
      doc["status"] = 0;
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// 合併 handleOledControl 和 handleNotFound
void handleNotFound()
{
    String uri = server.uri();
    String method = server.method() == HTTP_GET ? "GET" :
                    server.method() == HTTP_POST ? "POST" :
                    server.method() == HTTP_PUT ? "PUT" :
                    "UNKNOWN";

    Serial.printf("Received %s request for %s\n", method.c_str(), uri.c_str());

    // 檢查是否是 /api/oled/<value>
    if (uri.startsWith("/api/oled/"))
    {
        // 提取 <value>
        String valueStr = uri.substring(strlen("/api/oled/"));
        int value = valueStr.toInt();

        if (value == 1)
        {
            turnOledOn();
            server.send(200, "application/json", "{\"status\":\"OLED turned on\"}");
            return;
        }
        else if (value == 0)
        {
            turnOledOff();
            server.send(200, "application/json", "{\"status\":\"OLED turned off\"}");
            return;
        }
        else
        {
            // 無效的值
            DynamicJsonDocument doc(100);
            doc["error"] = "Invalid OLED value. Use 0 or 1.";
            String response;
            serializeJson(doc, response);
            server.send(400, "application/json", response);
            return;
        }
    }

    // 如果不是 /api/oled/<value>，處理為 404
    DynamicJsonDocument doc(100);
    doc["error"] = "404 Not Found";
    doc["uri"] = uri;
    doc["method"] = method;
    
    // 如果有參數，可以處理 args
    if (server.args() > 0)
    {
        JsonObject args = doc.createNestedObject("args");
        for (uint8_t i = 0; i < server.args(); i++)
        {
            args[server.argName(i)] = server.arg(i);
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(404, "application/json", response);
}


void setup(void)
{
    init_data_log();
    init_Millis = millis();
    log_save_timer = millis();
    // OLED 和溫度感測器初始化
    u8g2.setContrast(250);
    u8g2.begin();

    // 設置 LED 輸出
    pinMode(LED_ID_ACT, OUTPUT);
    pinMode(LED_ID_ERR, OUTPUT);

    // 初始化 Wi-Fi 連接
    WiFi.begin(SSID, PSWD);

    // 連接 Wi-Fi，嘗試連接直到成功為止
    while (WiFi.status() != WL_CONNECTED)
    {
        if (ledOn){
        delay(500);
        digitalWrite(LED_ID_ERR, HIGH); // 若無法連接，LED_ID_ERR 恆亮
        digitalWrite(LED_ID_ACT, LOW);  // 若無法連接，LED_ID_ACT 關閉
        }
    }
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    // 當連接成功，LED_ID_ACT 恆亮，LED_ID_ERR 關閉
    digitalWrite(LED_ID_ACT, HIGH);
    digitalWrite(LED_ID_ERR, LOW);

    // 啟動 WebServer，並且定義回應的路由
    server.on("/", handleRoot);

    // 設置 WebServer 路由
    server.on("/api/led", HTTP_PUT, handleSetLED);                        // PUT /api/led
    server.on("/api/led/status", HTTP_GET, handleGetLedStatus);           // GET /api/led/status
    server.on("/api/oled/status", HTTP_GET, handleGetOledStatus);         // GET /api/oled/status
    server.on("/api/time", HTTP_GET, handleGetTime);                      // GET /api/time
    server.on("/api/powerOn", HTTP_GET, handleGetPowerOn);                // GET /api/powerOn
    server.on("/api/temperature", HTTP_GET, handleGetTemperature);        // GET /api/temperature
    server.on("/api/temperature/log", HTTP_GET, handleGetTemperatureLog); // GET /api/temperature/log
    server.on("/api/wifi/status", HTTP_GET, handleGetWifiStatus);         // GET /api/wifi/status
    server.on("/api/temperature/sampling/status", HTTP_GET, handleGetTemperatureSampling);
    server.on("/api/temperature/sampling", HTTP_GET, handleGetTemperatureSamplingFreq);
    server.on("/api/temperature/reset", HTTP_GET, handleResetLog);
    // 設置動態路由處理 /api/oled/<value>
    // server.onNotFound(handleOledControl);

    // 處理其他未匹配的路由
    server.onNotFound(handleNotFound);

    server.begin();
}
void loop(void)
{
    unsigned long currentMillis = millis();
    unsigned long elapsed_Millis = currentMillis - init_Millis; // 計算從上次開始的經過時間
    unsigned long log_timer = currentMillis - log_save_timer;
    // 每當經過一天（86400000 毫秒），sys_days 增加，並將 init_Millis 重設為當前 millis()
    if (elapsed_Millis >= 86400000)
    {
        sys_days++;             // 增加天數
        init_Millis = millis(); // 重設 init_Millis 為當前 millis()，防止 overflow
        elapsed_Millis = 0;     // 重設經過時間
    }

    // 每秒更新一次時間
    if (currentMillis - previousMillis >= 1000)
    {
        previousMillis = currentMillis;
        seconds = (seconds + 1) % 60; // 秒數從 0 到 59，60 則重新計算
    }

    // 讀取溫度值
    float tempC = ktc.readCelsius();
    if (log_timer >= diff_save_timer * 1000)
    {
        update_data_log(tempC);
        log_save_timer = millis(); // 重設 log_save_timer 為當前 millis()
         // 更新時 LED_ID_ERR 閃爍
        digitalWrite(LED_ID_ERR, HIGH);
        delay(100); // 短暫閃爍
        digitalWrite(LED_ID_ERR, LOW);
    }

    // 更新 OLED 顯示
    u8g2.firstPage();
    do
    {
        u8g2.setFont(u8g2_font_ncenB10_tr);

        // 顯示溫度或 "N.A." 若溫度感測器未插入
        char buffer_tmp[20];
        if (tempC == -1.0)
        {
            sprintf(buffer_tmp, "Tp = N.A.");
        }
        else
        {
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