extern "C" {
  #include "user_interface.h"
  #include "thinx.h"
  #include <cont.h>
  extern cont_t g_cont;
}

#include "THiNXLib.h"

#ifndef UNIT_TEST // IMPORTANT LINE FOR UNIT-TESTING!

#ifndef THINX_FIRMWARE_VERSION_SHORT
#define THINX_FIRMWARE_VERSION_SHORT VERSION
#endif

#ifndef THINX_COMMIT_ID
#define THINX_COMMIT_ID "c2d13320cca4df4149bc94e7c19692299eca3fcf"
#endif

/* Debug tools (should be extradited from Release builds) */
register uint32_t *sp asm("a1");

void printStackHeap(String tag) {
  uint32_t memfree = system_get_free_heap_size(); Serial.print(F("*TH: memfree = ")); Serial.println(memfree);
  Serial.printf("*TH: loop(): unmodified stack   = %4d\n", cont_get_free_stack(&g_cont));
  Serial.printf("*TH: loop(): current free stack = %4d\n", 4 * (sp - g_cont.stack));
  Serial.print("*TH: loop(): heap = "); Serial.println(system_get_free_heap_size());
  Serial.print("*TH: loop(): tag = "); Serial.println(tag);
}

char* THiNX::thinx_api_key;
char* THiNX::thinx_owner_key;

#ifdef __USE_WIFI_MANAGER__
char THiNX::thx_api_key[65] = {0};
char THiNX::thx_owner_key[65] = {0};
int THiNX::should_save_config = 0;
WiFiManagerParameter * THiNX::api_key_param;
WiFiManagerParameter * THiNX::owner_param;

void THiNX::saveConfigCallback() {

#ifdef __DEBUG__
  printStackHeap("safeConfigCallback");
#endif

  Serial.println(F("* TH: WiFiManager's saveConfigCallback called. Counfiguration should be saved now!"));
  should_save_config = true;
  strcpy(thx_api_key, api_key_param->getValue());
  strcpy(thx_owner_key, owner_param->getValue());
}
#endif

double THiNX::latitude = 0.0;
double THiNX::longitude = 0.0;
String THiNX::statusString = "Registered";

/* Constructor */

THiNX::THiNX() {

}

/* Designated Initializers */

THiNX::THiNX(const char * __apikey) {

  THiNX(__apikey, "");
}

THiNX::THiNX(const char * __apikey, const char * __owner_id) {

  thinx_phase = INIT;

  #ifdef __USE_WIFI_MANAGER__
  should_save_config = false;
  WiFiManager wifiManager;
  api_key_param = new WiFiManagerParameter("apikey", "API Key", thinx_api_key, 64);
  wifiManager.addParameter(api_key_param);
  owner_param = new WiFiManagerParameter("owner", "Owner ID", thinx_owner_key, 64);
  wifiManager.addParameter(owner_param);
  wifiManager.setTimeout(5000);
  wifiManager.setDebugOutput(true); // does some logging on mode set
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.autoConnect("THiNX-AP");
  #endif

  Serial.print(F("\n*TH: THiNXLib rev. "));
  Serial.print(thx_revision);
  Serial.print(" (");

  if (strlen(thx_commit_id) > 8) { // unknown
    thinx_commit_id = strdup(thx_commit_id);
  } else {
    thinx_commit_id = strdup(THINX_COMMIT_ID);
  }

  Serial.print(thinx_commit_id); // returned string is "not declared in expansion of THX_CID, why?
  Serial.println(")");

  // see lines ../hardware/cores/esp8266/Esp.cpp:80..100
  wdt_disable(); // causes wdt reset after 8 seconds!
  wdt_enable(60000); // must be called from wdt_disable() state!

  if (once != true) {
    once = true;
  }

  status = WL_IDLE_STATUS;
  connected = false;
  mqtt_client = NULL;
  mqtt_payload = "";
  mqtt_result = false;
  mqtt_connected = false;
  performed_mqtt_checkin = false;
  wifi_connection_in_progress = false;
  wifi_retry = 0;

  app_version = strdup("");
  available_update_url = strdup("");
  thinx_cloud_url = strdup("thinx.cloud");

  thinx_firmware_version_short = strdup("");
  thinx_firmware_version = strdup("");
  thinx_mqtt_url = strdup("thinx.cloud");
  thinx_version_id = strdup("");
  thinx_api_key = strdup("");
  thinx_forced_update = false;

  // will be loaded from SPIFFS/EEPROM or retrieved on Registration later
  if (strlen(__owner_id) == 0) {
    thinx_owner = strdup("");
  }

  EEPROM.begin(512); // should be SPI_FLASH_SEC_SIZE

  import_build_time_constants();
  restore_device_info();

  #ifdef __USE_WIFI_MANAGER__
  connected = true;
  #else
  if ((WiFi.status() == WL_CONNECTED) && (WiFi.getMode() == WIFI_STA)) {
    connected = true;
    wifi_connection_in_progress = false;
  } else {
    WiFi.mode(WIFI_STA);
  }
  #endif

  if (strlen(thinx_api_key) > 4) {
    Serial.print(F("*TH: Init with stored API Key: "));
  } else {
    if (strlen(__apikey) > 4) {
      Serial.print(F("*TH: With custom API Key: "));
      thinx_api_key = strdup(__apikey);
      Serial.println(thinx_api_key);
    }
  }
  initWithAPIKey(thinx_api_key);

  wifi_connection_in_progress = false; // last
}

// Designated initializer
void THiNX::initWithAPIKey(const char * __apikey) {

  #ifdef __USE_SPIFFS__
  Serial.println(F("*TH: Checking FS..."));
  if (!fsck()) {
    Serial.println(F("*TH: Filesystem check failed, disabling THiNX."));
    return;
  }
  #endif

  if (strlen(thinx_api_key) < 4) {
    if (strlen(__apikey) > 1) {
      thinx_api_key = strdup(__apikey);
    }
  }
  Serial.println(F("*TH: Initialization completed..."));
  wifi_connection_in_progress = false;
  thinx_phase = CONNECT_WIFI;
}

/*
* Connection management
*/

void THiNX::connect() {

  if (connected) {
    Serial.println(F("*TH: connected"));
    return;
  }

  Serial.print(F("*TH: connecting: ")); Serial.println(wifi_retry);

  #ifndef __USE_WIFI_MANAGER__
  if (WiFi.SSID()) {

    if (wifi_connection_in_progress != true) {
      Serial.print(F("*TH: SSID ")); Serial.println(WiFi.SSID());
      if (WiFi.getMode() == WIFI_AP) {
        Serial.print(F("THiNX > LOOP > START() > AP SSID"));
        Serial.println(WiFi.SSID());
      } else {
        if (strlen(THINX_ENV_SSID) > 2) {
          Serial.println(F("*TH: LOOP > CONNECT > STA RECONNECT"));
          WiFi.begin(THINX_ENV_SSID, THINX_ENV_PASS);
          Serial.println(F("*TH: Enabling connection state (197)"));
        } else {
          Serial.println(F("*TH: LOOP > CONNECT > NO CREDS"));
          wifi_connection_in_progress = true;
          Serial.println(F("*TH: WARNING: Dead branch (201)"));
        }
        wifi_connection_in_progress = true; // prevents re-entering connect_wifi(); should timeout
      }
      //
    }
  } else {
    Serial.print(F("*TH: No SSID."));
  }
  #endif

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("THiNX > LOOP > ALREADY CONNECTED"));
    connected = true; // prevents re-entering start() [this method]
    wifi_connection_in_progress = false;
  } else {
    Serial.println(F("THiNX > LOOP > CONNECTING WiFi:"));
    connect_wifi();
    Serial.println(F("*TH: Enabling connection state (237)"));
    wifi_connection_in_progress = true;
  }
}

/*
* Connection to WiFi, called from connect() [if SSID & connected]
*/

void THiNX::connect_wifi() {

  #ifdef __USE_WIFI_MANAGER__
  return;
  #else

#ifdef __DEBUG__
  printStackHeap("connect_wifi");
#endif

  if (connected) {
    return;
  }

  if (wifi_connection_in_progress) {
    if (wifi_retry > 1000) {
      if (WiFi.getMode() == WIFI_STA) {
        Serial.println(F("*TH: Starting THiNX-AP without PASSWORD..."));
        WiFi.mode(WIFI_AP);
        WiFi.softAP("THiNX-AP", "PASSWORD"); // setup the AP on channel 1, not hidden, and allow 8 clients
        wifi_retry = 0;
        wifi_connection_in_progress = false;
        connected = true;
        return;
      } else {
        if (strlen(THINX_ENV_SSID) > 2) {
          Serial.println(F("*TH: Connecting to AP with pre-defined credentials...")); Serial.flush();
          WiFi.mode(WIFI_STA);
          WiFi.begin(THINX_ENV_SSID, THINX_ENV_PASS);
          Serial.println(F("*TH: Enabling connection state (283)"));
          wifi_connection_in_progress = true; // prevents re-entering connect_wifi()
          wifi_retry = 0; // waiting for sta...
        } else {
          Serial.println(F("*TH: WARNING! Dead code branch (261)")); Serial.flush();
        }
      }

    } else {
      Serial.print("*TH: WiFi retry"); Serial.println(wifi_retry); Serial.flush();
      wifi_retry++;
    }

  } else {

    if (strlen(THINX_ENV_SSID) > 2) {
      if (wifi_retry == 0) {
        Serial.println(F("*TH: Connecting to AP with pre-defined credentials..."));
        // 1st run
        if (WiFi.getMode() != WIFI_STA) {
          WiFi.mode(WIFI_STA);
        } else {
          WiFi.begin(THINX_ENV_SSID, THINX_ENV_PASS);
          Serial.println(F("*TH: Enabling connection state (272)"));
          wifi_connection_in_progress = true; // prevents re-entering connect_wifi()
        }
      }
    }
  }
  #endif
}

/*
* Registration
*/

void THiNX::checkin() {
  Serial.println(F("\n*TH: Contacting API..."));
  if(!connected) {
    Serial.println(F("*TH: Cannot checkin while not connected, exiting."));
  } else {
    senddata(checkin_body());
  }
}

/*
* Registration - JSON body constructor
*/

String THiNX::checkin_body() {

  DynamicJsonBuffer jsonBuffer(768);
  JsonObject& root = jsonBuffer.createObject();

  root["mac"] = thinx_mac();

  if (strlen(THINX_FIRMWARE_VERSION) > 1) {
    root["firmware"] = strdup(THINX_FIRMWARE_VERSION);
  }

  if (strlen(thinx_firmware_version_short) > 1) {
    root["version"] = thinx_firmware_version_short;
  }

  if (strlen(thx_commit_id) > 1) {
    root["commit"] = thx_commit_id;
  }

  if (strlen(thinx_owner) > 1) {
    root["owner"] = thinx_owner;
  }

  if (strlen(thinx_alias) > 1) {
    root["alias"] = thinx_alias;
  }

  if (strlen(thinx_udid) > 4) {
    root["udid"] = thinx_udid;
  }

  if (statusString.length() > 0) {
    root["status"] = statusString.c_str();
  }

  // Optional location data
  root["lat"] = String(latitude);
  root["lon"] = String(longitude);

  // Flag for THiNX CI
  #ifndef PLATFORMIO_IDE
  // THINX_PLATFORM is not overwritten by builder in Arduino IDE
  root["platform"] = "arduino";
  #else
  root["platform"] = strdup(THINX_PLATFORM);
  #endif

  DynamicJsonBuffer wrapperBuffer(1024);
  JsonObject& wrapper = wrapperBuffer.createObject();
  wrapper["registration"] = root;

#ifdef __DEBUG_JSON__
  wrapper.printTo(Serial);
  Serial.println();
#endif

  json_output = "";
  wrapper.printTo(json_output);
  return json_output;
}


/*
* Registration - HTTPS POST
*/

void THiNX::senddata(String body) {

  // Serial.print("Sending data over HTTP to: "); Serial.println(thinx_cloud_url);

  if (thx_wifi_client.connect(thinx_cloud_url, 7442)) {

    thx_wifi_client.println(F("POST /device/register HTTP/1.1"));
    thx_wifi_client.print(F("Host: ")); thx_wifi_client.println(thinx_cloud_url);
    thx_wifi_client.print(F("Authentication: ")); thx_wifi_client.println(thinx_api_key);
    thx_wifi_client.println(F("Accept: application/json")); // application/json
    thx_wifi_client.println(F("Origin: device"));
    thx_wifi_client.println(F("Content-Type: application/json"));
    thx_wifi_client.println(F("User-Agent: THiNX-Client"));
    thx_wifi_client.print(F("Content-Length: "));
    thx_wifi_client.println(body.length());
    thx_wifi_client.println();
    thx_wifi_client.println(body);

    long interval = 30000;
    unsigned long currentMillis = millis(), previousMillis = millis();

    Serial.println(F("*TH: Waiting for HTTP response..."));

    // Wait until client available or timeout...
    while(!thx_wifi_client.available()){
      delay(1);
      if( (currentMillis - previousMillis) > interval ){
        thx_wifi_client.stop();
        return;
      }
      currentMillis = millis();
    }

    // Read while connected
    String payload;
    while ( thx_wifi_client.connected() ) {
      delay(1);
      if ( thx_wifi_client.available() ) {
        char str = thx_wifi_client.read();
        payload = payload + String(str);
      }
    }

    parse(payload);

  } else {
    Serial.println(F("*TH: API connection failed."));
    return;
  }
}

/*
* Response Parser
*/

void THiNX::parse(String payload) {

  // TODO: Should parse response only for this device_id (which must be internal and not a mac)
  // printStackHeap("json-parser");

  payload_type ptype = Unknown;

  int start_index = 0;
  int endIndex = payload.length();

  int reg_index = payload.indexOf("{\"registration\"");
  int upd_index = payload.indexOf("{\"FIRMWARE_UPDATE\"");
  int not_index = payload.indexOf("{\"notification\"");
  int cfg_index = payload.indexOf("{\"configuration\"");
  int undefined_owner = payload.indexOf("old_protocol_owner:-undefined-");

  if (upd_index > start_index) {
    start_index = upd_index;
    ptype = UPDATE;
  }

  if (reg_index > start_index) {
    start_index = reg_index;
    endIndex = payload.indexOf("}}") + 2;
    ptype = REGISTRATION;
  }

  if (not_index > start_index) {
    start_index = not_index;
    endIndex = payload.indexOf("}}") + 2; // is this still needed?
    ptype = NOTIFICATION;
  }

  if (cfg_index > start_index) {
    start_index = cfg_index;
    endIndex = payload.indexOf("}}") + 2; // is this still needed?
    ptype = CONFIGURATION;
  }

  if (undefined_owner > start_index) {
    Serial.println(F("ERROR: Not authorized. Please copy your owner_id into thinx.h from RTM Console > User Profile."));
    return;
  }

  String body = payload.substring(start_index, endIndex);

#ifdef __DEBUG__
  printStackHeap("parse");
  Serial.print(F("*TH: Parsing response:\n'"));
  Serial.print(body);
  Serial.println("'");
#endif

  DynamicJsonBuffer jsonBuffer(1024);
  JsonObject& root = jsonBuffer.parseObject(body.c_str());

  if ( !root.success() ) {
    Serial.println(F("Failed parsing root node."));
    return;
  }

  switch (ptype) {

    case UPDATE: {

      JsonObject& update = root["registration"];
      Serial.println(F("TODO: Parse update payload..."));

      String mac = update["mac"];
      String this_mac = String(thinx_mac());
      Serial.println(String("mac: ") + mac);

      if (!mac.equals(this_mac)) {
        Serial.println(F("*TH: Warning: firmware is dedicated to device with different MAC."));
      }

      String udid = root["udid"];
      if ( udid.length() > 4 ) {
        thinx_udid = strdup(udid.c_str());
      }

      // Check current firmware based on commit id and store Updated state...
      String commit = update["commit"];
      Serial.println(String("commit: ") + commit);

      // Check current firmware based on version and store Updated state...
      String version = update["version"];
      Serial.println(String("version: ") + version);

      //if ((commit == thinx_commit_id) && (version == thinx_version_id)) { WHY?
      if (strlen(available_update_url) > 5) {
        Serial.println(F("*TH: firmware has same thx_commit_id as current and update availability is stored. Firmware has been installed."));
        available_update_url = strdup("");
        notify_on_successful_update();
        return;
      } else {
        Serial.println(F("*TH: Info: firmware has same thx_commit_id as current and no update is available."));
      }
      //}

      save_device_info();

      // In case automatic updates are disabled,
      // we must ask user to commence firmware update.
      if (thinx_auto_update == false) {
        if (mqtt_client != NULL) {
          Serial.println(F("* TH: Update availability notification..."));
          mqtt_client->publish(
            thinx_mqtt_channel().c_str(),
            F("{ title: \"Update Available\", body: \"There is an update available for this device. Do you want to install it now?\", type: \"actionable\", response_type: \"bool\" }")
          );
          mqtt_client->loop();
        }

      } else if (thinx_auto_update || thinx_forced_update){

        Serial.println(F("*TH: Starting update A..."));


        // FROM LUA: update variants
        // local files = payload['files']
        // local ott   = payload['ott']
        // local url   = payload['url']
        // local type  = payload['type']

        String type = update["type"];
        Serial.print(F("*TH: Payload type: ")); Serial.println(type);

        String files = update["files"];

        String url = update["url"]; // may be OTT URL
        available_update_url = url.c_str();

        String ott = update["ott"];
        available_update_url = ott.c_str();

        save_device_info();

        if (url) {
          mqtt_client->publish(
            mqtt_device_status_channel,
            F("{ \"status\" : \"update_started\" }")
          );

          mqtt_client->loop();
          Serial.print(F("*TH: Force update URL must not contain HTTP!!!: "));
          Serial.println(url);
          url.replace("http://", "");
          // TODO: must not contain HTTP, extend with http://thinx.cloud/"
          update_and_reboot(url);
        }
        return;
      }

    } break;

    case NOTIFICATION: {

      // Currently, this is used for update only, can be extended with request_category or similar.
      JsonObject& notification = root["notification"];

      if ( !notification.success() ) {
        Serial.println(F("*TH: Failed parsing notification node."));
        return;
      }

      String type = notification["response_type"];
      if ((type == "bool") || (type == "boolean")) {
        bool response = notification["response"];
        if (response == true) {
          Serial.println(F("*TH: User allowed update using boolean."));
          if (strlen(available_update_url) > 4) {
            update_and_reboot(available_update_url);
          }
        } else {
          Serial.println(F("*TH: User denied update using boolean."));
        }
      }

      if ((type == "string") || (type == "String")) {
        String response = notification["response"];
        if (response == "yes") {
          Serial.println(F("*TH: User allowed update using string."));
          if (strlen(available_update_url) > 4) {
            update_and_reboot(available_update_url);
          }
        } else if (response == "no") {
          Serial.println(F("*TH: User denied update using string."));
        }
      }

    } break;

    case REGISTRATION: {

      JsonObject& registration = root["registration"];

      if ( !registration.success() ) {
        Serial.println(F("*TH: Failed parsing registration node."));
        return;
      }

      bool success = registration["success"];
      String status = registration["status"];

      if (status == "OK") {

        String alias = registration["alias"];
        if ( alias.length() > 1 ) {
          thinx_alias = strdup(alias.c_str());
        }

        String owner = registration["owner"];
        if ( owner.length() > 1 ) {
          thinx_owner = strdup(owner.c_str());
        }

        String udid = registration["udid"];
        if ( udid.length() > 4 ) {
          thinx_udid = strdup(udid.c_str());
        }

        if (registration.containsKey(F("auto_update"))) {
          thinx_auto_update = (bool)registration[F("auto_update")];
        }

        if (registration.containsKey(F("forced_update"))) {
          thinx_forced_update = (bool)registration[F("forced_update")];
        }

        save_device_info();

      } else if (status == "FIRMWARE_UPDATE") {

        String mac = registration["mac"];
        Serial.println(String("*TH: Update for MAC: ") + mac);
        // TODO: must be current or 'ANY'

        String commit = registration["commit"];
        // Serial.println(String("commit: ") + commit);

        // should not be same except for forced update
        if (commit == thinx_commit_id) {
          Serial.println(F("*TH: Warning: new firmware has same thx_commit_id as current."));
        }

        String version = registration["version"];
        Serial.println(String(F("*TH: version: ")) + version);

        Serial.println(F("*TH: Starting direct update..."));

        String url = registration["url"];
        if (url.length() > 2) {
          Serial.println(url);
          update_and_reboot(url);
        }

        String ott = registration["ott"];
        if (ott.length() > 2) {
          String ott_url = "http://thinx.cloud:7442/device/firmware?ott="+ott;
          update_and_reboot(ott_url);
        }

      }

    } break;

    case CONFIGURATION: {

      JsonObject& configuration = root["configuration"];

      if ( !configuration.success() ) {
        Serial.println(F("*TH: Failed parsing configuration node."));
        return;
      }


      #ifdef __ENABLE_WIFI_MIGRATION__
      //
      // Built-in support for WiFi migration
      //

      const char *ssid = configuration["THINX_ENV_SSID"];
      const char *pass = configuration["THINX_ENV_PASS"];

      // password may be empty string
      if ((strlen(ssid) > 2) && (strlen(pass) > 0)) {
        WiFi.disconnect();
        WiFi.begin(ssid, pass);
        long timeout = millis() + 20000;
        Serial.println(F("*TH: Attempting WiFi migration..."));
        while (WiFi.status() != WL_CONNECTED) {
          yield();
          if (millis() > timeout) break;
        }
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println(F("*TH: WiFi migration failed."));
        } else {
          Serial.println(F("*TH: WiFi migration successful.")); // TODO: Notify using publish() to device status channel
        }
      }
      #endif
      // Forward update body to the library user
      if (_config_callback != NULL) {
        _config_callback(body);
      }

    } break;

    default:
    break;
  }

}

/*
* MQTT channel names
*/

// TODO: Should be called only on init and update (and store result for later)
String THiNX::thinx_mqtt_channel() {
  sprintf(mqtt_device_channel, "/%s/%s", thinx_owner, thinx_udid);
  return String(mqtt_device_channel);
}

// TODO: Should be called only on init and update (and store result for later)
String THiNX::thinx_mqtt_status_channel() {
  sprintf(mqtt_device_status_channel, "/%s/%s/status", thinx_owner, thinx_udid);
  return String(mqtt_device_status_channel);
}

/*
* Device MAC address
*/

// TODO: FIXME: Return real mac address through WiFi? Might solve compatibility issues.
const char * THiNX::thinx_mac() {
  //byte mac[] = {
  //   0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  //};
  //WiFi.macAddress(mac);
  sprintf(mac_string, "5CCF7F%6X", ESP.getChipId()); // ESP8266 only!
  /*
  #ifdef __ESP32__
  sprintf(mac_string, "5CCF7C%6X", ESP.getChipId()); // ESP8266 only!
  #endif
  #ifdef __ESP8266__
  sprintf(mac_string, "5CCF7F%6X", ESP.getChipId()); // ESP8266 only!
  #endif
  */
  return mac_string;
}

/*
* Sends a MQTT message on successful update (should be used after boot).
*/

void THiNX::notify_on_successful_update() {
  if (mqtt_client != NULL) {
    Serial.println(F("*TH: notify_on_successful_update()"));
    mqtt_client->publish(
      mqtt_device_status_channel,
      F("{ title: \"Update Successful\", body: \"The device has been successfully updated.\", type: \"success\" }")
    );
    mqtt_client->loop();
  } else {
    Serial.println(F("*TH: Device updated but MQTT not active to notify. TODO: Store."));
  }
}

/*
* Sends a MQTT message to Device's Status channel (/owner/udid/status)
*/

void THiNX::publishStatus(String message) {
  if (mqtt_client != NULL) {
    mqtt_client->publish(
      MQTT::Publish(mqtt_device_status_channel, message.c_str()).set_retain()
    );
    mqtt_client->loop();
  } else {
    Serial.println(F("*TH: MQTT not active."));
  }
}

/*
* Sends a MQTT message to the Device Channel (/owner/udid)
*/

void THiNX::publish(String message, String topic, bool retain)  {
  if (mqtt_client != NULL) {

    if (retain == true) {
      mqtt_client->publish(
        MQTT::Publish(mqtt_device_channel, message.c_str()).set_retain()
      );
    } else {
      mqtt_client->publish(
        mqtt_device_channel, message.c_str()
      );
    }
    mqtt_client->loop();
  } else {
    Serial.println(F("MQTT not active."));
  }
}

/*
* Starts the MQTT client and attach callback function forwarding payload to parser.
*/

bool THiNX::start_mqtt() {

  //

  if (mqtt_client != NULL) {
    if (mqtt_client->connected()) {
      return true;
    } else {
      return false;
    }
  }

  if (strlen(thinx_udid) < 4) {
    //Serial.println(F("*TH: MQTT NO-UDID!")); Serial.flush();
    return false;
  }

  Serial.print(F("*TH: UDID: ")); Serial.println(thinx_udid);
  Serial.print(F("*TH: Contacting MQTT server ")); Serial.println(thinx_mqtt_url);
  Serial.print(F("*TH: MQTT client with URL ")); Serial.println(thinx_mqtt_url); Serial.flush();

  mqtt_client = new PubSubClient(thx_wifi_client, thinx_mqtt_url);

  Serial.print(F(" started on port "));
  Serial.println(thinx_mqtt_port);

  last_mqtt_reconnect = 0;

  if (strlen(thinx_api_key) < 5) {
    Serial.println(F("*TH: API Key not set, exiting."));
    return false;
  }

  Serial.print(F("*TH: AK: "));
  Serial.println(thinx_api_key);
  Serial.print(F("*TH: DCH: "));
  Serial.println(thinx_mqtt_channel());

  const char* id = thinx_mac();
  const char* user = thinx_udid;
  const char* pass = thinx_api_key;
  String willTopic = thinx_mqtt_status_channel();
  int willQos = 0;
  bool willRetain = false;

  delay(1);

  Serial.println(F("*TH: Connecting to MQTT..."));

  if (mqtt_client->connect(MQTT::Connect(id)
  .set_will(willTopic.c_str(), F("{ \"status\" : \"disconnected\" }"))
  .set_auth(user, pass)
  .set_keepalive(30)
)) {

  Serial.println(F("*TH: mqtt_client->connected()!"));

  mqtt_connected = true;
  performed_mqtt_checkin = true;

  mqtt_client->set_callback([this](const MQTT::Publish &pub){

    delay(1);

    // >
    if (pub.has_stream()) {

      Serial.println(F("*TH: MQTT Type: Stream..."));
      uint32_t startTime = millis();
      uint32_t size = pub.payload_len();

      if ( ESP.updateSketch(*pub.payload_stream(), size, true, false) ) {

        // Notify on reboot for update
        mqtt_client->publish(
          mqtt_device_status_channel,
          "{ \"status\" : \"rebooting\" }"
        );
        mqtt_client->disconnect();
        pub.payload_stream()->stop();
        Serial.printf("Update Success: %u\nRebooting...\n", millis() - startTime);
        ESP.restart();
      } else {
        Serial.println(F("*TH: ESP MQTT Stream update failed..."));
        mqtt_client->publish(
          mqtt_device_status_channel,
          "{ \"status\" : \"mqtt_update_failed\" }"
        );
      }

    } else {
      Serial.println(F("*TH: MQTT Type: String or JSON..."));
      Serial.println(pub.payload_string());
      parse(pub.payload_string());
    }
  }); // end-of-callback

  return true;

} else {

  Serial.println(F("*TH: MQTT Not connected."));
  return false;
}
}

/*
* Restores Device Info. Calles (private): initWithAPIKey; save_device_info()
* Provides: alias, owner, update, udid, (apikey)
*/

void THiNX::restore_device_info() {

  Serial.println(F("*TH: Restoring device info..."));

  int json_end = 0;

  #ifndef __USE_SPIFFS__

  int value;
  long buf_len = 512;
  long data_len = 0;

  Serial.println(F("*TH: Restoring configuration from EEPROM..."));

  for (long a = 0; a < buf_len; a++) {
    value = EEPROM.read(a);
    json_output += char(value);
    // validate at least data start
    if (a == 0) {
      if (value != '{') {
        return; // Not a JSON, nothing to do...
      }
    }
    if (value == '{') {
      json_end++;
    }
    if (value == '}') {
      json_end--;
    }
    if (value == 0) {
      json_info[a] = char(value);
      data_len++;
      Serial.print("*TH: "); Serial.print(a); Serial.println(F(" bytes read from EEPROM."));
      // Validate JSON
      break;
    } else {
      json_info[a] = char(value);
      data_len++;
    }
    // Serial.flush(); // to debug reading EEPROM bytes
  }

  // Validating bracket count
  if (json_end != 0) {
    Serial.println(F("*TH: JSON invalid... bailing out."));
    return;
  }

  Serial.println(F("*TH: Converting data to String..."));

  #else
  if (!SPIFFS.exists("/thx.cfg")) {
    Serial.println(F("*TH: No persistent data found."));
    return;
  }
  File f = SPIFFS.open("/thx.cfg", "r");
  Serial.println(F("*TH: Found persistent data..."));
  if (!f) {
    Serial.println(F("*TH: No remote configuration found so far..."));
    return;
  }
  if (f.size() == 0) {
    Serial.println(F("*TH: Remote configuration file empty..."));
    return;
  }

  f.readBytesUntil('\n', json_info, 511);
  #endif

  Serial.println(json_info);
  Serial.println(F("*TH: Parsing..."));

  DynamicJsonBuffer jsonBuffer(512);
  JsonObject& config = jsonBuffer.parseObject((char*)json_info); // must not be String!

  if (!config.success()) {
    // Serial.println(F("*TH: No JSON data to be parsed..."));
    return;

  } else {

    Serial.println(F("*TH: Reading JSON..."));

    if (config["alias"]) {
      thinx_alias = strdup(config["alias"]);
      Serial.print("alias: ");
      Serial.println(thinx_alias);
    }

    if (config["udid"]) {
      const char *udid = config["udid"];
      if ( strlen(udid) > 2 ) {
        thinx_udid = strdup(udid);
      } else {
        thinx_udid = strdup(THINX_UDID);
      }
    } else {
      thinx_udid = strdup(THINX_UDID);
    }

    Serial.print("thinx_udid: ");
    Serial.println(thinx_udid);

    if (config["apikey"]) {
      thinx_api_key = strdup(config["apikey"]);
      Serial.print("apikey: ");
      Serial.println(thinx_api_key);
    }

    if (config["owner"]) {
      thinx_owner = strdup(config["owner"]);
      Serial.print("owner: ");
      Serial.println(thinx_owner);
    }

    if (config["ott"]) {
      available_update_url = strdup(config["ott"]);
      Serial.print("available_update_url: ");
      Serial.println(available_update_url);
    }

    #ifdef __USE_SPIFFS__
    Serial.print(F("*TH: Closing SPIFFS file."));
    f.close();
    #else
    #endif
  }
}

/*
* Stores mutable device data (alias, owner) retrieved from API
*/

void THiNX::save_device_info()
{
  deviceInfo(); // update json_output

  // disabled for it crashes when closing the file (LoadStoreAlignmentCause) when using String
  #ifdef __USE_SPIFFS__
  File f = SPIFFS.open("/thx.cfg", "w");
  if (f) {
    Serial.println(F("*TH: saving configuration to SPIFFS..."));
    f.println(String((char*)json_info)); // String instead of const char* due to LoadStoreAlignmentCause...
    Serial.println(F("*TH: closing file..."));
    f.close();
    delay(1);
  }
  #else
  Serial.println(F("*TH: Saving configuration to EEPROM: "));
  for (long addr = 0; addr < strlen((const char*)json_info); addr++) {
    uint8_t byte = json_info[addr];
    EEPROM.put(addr, json_info[addr]);
    if (byte == 0) break;
  }
  EEPROM.commit();
  Serial.println(F("*TH: EEPROM data committed..."));
  #endif
}

/*
* Fills output buffer with persistent dconfiguration JSON.
*/

void THiNX::deviceInfo() {

  Serial.println(F("*TH: Building device info:"));

  DynamicJsonBuffer jsonBuffer(512);
  JsonObject& root = jsonBuffer.createObject();

  // Mandatories

  if (strlen(thinx_owner) > 1) {
    root["owner"] = thinx_owner; // allow owner change
  }
  Serial.print(F("*TH: thinx_owner: "));
  Serial.println(thinx_owner);

  if (strlen(thinx_api_key) > 1) {
    root["apikey"] = thinx_api_key; // allow dynamic API Key
  }
  Serial.print(F("*TH: thinx_api_key: "));
  Serial.println(thinx_api_key);

  if (strlen(thinx_udid) > 1) {
    root["udid"] = thinx_udid; // allow setting UDID, skip 0
  }
  Serial.print(F("*TH: thinx_udid: "));
  Serial.println(thinx_udid);

  if (strlen(thinx_udid) > 1) {
    root["udid"] = thinx_udid; // allow setting UDID, skip 0
  }
  Serial.print(F("*TH: thinx_udid: "));
  Serial.println(thinx_udid);

  // Optionals
  if (strlen(available_update_url) > 1) {
    root["update"] = available_update_url; // allow update
    Serial.print(F("*TH: available_update_url: "));
    Serial.println(available_update_url);
  }

  json_output = "";
  Serial.print(F("*TH: Building JSON: "));
  root.printTo(json_output);
  Serial.println(json_output);
}

/*
* Updates
*/

// update_file(name, data)
// update_from_url(name, url)

void THiNX::update_and_reboot(String url) {

  Serial.print("*TH: Update with URL: ");
  Serial.println(url);

  // #define __USE_STREAM_UPDATER__ ; // Warning, this is MQTT-based streamed update!
  #ifdef __USE_STREAM_UPDATER__
  Serial.println(F("*TH: Starting MQTT & reboot..."));
  uint32_t size = pub.payload_len();
  if (ESP.updateSketch(*pub.payload_stream(), size, true, false)) {
    Serial.println(F("Clearing retained message."));
    mqtt_client->publish(MQTT::Publish(pub.topic(), "").set_retain());
    mqtt_client->disconnect();

    Serial.printf("Update Success: %u\nRebooting...\n", millis() - startTime);

    // Notify on reboot for update
    if (mqtt_client != NULL) {
      mqtt_client->publish(
        mqtt_device_status_channel,
        thx_reboot_response.c_str()
      );
      mqtt_client->disconnect();
    }

    ESP.restart();
  }
  #else

  Serial.println(F("*TH: Starting ESP8266 HTTP Update & reboot..."));
  t_httpUpdate_return ret = ESPhttpUpdate.update(url.c_str());

  switch(ret) {
    case HTTP_UPDATE_FAILED:
    Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
    setStatus(ESPhttpUpdate.getLastErrorString());
    break;

    case HTTP_UPDATE_NO_UPDATES:
    Serial.println(F("HTTP_UPDATE_NO_UPDATES"));
    break;

    case HTTP_UPDATE_OK:
    Serial.println(F("HTTP_UPDATE_OK"));
    break;
  }
  #endif
}

/*
* Imports all required build-time values from thinx.h
*/

void THiNX::import_build_time_constants() {

  Serial.println(F("*TH: Importing build-time constants..."));

  // Only if not overridden by user
  if (strlen(thinx_api_key) < 4) {
    thinx_api_key = strdup(THINX_API_KEY);
  }

  if (strlen(THINX_UDID) > 2) {
    thinx_udid = strdup(THINX_UDID);
  } else {
    thinx_udid = strdup("");
  }

  // Use commit-id from thinx.h if not given by environment
  #ifdef THX_COMMIT_ID
  thinx_commit_id = strdup(thx_commit_id);
  #else
  thinx_commit_id = strdup(THINX_COMMIT_ID);
  #endif

  thinx_mqtt_url = strdup(THINX_MQTT_URL);
  thinx_cloud_url = strdup(THINX_CLOUD_URL);
  thinx_alias = strdup(THINX_ALIAS);
  thinx_owner = strdup(THINX_OWNER);
  thinx_mqtt_port = THINX_MQTT_PORT;
  thinx_api_port = THINX_API_PORT;
  thinx_auto_update = THINX_AUTO_UPDATE;
  thinx_forced_update = THINX_FORCED_UPDATE;
  thinx_firmware_version = strdup(THINX_FIRMWARE_VERSION);
  thinx_firmware_version_short = strdup(THINX_FIRMWARE_VERSION_SHORT);
  app_version = strdup(THINX_APP_VERSION);

  Serial.println(F("*TH: Loaded build-time constants..."));
}

/*
* Performs the SPIFFS check and format if needed.
*/

bool THiNX::fsck() {
  String realSize = String(ESP.getFlashChipRealSize());
  String ideSize = String(ESP.getFlashChipSize());
  bool flashCorrectlyConfigured = realSize.equals(ideSize);
  bool fileSystemReady = false;
  if(flashCorrectlyConfigured) {
    Serial.println(F("* TH: Starting SPIFFS..."));
    fileSystemReady = SPIFFS.begin();
    if (!fileSystemReady) {
      Serial.println(F("* TH: Formatting SPIFFS..."));
      fileSystemReady = SPIFFS.format();;
      Serial.println(F("* TH: Format complete, rebooting...")); Serial.flush();
      ESP.restart();
      return false;
    }
    Serial.println(F("* TH: SPIFFS Initialization completed."));
  }  else {
    Serial.print(F("*TH: Flash incorrectly configured, SPIFFS cannot start, IDE size: "));
    Serial.println(ideSize + ", real size: " + realSize);
  }
  return fileSystemReady ? true : false;
}

#ifdef __USE_WIFI_MANAGER__
/*
* API key update event
*/

void THiNX::evt_save_api_key() {
  if (should_save_config) {
    if (strlen(thx_api_key) > 4) {
      thinx_api_key = thx_api_key;
      Serial.print(F("Saving thx_api_key from Captive Portal: "));
      Serial.println(thinx_api_key);
    }
    if (strlen(thx_owner_key) > 4) {
      thinx_owner_key = thx_owner_key;
      Serial.print(F("Saving thx_owner_key from Captive Portal: "));
      Serial.println(thinx_owner_key);
    }
    save_device_info();
    should_save_config = false;
  }
}
#endif

/*
* Final callback setter
*/

void THiNX::setPushConfigCallback( void (*func)(String) ) {
  _config_callback = func;
}

void THiNX::setFinalizeCallback( void (*func)(void) ) {
  _finalize_callback = func;
}

void THiNX::finalize() {
  thinx_phase = COMPLETED;
  if (_finalize_callback) {
    Serial.println(F("*TH: Checkin completed, calling finalize_callback()"));
    _finalize_callback();
  } else {
    Serial.println(F("*TH: Checkin completed (no _finalize_callback set)."));
  }
}

/*
* Core loop
*/

void THiNX::loop() {

  if (thinx_phase < COMPLETED) {
    // Serial.print("Phase: "); Serial.println(thinx_phase);
  }

  // CASE thinx_phase == CONNECT_WIFI

  if (thinx_phase == CONNECT_WIFI) {
    // If not connected manually or using WiFiManager, start connection in progress...
    if (WiFi.status() != WL_CONNECTED) {
      connected = false;
      if (wifi_connection_in_progress != true) {
        Serial.println(F("*TH: CONNECTING »"));
        Serial.println(F("*TH: LOOP «÷»")); Serial.flush();
        connect(); // blocking
        Serial.println(F("*TH: Enabling connection state (1283)"));
        wifi_connection_in_progress = true;
        Serial.println(F("*TH: LOOP «"));
        wifi_connection_in_progress = true;
        return;
      } else {
        return;
      }
    } else {
      connected = true;

      // Start MDNS broadcast
      if (!MDNS.begin(thinx_alias)) {
        Serial.println(F("*TH: Error setting up mDNS"));
      } else {
        // Query MDNS proxy
        Serial.println(F("*TH: Searching for thinx-connect on local network..."));
        int n = MDNS.queryService("thinx", "tcp"); // TODO: WARNING! may be _tcp!
        if (n > 0) {
          thinx_cloud_url = strdup(String(MDNS.hostname(0)).c_str());
          thinx_mqtt_url = strdup(String(MDNS.hostname(0)).c_str());

          Serial.println(F("*TH: Routing traffic through thinx-connect..."));
          Serial.print(F("     host            : ")); Serial.println(MDNS.hostname(0));
          Serial.print(F("     IP              : ")); Serial.println(MDNS.IP(0));
          Serial.print(F("     port            : ")); Serial.println(MDNS.port(0));
          Serial.print(F("     thinx_cloud_url : ")); Serial.println(thinx_cloud_url);
          Serial.print(F("     thinx_mqtt_url  : ")); Serial.println(thinx_mqtt_url);
        }
      }

      thinx_phase = CONNECT_API;
    }
  }

  // After MQTT gets connected:
  if (thinx_phase == CHECKIN_MQTT) {
    thinx_mqtt_channel(); // initialize channel variable
    if (strlen(mqtt_device_channel) > 5) {
      Serial.println(F("*TH: MQTT Subscribing device channel from loop..."));
      if (mqtt_client->subscribe(mqtt_device_channel)) {
        Serial.print(F("*TH: MQTT device topic: "));
        Serial.print(mqtt_device_channel);
        Serial.println(F(" successfully subscribed."));
        Serial.println(F("*TH: Publishing device status over MQTT... "));
        // Publish status on status channel
        mqtt_client->publish(
          mqtt_device_status_channel,
          F("{ \"status\" : \"connected\" }")
        );
        mqtt_client->loop();
        thinx_phase = FINALIZE;
      }
    }
  }

  if ( thinx_phase == CONNECT_MQTT ) {
    if (strlen(thinx_udid) > 4) {
      Serial.println(F("*TH: WiFi connected, starting MQTT..."));
      mqtt_result = start_mqtt(); // connect only, do not checkin (subscribe) yet...
      mqtt_client->loop();
      if (mqtt_result == true) {
        thinx_phase = CHECKIN_MQTT;
      }
    } else {
      thinx_phase = FINALIZE;
      // nothing to do, no dynamically assigned UDID from previously successful checkin connection...
    }
  }

  // CASE thinx_phase == CONNECT_API

  // If connected, perform the MQTT loop and bail out ASAP
  if (thinx_phase == CONNECT_API) {
    if (WiFi.getMode() == WIFI_AP) {
      Serial.println(F("*TH: LOOP « (AP_MODE)"));
      return;
    }
    if (strlen(thinx_api_key) > 4) {
      checkin(); // warning, this blocking and takes time, thus return...
      thinx_phase = CONNECT_MQTT;
    }
  }

  if ( thinx_phase == FINALIZE ) {
    Serial.println(F("*TH: Calling finalize()... "));
    finalize();
  }

  if ( thinx_phase == COMPLETED ) {
    if (mqtt_result == true) {
      mqtt_client->loop();
    }
  }

  #ifdef __USE_WIFI_MANAGER__
    // Save API key on change
    if (should_save_config) {
      Serial.println(F("*TH: Saving API key on change..."));
      evt_save_api_key();
      should_save_config = false;
    }
  #endif
}

void THiNX::setLocation(double lat, double lon) {
  latitude = lat;
  longitude = lon;
  if (connected) {
    checkin();
  }
}

void THiNX::setStatus(String newstatus) {
  statusString = newstatus;
  if (connected) {
    checkin();
  }
  if (mqtt_client) {
    String message = String("{ \"status\" : \"") + newstatus + String("\" }");
    mqtt_client->publish(mqtt_device_status_channel, message.c_str());
  }
}

#endif // IMPORTANT LINE FOR UNIT-TESTING!
