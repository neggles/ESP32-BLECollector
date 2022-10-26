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

*/

#define TICKS_TO_DELAY 1000

const char* processTemplateLong = "%s%d%s%d";
const char* processTemplateShort = "%s%d";
static char processMessage[20];

bool onScanProcessed = true;
bool onScanPopulated = true;
bool onScanPropagated = true;
bool onScanPostPopulated = true;
bool onScanRendered = true;
bool onScanDone = true;
bool scanTaskRunning = false;
bool scanTaskStopped = true;


#ifdef WITH_WIFI
static bool WiFiStarted = false;
static bool NTPDateSet = false;
static bool WiFiDownloaderRunning = false;
#endif

static bool DBStarted = false;

extern size_t devicesStatCount;

static char* serialBuffer = NULL;
static char* tempBuffer = NULL;
#define SERIAL_BUFFER_SIZE 64

unsigned long lastheap = 0;
uint16_t lastscanduration = SCAN_DURATION;
char heapsign[5]; // unicode sign terminated
char scantimesign[5]; // unicode sign terminated
BLEScanResults bleresults;
BLEScan *pBLEScan;

TaskHandle_t TimeServerTaskHandle;
TaskHandle_t TimeClientTaskHandle;


static uint16_t processedDevicesCount = 0;
bool foundDeviceToggler = true;


enum AfterScanSteps
{
  POPULATE  = 0,
  IFEXISTS  = 1,
  RENDER    = 2,
  PROPAGATE = 3
};


/*
// work in progress: MAC blacklist/whitelist
const char MacList[3][MAC_LEN + 1] = {
  "aa:aa:aa:aa:aa:aa",
  "bb:bb:bb:bb:bb:bb",
  "cc:cc:cc:cc:cc:cc"
};


static bool AddressIsListed( const char* address ) {
  for ( byte i = 0; i < sizeof(MacList); i++ ) {
    if ( strcmp(address, MacList[i] ) == 0) {
      return true;
    }
  }
  return false;
}
*/


// Covid Trackers Sources
//  - https://github.com/fs0c131y/covid19-tracker-apps
//  - https://docs.google.com/spreadsheets/d/1ATalASO8KtZMx__zJREoOvFh0nmB-sAqJ1-CjVRSCOw/edit#gid=0


// French "StopCodid" app's Service UUID
// TODO: add more of those
//static BLEUUID StopCovidServiceUUID("910c7798-9f3a-11ea-bb37-0242ac130002"); // test UUID
static BLEUUID StopCovidUUID("0000fd64-0000-1000-8000-00805f9b34fb");
static BLEUUID StopCovidCharUUID("a8f12d00-ee67-478b-b95f-65d599407756");

// International Covid19 Radar contact tracing app // https://github.com/Covid-19Radar/Covid19Radar/blob/master/doc/Tester/Tester-Instructions.md
static BLEUUID Covid19RadarUUID("550e8400-e29b-41d4-a716-446655440000");
static BLEUUID Covid19RadarTestUUID("7822fa0f-ce38-48ea-a7e8-e72af4e42c1c");

// Australia CovidSafe  https://github.com/xssfox/covidsafescan PRODUCTION_UUID = 'b82ab3fc-1595-4f6a-80f0-fe094cc218f9' STAGING_UUID = '17e033d3-490e-4bc9-9fe8-2f567643f4d3'
// Singapore TraceTogether https://github.com/lupyuen/ble-explorer      ServiceID = 'b82ab3fc-1595-4f6a-80f0-fe094cc218f9'
static BLEUUID CovidSafeUUID("b82ab3fc-1595-4f6a-80f0-fe094cc218f9");

// Belgium   Coronalert https://github.com/covid-be-app/cwa-app-android CharacteristicID = 0xFD6 / 0xFD6F (GAEN Exposure)
// https://blog.google/documents/70/Exposure_Notification_-_Bluetooth_Specification_v1.2.2.pdf
static BLEUUID GAENExposure( 0xfd6fU );





struct WatchedBLEService
{
  BLEUUID serviceUUID;
  const char* description;
  const bool dropPayload;
};

WatchedBLEService watchedServices[] =
{
  { StopCovidUUID,     "French StopCovid App", true },
  { Covid19RadarUUID,  "International Codiv19 Radar contact tracing app", true },
  { CovidSafeUUID,     "Australian/Singapore TraceTogether/CovidSafe App", true },
  { GAENExposure,      "GAEN Exposure", true }
};

static size_t watchedServicesCount = sizeof watchedServices / sizeof watchedServices[0];



static bool deviceHasKnownPayload( BLEAdvertisedDevice *advertisedDevice )
{
  if ( !advertisedDevice->haveServiceUUID() ) return false;
  if( advertisedDevice->isAdvertisingService( timeServiceUUID ) ) {
    log_i( "Found Time Server %s : %s", advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getServiceUUID().toString().c_str() );
    timeServerBLEAddress = advertisedDevice->getAddress().toString();
    timeServerClientType = advertisedDevice->getAddressType();
    foundTimeServer = true;
    if ( foundTimeServer && (!TimeIsSet || ForceBleTime) ) {
      return true;
    }
  }

  for( int i=0; i<watchedServicesCount; i++ ) {
    if( advertisedDevice->isAdvertisingService( watchedServices[i].serviceUUID ) ) {
      Serial.printf("Found %s Advertisement %s : %s\n", watchedServices[i].description, advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getServiceUUID().toString().c_str() );
      uint8_t *payLoad = advertisedDevice->getPayload();
      size_t payLoadLen = advertisedDevice->getPayloadLength();
      Serial.printf("Payload (%d bytes): ", payLoadLen);
      for (size_t i=0; i<payLoadLen; i++ ) {
        Serial.printf("%02x ", payLoad[i] );
      }
      Serial.println();
      break; // no need to finish the loop if something was found
    }
  }

  return false;
}



class FoundDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
{
    void onResult( BLEAdvertisedDevice *advertisedDevice )
    {
      devicesStatCount++; // raw stats for heapgraph

      bool scanShouldStop =  deviceHasKnownPayload( advertisedDevice );

      if ( onScanDone  ) return;

      if ( scan_cursor < MAX_DEVICES_PER_SCAN ) {
        log_i("will store advertisedDevice in cache #%d", scan_cursor);
        BLEDevHelper.store( BLEDevScanCache[scan_cursor], advertisedDevice );
        //bool is_random = strcmp( BLEDevScanCache[scan_cursor]->ouiname, "[random]" ) == 0;
        bool is_random = (BLEDevScanCache[scan_cursor]->addr_type == BLE_ADDR_RANDOM );
        //bool is_blacklisted = isBlackListed( BLEDevScanCache[scan_cursor]->address );
        if ( UI.filterVendors && is_random ) {
          //TODO: scan_cursor++
          log_i( "Filtering %s", BLEDevScanCache[scan_cursor]->address );
        } else {
          if ( DB.hasPsram ) {
            if ( !is_random ) {
              DB.getOUI( BLEDevScanCache[scan_cursor]->address, BLEDevScanCache[scan_cursor]->ouiname );
            }
            if ( BLEDevScanCache[scan_cursor]->manufid > -1 ) {
              DB.getVendor( BLEDevScanCache[scan_cursor]->manufid, BLEDevScanCache[scan_cursor]->manufname );
            }
            BLEDevScanCache[scan_cursor]->is_anonymous = BLEDevHelper.isAnonymous( BLEDevScanCache[scan_cursor] );
            log_i(  "  stored and populated #%02d : %s", scan_cursor, advertisedDevice->getName().c_str());
          } else {
            log_i(  "  stored #%02d : %s", scan_cursor, advertisedDevice->getName().c_str());
          }
          scan_cursor++;
          processedDevicesCount++;
        }
        if ( scan_cursor == MAX_DEVICES_PER_SCAN ) {
          onScanDone = true;
        }
      } else {
        onScanDone = true;
      }
      if ( onScanDone ) {
        advertisedDevice->getScan()->stop();
        scan_cursor = 0;
        if ( SCAN_DURATION - 1 >= MIN_SCAN_DURATION ) {
          SCAN_DURATION--;
        }
      }
      foundDeviceToggler = !foundDeviceToggler;
      if (foundDeviceToggler) {
        //UI.BLEStateIconSetColor(BLE_GREEN);
        BLEActivityIcon.setStatus( ICON_STATUS_ADV_WHITELISTED );
      } else {
        //UI.BLEStateIconSetColor(BLE_DARKGREEN);
        BLEActivityIcon.setStatus( ICON_STATUS_ADV_SCAN );
      }
      if( scanShouldStop ) {
        advertisedDevice->getScan()->stop();
      }
    }
};

FoundDeviceCallbacks *FoundDeviceCallback;// = new FoundDeviceCallbacks(); // collect/store BLE data


struct SerialCallback
{
  SerialCallback(void (*f)(void *) = 0, void *d = 0)
    : function(f), data(d) {}
  void (*function)(void *);
  void *data;
};

struct CommandTpl
{
  const char* command;
  SerialCallback cb;
  const char* description;
};

CommandTpl* SerialCommands;
uint16_t Csize = 0;

struct ToggleTpl
{
  const char *name;
  bool &flag;
};

ToggleTpl* TogglableProps;
uint16_t Tsize = 0;

static void(*ProcessHID)( unsigned long &lastHidCheck );


class BLEScanUtils
{

  public:

    void init()
    {
      Serial.begin(115200);
      mux = xSemaphoreCreateMutex();
      BLEDevice::init( PLATFORM_NAME " BLE Collector");
      getPrefs(); // load prefs from NVS
      UI.init(); // launch all UI tasks
      UI.BLEStarted = true;
      setBrightnessCB(); // apply thawed brightness
      VendorFilterIcon.setStatus( UI.filterVendors ? ICON_STATUS_filter : ICON_STATUS_filter_unset );
      doStartDBInit(); // init the DB
      doStartSerialTask(); // start listening to serial commands
      // only autorun commands on regular boot or after a crash, ignore after software reset
      if (resetReason != 12) { // HW Reset
        runCommand( (char*)"help" );
        //runCommand( (char*)"toggle" );
        //runCommand( (char*)"ls" );
      }
    }


    static void doStartDBInit()
    {
      xTaskCreatePinnedToCore( startDBInit, "startDBInit", 8192, NULL, 16, NULL, STATUSBAR_CORE );
      while( !DBStarted ) vTaskDelay(10);
    }

    void doStartSerialTask()
    {
      serialBuffer = (char*)calloc( SERIAL_BUFFER_SIZE, sizeof(char) );
      tempBuffer   = (char*)calloc( SERIAL_BUFFER_SIZE, sizeof(char) );
      #if HAS_GPS
        GPSInit();
      #endif
      #if HAS_EXTERNAL_RTC
        if( TimeIsSet ) {
          // auto share time if available
          // TODO: fix this, broken since NimBLE
          // runCommand( (char*)"bleclock" );
        }
      #endif
      // setup non-serial user input
      if( hasHID() ) {
        ProcessHID = M5ButtonCheck;
        log_w("Using native M5Buttons");
      } else {
        ProcessHID = NoHIDCheck;
        log_w("NO HID enabled");
      }
      xTaskCreatePinnedToCore(serialTask, "serialTask", 8192 + SERIAL_BUFFER_SIZE, this, 0, NULL, SERIALTASK_CORE ); /* last = Task Core */
    }


    static void startDBInit( void * param )
    {
      if ( ! DB.init() ) { // mount DB
        log_e("Error with .db files (not found or corrupted)");
        UI.stopUITasks();
        takeMuxSemaphore();
        Out.scrollNextPage();
        Out.println();
        Out.scrollNextPage();
        giveMuxSemaphore();
        //UI.PrintMessage( "[ERROR]: .db files not found", Out.scrollPosY );
        UI.PrintMessage( "Some db files were not found or corrupted"  );
        #ifdef WITH_WIFI
          UI.PrintMessage("Running WiFi Downloader...");
          doRunWiFiDownloader();
        #else
          // TODO: fix file transfer with NimBLE
          UI.PrintMessage("Please copy db files on SD");
        #endif
      } else {
        WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
        startScanCB();
        RamCacheReady = true;
      }
      DBStarted = true;
      vTaskDelete( NULL );
    }


    #ifdef WITH_WIFI

      static void setWiFiSSID( void * param = NULL )
      {
        if( param == NULL ) return;
        sprintf( WiFi_SSID, "%s", (const char*)param);
      }

      static void setWiFiPASS( void * param = NULL )
      {
        if( param == NULL ) return;
        sprintf( WiFi_PASS, "%s", (const char*)param);
      }

      static void doStopBLE( void * param = NULL )
      {
        xTaskCreatePinnedToCore( stopBLE, "stopBLE", 8192, param, 5, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
        while( UI.BLEStarted == true ) {
          vTaskDelay( 100 );
        }
        log_w("BLE stopped");
        UI.PrintMessage("Stopped BLE...");

      }

      static void stopBLE( void * param = NULL )
      {
        stopScanCB();
        stopBLETasks();
        stopBLEController();
        UI.BLEStarted = false;
        vTaskDelete( NULL );
      }

      static void doStartWiFi( void * param = NULL )
      {
        if( UI.BLEStarted ) {
          doStopBLE();
        }
        if( WiFiStarted ) return;
        xTaskCreatePinnedToCore( startWifi, "startWifi", 16384, param, 16, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
        while( WiFiStarted == false ) {
          // TODO: timeout this
          vTaskDelay( 100 );
        }
        UI.PrintMessage("Started WiFi...");
      }

      static void startWifi( void * param = NULL )
      {
        WiFi.mode(WIFI_STA);
        Serial.println(WiFi.macAddress());

        String previousSuccessfulSSID = WiFi.SSID();
        String previousSuccessfulPWD = WiFi.psk();

        if( previousSuccessfulSSID != "" && previousSuccessfulPWD != "" ) {
          log_w("Using credentials from last known connection, SSID: %s / PSK: %s", previousSuccessfulSSID.c_str(), previousSuccessfulPWD.c_str() );
          WiFi.begin();
        } else {
          if( String( WiFi_SSID ) !="" && String( WiFi_PASS ) !="" ) {
            log_w("Using credentials from NVS");
            WiFi.begin( WiFi_SSID, WiFi_PASS );
          } else {
            log_w("Using default credentials (WARN: no SSID/PASS saved in NVS)");
            WiFi.begin();
          }
        }
        while(WiFi.status() != WL_CONNECTED) {
          log_e("Not connected");
          delay(1000);
        }
        log_w("Connected!");
        Serial.print("Connected to ");
        Serial.println(WiFi_SSID);
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        Serial.println("");
        WiFiStarted = true;
        vTaskDelete( NULL );
      }

      static void doStopWiFi( void * param = NULL )
      {
        xTaskCreatePinnedToCore( stopWiFi, "stopWiFi", 8192, param, 5, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
        while( WiFiStarted == true ) {
          // TODO: timeout this
          vTaskDelay( 100 );
        } // wait until wifi stopped
        UI.PrintMessage("Stopped WiFi...");
      }

      static void stopWiFi( void* param = NULL )
      {
        log_w("Stopping WiFi");
        WiFi.mode(WIFI_OFF);
        WiFiStarted = false;
        vTaskDelete( NULL );
      }

      static void setPoolZone( void * param = NULL )
      {
        if( param == NULL ) {
          log_e("No pool zone to set, valid values are: africa, antarctica, asia, europe, north-america, oceania, south-america");
          return;
        }
        int poolZoneID = getPoolZoneID( (char*)param );
        if( poolZoneID > -1 ) {
          preferences.begin("BLEClock", false);
          preferences.putString( "poolZone", (const char*)param );
          preferences.end();
          Serial.printf("NTP Server will use %s.pool.ntp.org on next update\n", (const char*)param );
        } else {
          log_n("Error: %s is not a valid pool zone", (const char*)param );
        }
      }

      static void doStartNTPUpdater( void * param = NULL )
      {
        if( UI.BLEStarted ) {
          doStopBLE();
        }
        if( !WiFiStarted ) {
          doStartWiFi();
        }
        UI.PrintMessage("Contacting NTP Server...");
        NTPDateSet = false;
        xTaskCreatePinnedToCore( startNTPUpdater, "startNTPUpdater", 16384, param, 16, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
        while( NTPDateSet == false ) {
          // TODO: timeout this
          vTaskDelay( 100 );
        }
        if( param == NULL ) {
          #if HAS_EXTERNAL_RTC
          UI.PrintMessage("Restarting...");
          ESP.restart();
          #endif
        }
      }

      static void startNTPUpdater( void * param )
      {
        NTPDateSet = getNTPTime();
        vTaskDelete( NULL );
      }

      static void doRunWiFiDownloader( void * param = NULL )
      {
        if( UI.BLEStarted ) {
          doStopBLE();
        }
        if( !WiFiStarted ) {
          doStartWiFi();
        }
        if( !NTPDateSet ) {
          doStartNTPUpdater( (void*)true ); // Keep WiFi up after NTP Update
        }
        UI.PrintMessage("Checking DB Files...");

        WiFiDownloaderRunning = true;
        xTaskCreatePinnedToCore( runWifiDownloader, "runWifiDownloader", 16384, param, 16, NULL, WIFITASK_CORE ); /* last = Task Core */
        while( WiFiDownloaderRunning ) {
          vTaskDelay( 100 );
        }
        doStopWiFi();
        ESP.restart();
      }

      static void runWifiDownloader( void * param )
      {
        BLE_FS.begin();
        if( !DB.checkOUIFile() ) {
          if( ! wget( MAC_OUI_NAMES_DB_URL, BLE_FS, MAC_OUI_NAMES_DB_FS_PATH ) ) {
            UI.PrintMessage("OUIFile download fail");

            log_e("Failed to download %s from url %s", MAC_OUI_NAMES_DB_FS_PATH, MAC_OUI_NAMES_DB_URL );
          } else {
            UI.PrintMessage("OUIFile download success!");
            log_w("Successfully downloaded %s from url %s", MAC_OUI_NAMES_DB_FS_PATH, MAC_OUI_NAMES_DB_URL );
          }
        } else {
          UI.PrintMessage("OUIFile is up to date");
          log_w("Skipping download for %s file ", MAC_OUI_NAMES_DB_FS_PATH );
        }
        vTaskDelay( 1000 );

        if( !DB.checkVendorFile() ) {
          if( ! wget( BLE_VENDOR_NAMES_DB_URL, BLE_FS, BLE_VENDOR_NAMES_DB_FS_PATH ) ) {
            UI.PrintMessage("VendorFile download fail");
            log_e("Failed to download %s from url %s", BLE_VENDOR_NAMES_DB_FS_PATH, BLE_VENDOR_NAMES_DB_URL );
          } else {
            UI.PrintMessage("VendorFile download success!");
            log_w("Successfully downloaded %s from url %s", BLE_VENDOR_NAMES_DB_FS_PATH, BLE_VENDOR_NAMES_DB_URL );
          }
        } else {
          UI.PrintMessage("VendorFile is up to date");
          log_w("Skipping download for %s file ", MAC_OUI_NAMES_DB_FS_PATH );
        }
        vTaskDelay( 1000 );
        WiFiDownloaderRunning = false;
        vTaskDelete( NULL );
      }

      static bool /*yolo*/wget( const char* url, fs::FS &fs, const char* path )
      {
        WiFiClientSecure *client = new WiFiClientSecure;
        //client->setCACert( NULL ); // yolo security
        client->setInsecure();

        const char* UserAgent = "ESP32HTTPClient";

        http.setUserAgent( UserAgent );
        http.setConnectTimeout( 10000 ); // 10s timeout = 10000

        if( ! http.begin(*client, url ) ) {
          log_e("Can't open url %s", url );
          delete client;
          return false;
        }

        const char * headerKeys[] = {"location", "redirect"};
        const size_t numberOfHeaders = 2;
        http.collectHeaders(headerKeys, numberOfHeaders);

        log_w("URL = %s", url);

        int httpCode = http.GET();

        // file found at server
        if (httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String newlocation = "";
          for(int i = 0; i< http.headers(); i++) {
            String headerContent = http.header(i);
            if( headerContent !="" ) {
              newlocation = headerContent;
              Serial.printf("%s: %s\n", headerKeys[i], headerContent.c_str());
            }
          }

          http.end();
          if( newlocation != "" ) {
            log_w("Found 302/301 location header: %s", newlocation.c_str() );
            delete client;
            return wget( newlocation.c_str(), fs, path );
          } else {
            log_e("Empty redirect !!");
            delete client;
            return false;
          }
        }

        WiFiClient *stream = http.getStreamPtr();

        if( stream == nullptr ) {
          http.end();
          log_e("Connection failed!");
          delete client;
          return false;
        }

        File outFile = fs.open( path, FILE_WRITE );
        if( ! outFile ) {
          log_e("Can't open %s file to save url %s", path, url );
          delete client;
          return false;
        }

        //uint8_t buff[4096] = { 0 };
        //size_t sizeOfBuff = sizeof(buff);
        size_t sizeOfBuff = 4096;
        uint8_t *buff = new uint8_t[sizeOfBuff];//

        int len = http.getSize();
        int bytesLeftToDownload = len;
        int bytesDownloaded = 0;

        UI.PrintMessage("Download in progress...");

        while(http.connected() && (len > 0 || len == -1)) {
          size_t size = stream->available();
          if(size) {
            // read up to 512 byte
            int c = stream->readBytes(buff, ((size > sizeOfBuff) ? sizeOfBuff : size));
            outFile.write( buff, c );
            bytesLeftToDownload -= c;
            bytesDownloaded += c;
            Serial.printf("%d bytes left\n", bytesLeftToDownload );
            float progress = (((float)bytesDownloaded / (float)len) * 100.00);
            UI.PrintProgressBar( progress, 100.0 );
          }
          if( bytesLeftToDownload == 0 ) break;
        }
        outFile.close();
        //free( buff );
        //free( client );
        delete buff;
        delete client;
        return true;
        return fs.exists( path );
      }

    #endif // ifdef WITH_WIFI


    static void stopBLETasks( void * param = NULL )
    {
      log_w("[Free Heap: %d] Deleting BLE Tasks", freeheap);
      if( timeServerIsRunning ) destroyTaskNow( TimeServerTaskHandle );
      if( timeClientisStarted ) destroyTaskNow( TimeClientTaskHandle );
    }

    static void stopBLEController()
    {
      log_w("[Free Heap: %d] Shutting Down BlueTooth LE", freeheap);
      log_w("[Free Heap: %d] esp_bt_controller_disable()", freeheap);
      esp_bt_controller_disable();
      log_w("[Free Heap: %d] esp_bt_controller_deinit()", freeheap);
      esp_bt_controller_deinit() ;
      log_w("[Free Heap: %d] esp_bt_mem_release(ESP_BT_MODE_BTDM)", freeheap);
      esp_bt_mem_release(ESP_BT_MODE_BTDM);
      log_w("[Free Heap: %d] BT Shutdown finished", freeheap);
    }

    static void startScanCB( void * param = NULL )
    {
      if( timeClientisStarted )  destroyTaskNow( TimeClientTaskHandle );
      BLEDevice::setMTU(100);
      if ( !scanTaskRunning ) {
        log_d("Starting scan" );
        uint16_t stackSize = DB.hasPsram ? 5120 : 5120;
        xTaskCreatePinnedToCore( scanTask, "scanTask", stackSize, NULL, 8, NULL, SCANTASK_CORE ); /* last = Task Core */
        while ( scanTaskStopped ) {
          log_d("Waiting for scan to start...");
          vTaskDelay(1000);
        }
        Serial.println("Scan started...");
        UI.headerStats("Scan started...");
      }
    }

    static void stopScanCB( void * param = NULL)
    {
      if ( scanTaskRunning ) {
        log_d("Stopping scan" );
        scanTaskRunning = false;
        BLEDevice::getScan()->stop();
        while (!scanTaskStopped) {
          log_d("Waiting for scan to stop...");
          vTaskDelay(100);
        }
        Serial.println("Scan stopped...");
        UI.headerStats("Scan stopped...");
      }
    }

    static void restartCB( void * param = NULL )
    {
      // detach from this thread before it's destroyed
      xTaskCreatePinnedToCore( doRestart, "doRestart", 16384, param, 5, NULL, TASKLAUNCHER_CORE ); // last = Task Core
    }

    static void doRestart( void * param = NULL )
    {
      // "restart now" command skips db replication
      stopScanCB();
      stopBLETasks();
      if ( param != NULL && strcmp( "now", (const char*)param ) != 0 ) {
        DB.updateDBFromCache( BLEDevRAMCache, false, false );
      }
      log_w("Will restart");
      delay( 50 );
      ESP.restart();
    }

    static void setTimeClientOn( void * param = NULL )
    {
      if( !timeClientisStarted ) {
        timeClientisStarted = true;
        xTaskCreatePinnedToCore( startTimeClient, "startTimeClient", 2560, param, 0, NULL, TASKLAUNCHER_CORE ); // last = Task Core
      } else {
        log_w("startTimeClient already called, time is also about patience");
      }
    }

    static void startTimeClient( void * param = NULL )
    {
      bool scanWasRunning = scanTaskRunning;
      int8_t oldrole = BLERoleIcon.status;
      ForceBleTime = false;
      while ( ! foundTimeServer ) {
        vTaskDelay( 100 );
      }
      if ( scanTaskRunning ) stopScanCB();
      BLERoleIcon.setStatus( ICON_STATUS_ROLE_CLOCK_SEEKING );
      xTaskCreatePinnedToCore( TimeClientTask, "TimeClientTask", 2560, NULL, 5, &TimeClientTaskHandle, TIMECLIENTTASK_CORE ); // TimeClient task prefers core 0
      if ( scanWasRunning ) {
        while ( timeClientisRunning ) {
          vTaskDelay( 1000 );
        }
        log_w("Resuming operations after TimeClientTask");
        timeClientisStarted = false;
        startScanCB();
        BLERoleIcon.setStatus( oldrole );
      } else {
        log_w("TimeClientTask started with no held task");
      }
      vTaskDelete( NULL );
    }

    static void setTimeServerOn( void * param = NULL )
    {
      if( !timeServerStarted ) {
        timeServerStarted = true;
        xTaskCreatePinnedToCore( startTimeServer, "startTimeServer", 8192, param, 0, NULL, TASKLAUNCHER_CORE ); // last = Task Core
      }
    }

    static void startTimeServer( void * param = NULL )
    {
      bool scanWasRunning = scanTaskRunning;
      if ( scanTaskRunning ) stopScanCB();
      // timeServer runs forever
      timeServerIsRunning = true;
      timeServerStarted   = false; // will be updated from task
      BLERoleIcon.setStatus(ICON_STATUS_ROLE_CLOCK_SHARING );
      UI.headerStats( "Starting Time Server" );
      vTaskDelay(1);
      xTaskCreatePinnedToCore( TimeServerTask, "TimeServerTask", 4096, NULL, 1, &TimeServerTaskHandle, TIMESERVERTASK_CORE ); // TimeServerTask prefers core 1
      log_w("TimeServerTask started");
      if ( scanWasRunning ) {
        while( ! timeServerStarted ) {
          vTaskDelay( 100 );
        }
        startScanCB();
      }
      vTaskDelete( NULL );
    }

    static void setBrightnessCB( void * param = NULL )
    {
      if( param != NULL ) {
        UI.brightness = atoi( (const char*) param );
      }
      takeMuxSemaphore();
      tft_setBrightness( UI.brightness );
      giveMuxSemaphore();
      setPrefs();
      log_w("Brightness is now at %d", UI.brightness);
    }

    static void resetCB( void * param = NULL )
    {
      DB.needsReset = true;
      Serial.println("DB Scheduled for reset");
      stopScanCB();
      DB.maintain();
      delay(100);
      //startScanCB();
    }

    static void pruneCB( void * param = NULL )
    {
      DB.needsPruning = true;
      Serial.println("DB Scheduled for pruning");
      stopScanCB();
      DB.maintain();
      startScanCB();
    }

    static void toggleFilterCB( void * param = NULL )
    {
      UI.filterVendors = ! UI.filterVendors;
      VendorFilterIcon.setStatus( UI.filterVendors ? ICON_STATUS_filter : ICON_STATUS_filter_unset );
      UI.cacheStats(); // refresh icon
      setPrefs(); // save prefs
      Serial.printf("UI.filterVendors = %s\n", UI.filterVendors ? "true" : "false" );
    }

    static void startDumpCB( void * param = NULL )
    {
      DBneedsReplication = true;
      bool scanWasRunning = scanTaskRunning;
      if ( scanTaskRunning ) stopScanCB();
      DB.maintain();
      while ( DBneedsReplication ) {
        vTaskDelay(1000);
      }
      if ( scanWasRunning ) startScanCB();
    }

    static void toggleEchoCB( void * param = NULL )
    {
      Out.serialEcho = !Out.serialEcho;
      setPrefs();
      Serial.printf("Out.serialEcho = %s\n", Out.serialEcho ? "true" : "false" );
    }

    static void rmFileCB( void * param = NULL )
    {
      xTaskCreatePinnedToCore(rmFileTask, "rmFileTask", 5000, param, 2, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
    }

    static void rmFileTask( void * param = NULL )
    {
      // YOLO style
      isQuerying = true;
      if ( param != NULL ) {
        if ( BLE_FS.remove( (const char*)param ) ) {
          Serial.printf("File %s deleted\n", (const char*)param );
        } else {
          Serial.printf("File %s could not be deleted\n", (const char*)param );
        }
      } else {
        Serial.println("Nothing to delete");
      }
      isQuerying = false;
      vTaskDelete( NULL );
    }

    static void screenShowCB( void * param = NULL )
    {
      xTaskCreate(screenShowTask, "screenShowTask", 16000, param, 2, NULL);
    }

    static void screenShowTask( void * param = NULL )
    {
      UI.screenShow( param );
      vTaskDelete(NULL);
    }

    static void screenShotCB( void * param = NULL )
    {
      xTaskCreate(screenShotTask, "screenShotTask", 16000, NULL, 2, NULL);
    }

    static void screenShotTask( void * param = NULL )
    {
      if( !UI.ScreenShotLoaded ) {
        log_w("Cold ScreenShot");
        M5.ScreenShot->init(/* &M5.Lcd, BLE_FS */);
        if( M5.ScreenShot->begin() ) {
          UI.ScreenShotLoaded = true;
          UI.screenShot();
        } else {
          log_e("Sorry, ScreenShot is not available");
        }
      } else {
        log_w("Hot ScreenShot");
        UI.screenShot();
      }
      vTaskDelete(NULL);
    }

    static void setTimeZone( void * param = NULL )
    {
      if( param == NULL ) {
        log_n("Please provide a valid timeZone (0-24), floats are accepted");
        return;
      }
      float oldTimeZone = timeZone;
      timeZone = strtof((char*)param, NULL);
      float diff = oldTimeZone - timeZone;
      Serial.printf("Local time will use timeZone %.2g on next NTP update\n", timeZone );
      setPrefs();
    }

    static void setSummerTime( void * param = NULL )
    {
      summerTime = !summerTime;
      Serial.printf("Local time will use [%s] on next NTP update\n", summerTime?"CEST":"CET");
      setPrefs();
    }

    static void listDirCB( void * param = NULL )
    {
      xTaskCreatePinnedToCore(listDirTask, "listDirTask", 5000, param, 8, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
    }

    static void listDirTask( void * param = NULL )
    {
      isQuerying = true;
      bool scanWasRunning = scanTaskRunning;
      if ( scanTaskRunning ) stopScanCB();
      if( param != NULL ) {
        if(! BLE_FS.exists( (const char*)param ) ) {
          Serial.printf("Directory %s does not exist\n", (const char*)param );
        } else {
          takeMuxSemaphore();
          listDir(BLE_FS, (const char*)param, 0, DB.BLEMacsDbFSPath);
          giveMuxSemaphore();
        }
      } else {
        takeMuxSemaphore();
        listDir(BLE_FS, "/", 0, DB.BLEMacsDbFSPath);
        giveMuxSemaphore();
      }
      if ( scanWasRunning ) startScanCB();
      isQuerying = false;
      vTaskDelete( NULL );
    }

    static void toggleCB( void * param = NULL )
    {
      if( Tsize == 0 ) return; // no variables to toggle, too early to call
      bool setbool = true;
      if ( param != NULL ) {
        //
      } else {
        setbool = false;
        Serial.println("\nCurrent property values:");
      }
      for ( uint16_t i = 0; i < Tsize; i++ ) {
        if ( setbool ) {
          if ( strcmp( TogglableProps[i].name, (const char*)param ) == 0 ) {
            TogglableProps[i].flag = !TogglableProps[i].flag;
            Serial.printf("Toggled flag %s to %s\n", TogglableProps[i].name, TogglableProps[i].flag ? "true" : "false");
          }
        } else {
          Serial.printf("  %24s : [%s]\n", TogglableProps[i].name, TogglableProps[i].flag ? "true" : "false");
        }
      }
    }

    static void nullCB( void * param = NULL )
    {
      if ( param != NULL ) {
        Serial.printf("nullCB param: %s\n", (const char*)param);
      }
      // zilch, niente, nada, que dalle, nothing
    }

    static void SerialRead()
    {
      // Read Serial1 and process commands if any
      static byte idx = 0;
      char lf = '\n';
      char cr = '\r';
      char c;
      while (Serial.available() > 0) {
        c = Serial.read();
        if (c != cr && c != lf) {
          serialBuffer[idx] = c;
          idx++;
          if (idx >= SERIAL_BUFFER_SIZE) {
            idx = SERIAL_BUFFER_SIZE - 1;
          }
        } else {
          serialBuffer[idx] = '\0'; // null terminate
          memcpy( tempBuffer, serialBuffer, idx + 1 );
          runCommand( tempBuffer );
          idx = 0;
        }
        vTaskDelay(1);
      }
    }


    static void serialTask( void * param )
    {
      if( param == NULL ) {
        Serial.println("NOT listening to Serial");
        vTaskDelete( NULL );
        return;
      }

      BLEScanUtils *o = (BLEScanUtils*)param;

      CommandTpl Commands[] = {
        { "help",          o->nullCB,                 "Print this list" },
        { "halp",          o->nullCB,                 "Same as help except it doesn't print anything" },
        { "start",         o->startScanCB,            "Start/resume scan" },
        { "stop",          o->stopScanCB,             "Stop scan" },
        { "toggleFilter",  o->toggleFilterCB,         "Toggle vendor filter on the TFT (persistent)" },
        { "toggleEcho",    o->toggleEchoCB,           "Toggle BLECards in the Serial Console (persistent)" },
        { "setTimeZone",   o->setTimeZone,            "Set the timezone for next NTP Sync (persistent)"},
        { "setSummerTime", o->setSummerTime,          "Toggle CEST / CET for next NTP Sync (persistent)" },
        { "dump",          o->startDumpCB,            "Dump returning BLE devices to the display and updates DB" },
        { "setBrightness", o->setBrightnessCB,        "Set brightness to [value] (0-255) (persistent)" },
        { "ls",            o->listDirCB,              "Show [dir] Content on the SD" },
        { "rm",            o->rmFileCB,               "Delete [file] from the SD" },
        { "restart",       o->restartCB,              "Restart BLECollector ('restart now' to skip replication)" },
        { "screenshot",    o->screenShotCB,           "Make a screenshot and save it on the SD" },
        { "screenshow",    o->screenShowCB,           "Show screenshot" },
        { "toggle",        o->toggleCB,               "toggle a bool value" },
        { "resetDB",       o->resetCB,                "Hard Reset DB + forced restart" },
        { "pruneDB",       o->pruneCB,                "Soft Reset DB without restarting (hopefully)" },
        #if HAS_EXTERNAL_RTC
          { "bleclock",      o->setTimeServerOn,        "Broadcast time to another BLE Device (implicit)" },
          { "bletime",       o->setTimeClientOn,        "Get time from another BLE Device (explicit)" },
        #else
          { "bleclock",      o->setTimeServerOn,        "Broadcast time to another BLE Device (explicit)" },
          { "bletime",       o->setTimeClientOn,        "Get time from another BLE Device (implicit)" },
        #endif
        #if HAS_GPS
          { "gpstime",       setGPSTime,                "Sync time from GPS" },
          { "latlng",        getLatLng,                 "Print the GPS lat/lng" },
        #endif
        #ifdef WITH_WIFI
          { "stopBLE",       o->doStopBLE,              "Stop BLE (use 'restart' command to re-enable)" },
          { "startWiFi",     o->doStartWiFi,            "Start WiFi (will stop BLE)" },
          { "setPoolZone",   o->setPoolZone,            "Set NTP Pool Zone for next NTP Sync (persistent)" },
          { "NTPSync",       o->doStartNTPUpdater,      "Update time from NTP (will start WiFi)" },
          { "DownloadDB",    o->doRunWiFiDownloader,    "Download or update db files (will start WiFi and update NTP first)" },
          { "setWiFiSSID",   o->setWiFiSSID,            "Set WiFi SSID" },
          { "setWiFiPASS",   o->setWiFiPASS,            "Set WiFi Password" },
        #endif

      };

      // bind static SerialCommands to local Commands
      SerialCommands = Commands;
      Csize = (sizeof(Commands) / sizeof(Commands[0]));

      ToggleTpl ToggleProps[] = {
        { "Out.serialEcho",      Out.serialEcho },
        { "DB.needsReset",       DB.needsReset },
        { "DBneedsReplication",  DBneedsReplication },
        { "DB.needsPruning",     DB.needsPruning },
        { "TimeIsSet",           TimeIsSet },
        { "foundTimeServer",     foundTimeServer },
        { "RTCisRunning",        RTCisRunning },
        { "ForceBleTime",        ForceBleTime },
        { "DayChangeTrigger",    DayChangeTrigger },
        { "HourChangeTrigger",   HourChangeTrigger },
        { "timeServerIsRunning", timeServerIsRunning },
        #if HAS_GPS
          { "GPSDebugToSerial",    GPSDebugToSerial },
        #endif
      };
      // bind static TogglableProps to local ToggleProps
      TogglableProps = ToggleProps;
      Tsize = (sizeof(ToggleProps) / sizeof(ToggleProps[0]));

      /*
      // only autorun "help" command on regular boot (or after a crash)
      if (resetReason != 12) { // HW Reset
        while( !DBStarted ) vTaskDelay(10); // wait until DB has started before showing help
        runCommand( (char*)"help" );
        //runCommand( (char*)"toggle" );
        //runCommand( (char*)"ls" );
      }*/

      unsigned long lastHidCheck = millis();

      while ( 1 ) {
        // Read Serial1 and process commands if any
        SerialRead();
        #if HAS_GPS
          // Read Serial2
          GPSRead();
        #endif
        // read HID if any
        ProcessHID( lastHidCheck );
        vTaskDelay(10);
      }

      vTaskDelete( NULL );
    }

    static void runCommand( char* command )
    {
      if ( isEmpty( command ) ) return;
      if ( Csize == 0 ) return; // no commands yet to parse, too early to call
      if ( strcmp( command, "help" ) == 0 ) {
        Serial.println("\nAvailable Commands:\n");
        for ( uint16_t i = 0; i < Csize; i++ ) {
          Serial.printf("  %02d) %16s : %s\n", i + 1, SerialCommands[i].command, SerialCommands[i].description);
        }
        Serial.println();
      } else {
        char *token;
        char delim[2];
        char *args;
        bool has_args = false;
        strncpy(delim, " ", 2); // strtok_r needs a null-terminated string

        if ( strstr(command, delim) ) {
          // turn command into token/arg
          token = strtok_r(command, delim, &args); // Search for command at start of buffer
          if ( token != NULL ) {
            has_args = true;
            //Serial.printf("[%s] Found arg for token '%s' : %s\n", command, token, args);
          }
        }
        for ( uint16_t i = 0; i < Csize; i++ ) {
          if ( strcmp( SerialCommands[i].command, command ) == 0 ) {
            if ( has_args ) {
              Serial.printf( "Running '%s %s' command\n", token, args );
              SerialCommands[i].cb.function( args );
            } else {
              Serial.printf( "Running '%s' command\n", SerialCommands[i].command );
              SerialCommands[i].cb.function( NULL );
            }
            //sprintf(command, "%s", "");
            return;
          }
        }
        Serial.printf( "Command '%s' not found\n", command );
      }
    }

    static void NoHIDCheck( unsigned long &lastHidCheck ) { ; }

    static void M5ButtonCheck( unsigned long &lastHidCheck )
    {
      if( lastHidCheck + 150 < millis() ) {
        takeMuxSemaphore();
        M5.update();
        giveMuxSemaphore();
        if( M5.BtnA.wasPressed() ) {
          UI.brightness -= UI.brightnessIncrement;
          setBrightnessCB();
        }
        if( M5.BtnB.wasPressed() ) {
          UI.brightness += UI.brightnessIncrement;
          setBrightnessCB();
        }
        if( M5.BtnC.wasPressed() ) {
          toggleFilterCB();
        }
        lastHidCheck = millis();
      }
    }

    static void scanInit()
    {
      UI.update(); // run after-scan display stuff
      DB.maintain();
      scanTaskRunning = true;
      scanTaskStopped = false;

      if ( FoundDeviceCallback == NULL ) {
        FoundDeviceCallback = new FoundDeviceCallbacks(); // collect/store BLE data
      }
      pBLEScan = BLEDevice::getScan(); //create new scan
      pBLEScan->setAdvertisedDeviceCallbacks( FoundDeviceCallback );

      pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
      pBLEScan->setInterval(0x50); // 0x50
      pBLEScan->setWindow(0x30); // 0x30
    }

    static void scanDeInit()
    {
      scanTaskStopped = true;
      delete FoundDeviceCallback; FoundDeviceCallback = NULL;
    }

    static void scanTask( void * parameter )
    {
      scanInit();
      byte onAfterScanStep = 0;
      while ( scanTaskRunning ) {
        if ( onAfterScanSteps( onAfterScanStep, scan_cursor ) ) continue;
        dumpStats("BeforeScan::");
        onBeforeScan();
        pBLEScan->start(SCAN_DURATION);
        onAfterScan();
        //DB.maintain();
        dumpStats("AfterScan:::");
        scan_rounds++;
      }
      scanDeInit();
      vTaskDelete( NULL );
    }

    static bool onAfterScanSteps( byte &onAfterScanStep, uint16_t &scan_cursor )
    {
      switch ( onAfterScanStep ) {
        case POPULATE: // 0
          onScanPopulate( scan_cursor ); // OUI / vendorname / isanonymous
          onAfterScanStep++;
          return true;
          break;
        case IFEXISTS: // 1
          onScanIfExists( scan_cursor ); // exists + hits
          onAfterScanStep++;
          return true;
          break;
        case RENDER: // 2
          onScanRender( scan_cursor ); // ui work
          onAfterScanStep++;
          return true;
          break;
        case PROPAGATE: // 3
          onAfterScanStep = 0;
          if ( onScanPropagate( scan_cursor ) ) { // copy to DB / cache
            scan_cursor++;
            return true;
          }
          break;
        default:
          log_w("Exit flat loop on afterScanStep value : %d", onAfterScanStep);
          onAfterScanStep = 0;
          break;
      }
      return false;
    }

    static bool onScanPopulate( uint16_t _scan_cursor )
    {
      if ( onScanPopulated ) {
        log_v("%s", " onScanPopulated = true ");
        return false;
      }
      if ( _scan_cursor >= devicesCount) {
        onScanPopulated = true;
        log_d("%s", "done all");
        return false;
      }
      if ( isEmpty( BLEDevScanCache[_scan_cursor]->address ) ) {
        log_w("empty addess");
        return true; // end of cache
      }
      populate( BLEDevScanCache[_scan_cursor] );
      return true;
    }

    static bool onScanIfExists( int _scan_cursor )
    {
      if ( onScanPostPopulated ) {
        log_v("onScanPostPopulated = true");
        return false;
      }
      if ( _scan_cursor >= devicesCount) {
        log_d("done all");
        onScanPostPopulated = true;
        return false;
      }
      int deviceIndexIfExists = -1;
      deviceIndexIfExists = getDeviceCacheIndex( BLEDevScanCache[_scan_cursor]->address );
      if ( deviceIndexIfExists > -1 ) {
        inCacheCount++;
        BLEDevRAMCache[deviceIndexIfExists]->hits++;
        if ( TimeIsSet ) {
          if ( BLEDevRAMCache[deviceIndexIfExists]->created_at.year() <= 1970 ) {
            BLEDevRAMCache[deviceIndexIfExists]->created_at = nowDateTime;
          }
          BLEDevRAMCache[deviceIndexIfExists]->updated_at = nowDateTime;
        }
        BLEDevHelper.mergeItems( BLEDevScanCache[_scan_cursor], BLEDevRAMCache[deviceIndexIfExists] ); // merge scan data into existing psram cache
        BLEDevHelper.copyItem( BLEDevRAMCache[deviceIndexIfExists], BLEDevScanCache[_scan_cursor] ); // copy back merged data for rendering
        log_i( "Device %d / %s exists in cache, increased hits to %d", _scan_cursor, BLEDevScanCache[_scan_cursor]->address, BLEDevScanCache[_scan_cursor]->hits );
      } else {
        if ( BLEDevScanCache[_scan_cursor]->is_anonymous ) {
          // won't land in DB (won't be checked either) but will land in cache
          uint16_t nextCacheIndex = BLEDevHelper.getNextCacheIndex( BLEDevRAMCache, BLEDevCacheIndex );
          BLEDevHelper.reset( BLEDevRAMCache[nextCacheIndex] );
          BLEDevScanCache[_scan_cursor]->hits++;
          BLEDevHelper.copyItem( BLEDevScanCache[_scan_cursor], BLEDevRAMCache[nextCacheIndex] );
          log_v( "Device %d / %s is anonymous, won't be inserted", _scan_cursor, BLEDevScanCache[_scan_cursor]->address, BLEDevScanCache[_scan_cursor]->hits );
        } else {
          deviceIndexIfExists = DB.deviceExists( BLEDevScanCache[_scan_cursor]->address ); // will load returning devices from DB if necessary
          if (deviceIndexIfExists > -1) {
            uint16_t nextCacheIndex = BLEDevHelper.getNextCacheIndex( BLEDevRAMCache, BLEDevCacheIndex );
            BLEDevHelper.reset( BLEDevRAMCache[nextCacheIndex] );
            BLEDevDBCache->hits++;
            if ( TimeIsSet ) {
              if ( BLEDevDBCache->created_at.year() <= 1970 ) {
                BLEDevDBCache->created_at = nowDateTime;
              }
              BLEDevDBCache->updated_at = nowDateTime;
            }
            BLEDevHelper.mergeItems( BLEDevScanCache[_scan_cursor], BLEDevDBCache ); // merge scan data into BLEDevDBCache
            BLEDevHelper.copyItem( BLEDevDBCache, BLEDevRAMCache[nextCacheIndex] ); // copy merged data to assigned psram cache
            BLEDevHelper.copyItem( BLEDevDBCache, BLEDevScanCache[_scan_cursor] ); // copy back merged data for rendering

            log_v( "Device %d / %s is already in DB, increased hits to %d", _scan_cursor, BLEDevScanCache[_scan_cursor]->address, BLEDevScanCache[_scan_cursor]->hits );
          } else {
            // will be inserted after rendering
            BLEDevScanCache[_scan_cursor]->in_db = false;
            log_v( "Device %d / %s is not in DB", _scan_cursor, BLEDevScanCache[_scan_cursor]->address );
          }
        }
      }
      return true;
    }

    static bool onScanRender( uint16_t _scan_cursor )
    {
      if ( onScanRendered ) {
        log_v("onScanRendered = true");
        return false;
      }
      if ( _scan_cursor >= devicesCount) {
        log_v("done all");
        onScanRendered = true;
        return false;
      }
      UI.BLECardTheme.setTheme( IN_CACHE_ANON );
      BLEDevTmp = BLEDevScanCache[_scan_cursor];
      UI.printBLECard( (BlueToothDeviceLink){.cacheIndex=_scan_cursor,.device=BLEDevTmp} ); // render
      delay(1);
      sprintf( processMessage, processTemplateLong, "Rendered ", _scan_cursor + 1, " / ", devicesCount );
      UI.headerStats( processMessage );
      delay(1);
      UI.cacheStats();
      delay(1);
      UI.footerStats();
      delay(1);
      return true;
    }

    static bool onScanPropagate( uint16_t &_scan_cursor )
    {
      if ( onScanPropagated ) {
        log_v("onScanPropagated = true");
        return false;
      }
      if ( _scan_cursor >= devicesCount) {
        log_v("done all");
        onScanPropagated = true;
        _scan_cursor = 0;
        return false;
      }
      //BLEDevScanCacheIndex = _scan_cursor;
      if ( isEmpty( BLEDevScanCache[_scan_cursor]->address ) ) {
        return true;
      }
      if ( BLEDevScanCache[_scan_cursor]->is_anonymous || BLEDevScanCache[_scan_cursor]->in_db ) { // don't DB-insert anon or duplicates
        sprintf( processMessage, processTemplateLong, "Released ", _scan_cursor + 1, " / ", devicesCount );
        if ( BLEDevScanCache[_scan_cursor]->is_anonymous ) AnonymousCacheHit++;
      } else {
        if ( DB.insertBTDevice( BLEDevScanCache[_scan_cursor] ) == DBUtils::INSERTION_SUCCESS ) {
          sprintf( processMessage, processTemplateLong, "Saved ", _scan_cursor + 1, " / ", devicesCount );
          log_d( "Device %d successfully inserted in DB", _scan_cursor );
          entries++;
        } else {
          log_e( "  [!!! BD INSERT FAIL !!!] Device %d could not be inserted", _scan_cursor );
          sprintf( processMessage, processTemplateLong, "Failed ", _scan_cursor + 1, " / ", devicesCount );
        }
      }
      BLEDevHelper.reset( BLEDevScanCache[_scan_cursor] ); // discard
      UI.headerStats( processMessage );
      return true;
    }

    static void onBeforeScan()
    {
      DB.maintain();
      UI.headerStats("Scan in progress");
      UI.startBlink();
      processedDevicesCount = 0;
      devicesCount = 0;
      scan_cursor = 0;
      onScanProcessed = false;
      onScanDone = false;
      onScanPopulated = false;
      onScanPropagated = false;
      onScanPostPopulated = false;
      onScanRendered = false;
      foundTimeServer = false;
    }

    static void onAfterScan()
    {
      UI.stopBlink();
      if ( foundTimeServer && (!TimeIsSet || ForceBleTime) ) {
        if( ! timeClientisStarted ) {
          if( timeServerBLEAddress != "" ) {
            UI.headerStats("BLE Time sync ...");
            log_w("HOBO mode: found a peer with time provider service, launching BLE TimeClient Task");
            xTaskCreatePinnedToCore(startTimeClient, "startTimeClient", 2048, NULL, 0, NULL, TASKLAUNCHER_CORE ); /* last = Task Core */
            while( scanTaskRunning ) {
              vTaskDelay( 10 );
            }
            return;
          }
        }
      }
      UI.headerStats("Showing results ...");
      devicesCount = processedDevicesCount;
      BLEDevice::getScan()->clearResults();
      if ( devicesCount < MAX_DEVICES_PER_SCAN ) {
        if ( SCAN_DURATION + 1 < MAX_SCAN_DURATION ) {
          SCAN_DURATION++;
        }
      } else if ( devicesCount > MAX_DEVICES_PER_SCAN ) {
        if ( SCAN_DURATION - 1 >= MIN_SCAN_DURATION ) {
          SCAN_DURATION--;
        }
        log_w("Cache overflow (%d results vs %d slots), truncating results...", devicesCount, MAX_DEVICES_PER_SCAN);
        devicesCount = MAX_DEVICES_PER_SCAN;
      } else {
        // same amount
      }
      sessDevicesCount += devicesCount;
      notInCacheCount = 0;
      inCacheCount = 0;
      onScanDone = true;
      scan_cursor = 0;
      UI.update();
    }

    static int getDeviceCacheIndex(const char* address)
    {
      if ( isEmpty( address ) )  return -1;
      for (int i = 0; i < BLEDEVCACHE_SIZE; i++) {
        if ( strcmp(address, BLEDevRAMCache[i]->address ) == 0  ) {
          BLEDevCacheHit++;
          log_v("[CACHE HIT] BLEDevCache ID #%s has %d cache hits", address, BLEDevRAMCache[i]->hits);
          return i;
        }
        delay(1);
      }
      return -1;
    }

    // used for serial debugging
    static void dumpStats(const char* prefixStr)
    {
      if (lastheap > freeheap) {
        // heap decreased
        sprintf(heapsign, "%s", "↘");
      } else if (lastheap < freeheap) {
        // heap increased
        sprintf(heapsign, "%s", "↗");
      } else {
        // heap unchanged
        sprintf(heapsign, "%s", "⇉");
      }
      if (lastscanduration > SCAN_DURATION) {
        sprintf(scantimesign, "%s", "↘");
      } else if (lastscanduration < SCAN_DURATION) {
        sprintf(scantimesign, "%s", "↗");
      } else {
        sprintf(scantimesign, "%s", "⇉");
      }
      lastheap = freeheap;
      lastscanduration = SCAN_DURATION;
      log_i("%s[Scan#%02d][%s][Duration%s%d][Processed:%d of %d][Heap%s%d / %d] [Cache hits][BLEDevCards:%d][Anonymous:%d][Oui:%d][Vendor:%d]",
        prefixStr,
        scan_rounds,
        hhmmssString,
        scantimesign,
        lastscanduration,
        processedDevicesCount,
        devicesCount,
        heapsign,
        lastheap,
        freepsheap,
        BLEDevCacheHit,
        AnonymousCacheHit,
        OuiCacheHit,
        VendorCacheHit
      );
    }

  private:

    static void getPrefs()
    {
      preferences.begin("BLEPrefs", true);
      Out.serialEcho   = preferences.getBool("serialEcho", true);
      UI.filterVendors = preferences.getBool("filterVendors", false);
      UI.brightness    = preferences.getUChar("brightness", BASE_BRIGHTNESS);
      timeZone         = preferences.getFloat("timeZone", timeZone);
      summerTime       = preferences.getBool("summerTime", summerTime);
      log_d("Defrosted brightness: %d", UI.brightness );
      log_w("Loaded NVS Prefs:");
      log_w("  serialEcho\t%s",    Out.serialEcho?"true":"false");
      log_w("  filterVendors\t%s", UI.filterVendors?"true":"false");
      log_w("  brightness\t%d",    UI.brightness );
      log_w("  timeZone\t\t%.2g",    timeZone );
      log_w("  summerTime\t%s",    summerTime?"true":"false");
      #ifdef WITH_WIFI
        String poolZone  = preferences.getString( "poolZone", String( DEFAULT_NTP_SERVER ) );
        log_w("  poolZone\t\t%s", poolZone );
      #endif
      preferences.end();
    }
    static void setPrefs()
    {
      preferences.begin("BLEPrefs", false);
      preferences.putBool("serialEcho", Out.serialEcho);
      preferences.putBool("filterVendors", UI.filterVendors );
      preferences.putUChar("brightness", UI.brightness );
      preferences.putFloat("timeZone", timeZone);
      preferences.putBool("summerTime", summerTime);
      preferences.end();
    }

    // completes unpopulated fields of a given entry by performing DB oui/vendor lookups
    static void populate( BlueToothDevice *CacheItem )
    {
      if ( strcmp( CacheItem->ouiname, "[unpopulated]" ) == 0 ) {
        log_d("  [populating OUI for %s]", CacheItem->address);
        DB.getOUI( CacheItem->address, CacheItem->ouiname );
      }
      if ( strcmp( CacheItem->manufname, "[unpopulated]" ) == 0 ) {
        if ( CacheItem->manufid != -1 ) {
          log_d("  [populating Vendor for :%d]", CacheItem->manufid );
          DB.getVendor( CacheItem->manufid, CacheItem->manufname );
        } else {
          BLEDevHelper.set( CacheItem, "manufname", '\0');
        }
      }
      CacheItem->is_anonymous = BLEDevHelper.isAnonymous( CacheItem );
      log_v("[populated :%s]", CacheItem->address);
    }

};




BLEScanUtils BLECollector;
