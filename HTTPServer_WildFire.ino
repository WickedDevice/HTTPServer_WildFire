#include <SPI.h>
#include <WildFire.h>
#include <WildFire_CC3000.h>
#include <WildFire_CC3000_MDNS.h>
#include <TinyWatchdog.h>
#include <Flash.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "utility/debug.h"
#include "utility/socket.h"
#include <avr/eeprom.h>  
uint8_t * eeprom_led_state_address = (uint8_t *) 128;

/****************VALUES YOU CHANGE*************/
// The LED attached to PIN X on an Arduino board.
const int LEDPIN = 6;

// pin 4 is the SPI select pin for the SDcard
const int SD_CS = 16;

int ledState = LOW;
WildFire wf;
WildFire_CC3000 cc3000;
MDNSResponder mdns;

boolean has_filesystem = true;
Sd2Card card;
SdVolume volume;
SdFile root;
SdFile file;

// if would rather just use hard wired settings and skip the smart config business
// then comment out the #define USE_SMART_CONFIG line
// and fill in the WLAN_SSID, WLAN_PASS, and WLAN_SECURITY settings for your network
#define USE_SMART_CONFIG
#ifndef USE_SMART_CONFIG
  #define WLAN_SSID       "myNetwork"        // cannot be longer than 32 characters!
  #define WLAN_PASS       "myPassword"
  // Security can be WLAN_SEC_UNSEC, WLAN_SEC_WEP, WLAN_SEC_WPA or WLAN_SEC_WPA2
  #define WLAN_SECURITY   WLAN_SEC_WPA2
#endif

// keep this around so we can ping the gateway occasionally to 
// convince ourselves that we are still connected to the network
uint32_t gateway_ip_address = 0;

#define LISTEN_PORT           80    // What TCP port to listen on for connections.  
                                      // The HTTP protocol uses port 80 by default.

#define MAX_ACTION            10      // Maximum length of the HTTP action that can be parsed.

#define MAX_PATH              64      // Maximum length of the HTTP request path that can be parsed.
                                      // There isn't much memory available so keep this short!

#define BUFFER_SIZE           MAX_ACTION + MAX_PATH + 20  // Size of buffer for incoming request data.
                                                          // Since only the first line is parsed this
                                                          // needs to be as large as the maximum action
                                                          // and path plus a little for whitespace and
                                                          // HTTP version.

#define TIMEOUT_MS            500    // Amount of time in milliseconds to wait for
                                     // an incoming request to finish.  Don't set this
                                     // too high or your server could be slow to respond.

WildFire_CC3000_Server httpServer(LISTEN_PORT);

#define TINY_WATCHDOG_INTERVAL (500) // pet it once a second at most
TinyWatchdog wdt;

uint8_t buffer[BUFFER_SIZE+1];
int bufindex = 0;
char action[MAX_ACTION+1];
char path[MAX_PATH+1];


void setLedEnabled(boolean state) {
  // add perseistence support
  uint8_t eeprom_led_state = eeprom_read_byte(eeprom_led_state_address);
  if((state && eeprom_led_state != 1) || (!state && eeprom_led_state != 0)){
    eeprom_write_byte(eeprom_led_state_address, state ? 1 : 0);
  }
  ledState = state;
  digitalWrite(LEDPIN, ledState);
}

inline boolean getLedState() { return ledState; }

void setup() {
  wf.begin();
  Serial.begin(115200);
  wdt.begin(100, 10000);
  cc3000.enableTinyWatchdog(14, TINY_WATCHDOG_INTERVAL);
  
  // perseistence restore
  uint8_t eeprom_led_state = eeprom_read_byte(eeprom_led_state_address);
  ledState = (eeprom_led_state == 0) ? false : true;
  pinMode(LEDPIN, OUTPUT);  
  setLedEnabled(ledState);
  
  Serial << F("Free RAM: ") << FreeRam() << "\n";


  // initialize the SD card.
  Serial << F("Setting up SD card...\n");
  // Pass over the speed and Chip select for the SD card
  if (!card.init(SPI_FULL_SPEED, SD_CS)) {
    Serial << F("card failed\n");
    has_filesystem = false;
  }
  
  // initialize a FAT volume.
  if (!volume.init(&card)) {
    Serial << F("vol.init failed!\n");
    has_filesystem = false;
  }
  
  if (!root.openRoot(&volume)) {
    Serial << F("openRoot failed");
    has_filesystem = false;
  }

  Serial.print(F("Setting up the WiFi connection..."));
#ifdef USE_SMART_CONFIG
  Serial.println(F("using Smart Config"));
  if(!attemptSmartConfigReconnect()){
    // TODO: This is not a good strategy
    // once attemptSmartConfigCreate gets called
    // the CC3000 forgets the previous Smart Config profile
    while(!attemptSmartConfigCreate()){
      Serial.println(F("Waiting for Smart Config Create"));
    }
  }
#else
  Serial.println(F("using Sketch Credentials"));
  connectWithoutSmartConfig();
#endif

  while(!displayConnectionDetails());
  // Start multicast DNS responder
  if (!mdns.begin("wildfire", cc3000)) {
    Serial.println(F("Error setting up MDNS responder!"));
    while(1);
  }
  
  // it seems important that we do a DNS resolve up front for some reason
  resolveWickedDevice();

  httpServer.begin();
  Serial << F("Ready to accept HTTP requests.\n");
}

// would be better to use a proper scheduler
uint32_t previous_tiny_watchdog_millis = 0;
const int32_t tiny_watchdog_interval = TINY_WATCHDOG_INTERVAL;
uint32_t previous_ping_gateway_millis = 0;
const int32_t ping_gateway_interval = 10000; // ping the gateway every 10 seconds

void loop() {
  StaticJsonBuffer<200> jsonBuffer;
  char json[256] = {0};
  uint32_t current_millis = millis();
  
  // Try to get a client which is connected.
  WildFire_CC3000_ClientRef client = httpServer.available();
  if (client) {
    Serial.println(F("Client connected."));
    // Process this request until it completes or times out.
    // Note that this is explicitly limited to handling one request at a time!

    // Clear the incoming data buffer and point to the beginning of it.
    bufindex = 0;
    memset(&buffer, 0, sizeof(buffer));
    
    // Clear action and path strings.
    memset(&action, 0, sizeof(action));
    memset(&path,   0, sizeof(path));

    // Set a timeout for reading all the incoming data.
    unsigned long endtime = millis() + TIMEOUT_MS;
    
    // Read all the incoming data until it can be parsed or the timeout expires.
    bool parsed = false;
    while (!parsed && (millis() < endtime) && (bufindex < BUFFER_SIZE)) {
      if (client.available()) {
        buffer[bufindex++] = client.read();
      }
      parsed = parseRequest(buffer, bufindex, action, path);
    }

    // Handle the request if it was parsed.
    if (parsed) {
      Serial.println(F("Processing request"));
      Serial.print(F("Action: ")); Serial.println(action);
      Serial.print(F("Path: ")); Serial.println(path);
      // Check the action to see if it was a GET request.
      if (strcmp(action, "GET") == 0) {
        // Respond with the path that was accessed.
        // First send the success response code.
        client.fastrprintln(F("HTTP/1.1 200 OK"));
        // Then send a few headers to identify the type of data returned and that
        // the connection will not be held open.
        
        
        // Make an if block for each path you want to handle
        if(strncmp_P(path, PSTR("/led/state"), sizeof(path)) == 0){
          // TODO: determine the MIME type from the extension  
          // for now we assume all things accessed in this way are text/html      
          client.fastrprintln(F("Content-Type: application/json"));                
          
          client.fastrprintln(F("Connection: close"));
          client.fastrprintln(F("Server: WildFire CC3000"));
          // Send an empty line to signal start of body.
          client.fastrprintln(F(""));
          
          JsonObject& root = jsonBuffer.createObject();
          root["status"] = "ok";
          if(getLedState()) {
            root["led"] = "ON";
          }
          else{
            root["led"] = "OFF";              
          }
                    
          root.printTo(json, sizeof(json));
          client.fastrprintln(json);
        } 
        else{ // the only other possibility is it's a file on the SD card             
          char filename[64] = {0};
          if(strncmp_P(path, PSTR("/"), sizeof(path)) == 0){          
            strcpy_P(filename, PSTR("INDEX.HTM"));
          }
          else{
            strncpy(filename, path + 1, strlen(path) - 1); // strip off the leading slash
          }
          // TODO: Now send the response data.
          Serial.print("Attempting to open File ");
          Serial.println(filename);
          #define CHUNK_SIZE (256)
          char buf[CHUNK_SIZE+1] = {0};
          if (file.open(&root, filename, O_READ)) {
            Serial.println("File opened successfully");                       
            // determine the MIME type from the extension  
            char path_copy[MAX_PATH + 1] = {0};
            strncpy(path_copy, path, MAX_PATH);
            char* extension = 0;
            char* token = strtok(path_copy, ".");
            while (token) {
              extension = token;
              token = strtok(NULL, ".");
            }            
            
            // crude tolower routine
            for(uint16_t ii = 0; ii < sizeof(extension); ii++){
              if(extension[ii] == 0) break;
              if(extension[ii] >= 'A' && extension[ii] <= 'Z'){
                extension[ii] = extension[ii] - 'A' + 'a'; 
              }
            }
            
            Serial.print("Extension: ");
            Serial.println(extension);
            
            boolean format_is_binary = false;
            if(strncmp_P(extension,PSTR("htm"),sizeof(extension)) == 0 || strcmp_P(path, PSTR("/")) == 0){
              client.fastrprintln(F("Content-Type: text/html"));                   
            }             
            else if(strncmp_P(extension,PSTR("js"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: text/javascript"));                   
            } 
            else if(strncmp_P(extension,PSTR("png"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: image/png")); 
              format_is_binary = true;
            } 
            else if(strncmp_P(extension,PSTR("gif"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: image/gif"));  
              format_is_binary = true;              
            } 
            else if(strncmp_P(extension,PSTR("jpg"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: image/jpeg"));
              format_is_binary = true;              
            }
            else if(strncmp_P(extension,PSTR("css"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: text/css"));                   
            }
            else if(strncmp_P(extension,PSTR("ico"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: image/vnd.microsoft.icon"));   
              format_is_binary = true;              
            }
            else if(strncmp_P(extension,PSTR("xml"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: text/xml"));                   
            }            
            else{ //if(strncmp_P(extension,PSTR("txt"),sizeof(extension)) == 0){
              client.fastrprintln(F("Content-Type: text/plain"));                   
            } 
            
            client.fastrprintln(F("Connection: close"));
            client.fastrprintln(F("Server: WildFire CC3000"));
            // Send an empty line to signal start of body.
            client.fastrprintln(F(""));            
            
            memset(buf, 0, CHUNK_SIZE + 1); // clear the buffer
            size_t sz = 0;            
            while ((sz = file.read(buf, CHUNK_SIZE)) > 0){
              Serial.print("Read file chunk size = ");
              Serial.println(sz);
              buf[sz] = '\0';
              if(format_is_binary){
                client.write(buf, sz, 0);
              }
              else{
                client.fastrprint(buf);
              }
            }
            
            file.close();
          }        
          else{
            client.fastrprintln(F("Content-Type: text/html"));                           
            client.fastrprintln(F("Connection: close"));
            client.fastrprintln(F("Server: WildFire CC3000"));
            // Send an empty line to signal start of body.
            client.fastrprintln(F(""));            
            client.fastrprintln(F("Hello world!"));
            client.fastrprint(F("You accessed (via GET) path: ")); client.fastrprintln(path);          
          }    
        }          
      }
      else if(strcmp(action, "POST") == 0){
        // TODO: if we expect to get a JSON data structure as input
        // I think we would need to keep reading the client input at this point
        // for now, we'll take the path to contain all the relevant information
        
        // Respond with the path that was accessed.
        // First send the success response code.0
        client.fastrprintln(F("HTTP/1.1 200 OK"));
        // Then send a few headers to identify the type of data returned and that
        // the connection will not be held open.
        
        // NOTE: We will respond to all POST requests with JSON responses
        client.fastrprintln(F("Content-Type: application/json"));
        client.fastrprintln(F("Connection: close"));
        client.fastrprintln(F("Server: WildFire CC3000"));
        // Send an empty line to signal start of body.
        client.fastrprintln(F(""));
        // Now send the response data.
        Serial.print(F("Got POST request to path "));  
        Serial.println(path);
        JsonObject& root = jsonBuffer.createObject();        
        
        if(strncmp_P(path, PSTR("/led/on"), sizeof(path)) == 0){
          setLedEnabled(true);      
          root["status"] = "ok";
          if(getLedState()) {
            root["led"] = "ON";
          }
          else{
            root["led"] = "OFF";              
          }              
          root.printTo(json, sizeof(json));
          Serial.print("JSON: ");
          Serial.println(json);
          
          client.fastrprintln(json);          
        }
        else if(strncmp_P(path, PSTR("/led/off"), sizeof(path)) == 0){
          setLedEnabled(false);          
          root["status"] = "ok";
         if(getLedState()) {
            root["led"] = "ON";
          }
          else{
            root["led"] = "OFF";              
          }               
          root.printTo(json, sizeof(json));
          Serial.print("JSON: ");
          Serial.println(json);          
          client.fastrprintln(json);          
        }
        else if(strncmp_P(path, PSTR("/led/toggle"), sizeof(path)) == 0){
          if(ledState){
            setLedEnabled(false); 
          }
          else{
            setLedEnabled(true); 
          }          
          root["status"] = "ok";
          if(getLedState()) {
            root["led"] = "ON";
          }
          else{
            root["led"] = "OFF";              
          }
          
          root.printTo(json, sizeof(json));
          Serial.print("JSON: ");
          Serial.println(json);
          client.fastrprintln(json);          
        }        
        else{
          root["status"] = "error";
          root["path"] = path;
          if(getLedState()) {
            root["led"] = "ON";
          }
          else{
            root["led"] = "OFF";              
          }          
          root.printTo(json, sizeof(json));
          Serial.print("JSON: ");
          Serial.println(json);          
          client.fastrprintln(json);              
        }
      }
      else {
        // Unsupported action, respond with an HTTP 405 method not allowed error.
        client.fastrprintln(F("HTTP/1.1 405 Method Not Allowed"));
        client.fastrprintln(F(""));
      }
    }
    client.fastrprintln(F(""));
    // Wait a short period to make sure the response had time to send before
    // the connection is closed (the CC3000 sends data asyncronously).
    delay(100);

    // Close the connection when done.
    Serial.println(F("Client disconnected"));
    client.close();
  }
  
  if(current_millis - previous_tiny_watchdog_millis >= tiny_watchdog_interval){
    previous_tiny_watchdog_millis = current_millis;
    wdt.pet();
    Serial.print(".");
  }

  if(current_millis - previous_ping_gateway_millis >= ping_gateway_interval){
    if(!cc3000.checkConnected()){
      Serial.println("Not Connected to Network - Restarted");
      Serial.flush();
      wdt.force_reset();
    }    
    else{
      Serial.println("Still Connected"); 
    }
    
    /* Do a quick ping test on wickeddevice.com */  
    resolveWickedDevice();
    
    if(gateway_ip_address != 0){
      Serial.print(F("\n\rPinging ")); cc3000.printIPdotsRev(gateway_ip_address); Serial.print("...");  
      uint8_t replies = cc3000.ping(gateway_ip_address, 1);
      Serial.print(replies); Serial.println(F(" replies"));
    }
    
    previous_ping_gateway_millis = current_millis;        
  }
  
}

bool displayConnectionDetails(void) {
  uint32_t addr, netmask, gateway, dhcpserv, dnsserv;

  if(!cc3000.getIPAddress(&addr, &netmask, &gateway, &dhcpserv, &dnsserv))
    return false;

  Serial.print(F("IP Addr: ")); cc3000.printIPdotsRev(addr);
  Serial.print(F("\r\nNetmask: ")); cc3000.printIPdotsRev(netmask);
  Serial.print(F("\r\nGateway: ")); cc3000.printIPdotsRev(gateway);
  Serial.print(F("\r\nDHCPsrv: ")); cc3000.printIPdotsRev(dhcpserv);
  Serial.print(F("\r\nDNSserv: ")); cc3000.printIPdotsRev(dnsserv);
  Serial.println();
  return true;
}

boolean attemptSmartConfigReconnect(void){
  /* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
  /* !!! Note the additional arguments in .begin that tell the   !!! */
  /* !!! app NOT to deleted previously stored connection details !!! */
  /* !!! and reconnected using the connection details in memory! !!! */
  /* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
  if (!cc3000.begin(false, true))
  {
    Serial.println(F("Unable to re-connect!? Did you run the SmartConfigCreate"));
    Serial.println(F("sketch to store your connection details?"));
    return false;
  }

  /* Round of applause! */
  Serial.println(F("Reconnected!"));

  /* Wait for DHCP to complete */
  Serial.println(F("\nRequesting DHCP"));
  while (!cc3000.checkDHCP()) {
    delay(100); // ToDo: Insert a DHCP timeout!
  }
  return true;
}

boolean attemptSmartConfigCreate(void){
  /* Initialise the module, deleting any existing profile data (which */
  /* is the default behaviour)  */
  Serial.println(F("\nInitialising the CC3000 ..."));
  if (!cc3000.begin(false))
  {
    return false;
  }

  /* Try to use the smart config app (no AES encryption), saving */
  /* the connection details if we succeed */
  Serial.println(F("Waiting for a SmartConfig connection (~60s) ..."));
  if (!cc3000.startSmartConfig(false))
  {
    Serial.println(F("SmartConfig failed"));
    return false;
  }

  Serial.println(F("Saved connection details and connected to AP!"));

  /* Wait for DHCP to complete */
  Serial.println(F("Request DHCP"));
  while (!cc3000.checkDHCP())
  {
    delay(100); // ToDo: Insert a DHCP timeout!
  }

  return true;
}

// Return true if the buffer contains an HTTP request.  Also returns the request
// path and action strings if the request was parsed.  This does not attempt to
// parse any HTTP headers because there really isn't enough memory to process
// them all.
// HTTP request looks like:
//  [method] [path] [version] \r\n
//  Header_key_1: Header_value_1 \r\n
//  ...
//  Header_key_n: Header_value_n \r\n
//  \r\n
bool parseRequest(uint8_t* buf, int bufSize, char* action, char* path) {
  // Check if the request ends with \r\n to signal end of first line.
  if (bufSize < 2)
    return false;
  if (buf[bufSize-2] == '\r' && buf[bufSize-1] == '\n') {
    parseFirstLine((char*)buf, action, path);
    return true;
  }
  return false;
}

// Parse the action and path from the first line of an HTTP request.
void parseFirstLine(char* line, char* action, char* path) {
  // Parse first word up to whitespace as action.
  char* lineaction = strtok(line, " ");
  if (lineaction != NULL)
    strncpy(action, lineaction, MAX_ACTION);
  // Parse second word up to whitespace as path.
  char* linepath = strtok(NULL, " ");
  if (linepath != NULL)
    strncpy(path, linepath, MAX_PATH);
}

#ifndef USE_SMART_CONFIG
void connectWithoutSmartConfig(void){
  Serial.println(F("\nInitialising the CC3000 ..."));
  if (!cc3000.begin())
  {
    Serial.println(F("Unable to initialise the CC3000! Check your wiring?"));
    while(1);
  }  
  
  Serial.println("Connecting to Access Point");
  if (!cc3000.connectToAP(WLAN_SSID, WLAN_PASS, WLAN_SECURITY)) {
    Serial.println(F("Failed!"));
    while(1);
  }

  Serial.println(F("Connected!"));
  
  /* Wait for DHCP to complete */
  Serial.println(F("Request DHCP"));
  while (!cc3000.checkDHCP())
  {
    delay(100); // ToDo: Insert a DHCP timeout!
  }     
}
#endif

void resolveWickedDevice(void){
  gateway_ip_address = 0;
  Serial.print(F("www.wickeddevice.com -> "));
  if  (!  cc3000.getHostByName("www.wickeddevice.com", &gateway_ip_address))  {
    Serial.println(F("Couldn't resolve www.wickeddevice.com!"));
  }    
  cc3000.printIPdotsRev(gateway_ip_address);  
  Serial.println();
}
