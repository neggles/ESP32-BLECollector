/*

  ESP32 BLE Collector - A BLE scanner with sqlite data persistence on the SD Card
  Source: https://github.com/tobozo/ESP32-BLECollector

  MIT License

  Copyright (c) 2018 tobozo

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

  -----------------------------------------------------------------------------

  RTC Profiles:
    1 - "Hobo": No TinyRTC module in your build, only uptime will be displayed until another BLECollector broadcasts time
    2 - "Rogue": TinyRTC module adjusted after flashing, will broadcast time for other BLECollectors
    3 - "Chronomaniac": TinyRTC module adjusts itself via NTP (separate binary), will broadcast time for other BLECollectors

*/
// don't edit those
#define HOBO         1 // No TinyRTC module in your build, will use BLE TimeClient otherwise only uptime will be displayed
#define ROGUE        2 // TinyRTC module adjusted via flashing or BLE TimeClient
#define CHRONOMANIAC 3 // TinyRTC module adjusted via flashing, BLE TimeClient or GPS
#define TIME_UPDATE_NONE 0 // update from nowhere, only using uptime as timestamps
#define TIME_UPDATE_BLE  1 // update from BLE Time Server (see the /tools directory of this project)
#define TIME_UPDATE_NTP  2 // update from NTP Time Server (requires WiFi and a separate build)
#define TIME_UPDATE_GPS  3 // update from GPS external module, see below for HardwareSerial pin settings


// Edit these values to fit your mode.
// They will eventually be overriden in Display.h
// depending on the detected device
#define HAS_EXTERNAL_RTC   false // uses I2C, search this file for RTC_SDA or RTC_SCL to change pins
#define HAS_GPS            false // uses hardware serial, search this file for GPS_RX and GPS_TX to change pins
#define TIME_UPDATE_SOURCE TIME_UPDATE_GPS // TIME_UPDATE_GPS // soon deprecated, will be implicit

// Timezone is using a float because Newfoundland, India, Iran, Afghanistan, Myanmar, Sri Lanka, the Marquesas,
// as well as parts of Australia use half-hour deviations from standard time, and some nations,
// such as Nepal, and some provinces, such as the Chatham Islands of New Zealand, use quarter-hour deviations.
float timeZone = 1; // 1 = GMT+1, 2 = GMT+2, etc
bool summerTime = false;

#define WITH_WIFI          1 // used to download oui databases, NTP sync, can be disabled if HAS_GPS is used
// or disabled if specified by build flag
#if defined WITHOUT_WIFI
  #undef WITH_WIFI
#endif

byte SCAN_DURATION = 20; // seconds, will be adjusted upon scan results
#define MIN_SCAN_DURATION 10 // seconds min
#define MAX_SCAN_DURATION 120 // seconds max
#define VENDORCACHE_SIZE 16 // use some heap to cache vendor query responses, min = 5, max = 256
#define OUICACHE_SIZE 8 // use some heap to cache mac query responses, min = 16, max = 4096
#define MAX_FIELD_LEN 32 // max chars returned by field
#define MAC_LEN 17 // chars used by a mac address
#define SHORT_MAC_LEN 7 // chars used by the oui part of a mac address


#if HAS_GPS
  // this will be overriden in Display.h
  // depending on the detected device
  #define GPS_RX 39 // io pin number
  #define GPS_TX 35 // io pin number
#endif

#if HAS_EXTERNAL_RTC
  // this will be overriden in Display.h
  // depending on the detected device
  // EX: On Wrover Kit you can use the following pins (from the camera connector)
  // RTC_SCL = GPIO27 (SIO_C / SCCB Clock 4)
  // RTC_SDA = GPIO26 (SIO_D / SCCB Data)
  #define RTC_SDA 21 // pin number
  #define RTC_SCL 22 // pin number
#endif


// don't edit anything below this

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wchar-subscripts"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wempty-body"

#if defined( ARDUINO_M5STACK_Core2 )
  #define PLATFORM_NAME "M5Core2"
#elif defined( ARDUINO_M5Stack_Core_ESP32 )
  //#warning M5STACK CLASSIC DETECTED !!
  #define PLATFORM_NAME "M5Stack"
#elif defined( ARDUINO_M5STACK_FIRE )
  //#warning M5STACK FIRE DETECTED !!
  #define PLATFORM_NAME "M5Fire"
#elif defined( ARDUINO_ODROID_ESP32 )
  //#warning ODROID DETECTED !!
  #define PLATFORM_NAME "Odroid-GO"
#elif defined ( ARDUINO_ESP32_DEV )
  //#warning WROVER DETECTED !!
  #define PLATFORM_NAME "WROVER KIT"
#else
  #define PLATFORM_NAME "ESP32"
#endif

#define MAX_BLECARDS_WITH_TIMESTAMPS_ON_SCREEN 4
#define MAX_BLECARDS_WITHOUT_TIMESTAMPS_ON_SCREEN 5
#define BLEDEVCACHE_PSRAM_SIZE 1024 // use PSram to cache BLECards
#define BLEDEVCACHE_HEAP_SIZE 32 // use some heap to cache BLECards. min = 5, max = 64, higher value = less SD/SD_MMC sollicitation
#define MAX_DEVICES_PER_SCAN MAX_BLECARDS_WITH_TIMESTAMPS_ON_SCREEN // also max displayed devices on the screen, affects initial scan duration
#define BLE_MENU_NAME "BLEMenu"
#define BUILD_TYPE BLE_MENU_NAME
#define BUILD_NEEDLE PLATFORM_NAME "-BLECollector by tobozo, Compiled On "
#define BUILD_SIGNATURE __DATE__ " - " __TIME__ " - " BUILD_TYPE
#define WELCOME_MESSAGE BUILD_NEEDLE BUILD_SIGNATURE
const char* welcomeMessage = R"asciiWelcome(
▄▄▄▄· ▄▄▌  ▄▄▄ .     ▄▄·       ▄▄▌  ▄▄▌  ▄▄▄ . ▄▄· ▄▄▄▄▄      ▄▄▄
▐█ ▀█▪██•  ▀▄.▀·    ▐█ ▌▪▪     ██•  ██•  ▀▄.▀·▐█ ▌▪•██  ▪     ▀▄ █·
▐█▀▀█▄██▪  ▐▀▀▪▄    ██ ▄▄ ▄█▀▄ ██▪  ██▪  ▐▀▀▪▄██ ▄▄ ▐█.▪ ▄█▀▄ ▐▀▀▄
██▄▪▐█▐█▌▐▌▐█▄▄▌    ▐███▌▐█▌.▐▌▐█▌▐▌▐█▌▐▌▐█▄▄▌▐███▌ ▐█▌·▐█▌.▐▌▐█•█▌
·▀▀▀▀ .▀▀▀  ▀▀▀     ·▀▀▀  ▀█▄▀▪.▀▀▀ .▀▀▀  ▀▀▀ ·▀▀▀  ▀▀▀  ▀█▄▀▪.▀  ▀

)asciiWelcome" WELCOME_MESSAGE;

static xSemaphoreHandle mux = NULL; // this is needed to mitigate SPI collisions

// str helpers
char *substr(const char *src, int pos, int len)
{
  char* dest = NULL;
  if (len > 0) {
    dest = (char*)calloc(len + 1, 1);
    if (NULL != dest) {
      strncat(dest, src + pos, len);
    }
  }
  return dest;
}
int strpos(const char *hay, const char *needle, int offset)
{
  char haystack[strlen(hay)];
  strncpy(haystack, hay+offset, strlen(hay)-offset);
  char *p = strstr(haystack, needle);
  if (p)
    return p - haystack+offset;
  return -1;
}

// used to get the resetReason
#include <rom/rtc.h>
#include <Preferences.h>
Preferences preferences;
// use the primitive because ESP.getFreeHeap() is inconsistent across SDK versions
#define freeheap heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
#define freepsheap ESP.getFreePsram()
#define resetReason (int)rtc_get_reset_reason(0)
#define takeMuxSemaphore() if( mux ) { xSemaphoreTake(mux, portMAX_DELAY); log_v("Took Semaphore"); }
#define giveMuxSemaphore() if( mux ) { xSemaphoreGive(mux); log_v("Gave Semaphore"); }

// core affinity
#define SCANTASK_CORE       0
#define TASKLAUNCHER_CORE   1-SCANTASK_CORE
#define TIMESERVERTASK_CORE 1
#define TIMECLIENTTASK_CORE 1
#define FILESHARETASK_CORE  0
#define WIFITASK_CORE       1
#define SERIALTASK_CORE     1
#define CLOCKSYNC_CORE      1
#define STATUSBAR_CORE      1
#define HEAPGRAPH_CORE      1
#define SCROLLINTRO_CORE    0

static void destroyTaskNow( TaskHandle_t &task )
{
  vTaskSuspendAll();
  TaskHandle_t xTask = task;
  if( task != NULL ) {
    //log_w("[Free Heap: %d] Killing BLE Task %s", freeheap, task);
    task = NULL; // The task is going to be deleted, Set the handle to NULL.
    vTaskDelete( xTask ); // Delete using the copy of the handle.
  }
  xTaskResumeAll();
}

#include "Display.h" // some config settings are overwritten here
#include "DateTime.h"

#if HAS_EXTERNAL_RTC
  #include <Wire.h>
  #include "RTC.h"
  static BLE_RTC RTC;
#endif



// RF stack

#ifdef WITH_WIFI
  // minimal spiffs partition size is required for that
  // #include "ftpserver/ESP32FtpServer.h"
  // #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>

  char WiFi_SSID[32];
  char WiFi_PASS[32];

  HTTPClient http;

#endif

// NimBLE Stack
#define CONFIG_NIMBLE_STACK_USE_MEM_POOLS 0
#define CONFIG_BT_NIMBLE_ENABLED 0
#include <NimBLEDevice.h> // https://github.com/h2zero/NimBLE-Arduino
#include "NimBLEEddystoneURL.h"
#include "NimBLEEddystoneTLM.h"
#include "NimBLEBeacon.h"

// SQLite stack
#define NDEBUG // disable sqlite debug
#include <sqlite3.h> // https://github.com/siara-cc/esp32_arduino_sqlite3_lib

// used to disable brownout detector
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_task_wdt.h"

/*
 * Data sources:
 * - HEAP Cache : used if no SPIRAM detected for BLEDEV, OUI and Vendors
 * - SPIRAM BLEDEV Cache L1 : all scanned BLE Devices are copied there for further analysis
 * - SPIRAM BLEDEV Cache L2 : all returning BLE Devices are copied there for hits counting
 * - SPIRAM OUI Lookup : OUI/Mac Database is copied there
 * - SPIRAM Vendor Lookup : Manufacturer names/id Database is copied there
 * - SQLite3 DB OUI : readonly, mainly a getter for vendor names by mac address
 * - SQLite3 DB Vendor : readonly, mainly a getter for vendor names by manufacturer data
 * - SQLite3 DB blemacs : read/write, getter and setter for non anonymous BLE Advertised Devices
 *
 */

// statistical values
static int devicesCount      = 0; // devices count per scan
static int sessDevicesCount  = 0; // total devices count per session
static int results           = 0; // total results during last query
static unsigned int entries  = 0; // total entries in database
static uint8_t prune_trigger = 0; // incremented on every insertion, reset on prune()

// load application stack
#include "BLECache.h" // data struct
#include "ScrollPanel.h" // scrolly methods
#include "TimeUtils.h"
#include "UI.h"
#include "DB.h"
#include "BLEFileSharing.h"
#include "BLE.h"
