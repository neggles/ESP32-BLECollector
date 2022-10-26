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




const char* data = 0; // for some reason sqlite3 db callback needs this
const char* dataBLE = 0; // for some reason sqlite3 db callback needs this
const char* dataOUI = 0; // for some reason sqlite3 db callback needs this
const char* dataVendor = 0;
char *zErrMsg = 0; // holds DB Error message
const char BACKSLASH = '\\'; // used to clean() slashes
static char *colNeedle = 0; // search criteria
static char colValue[32] = {'\0'}; // search result
static bool DBneedsReplication = false;


#define BLEMAC_CREATE_FIELDNAMES " \
  appearance INTEGER, \
  name, \
  address, \
  ouiname, \
  rssi INTEGER, \
  manufid INTEGER, \
  manufname, \
  uuid, \
  created_at DATETIME, \
  updated_at DATETIME, \
  hits INTEGER \
"
#define BLEMAC_INSERT_FIELDNAMES " \
  appearance, \
  name, \
  address, \
  ouiname, \
  rssi, \
  manufid, \
  manufname, \
  uuid, \
  created_at, \
  updated_at, \
  hits \
"
#define BLEMAC_SELECT_FIELDNAMES " \
  appearance, \
  name, \
  address, \
  ouiname, \
  rssi, \
  manufid, \
  manufname, \
  uuid, \
  strftime('%s', created_at) as created_at, \
  strftime('%s', updated_at) as updated_at, \
  hits \
"
#define insertQueryTemplate "INSERT INTO blemacs(" BLEMAC_INSERT_FIELDNAMES ") VALUES(%d,'%s','%s','%s',%d,%d,'%s','%s','%s.000000','%s.000000', '%d')"

// all DB queries
#define nameQuery    "SELECT DISTINCT SUBSTR(name,0,32) FROM blemacs where TRIM(name)!=''"
#define manufnameQuery   "SELECT DISTINCT SUBSTR(manufname,0,32) FROM blemacs where TRIM(manufname)!=''"
#define ouinameQuery "SELECT DISTINCT SUBSTR(ouiname,0,32) FROM blemacs where TRIM(ouiname)!=''"
#define allEntriesQuery "SELECT " BLEMAC_SELECT_FIELDNAMES " FROM blemacs;"
#define countEntriesQuery "SELECT count(*) FROM blemacs;"
#define dropTableQuery   "DROP TABLE IF EXISTS blemacs;"
#define createTableQuery "CREATE TABLE IF NOT EXISTS blemacs( " BLEMAC_CREATE_FIELDNAMES " )"
#define pruneTableQuery "DELETE FROM blemacs"
#define testVendorNamesQuery "SELECT SUBSTR(vendor,0,32)  FROM 'ble-oui' LIMIT 10"
#define testOUIQuery "SELECT * FROM 'oui-light' limit 10"
static char insertQuery[1024]; // stack overflow ? pray that 1024 is enough :D
#define searchDeviceTemplate "SELECT " BLEMAC_SELECT_FIELDNAMES " FROM blemacs WHERE address='%s'"
static char searchDeviceQuery[1024];
#define vendorRequestTpl "SELECT vendor FROM 'ble-oui' WHERE id='%d'"
#define OUIRequestTpl "SELECT * FROM 'oui-light' WHERE Assignment=UPPER('%s');"


// used by getVendor()
#ifndef VENDORCACHE_SIZE // override this from Settings.h
#define VENDORCACHE_SIZE 16
#endif

struct VendorHeapCacheStruct
{
  int devid = -1;
  char *vendor = NULL;
  void init( bool hasPsram=false ) {
    if( hasPsram ) {
      vendor = (char*)ps_calloc(MAX_FIELD_LEN+1, sizeof(char));
    } else {
      vendor = (char*)calloc(MAX_FIELD_LEN+1, sizeof(char));
    }
  }
};

VendorHeapCacheStruct VendorHeapCache[VENDORCACHE_SIZE];
uint16_t VendorCacheIndex = 0; // index in the circular buffer
static int VendorCacheHit = 0;


struct VendorPsramCacheStruct
{
  uint16_t *devid = NULL;
  uint16_t hits   = 0; // cache hits
  char *vendor    = NULL;
};


// #define VendorDBSize 1740 // how many entries in the OUI lookup DB
#define VendorDBSize 1889 // how many entries in the OUI lookup DB
VendorPsramCacheStruct** VendorPsramCache = NULL;

// used by getOUI()
#ifndef OUICACHE_SIZE // override this from Settings.h
#define OUICACHE_SIZE 32
#endif


struct OUIHeapCacheStruct
{
  char *mac = NULL;
  char *assignment = NULL;
  void init( bool hasPsram=false ) {
    if( hasPsram ) {
      mac        = (char*)ps_calloc(SHORT_MAC_LEN+1, sizeof(char));
      assignment = (char*)ps_calloc(MAX_FIELD_LEN+1, sizeof(char));
    } else {
      mac        = (char*)calloc(SHORT_MAC_LEN+1, sizeof(char));
      assignment = (char*)calloc(MAX_FIELD_LEN+1, sizeof(char));
    }
  }
  void setMac( const char* _mac ) {
    copy( mac, _mac, SHORT_MAC_LEN );
  }
  void setAssignment( const char* _assignment ) {
    copy( assignment, _assignment, MAX_FIELD_LEN );
  }
};

OUIHeapCacheStruct OuiHeapCache[OUICACHE_SIZE];
uint16_t OuiCacheIndex = 0; // index in the circular buffer
static int OuiCacheHit = 0;

struct OUIPsramCacheStruct
{
  char *mac = NULL;
  uint16_t hits    = 0; // cache hits
  char *assignment = NULL;
};

#define OUIDBSize 25523 // how many entries in the OUI lookup DB
OUIPsramCacheStruct** OuiPsramCache = NULL;

#define BLE_COLLECTOR_DB_FILE    "blemacs.db" // default filename for storing collected data
#define MAC_OUI_NAMES_DB_FILE    "mac-oui-light.db" // oui list of known mac addresses
#define BLE_VENDOR_NAMES_DB_FILE "ble-oui.db" // ble device/service names by mac address
#define BLE_DB_FILES_URL_PREFIX  "https://github.com/tobozo/ESP32-BLECollector/releases/download/1.0.0/"
#define MAC_OUI_NAMES_DB_FS_SIZE         933888 // change this according to the file size
#define BLE_VENDOR_NAMES_DB_FS_SIZE      73728 // change this according to the file size

#define MAC_OUI_NAMES_DB_URL             BLE_DB_FILES_URL_PREFIX MAC_OUI_NAMES_DB_FILE
#define BLE_VENDOR_NAMES_DB_URL          BLE_DB_FILES_URL_PREFIX BLE_VENDOR_NAMES_DB_FILE
#define BLE_COLLECTOR_DB_SQLITE_PATH     "/" BLE_FS_TYPE "/" BLE_COLLECTOR_DB_FILE
#define MAC_OUI_NAMES_DB_SQLITE_PATH     "/" BLE_FS_TYPE "/" MAC_OUI_NAMES_DB_FILE
#define BLE_VENDOR_NAMES_DB_SQLITE_PATH  "/" BLE_FS_TYPE "/" BLE_VENDOR_NAMES_DB_FILE
#define BLE_COLLECTOR_DB_FS_PATH         "/" BLE_COLLECTOR_DB_FILE
#define MAC_OUI_NAMES_DB_FS_PATH         "/" MAC_OUI_NAMES_DB_FILE
#define BLE_VENDOR_NAMES_DB_FS_PATH      "/" BLE_VENDOR_NAMES_DB_FILE




class DBUtils
{
  public:

    char currentBLEAddress[MAC_LEN+1] = "00:00:00:00:00:00"; // used to proxy BLE search term to DB query

    char* BLEMacsDbSQLitePath = NULL;//"/sdcard/blemacs.db";
    char* BLEMacsDbFSPath = NULL;// "/blemacs.db";

    sqlite3 *BLECollectorDB; // read/write
    sqlite3 *BLEVendorsDB; // readonly
    sqlite3 *OUIVendorsDB; // readonly

    enum DBMessage
    {
      TABLE_CREATION_FAILED = -1,
      INSERTION_FAILED = -2,
      INCREMENT_FAILED = -3,
      INSERTION_IGNORED = -4,
      DB_IS_OOM = -5,
      INSERTION_SUCCESS = 1,
      INCREMENT_SUCCESS = 2
    };

    enum DBName
    {
      BLE_COLLECTOR_DB = 0,
      MAC_OUI_NAMES_DB = 1,
      BLE_VENDOR_NAMES_DB =2
    };

    struct DBInfo
    {
      DBName id;
      char* sqlitepath;
      char* fspath;
    };

    DBInfo dbcollection[3] =
    {
      { .id=BLE_COLLECTOR_DB,    .sqlitepath=(char *)BLE_COLLECTOR_DB_SQLITE_PATH,    .fspath=(char *)BLE_COLLECTOR_DB_FS_PATH },
      { .id=MAC_OUI_NAMES_DB,    .sqlitepath=(char *)MAC_OUI_NAMES_DB_SQLITE_PATH,    .fspath=(char *)MAC_OUI_NAMES_DB_FS_PATH },
      { .id=BLE_VENDOR_NAMES_DB, .sqlitepath=(char *)BLE_VENDOR_NAMES_DB_SQLITE_PATH, .fspath=(char *)BLE_VENDOR_NAMES_DB_FS_PATH }
    };


    bool isOOM = false; // for stability
    bool isCorrupt = false; // for maintenance
    bool hasPsram = false;
    bool needsPruning = false;
    bool needsReset = false;
    //bool needsReplication = false;
    bool needsRestart = false;
    bool initDone = false;


    bool init()
    {
      while(SDSetup()==false) {
        UI.headerStats("Card Mount Failed");
        delay(500);
        UI.headerStats(" ");
        delay(300);
      }
      hasPsram = psramInit();

      log_i("Has PSRAM: %s", hasPsram?"true":"false");

      BLEMacsDbSQLitePath = (char*)malloc(32);
      BLEMacsDbFSPath = (char*)malloc(32);

      setBLEDBPath();

      if( !checkDBFiles() ) {
        log_e("[ERROR] OUI and/or Mac DB Files could not be checked");
        return false;
      } else {
        log_w("[OK] OUI File: %s", MAC_OUI_NAMES_DB_FS_PATH );
        log_w("[OK] Vendor File: %s", BLE_VENDOR_NAMES_DB_FS_PATH );
      }

      initial_free_heap = freeheap;
      isQuerying = true;
      sqlite3_initialize();

      takeMuxSemaphore();

      bool create_DB = false;

      if( !BLE_FS.exists( BLEMacsDbFSPath ) ) {
        log_w("%s DB does not exist", BLEMacsDbFSPath);
        create_DB = true;
      } else {
        fs::File dbFile = BLE_FS.open( BLEMacsDbFSPath );
        if( dbFile.size() == 0 ) {
          create_DB = true;
          log_w("%s DB file is empty", BLEMacsDbFSPath);
        }
        dbFile.close();
      }

      if( create_DB ) {
        createDB(); // only if no exists
      } else {
        log_d("%s DB file already exists", BLEMacsDbFSPath);
      }
      isQuerying = false;

      entries = getEntries();

      giveMuxSemaphore();

      return cacheWarmup();
    }


    void setBLEDBPath()
    {
      if( TimeIsSet ) {
        //DateTime epoch = RTC.now();
        DateTime epoch = DateTime(year(), month(), day(), hour(), minute(), second());
        sprintf(BLEMacsDbSQLitePath, "/%s/ble-%04d-%02d-%02d.db",
          BLE_FS_TYPE, // sdcard / sd / spiffs / littlefs
          epoch.year(),
          epoch.month(),
          epoch.day()
        );
        sprintf(BLEMacsDbFSPath, "/ble-%04d-%02d-%02d.db",
          epoch.year(),
          epoch.month(),
          epoch.day()
        );
        log_d("Assigning db path : %s / %s", BLEMacsDbSQLitePath, BLEMacsDbFSPath);
      } else {
        sprintf(BLEMacsDbSQLitePath, "/%s/blemacs.db", BLE_FS_TYPE);
        sprintf(BLEMacsDbFSPath, "%s", "/blemacs.db");
      }
      dbcollection[BLE_COLLECTOR_DB].sqlitepath = BLEMacsDbSQLitePath;
    }


    static bool checkDBFiles()
    {
      bool OUIFileChecksOut    = checkOUIFile();
      bool VendorFileChecksOut = checkVendorFile();
      if( !OUIFileChecksOut || !VendorFileChecksOut ) return false;
      return true;
    }


    static bool checkOUIFile()
    {
      return checkFile( MAC_OUI_NAMES_DB_FS_PATH, MAC_OUI_NAMES_DB_FS_SIZE );
    }


    static bool checkVendorFile()
    {
      return checkFile( BLE_VENDOR_NAMES_DB_FS_PATH, BLE_VENDOR_NAMES_DB_FS_SIZE );
    }


    static bool checkFile( const char* fileName, const size_t expectedSize )
    {
      bool ret = true;
      isQuerying = true;
      if( ! BLE_FS.exists( fileName ) ) {
        log_e( "[ERROR] DB file not found: %s", fileName );
        ret = false;
      } else {
        File tmpFile = BLE_FS.open( fileName );
        size_t size = tmpFile.size();
        tmpFile.close();
        if( size != expectedSize ) {
          log_e("[CRITITAL] DB file %s is corrupted (expected size: %d, found size: %d), aborting", fileName, expectedSize, size);
          ret = false;
        }
      }
      isQuerying = false;
      return ret;
    }


    void OUICacheWarmup()
    {
      if( hasPsram ) {
        OuiPsramCache = (OUIPsramCacheStruct**)ps_calloc(OUIDBSize+1, sizeof( OUIPsramCacheStruct* ) );
        for(int i=0; i<OUIDBSize; i++) {
          OuiPsramCache[i] = (OUIPsramCacheStruct*)ps_calloc(1, sizeof( OUIPsramCacheStruct ));
          if( OuiPsramCache[i] == NULL ) {
            log_e("[ERROR][%d][%d][%d] can't allocate", i, freeheap, freepsheap);
            continue;
          }
          OuiPsramCache[i]->mac        = (char*)ps_calloc(SHORT_MAC_LEN+1, sizeof(char));
          OuiPsramCache[i]->assignment = (char*)ps_calloc(MAX_FIELD_LEN+1, sizeof(char));
        }
      } else {
        for(uint16_t i=0; i<OUICACHE_SIZE; i++) {
          OuiHeapCache[i].init( false );
        }
      }
    }


    void VendorCacheWarmup()
    {
      if( hasPsram ) {
        VendorPsramCache = (VendorPsramCacheStruct**)ps_calloc(VendorDBSize+1, sizeof( VendorPsramCacheStruct* ) );
        if( VendorPsramCache == NULL ) {
          log_e("[ERROR][%d][%d] can't allocate %d bytes", freeheap, freepsheap, sizeof( VendorPsramCacheStruct* ) );
        }

        for(int i=0; i<VendorDBSize; i++) {
          VendorPsramCache[i] = (VendorPsramCacheStruct*)ps_calloc(1, sizeof( VendorPsramCacheStruct ));
          if( VendorPsramCache[i] == NULL ) {
            log_e("[ERROR][%d][%d][%d] can't allocate", i, freeheap, freepsheap);
            continue;
          }
          VendorPsramCache[i]->devid  = (uint16_t*)ps_malloc(sizeof(uint16_t));
          VendorPsramCache[i]->vendor = (char*)ps_calloc(MAX_FIELD_LEN+1, sizeof(char));
        }
      } else {
        for(uint16_t i=0; i<VENDORCACHE_SIZE; i++) {
          VendorHeapCache[i].init( false );
        }
      }
    }


    void *ble_calloc(size_t n, size_t size)
    {
      if( hasPsram ) {
        return ps_calloc( n, size );
      } else {
        return calloc( n, size );
      }
    }


    void BLEDevCacheWarmup()
    {
      BLEDevRAMCache = (BlueToothDevice**)ble_calloc(BLEDEVCACHE_SIZE, sizeof( BlueToothDevice ) );
      for(uint16_t i=0; i<BLEDEVCACHE_SIZE; i++) {
        BLEDevRAMCache[i] = (BlueToothDevice*)ble_calloc(1, sizeof( BlueToothDevice ) );
        if( BLEDevRAMCache[i] == NULL ) {
          log_e("[ERROR][%d][%d][%d] can't allocate", i, freeheap, freepsheap);
          continue;
        }
        BLEDevHelper.init( BLEDevRAMCache[i], hasPsram );
        delay(1);
      }
      BLEDevScanCache = (BlueToothDevice**)ble_calloc(MAX_DEVICES_PER_SCAN, sizeof( BlueToothDevice ) );
      for(uint16_t i=0; i<MAX_DEVICES_PER_SCAN; i++) {
        BLEDevScanCache[i] = (BlueToothDevice*)ble_calloc(1, sizeof( BlueToothDevice ) );
        if( BLEDevScanCache[i] == NULL ) {
          log_e("[ERROR][%d][%d][%d] can't allocate", i, freeheap, freepsheap);
          continue;
        }
        BLEDevHelper.init( BLEDevScanCache[i], hasPsram );
        delay(1);
      }
    }


    void setCacheSize()
    {
      if( hasPsram ) {
        BLEDEVCACHE_SIZE = BLEDEVCACHE_PSRAM_SIZE;
        log_d("[PSRAM] OK");
      } else {
        BLEDEVCACHE_SIZE = BLEDEVCACHE_HEAP_SIZE;
        log_w("[PSRAM] NOT DETECTED, will use heap");
      }
    }


    bool cacheWarmup()
    {
      setCacheSize();
      OUICacheWarmup();
      VendorCacheWarmup();
      BLEDevCacheWarmup();

      if( hasPsram ) {
        loadOUIToPSRam();
        loadVendorsToPSRam();
      } else {
        if( !testOUI() || !testVendorNames() ) {
          return false;
        }
      }

      BLEDevTmp = (BlueToothDevice*)calloc(1, sizeof( BlueToothDevice ) );
      BLEDevDBCache = (BlueToothDevice*)calloc(1, sizeof( BlueToothDevice ) );
      BLEDevHelper.init( BLEDevTmp, false ); // false = make sure the copy placeholder isn't using SPI ram
      BLEDevHelper.init( BLEDevDBCache, false ); // false = make sure the copy placeholder isn't using SPI ram
      initDone = true;
      return initDone;
    }


    bool maintain()
    {
      bool ret = true;
      if( isOOM ) {
        isOOM = false;
        log_e("[DB OOM], please run pruneDB and restart manually before it crashes...");
        ret = false;
      }
      if( isCorrupt ) {
        isCorrupt = false;
        log_e("[I/O ERROR] a DB file is corrupt, please run fsck on this SD card");
        ret = false;
      }
      if( needsPruning ) {
        needsPruning = false;
        pruneDB();
      }
      if( needsReset ) {
        needsReset = true;
        resetDB();
      }
      if( DayChangeTrigger ) {
        log_w("Day changed, will generate new DB filename");
        DBneedsReplication = true;
        HourChangeTrigger = false;
        DayChangeTrigger = false;
        setBLEDBPath();
        if( !BLE_FS.exists( BLEMacsDbFSPath ) ) {
          log_w("%s DB does not exist, will create", BLEMacsDbFSPath);
          createDB();
        }
      }
      if( HourChangeTrigger ) {
        #if HAS_GPS
          // setGPSTime( NULL );
        #else
          // try to adjust time if available
          ForceBleTime = true;
        #endif
        log_w("Hour changed, will trigger replication");

        DBneedsReplication = true;
        HourChangeTrigger = false;
        DayChangeTrigger = false;

      }
      if( DBneedsReplication ) {
        DBneedsReplication = false;
        log_w("Replicating DB");
        updateDBFromCache( BLEDevRAMCache, false, false );
      }
      if( needsRestart ) {
        ESP.restart();
        while(1) { ; };
      }
      cacheState();
      return ret;
    }


    void cacheState()
    {
      BLEDevCacheUsed = 0;
      for( uint16_t i=0; i<BLEDEVCACHE_SIZE; i++) {
        if( !isEmpty( BLEDevRAMCache[i]->address ) ) {
          BLEDevCacheUsed++;
        }
      }
      if( hasPsram ) {
        VendorCacheUsed = VENDORCACHE_SIZE;
        OuiCacheUsed = OUICACHE_SIZE;
      } else {
        VendorCacheUsed = 0;
        for( uint16_t i=0; i<VENDORCACHE_SIZE; i++) {
          if( VendorHeapCache[i].devid > -1 ) {
            VendorCacheUsed++;
          }
        }
        OuiCacheUsed = 0;
        for( uint16_t i=0; i<OUICACHE_SIZE; i++) {
          if( !isEmpty( OuiHeapCache[i].assignment ) ) {
            OuiCacheUsed++;
          }
        }
      }
      BLEDevCacheUsed = BLEDevCacheUsed*100 / BLEDEVCACHE_SIZE;
      VendorCacheUsed = VendorCacheUsed*100 / VENDORCACHE_SIZE;
      OuiCacheUsed = OuiCacheUsed*100 / OUICACHE_SIZE;
      log_v("Circular-Buffered Cache Fill: BLEDevRAMCache: %d%s, VendorCache: %d%s, OUICache: %d%s", BLEDevCacheUsed, "%", VendorCacheUsed, "%", OuiCacheUsed, "%");
    }


    int open(DBName dbName, bool readonly=true)
    {
     isQuerying = true;
     int rc = 1;
      switch(dbName) {
        case BLE_COLLECTOR_DB: // will be created upon first boot
          rc = sqlite3_open( dbcollection[dbName].sqlitepath/*"/sdcard/blemacs.db"*/, &BLECollectorDB);
        break;
        case MAC_OUI_NAMES_DB: // https://code.wireshark.org/review/gitweb?p=wireshark.git;a=blob_plain;f=manuf
          rc = sqlite3_open( dbcollection[dbName].sqlitepath /*"/sdcard/mac-oui-light.db"*/, &OUIVendorsDB);
        break;
        case BLE_VENDOR_NAMES_DB: // https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers
          rc = sqlite3_open( dbcollection[dbName].sqlitepath /*"/sdcard/ble-oui.db"*/, &BLEVendorsDB);
        break;
        default: log_e("Can't open null DB"); UI.SetDBStateIcon(-1); isQuerying = false; return rc;
      }
      if (rc) {
        log_e("Can't open database %s", dbcollection[dbName].sqlitepath);
        // SD Card removed ? File corruption ? OOM ?
        // isOOM = true;
        UI.SetDBStateIcon(-1); // OOM or I/O error
        delay(1);
        isQuerying = false;
      } else {
        log_i("Opened database %s successfully", dbcollection[dbName].sqlitepath);
        if(readonly) {
          UI.SetDBStateIcon(1); // R/O
        } else {
          UI.SetDBStateIcon(2); // R+W
        }
        delay(1);
      }
      return rc;
    }

    // close the (hopefully) previously opened DB
    void close(DBName dbName)
    {
      UI.SetDBStateIcon(0);
      switch(dbName) {
        case BLE_COLLECTOR_DB:    sqlite3_close(BLECollectorDB); break;
        case MAC_OUI_NAMES_DB:    sqlite3_close(OUIVendorsDB); break;
        case BLE_VENDOR_NAMES_DB: sqlite3_close(BLEVendorsDB); break;
        default: /* duh ! */ log_e("Can't open null DB");
      }
      delay(1);
      isQuerying = false;
    }

    // replaces any needle from haystack (defaults to double=>single quotes)
    static void clean(char *haystack, const char needle = '"', const char replacewith='\'')
    {
      if( isEmpty( haystack ) ) return;
      int len = strlen( (const char*) haystack );
      for( int _i=0;_i<len;_i++ ) {
        if( haystack[_i] == needle ) {
          haystack[_i] = replacewith;
        }
      }
    }

    // checks if a BLE Device exists, returns its cache index if found
    int deviceExists(const char* address)
    {
      results = 0;
      if( isEmpty( address ) || strlen( address ) > MAC_LEN+1 || strlen( address ) < 17 || address[0]==3) {
        log_w("Cowardly refusing to perform an empty or invalid request : %s / %s", address, currentBLEAddress);
        return -1;
      }
      open(BLE_COLLECTOR_DB);
      log_v("will run on template %s", searchDeviceTemplate );
      sprintf(searchDeviceQuery, searchDeviceTemplate, "%s", "%s", address);
      log_d( "[SEARCH QUERY] : %s", searchDeviceQuery );
      int rc = sqlite3_exec(BLECollectorDB, searchDeviceQuery, BLEDevDBCacheCallback, (void*)dataBLE, &zErrMsg);
      if (rc != SQLITE_OK) {
        error(zErrMsg);
        sqlite3_free(zErrMsg);
        close(BLE_COLLECTOR_DB);
        return -2;
      }
      close(BLE_COLLECTOR_DB);
      // if the device exists, it's been loaded into BLEDevRAMCache[BLEDevCacheIndex]
      return results>0 ? BLEDevCacheIndex : -1;
    }

    // make a copy of the DB to psram to save the SD ^_^
    void loadVendorsToPSRam()
    {
      results = 0;
      open(BLE_VENDOR_NAMES_DB);
      //Out.println("Cloning Vendors DB to PSRam...");
      UI.headerStats("PSRam Cloning...");
      int rc = sqlite3_exec(BLEVendorsDB, "SELECT id, SUBSTR(vendor, 0, 32) AS vendor FROM 'ble-oui' WHERE vendor!=''", VendorDBCallback, (void*)dataVendor, &zErrMsg);
      UI.PrintProgressBar( Out.width );
      if (rc != SQLITE_OK) {
        error(zErrMsg);
        sqlite3_free(zErrMsg);
        close(BLE_VENDOR_NAMES_DB);
        //return -2;
      }
      close(BLE_VENDOR_NAMES_DB);
      for(byte i=0;i<8;i++) {
        __attribute__((unused)) uint32_t rnd = random(0, VendorDBSize);
        log_i("Testing random vendor mac #%d: %d / %s", random(0, VendorDBSize), VendorPsramCache[rnd]->devid[0], VendorPsramCache[rnd]->vendor );
      }
    }

    // make a copy of the DB to psram to save the SD ^_^
    void loadOUIToPSRam()
    {
      results = 0;
      open(MAC_OUI_NAMES_DB);
      //Out.println("Cloning Manufacturers DB to PSRam...");
      UI.headerStats("PSRam Cloning...");
      int rc = sqlite3_exec(OUIVendorsDB, "SELECT LOWER(assignment) AS mac, SUBSTR(`Organization Name`, 0, 32) AS ouiname FROM 'oui-light' WHERE assignment!=''", OUIDBCallback, (void*)dataOUI, &zErrMsg);
      UI.PrintProgressBar( Out.width );
      if (rc != SQLITE_OK) {
        error(zErrMsg);
        sqlite3_free(zErrMsg);
        close(MAC_OUI_NAMES_DB);
        //return -2;
      }
      close(MAC_OUI_NAMES_DB);
      for(byte i=0;i<8;i++) {
        __attribute__((unused)) uint32_t rnd = random(0, OUIDBSize);
        log_i("Testing random mac #%06X: %s / %s", random(0, OUIDBSize), OuiPsramCache[rnd]->mac, OuiPsramCache[rnd]->assignment );
      }
    }

    // shit happens
    void error(const char* zErrMsg)
    {
      if( zErrMsg!= nullptr && zErrMsg != NULL ) {
        log_e( "SQL error: %s", zErrMsg );
      } else {
        log_e( "Unknown SQL error" );
        isCorrupt = true;
        return;
      }
      HourChangeTrigger = false;
      DayChangeTrigger = false;
      if (strcmp(zErrMsg, "database disk image is malformed")==0) {
        isCorrupt = true;
      } else if (strcmp(zErrMsg, "file is not a database")==0) {
        //needsReset = true;
      } else if (strcmp(zErrMsg, "no such table: blemacs")==0) {
        needsReset = true;
      } else if (strcmp(zErrMsg, "out of memory")==0) {
        isOOM = true;
      } else if(strcmp(zErrMsg, "disk I/O error")==0) {
        isCorrupt = true; // TODO: rename the DB file and create a new DB
      } else if(strstr("no such column", zErrMsg)) {
        needsReset = true;
      } else {
        UI.headerStats(zErrMsg);
        Out.println( zErrMsg );
        delay(1000);
      }
    }


    int DBExec(sqlite3 *db, const char *sql, char *_colNeedle = 0)
    {
      results = 0;
      //print_results = _print_results;
      colNeedle = _colNeedle;
      *colValue = {'\0'};
      int rc = sqlite3_exec(db, sql, DBCallback, (void*)data, &zErrMsg);
      if (rc != SQLITE_OK) {
        error(zErrMsg);
        sqlite3_free(zErrMsg);
      }
      return rc;
    }


    DBMessage insertBTDevice( BlueToothDevice *CacheItem)
    {
      if(isOOM) {
        // cowardly refusing to use DB when OOM
        return DB_IS_OOM;
      }
      if( CacheItem->appearance==0
       && isEmpty( CacheItem->name )
       && isEmpty( CacheItem->uuid )
       && isEmpty( CacheItem->ouiname )
       && isEmpty( CacheItem->manufname )
       ) {
        // cowardly refusing to insert empty result
        return INSERTION_IGNORED;
      }

      int rc = open(BLE_COLLECTOR_DB, false);
      if( rc ) {
        log_e("Can't open database");
        return INSERTION_FAILED;
      }

      String tmpName      = String( CacheItem->name );
      String tmpOuiname   = String ( CacheItem->ouiname );
      String tmpManufname = String( CacheItem->manufname );
      String tmpUuid      = String( CacheItem->uuid );

      // TODO: use prepared statements https://github.com/siara-cc/esp32_arduino_sqlite3_lib/blob/master/examples/sqlite3_insert_long_blob/sqlite3_insert_long_blob.ino
      tmpName.replace("'", "''");
      tmpOuiname.replace("'", "''");
      tmpManufname.replace("'", "''");
      tmpUuid.replace("'", "''");
/*
      clean( CacheItem->name );
      clean( CacheItem->ouiname );
      clean( CacheItem->manufname );
      clean( CacheItem->uuid );
*/
      sprintf(YYYYMMDD_HHMMSS_Str, YYYYMMDD_HHMMSS_Tpl,
        CacheItem->created_at.year(),
        CacheItem->created_at.month(),
        CacheItem->created_at.day(),
        CacheItem->created_at.hour(),
        CacheItem->created_at.minute(),
        CacheItem->created_at.second()
      );

      sprintf(insertQuery, insertQueryTemplate,
        CacheItem->appearance,
        tmpName.c_str(), // CacheItem->name, // SQL Injection or crash ? :-)
        CacheItem->address,
        tmpOuiname.c_str(), // CacheItem->ouiname,
        CacheItem->rssi,
        CacheItem->manufid,
        tmpManufname.c_str(), // CacheItem->manufname,
        tmpUuid.c_str(), // CacheItem->uuid,
        YYYYMMDD_HHMMSS_Str,
        YYYYMMDD_HHMMSS_Str,
        CacheItem->hits
      );
      log_d( "[INSERT QUERY] : %s", insertQuery );
      rc = DBExec( BLECollectorDB, insertQuery );
      if (rc != SQLITE_OK) {
        log_e("SQlite Error occured when heap level was at %d : %s", freeheap, insertQuery);
        log_e("Heap size: %d\n", ESP.getHeapSize());
        log_e("Free Heap: %d", esp_get_free_heap_size());
        log_e("Min Free Heap: %d", esp_get_minimum_free_heap_size());
        close(BLE_COLLECTOR_DB);
        CacheItem->in_db = false;
        return INSERTION_FAILED;
      }
      close(BLE_COLLECTOR_DB);
      CacheItem->in_db = true;
      return INSERTION_SUCCESS;
    }

    void deleteBLEDevice( const char* address )
    {
      char deleteItemStr[64];
      const char* deleteTpl = "DELETE FROM blemacs WHERE address='%s'";
      sprintf(deleteItemStr, deleteTpl, address );
      open(BLE_COLLECTOR_DB);
      DBExec( BLECollectorDB, deleteItemStr );
      close(BLE_COLLECTOR_DB);
    }

    void getVendor(uint16_t devid, char *dest)
    {
      if( hasPsram ) {
        getPsramVendor(devid, dest);
      } else {
        getHeapVendor(devid, dest);
      }
    }


    void getOUI(const char* mac, char* dest)
    {
      if( hasPsram ) {
        getPsramOUI(mac, dest);
      } else {
        getHeapOUI(mac, dest);
      }
    }


    unsigned int getEntries(bool _display_results = false)
    {
      open(BLE_COLLECTOR_DB);
      if (_display_results) {
        DBExec( BLECollectorDB, allEntriesQuery );
      } else {
        DBExec( BLECollectorDB, countEntriesQuery, (char*)"count(*)" );
        results = atoi(colValue);
      }
      close(BLE_COLLECTOR_DB);
      return results;
    }


    void resetDB()
    {
      Serial.println("Re-creating database :");
      Serial.println( BLEMacsDbFSPath );
      isQuerying = true;
      BLE_FS.remove( BLEMacsDbFSPath );
      isQuerying = false;
      ESP.restart();
    }


    void createDB()
    {
      log_w("Will create %s (POSIX path) database", BLEMacsDbSQLitePath);
      //UI.headerStats("DB: creating...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);

      if( strcmp( BLE_FS_TYPE, "littlefs" ) == 0 ) {
        // LittleFS Exception: create file first or sqlite3 won't see it
        if( ! BLE_FS.exists( BLEMacsDbFSPath ) ) {
          log_i("Creating empty file %s (FS path)", BLEMacsDbFSPath );
          fs::File tmp = BLE_FS.open( BLEMacsDbFSPath, FILE_WRITE );
          tmp.close();
        }
      }

      if( open(BLE_COLLECTOR_DB, false) ) {
        // duh !
        log_e("Could not open database");
        return;
      }
      if( DBExec( BLECollectorDB, createTableQuery ) == SQLITE_OK ) {
        log_i("created %s if no exists:  : %s", BLEMacsDbSQLitePath, createTableQuery);
      } else {
        log_e("CRITICAL: Failed to create db, halting system");
        close(BLE_COLLECTOR_DB);
        BLE_FS.remove( BLEMacsDbFSPath );
        while(1) vTaskDelay(1);
      }
      close(BLE_COLLECTOR_DB);
      //UI.headerStats(" ");
    }

    void dropDB()
    {
      UI.headerStats("Dropping DB");
      open(BLE_COLLECTOR_DB, false);
      log_d("dropped if exists: %s DB", BLEMacsDbSQLitePath);
      DBExec( BLECollectorDB, dropTableQuery );
      close(BLE_COLLECTOR_DB);
      UI.headerStats("DB Dropped");
    }

    void pruneDB()
    {
      UI.headerStats("Pruning DB");
      open(BLE_COLLECTOR_DB, false);
      DBExec(BLECollectorDB, pruneTableQuery );
      close(BLE_COLLECTOR_DB);
      entries = getEntries();
      prune_trigger = 0;
      UI.headerStats("DB Pruned");
      UI.footerStats();
    }

    bool testVendorNames()
    {
      open(BLE_VENDOR_NAMES_DB);
      DBExec( BLEVendorsDB, testVendorNamesQuery );
      close(BLE_VENDOR_NAMES_DB);
      char *vendorname = (char*)calloc(MAX_FIELD_LEN+1, sizeof(char));
      getVendor( 0x001D /*Qualcomm*/, vendorname );
      if (strcmp(vendorname, "Qualcomm")!=0) {
        tft.setTextColor(BLE_RED);
        Out.println(vendorname);
        Out.println("Vendor Names Test failed");
        return false;
      }
      return true;
    }

    bool testOUI()
    {
      open(MAC_OUI_NAMES_DB);
      DBExec( OUIVendorsDB, testOUIQuery );
      close(MAC_OUI_NAMES_DB);
      char *ouiname = (char*)calloc(MAX_FIELD_LEN+1, sizeof(char));
      getOUI( "B499BA" /*Hewlett Packard */, ouiname );
      if ( strcmp(ouiname, "Hewlett Packard")!=0 ) {
        tft.setTextColor(BLE_RED);
        Out.println(ouiname);
        Out.println("MAC OUI Test failed");
        return false;
      }
      return true;
    }

    void updateItemFromCache( BlueToothDevice* CacheItem )
    {
      // not really an update, more of a delete+reinsert
      deleteBLEDevice( CacheItem->address );
      if( insertBTDevice( CacheItem ) != INSERTION_SUCCESS ) {
        // whoops
        Serial.printf("[BUMMER] Failed to re-insert device %s\n", CacheItem->address);
        //UI.headerStats("Updated failed");
      } else {
        //UI.headerStats("Updated item");
      }
    }

    bool updateDBFromCache( BlueToothDevice** SourceCache, bool showBLECards = true, bool resetAfter = true )
    {
      UI.headerStats("DB replicating...");
      UI.PrintProgressBar( Out.width );
      for(uint16_t i=0; i<BLEDEVCACHE_SIZE ;i++) {
        //vTaskDelay(5);

        if( isEmpty( SourceCache[i]->address ) ) continue;
        if( SourceCache[i]->is_anonymous ) {
          if( resetAfter ) {
            BLEDevHelper.reset( SourceCache[i] );
          }
          continue;
        }
        BLEDevTmp = SourceCache[i];

        takeMuxSemaphore();
        updateItemFromCache( SourceCache[i] );
        giveMuxSemaphore();

        if( showBLECards ) {
          float percent = i*100 / BLEDEVCACHE_SIZE;
          UI.PrintProgressBar( (Out.width * percent) / 100 );
          UI.printBLECard( (BlueToothDeviceLink){.cacheIndex=i,.device=BLEDevTmp}/*BLEDevTmp*/ ); // render
        }

        //vTaskDelay(5);

        if( resetAfter ) {
          BLEDevHelper.reset( SourceCache[i] );
        }
      }
      cacheState();
      UI.cacheStats();
      UI.PrintProgressBar( Out.width );
      UI.headerStats(" ");
      return true;
    }


  private:

    static void VendorHeapCacheSet(uint16_t cacheindex, int devid, const char* manufname)
    {
      VendorHeapCache[cacheindex].devid = devid;
      memset( VendorHeapCache[cacheindex].vendor, '\0', MAX_FIELD_LEN+1);
      memcpy( VendorHeapCache[cacheindex].vendor, manufname, strlen(manufname) );
      log_d("[+] VendorHeapCacheSet: %s", manufname );
    }
    static uint16_t getNextVendorCacheIndex()
    {
      VendorCacheIndex++;
      VendorCacheIndex = VendorCacheIndex % VENDORCACHE_SIZE;
      return VendorCacheIndex;
    }

    // checks for existence in heap cache
    int vendorHeapExists(uint16_t devid)
    {
      // try fast answer first
      for(int i=0;i<VENDORCACHE_SIZE;i++) {
        if( VendorHeapCache[i].devid == devid) {
          VendorCacheHit++;
          return i;
        }
      }
      return -1;
    }

    // vendor Heap/DB lookup
    void getHeapVendor(uint16_t devid, char *dest)
    {
      int vendorCacheIdIfExists = vendorHeapExists( devid );
      if(vendorCacheIdIfExists>-1) {
        uint16_t vendorCacheLen = strlen( VendorHeapCache[vendorCacheIdIfExists].vendor );
        memcpy( dest, VendorHeapCache[vendorCacheIdIfExists].vendor, vendorCacheLen );
        dest[vendorCacheLen] = '\0';
        return;
      } else {
        *dest = {'\0'};
      }
      uint16_t vendorcacheindex = getNextVendorCacheIndex();
      open(BLE_VENDOR_NAMES_DB);
      char vendorRequestStr[64] = {'\0'};
      sprintf(vendorRequestStr, vendorRequestTpl, devid);
      DBExec( BLEVendorsDB, vendorRequestStr, (char*)"vendor" );
      close(BLE_VENDOR_NAMES_DB);
      uint16_t colValueLen = 10; // sizeof("[unknown]")
      if ( !isEmpty(colValue) ) {
        colValueLen = strlen( colValue );
        if(colValueLen >= MAX_FIELD_LEN) {
          colValue[MAX_FIELD_LEN-1] = '\0';
          colValueLen = MAX_FIELD_LEN;
        } else {
          colValue[colValueLen] = '\0';
          colValueLen++;
        }
        String vName = String(colValue);
        vName.replace("'", ""); // escape quotes
        VendorHeapCacheSet( vendorcacheindex, devid, vName.c_str() );
      } else {
        VendorHeapCacheSet(vendorcacheindex, devid, "[unknown]");
      }
      memcpy( dest, VendorHeapCache[vendorcacheindex].vendor, colValueLen );
      delay(1);
    }

    // checks for existence in psram cache
    int vendorPsramExists(uint16_t devid)
    {
      // try fast answer first
      for(int i=0;i<VendorDBSize;i++) {
        log_v("comparing %d / %s with %d", VendorPsramCache[i]->devid, VendorPsramCache[i]->vendor, devid );
        if( VendorPsramCache[i]->devid[0] == devid) {
          VendorCacheHit++;
          return i;
        }
      }
      return -1;
    }

    // vendor PSRam lookup
    void getPsramVendor(uint16_t devid, char *dest)
    {
      *dest = {'\0'};
      int VendorCacheIdIfExists = vendorPsramExists( devid );
      if(VendorCacheIdIfExists>-1) {
        byte VendorCacheLen = strlen( VendorPsramCache[VendorCacheIdIfExists]->vendor );
        memcpy( dest, VendorPsramCache[VendorCacheIdIfExists]->vendor, VendorCacheLen );
        VendorPsramCache[VendorCacheIdIfExists]->hits++;
        dest[VendorCacheLen] = '\0';
        return;
      }
      memcpy( dest, "[unknown]", 10 ); // sizeof("[unknown]")
    }

    static void OUIHeapCacheSet(uint16_t cacheindex, const char* shortmac, const char* assignment)
    {
      memset( OuiHeapCache[cacheindex].mac, '\0', SHORT_MAC_LEN+1);
      memcpy( OuiHeapCache[cacheindex].mac, shortmac, strlen(shortmac) );
      memset( OuiHeapCache[cacheindex].assignment, '\0', MAX_FIELD_LEN+1);
      memcpy( OuiHeapCache[cacheindex].assignment, assignment, strlen(assignment) );
      log_d("[+] OUICacheSet: %s", assignment );
    }
    static uint16_t getNextOUICacheIndex() {
      OuiCacheIndex++;
      OuiCacheIndex = OuiCacheIndex % OUICACHE_SIZE;
      return OuiCacheIndex;
    }

    // checks for existence in heap cache
    int OUIHeapExists(const char* shortmac)
    {
      // try fast answer first
      for(int i=0;i<OUICACHE_SIZE;i++) {
        if( strcmp(OuiHeapCache[i].mac, shortmac)==0 ) {
          OuiCacheHit++;
          return i;
        }
      }
      return -1;
    }

    // OUI heap/DB lookup
    void getHeapOUI(const char* mac, char *dest)
    {
      *dest = {'\0'};
      char shortmac[7] = {'\0'};
      byte bytepos =  0;
      for(byte i=0;i<9;i++) {
        if(mac[i]!=':') {
          shortmac[bytepos] = mac[i];
          bytepos++;
        }
      }

      int OUICacheIdIfExists = OUIHeapExists( shortmac );
      if(OUICacheIdIfExists>-1) {
        byte OUICacheLen = strlen( OuiHeapCache[OUICacheIdIfExists].assignment );
        memcpy( dest, OuiHeapCache[OUICacheIdIfExists].assignment, OUICacheLen );
        dest[OUICacheLen] = '\0';
        return;
      }
      uint16_t assignmentcacheindex = getNextOUICacheIndex();
      open(MAC_OUI_NAMES_DB);
      char OUIRequestStr[76];
      sprintf( OUIRequestStr, OUIRequestTpl, shortmac);
      DBExec( OUIVendorsDB, OUIRequestStr, (char*)"Organization Name" );
      close(MAC_OUI_NAMES_DB);
      uint16_t colValueLen = 10; // sizeof("[private]")
      if ( !isEmpty( colValue ) ) {
        colValueLen = strlen( colValue );
        if(colValueLen >= MAX_FIELD_LEN) {
          colValue[MAX_FIELD_LEN-1] = '\0';
          colValueLen = MAX_FIELD_LEN;
        } else {
          colValue[colValueLen] = '\0';
          colValueLen++;
        }
        String oName = String(colValue);
        oName.replace("'", ""); // escape quotes
        OUIHeapCacheSet( assignmentcacheindex, shortmac, oName.c_str() );
      } else {
        OUIHeapCacheSet( assignmentcacheindex, shortmac, "[private]" );
      }
      memcpy( dest, OuiHeapCache[assignmentcacheindex].assignment, colValueLen );
      delay(1);
    }

    // checks for existence in PSram cache
    int OUIPsramExists(const char* shortmac)
    {
      // try fast answer first
      for(int i=0; i<OUIDBSize; i++) {
        log_v("comparing %s / %s with %s", OuiPsramCache[i]->mac, OuiPsramCache[i]->assignment, shortmac );
        if( strstr(OuiPsramCache[i]->mac, shortmac) /*==0*/ ) {
          OuiCacheHit++;
          return i;
        }
      }
      return -1;
    }

    // OUI psram lookup
    void getPsramOUI(const char* mac, char *dest)
    {
      *dest = {'\0'};
      char shortmac[7] = {'\0'};
      byte bytepos =  0;
      for(byte i=0;i<9;i++) {
        if(mac[i]!=':') {
          shortmac[bytepos] = tolower(mac[i]);
          bytepos++;
        }
      }

      int OUICacheIdIfExists = OUIPsramExists( shortmac );
      if(OUICacheIdIfExists>-1) {
        byte OUICacheLen = strlen( OuiPsramCache[OUICacheIdIfExists]->assignment );
        memcpy( dest, OuiPsramCache[OUICacheIdIfExists]->assignment, OUICacheLen );
        OuiPsramCache[OUICacheIdIfExists]->hits++;
        dest[OUICacheLen] = '\0';
        return;
      }
      memcpy( dest, "[private]", 10 ); // sizeof("[private]")
    }

    // loads a DB entry into a BLEDevice struct
    static int BLEDevDBCacheCallback( void *dataBLE, int argc, char **argv, char **azColName)
    {
      results++;
      if(results < 2) {
        //BLEDevCacheIndex = BLEDevHelper.getNextCacheIndex(BLEDevRAMCache, BLEDevCacheIndex);
        BLEDevHelper.reset( BLEDevDBCache ); // avoid mixing new and old data
        for (int i = 0; i < argc; i++) {
          BLEDevHelper.set( BLEDevDBCache, azColName[i], ( argv[i] ? argv[i] : "") );
        }
        BLEDevHelper.set( BLEDevDBCache, "in_db", true );
        BLEDevHelper.set( BLEDevDBCache, "is_anonymous", false );
      } else {
        log_e("Device Pool Size Exceeded, ignoring: ");
        for (int i = 0; i < argc; i++) {
          log_e("    %s = %s", azColName[i], argv[i] ? argv[i] : "NULL");
        }
      }
      return 0;
    }

    // loads a DB entry into a VendorPsramCache struct
    static int VendorDBCallback(void *dataVendor, int argc, char **argv, char **azColName)
    {
      results++;
      if( results > VendorDBSize ) {
        log_w("Too many vendors (max=%d, resultid=%d), ignoring callback", VendorDBSize, results);
        float percent = 100;
        UI.PrintProgressBar( (Out.width * percent) / 100 );
        return 0;
      }
      for (int i = 0; i < argc; i++) {
        if( strcmp( azColName[i], "id" ) == 0 ) {
          log_v("[%d] Attempting to copy result # %d %s, %d", freepsheap, results, argv[i], atoi( argv[i] ) );
          VendorPsramCache[results-1]->devid[0] = atoi( argv[i] );
          //copy( VendorPsramCache[results-1]->devid, atoi( argv[i] ) );
        }
        if( strcmp( azColName[i], "vendor" ) == 0 ) {
          log_v("[%d] Attempting to copy result # %d %s", freepsheap, results, argv[i] );
          copy( VendorPsramCache[results-1]->vendor, argv[i], MAX_FIELD_LEN+1);
        }
      }
      VendorPsramCache[results-1]->hits = 0;
      if(results%100==0) {
        float percent = results*100 / VendorDBSize;
        //UI.PrintProgressBar( (Out.width * percent) / 100 );
      }
      return 0;
    }

    // loads a DB entry into a OuiPsramCache struct
    static int OUIDBCallback(void *dataOUI, int argc, char **argv, char **azColName)
    {
      results++;
      if( results > OUIDBSize ) {
        log_w("Too many OUI's (max=%d, resultid=%d), ignoring callback", OUIDBSize, results);
        float percent = 100;
        UI.PrintProgressBar( (Out.width * percent) / 100 );
        return 0;
      }
      for (int i = 0; i < argc; i++) {
        if( strcmp( azColName[i], "mac" ) == 0 ) {
          copy( OuiPsramCache[results-1]->mac, argv[i], SHORT_MAC_LEN+1 );
        }
        if( strcmp( azColName[i], "ouiname" ) == 0 ) {
          copy( OuiPsramCache[results-1]->assignment, argv[i], MAX_FIELD_LEN+1);
        }
      }
      OuiPsramCache[results-1]->hits = 0;
      if(results%100==0) {
        float percent = results*100 / OUIDBSize;
        //UI.PrintProgressBar( (Out.width * percent) / 100 );
        log_v("[Copied %d as %s / %s]", results, OuiPsramCache[results-1]->mac, OuiPsramCache[results-1]->assignment );
      }
      return 0;
    }

    // counts results from a DB query
    static int DBCallback(void *data, int argc, char **argv, char **azColName)
    {
      //log_e("got one result");
      results++;
      int i;
      for (i = 0; i < argc; i++) {
        if (colNeedle != 0) {
          if ( strcmp(colNeedle, azColName[i])==0 ) {
            memset( colValue, 0, MAX_FIELD_LEN );
            if( argv[i] ) {
              memcpy( colValue, argv[i], strlen(argv[i]) );
              log_i("got one result :%s", colValue);
            }
            //colValue = argv[i] ? argv[i] : "";
          }
          continue;
        }
      }
      return 0;
    }

};


DBUtils DB;
