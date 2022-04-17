#define FWVERSION "2.4.0-cmdlogtest2"

#define LOGGING_INTERVAL 21600000  //Log system status at regular intervals
#define ENABLE_FAILSAVE_BOOT
//#define ENABLE_SERIAL


/*
 * 
 * Info to compile the firmware yourself:
 * Build is done using Arduino IDE, used libs are downloaded when cloning from git.
 * 
 * For CVE rev 2 (module for itho fans):
 * Select 'ESP32 Dev Module' as board
 * Choose partition scheme: Minimal SPIFFS (1.9 APP with OTA)
 * goto file hardware.h and uncomment CVE and comment NON_CVE define
 * 
 * For non CVE rev 1 (module with RJ45 connector):
 * Select 'ESP32 Dev Module' as board
 * Choose partition scheme: Minimal SPIFFS (1.9 APP with OTA)
 * goto file hardware.h and uncomment NON_CVE and comment CVE define
 * 
 */

/*
 Backlog:
 * (todo) make front end buttons dependent on vremote0 type
 * (todo) check load of remotes config file between 2.3.5 and 2.4.0
 * (todo) After timer, go back to fallback or last value
 * (todo) Prevent flash write during interrupt to prevent crashes
 * (todo) restructure code and add PlatformIO support
 * (todo) add support for 536-0106 remote, https://github.com/arjenhiemstra/ithowifi/issues/78
 * (todo) make status parameters selectable to include in update or not
 * 
 */


#include "hardware.h"
#include "dbglog.h"
#include "flashLog.h"
#include "statics.h"
#include "i2c_esp32.h"
#include "IthoSystem.h"
#include "notifyClients.h"
#include "IthoCC1101.h"
#include <ArduinoJson.h>  // https://github.com/bblanchon/ArduinoJson [6.19.3]
#include <ESPAsyncWebServer.h>  // https://github.com/me-no-dev/ESPAsyncWebServer [latest]
#include <SPIFFSEditor.h>       // https://github.com/me-no-dev/ESPAsyncWebServer [latest]
#include <DNSServer.h>
#include <PubSubClient.h>     // https://github.com/arjenhiemstra/PubSubClientStatic [latest], forked from https://github.com/knolleary/pubsubclient and https://github.com/mhmtsui/pubsubclient
#include <Wire.h>
#include <time.h>
#include <Ticker.h>
#include <string>

#include "IthoQueue.h"
#include "System.h"
#include "SystemConfig.h"
#include "WifiConfig.h"

#include <ArduinoOTA.h>
#include <FS.h>
#include "SHTSensor.h"

//#include "SPIFFS.h"
#include <LITTLEFS.h>

#include <WiFi.h>
#include <WiFiClient.h>
#include "esp_wifi.h"
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_task_wdt.h"
#include "IthoCC1101.h"   // Largly based on and thanks to https://github.com/supersjimmie/IthoEcoFanRFT
#include "IthoPacket.h"   // Largly based on and thanks to https://github.com/supersjimmie/IthoEcoFanRFT
#include "IthoRemote.h"


WiFiClient client;
DNSServer dnsServer;
PubSubClient mqttClient(client);

AsyncWebServer server(80);
AsyncEventSource events("/events");


Ticker IthoCMD;
Ticker DelayedReq;
Ticker DelayedSave;
Ticker scan;
Ticker timerLearnLeaveMode;

IthoCC1101 rf;
IthoPacket packet;
IthoRemote remotes;
IthoRemote virtualRemotes;

#define STACK_SIZE_SMALL          2048
#define STACK_SIZE                4096
#define STACK_SIZE_LARGE          8192
#define TASK_MAIN_PRIO            5
#define TASK_CC1101_PRIO          5
#define TASK_MQTT_PRIO            5
#define TASK_WEB_PRIO             5
#define TASK_CONFIG_AND_LOG_PRIO  5
#define TASK_SYS_CONTROL_PRIO     5
Ticker TaskCC1101Timeout;
Ticker TaskMQTTTimeout;
Ticker TaskConfigAndLogTimeout;
TaskHandle_t xTaskInitHandle = NULL;
TaskHandle_t xTaskCC1101Handle = NULL;
TaskHandle_t xTaskMQTTHandle = NULL;
TaskHandle_t xTaskWebHandle = NULL;
TaskHandle_t xTaskConfigAndLogHandle = NULL;
TaskHandle_t xTaskSysControlHandle = NULL;
uint32_t TaskInitHWmark = 0;
uint32_t TaskCC1101HWmark = 0;
uint32_t TaskMQTTHWmark = 0;
uint32_t TaskWebHWmark = 0;
uint32_t TaskConfigAndLogHWmark = 0;
uint32_t TaskSysControlHWmark = 0;
bool TaskInitReady = false;
StaticTask_t xTaskInitBuffer;
StaticTask_t xTaskCC1101Buffer;
StaticTask_t xTaskMQTTBuffer;
StaticTask_t xTaskWebBuffer;
StaticTask_t xTaskConfigAndLogBuffer;
StaticTask_t xTaskSysControlBuffer;
StackType_t xTaskInitStack[ STACK_SIZE ];
StackType_t xTaskCC1101Stack[ STACK_SIZE ];
StackType_t xTaskMQTTStack[ STACK_SIZE_LARGE ];
StackType_t xTaskWebStack[ STACK_SIZE_LARGE ];
StackType_t xTaskConfigAndLog[ STACK_SIZE ];
StackType_t xTaskSysControlStack[ STACK_SIZE ];
void TaskInit( void * parameter );
void TaskCC1101( void * parameter );
void TaskMQTT( void * parameter );
void TaskWeb( void * parameter );
void TaskConfigAndLog( void * parameter );
void TaskSysControl( void * parameter );


SHTSensor sht_org(SHTSensor::SHT3X);
SHTSensor sht_alt(SHTSensor::SHT3X_ALT);

const char* espName = "nrg-itho-";
const char* WiFiAPPSK = "password"; //default AP mode password

// Global variables
int MQTT_conn_state = -5;
int MQTT_conn_state_new = 0;
unsigned long lastMQTTReconnectAttempt = 0;
unsigned long lastWIFIReconnectAttempt = 0;
uint8_t debugLevel = 0;

volatile uint16_t nextIthoVal = 0;
volatile unsigned long nextIthoTimer = 0;

#define LOG_BUF_SIZE 128

unsigned long updatetimer = 0;
unsigned long APmodeTimeout = 0;
unsigned long lastSysMessage = 0;
unsigned long previousUpdate = 0;
unsigned long wifiLedUpdate = 0;
unsigned long SHT3x_readout = 0;
unsigned long query2401tim = 0;
unsigned long mqttUpdatetim = 0;
unsigned long lastLog = 0;
unsigned long lastVersionCheck;

//flags used
bool joinSend = false;
bool i2cStartCommands = false;
int8_t ithoInitResult = 0;
bool IthoInit = false;
bool shouldReboot = false;
bool onOTA = false;
bool dontSaveConfig = false;
bool saveSystemConfigflag = false;
bool saveWifiConfigflag = false;
bool resetWifiConfigflag = false;
bool resetSystemConfigflag = false;
bool dontReconnectMQTT = false;
bool clearQueue = false;
bool wifiModeAP = false;
bool sysStatReq = false;
bool ithoCCstatReq = false;
bool formatFileSystem = false;
bool runscan = false;
bool updateIthoMQTT = false;
bool updateMQTTihtoStatus = false;
bool sendHomeAssistantDiscovery = false;
volatile bool ithoCheck = false;
volatile bool saveRemotesflag = false;
volatile bool saveVremotesflag = false;
bool SHT3x_original = false;
bool SHT3x_alternative = false;
bool rfInitOK = false;

void setup() {

#if defined (ENABLE_SERIAL)
  Serial.begin(115200);
  Serial.flush();
  delay(100);
#endif

  xTaskInitHandle = xTaskCreateStaticPinnedToCore(
                      TaskInit,
                      "TaskInit",
                      STACK_SIZE,
                      ( void * ) 1,
                      TASK_MAIN_PRIO,
                      xTaskInitStack,
                      &xTaskInitBuffer,
                      CONFIG_ARDUINO_RUNNING_CORE);

}

void loop() {
  yield();
  esp_task_wdt_reset();
}
