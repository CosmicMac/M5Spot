/*
pio platform update
pio init --ide clion --board m5stack-core-esp32
*/

#include <M5Stack.h>
#include <Arduino.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <base64.h>
#include "main.h"
#include "config.h"

#ifdef WITH_APDS9960
#include <SparkFun_APDS9960.h>
SparkFun_APDS9960 apds = SparkFun_APDS9960();
volatile int isr_flag = 0;
#endif

// MISC GLOBALS

WiFiMulti wifiMulti;
AsyncWebServer server(80);
AsyncEventSource events("/events");

String auth_code;
String access_token;
String refresh_token;

uint32_t token_lifetime_ms = 0;
uint32_t token_millis = 0;
uint32_t last_curplay_millis = 0;
uint32_t next_curplay_millis = 0;

bool getting_token = false;
bool ota_in_progress = false;
bool sptf_is_playing = true;
bool send_events = true;

uint16_t sptf_green = M5.Lcd.color565(30, 215, 96);

SptfActions sptfAction = Iddle;


/**
 * Setup
 */
void setup() {

    //-----------------------------------------------
    // Initialize M5Stack
    //-----------------------------------------------
    M5.begin();
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(sptf_green);

    char title[17];
    snprintf(title, sizeof(title), "M5Spot v%s", M5S_VERSION);

#ifdef WITH_APDS9960
    //-----------------------------------------------
    // Initialize APDS-9960
    //-----------------------------------------------

    pinMode(APDS9960_INT_PIN, INPUT);

    if (apds.init()) {
        M5S_DBG("APDS-9960 initialization complete\n");
        if (apds.enableGestureSensor(true)) {
            attachInterrupt(digitalPinToInterrupt(APDS9960_INT_PIN), interruptRoutine, FALLING);
            M5S_DBG("Gesture sensor is now running\n");
        } else {
            M5S_DBG("Something went wrong during gesture sensor init!\n");
        }
    } else {
        M5S_DBG("Something went wrong during APDS-9960 init!\n");
    }
#endif

    //-----------------------------------------------
    // Initialize SPIFFS
    //-----------------------------------------------

    if (!SPIFFS.begin()) {
        m5sEpitaph("Unable to begin SPIFFS");
    }

    //-----------------------------------------------
    // Initialize Wifi
    //-----------------------------------------------

    M5.Lcd.drawJpgFile(SPIFFS, "/logo128.jpg", 96, 50, 128, 128);

    M5.Lcd.setFreeFont(&FreeSansBoldOblique12pt7b);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(TC_DATUM);
    M5.Lcd.drawString(title, 160, 10);

    M5.Lcd.setFreeFont(&FreeSans9pt7b);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(BC_DATUM);
    M5.Lcd.drawString("Connecting to WiFi...", 160, 215);

    WiFi.mode(WIFI_STA);
    for (auto i : AP_LIST) {
        wifiMulti.addAP(i.ssid, i.passphrase);
    }

    uint8_t count = 20;
    while (count-- && (wifiMulti.run() != WL_CONNECTED)) {
        delay(500);
    }

    if (!WiFi.isConnected()) {
        m5sEpitaph("Unable to connect to WiFi");
    }

    WiFi.setHostname("M5Spot");

    //-----------------------------------------------
    // Initialize OTA handlers
    //-----------------------------------------------

    ArduinoOTA.onStart([]() {
        ota_in_progress = true;
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setFreeFont(&FreeSans9pt7b);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextDatum(CC_DATUM);
        M5.Lcd.drawString("OTA update", 160, 120, 2);
        progressBar(140, 0);
    });

    ArduinoOTA.onProgress([](uint32_t progress, uint32_t total) {
        progressBar(140, progress / (total / 100));
    });

    ArduinoOTA.onEnd([]() {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.drawString("OTA update done", 160, 120, 2);
        ota_in_progress = false;
    });

    ArduinoOTA.onError([](ota_error_t error) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextDatum(CC_DATUM);
        M5.Lcd.drawString("OTA update error. Please retry...", 160, 120, 2);
    });

    ArduinoOTA.setHostname("M5Spot");
    ArduinoOTA.begin();
    MDNS.addService("http", "tcp", 80);

    //-----------------------------------------------
    // Display some infos
    //-----------------------------------------------

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawJpgFile(SPIFFS, "/logo128d.jpg", 96, 50, 128, 128);

    M5.Lcd.setFreeFont(&FreeSansBoldOblique12pt7b);
    M5.Lcd.setTextColor(sptf_green);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(TC_DATUM);
    M5.Lcd.drawString(title, 160, 10);

    M5.Lcd.setFreeFont(&FreeMono9pt7b);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(0, 75);
    M5.Lcd.printf(" SSID:      %s\n", WiFi.SSID().c_str());
    M5.Lcd.printf(" IP:        %s\n", WiFi.localIP().toString().c_str());
    M5.Lcd.printf(" STA MAC:   %s\n", WiFi.macAddress().c_str());
    M5.Lcd.printf(" AP MAC:    %s\n", WiFi.softAPmacAddress().c_str());
    M5.Lcd.printf(" Chip size: %s\n", prettyBytes(ESP.getFlashChipSize()).c_str());
    M5.Lcd.printf(" Free heap: %s\n", prettyBytes(ESP.getFreeHeap()).c_str());

    M5.Lcd.setFreeFont(&FreeSans9pt7b);
    M5.Lcd.setTextColor(sptf_green);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(BC_DATUM);
    M5.Lcd.drawString("Press any button to continue...", 160, 230);

    uint32_t pause = millis();
    while (true) {
        m5.update();
        ArduinoOTA.handle();
        if (m5.BtnA.wasPressed() || m5.BtnB.wasPressed() || m5.BtnC.wasPressed() || (millis() - pause > 20000)) {
            break;
        }
        yield();
    }

    //-----------------------------------------------
    // Initialize HTTP server handlers
    //-----------------------------------------------
    events.onConnect([](AsyncEventSourceClient *client) {
        M5S_DBG("\n> [%d] events.onConnect\n", micros());
    });
    server.addHandler(&events);

    server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint32_t ts = micros();
        M5S_DBG("\n> [%d] server.on /\n", ts);
        if (access_token == "" && !getting_token) {
            getting_token = true;
            char auth_url[300] = "";
            snprintf(auth_url, sizeof(auth_url),
                     "https://accounts.spotify.com/authorize/"
                     "?response_type=code"
                     "&scope=user-read-private+user-read-currently-playing+user-read-playback-state+user-modify-playback-state"
                     "&redirect_uri=http%%3A%%2F%%2Fm5spot.local%%2Fcallback%%2F"
                     "&client_id=%s",
                     SPTF_CLIENT_ID
            );
            M5S_DBG("  [%d] Redirect to: %s\n", ts, auth_url);
            request->redirect(auth_url);
        } else {
            request->send(SPIFFS, "/index.html");
        }
    });

    server.on("/callback", HTTP_GET, [](AsyncWebServerRequest *request) {
        auth_code = "";
        uint8_t paramsNr = request->params();
        for (uint8_t i = 0; i < paramsNr; i++) {
            AsyncWebParameter *p = request->getParam(i);
            if (p->name() == "code") {
                auth_code = p->value();
                sptfAction = GetToken;
                break;
            }
        }
        if (sptfAction == GetToken) {
            request->redirect("/");
        } else {
            request->send(204);
        }
    });

    server.on("/next", HTTP_GET, [](AsyncWebServerRequest *request) {
        sptfAction = Next;
        request->send(204);
    });

    server.on("/previous", HTTP_GET, [](AsyncWebServerRequest *request) {
        sptfAction = Previous;
        request->send(204);
    });

    server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
        sptfAction = Toggle;
        request->send(204);
    });

    server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });

    server.on("/resettoken", HTTP_GET, [](AsyncWebServerRequest *request) {
        access_token = "";
        refresh_token = "";
        deleteRefreshToken();
        sptfAction = Iddle;
        request->send(200, "text/plain", "Tokens deleted, M5Spot will restart");
        uint32_t start = millis();
        while (true) {
            if(millis() -  start > 5000) {
                ESP.restart();
            }
            yield();
        }
    });
    server.on("/resetwifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        WiFi.disconnect(true);
        request->send(200, "text/plain", "WiFi credentials deleted, M5Spot will restart");
        uint32_t start = millis();
        while (true) {
            if(millis() -  start > 5000) {
                ESP.restart();
            }
            yield();
        }
    });

    server.on("/toggleevents", HTTP_GET, [](AsyncWebServerRequest *request) {
        send_events = !send_events;
        request->send(200, "text/plain", send_events ? "1" : "0");
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404);
    });

    server.begin();

    //-----------------------------------------------
    // Get refresh token from EEPROM
    //-----------------------------------------------
    refresh_token = readRefreshToken();

    //-----------------------------------------------
    // End of setup
    //-----------------------------------------------
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawJpgFile(SPIFFS, "/logo128.jpg", 96, 50, 128, 128);

    M5.Lcd.setFreeFont(&FreeSansBoldOblique12pt7b);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(TC_DATUM);
    M5.Lcd.drawString(title, 160, 10);

    M5.Lcd.setTextDatum(BC_DATUM);
    if (refresh_token == "") {
        M5.Lcd.setFreeFont(&FreeSans9pt7b);
        M5.Lcd.drawString("Point your browser to", 160, 205);

        M5.Lcd.setFreeFont(&FreeSans12pt7b);
        M5.Lcd.drawString("http://m5spot.local", 160, 235);
    } else {
        M5.Lcd.setFreeFont(&FreeSans9pt7b);
        M5.Lcd.drawString("Ready...", 160, 230);

        sptfAction = CurrentlyPlaying;
    }
}

/**
 * Main loop
 */
void loop() {

    uint32_t cur_millis = millis();


    // OTA handler
    ArduinoOTA.handle();
    if (ota_in_progress) {
        return;
    }

    // Refreh Spotify access token either on M5Spot startup or at token expiration delay
    // The number of requests is limited to 1 every 5 seconds
    if (refresh_token != ""
        && (token_millis == 0 || (cur_millis - token_millis >= token_lifetime_ms))) {
        static uint32_t gettoken_millis = 0;
        if (cur_millis - gettoken_millis >= 5000) {
            sptfGetToken(refresh_token);
            gettoken_millis = cur_millis;
        }
    }

#ifdef WITH_APDS9960
        // Gesture event handler
        if (isr_flag == 1) {
            detachInterrupt(digitalPinToInterrupt(APDS9960_INT_PIN));
            handleGesture();
            isr_flag = 0;
            attachInterrupt(digitalPinToInterrupt(APDS9960_INT_PIN), interruptRoutine, FALLING);
        }
#endif

    // M5Stack handler
    m5.update();
    if (m5.BtnA.wasPressed()) {
        sptfAction = Previous;
    }

    if (m5.BtnB.wasPressed()) {
        sptfAction = Toggle;
    }

    if (m5.BtnC.wasPressed()) {
        sptfAction = Next;
    }

    // Spotify action handler
    switch (sptfAction) {
        case Iddle:
            break;
        case GetToken:
            sptfGetToken(auth_code, gt_authorization_code);
            break;
        case CurrentlyPlaying:
            if (next_curplay_millis && (cur_millis >= next_curplay_millis)) {
                sptfCurrentlyPlaying();
            } else if (cur_millis - last_curplay_millis >= SPTF_POLLING_DELAY) {
                sptfCurrentlyPlaying();
            }
            break;
        case Next:
            sptfNext();
            break;
        case Previous:
            sptfPrevious();
            break;
        case Toggle:
            sptfToggle();
            break;
    }
}


/**
 * Draw a progress bar
 *
 * @param y
 * @param width
 * @param val
 */
void progressBar(uint8_t y, uint8_t val, uint16_t width, uint16_t height, uint16_t color) {
    uint8_t x = (M5.Lcd.width() - width) / 2;
    M5.Lcd.drawRect(x, y, width, height, color);
    M5.Lcd.fillRect(x, y, width * (((float) val) / 100.0), height, color);
}


/**
 * Send log to browser
 *
 * @param logData
 * @param event_type
 */
void eventsSendLog(const char *logData, EventsLogTypes type) {
    if(!send_events) return;
    events.send(logData, type == log_line ? "line" : "raw");
}


/**
 * Send infos to browser
 *
 * @param msg
 * @param payload
 */
void eventsSendInfo(const char *msg, const char *payload) {
    if(!send_events) return;

    DynamicJsonBuffer jsonBuffer(256);
    JsonObject &json = jsonBuffer.createObject();
    json["msg"] = msg;
    if (strlen(payload)) {
        json["payload"] = payload;
    }

    String info;
    json.printTo(info);
    events.send(info.c_str(), "info");
}


/**
 * Send errors to browser
 *
 * @param errCode
 * @param errMsg
 * @param payload
 */
void eventsSendError(int code, const char *msg, const char *payload) {
    if(!send_events) return;

    DynamicJsonBuffer jsonBuffer(256);
    JsonObject &json = jsonBuffer.createObject();
    json["code"] = code;
    json["msg"] = msg;
    if (strlen(payload)) {
        json["payload"] = payload;
    }

    String error;
    json.printTo(error);
    events.send(error.c_str(), "error");
}


/**
 * Base 64 encode
 *
 * @param str
 * @return
 */
String b64Encode(String str) {

    String encodedStr = base64::encode(str);

    // Remove unnecessary linefeeds
    int idx = -1;
    while ((idx = encodedStr.indexOf('\n')) != -1) {
        encodedStr.remove(idx, 1);
    }

    return encodedStr;
}


/**
 * Write refresh token to EEPROM
 */
void writeRefreshToken() {
    M5S_DBG("\n> [%d] writeRefreshToken()\n", micros());

    EEPROM.begin(256);

    String tmpTok;
    tmpTok.reserve(refresh_token.length() + 6);
    tmpTok = "rtok:";
    tmpTok += refresh_token;

    uint16_t i = 0, addr = 0, tt_len = tmpTok.length();
    for (i = 0; addr < 256 && i < tt_len; i++, addr++) {
        EEPROM.write(addr, (uint8_t) tmpTok.charAt(i));
    }

    EEPROM.write(addr, '\0');
    EEPROM.end();
}


/**
 * Delete refresh token from EEPROM
 */
void deleteRefreshToken() {
    M5S_DBG("\n> [%d] deleteRefreshToken()\n", micros());

    EEPROM.begin(256);
    EEPROM.write(0, '\0'); // Deleting only the first character is enough to make the key invalid
    EEPROM.end();
}


/**
 * Read refresh token from EEPROM
 *
 * @return String
 */
String readRefreshToken() {
    M5S_DBG("\n> [%d] readRefreshToken()\n", micros());

    String tok;
    tok.reserve(150);

    EEPROM.begin(1024);

    for (int i = 0; i < 5; i++) {
        tok += String(char(EEPROM.read(i)));
    }

    if (tok == "rtok:") {
        tok = "";
        char c;
        uint16_t addr = 5;
        while (addr < 256) {
            c = char(EEPROM.read(addr++));
            if (c == '\0') {
                break;
            } else {
                tok += c;
            }
            yield();
        }
    } else {
        tok = "";
    }

    EEPROM.end();

    return tok;
}


/**
 * Display album art
 *
 * @param url
 */
void sptfDisplayAlbumArt(String url) {
    uint32_t ts = micros();
    M5S_DBG("\n> [%d] sptfDisplayAlbumArt(%s)\n", ts, url.c_str());

    HTTPClient http;

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {

            const char filename[] = "/albart.jpg";

            SD.remove(filename);

            File file = SD.open(filename, FILE_APPEND);
            if (!file) {
                M5S_DBG("  [%d] File open failed\n", ts);
                eventsSendError(500, "File open failed");
                http.end();
                return;
            }

            WiFiClient *stream = http.getStreamPtr();
            int jpgSize = http.getSize();
            if (jpgSize < 0) {
                M5S_DBG("  [%d] Unable to get JPEG size\n", ts);
                eventsSendError(500, "Unable to get JPEG size");
                http.end();
                return;
            }
            uint16_t buffSize = 1024;
            uint8_t buff[buffSize] = {0};

            while (http.connected() && jpgSize > 0) {
                int availableSize = stream->available();
                if (availableSize > 0) {

                    // Read data from stream into buffer
                    int B = stream->readBytes(buff, (size_t) min(availableSize, buffSize));

                    // Write buffer to file
                    file.write(buff, (size_t) B);

                    jpgSize -= B;
                }
                delay(10);
            }
            file.close();

            // Display album art
            M5.Lcd.fillScreen(WHITE);
            M5.Lcd.drawJpgFile(SD, filename, 10, 30);

        } else {
            M5S_DBG("  [%d] Unable to get album art: %s\n", ts, http.errorToString(httpCode).c_str());
            eventsSendError(httpCode, "Unable to get album art", http.errorToString(httpCode).c_str());
        }
    } else {
        M5S_DBG("  [%d] Unable to get album art: %s\n", ts, http.errorToString(httpCode).c_str());
        eventsSendError(httpCode, "Unable to get album art", http.errorToString(httpCode).c_str());
    }

    http.end();
}


/**
 * HTTP request
 *
 * @param host
 * @param port
 * @param headers
 * @param content
 * @return
 */
HTTP_response_t httpRequest(const char *host, uint16_t port, const char *headers, const char *content) {
    uint32_t ts = micros();
    M5S_DBG("\n> [%d] httpRequest(%s, %d, ...)\n", ts, host, port);

    WiFiClientSecure client;

    if (!client.connect(host, port)) {
        return {503, "Service unavailable (unable to connect)"};
    }

    /*
     * Send HTTP request
     */

    M5S_DBG("  [%d] Request:\n%s%s\n", ts, headers, content);
    eventsSendLog(">>>> REQUEST");
    eventsSendLog(headers);
    eventsSendLog(content);

    client.print(headers);
    if (strlen(content)) {
        client.print(content);
    }

    /*
     * Get HTTP response
     */

    uint32_t timeout = millis();
    while (!client.available()) {
        if (millis() - timeout > 5000) {
            client.stop();
            return {503, "Service unavailable (timeout)"};
        }
        yield();
    }

    M5S_DBG("  [%d] Response:\n", ts);
    eventsSendLog("<<<< RESPONSE");

    HTTP_response_t response = {0, ""};
    boolean EOH = false;
    uint32_t contentLength = 0;
    uint16_t buffSize = 1024;
    uint32_t readSize = 0;
    uint32_t totatlReadSize = 0;
    uint32_t lastAvailableMillis = millis();
    char buff[buffSize];

    // !HERE
    // client.setNoDelay(false);

    while (client.connected()) {
        int availableSize = client.available();
        if (availableSize) {
            lastAvailableMillis = millis();

            if (!EOH) {
                // Read response headers
                readSize = client.readBytesUntil('\n', buff, buffSize);
                buff[readSize - 1] = '\0'; // replace /r by \0
                M5S_DBG("%s\n", buff);
                eventsSendLog(buff);
                if (startsWith(buff, "HTTP/1.")) {
                    buff[12] = '\0';
                    response.httpCode = atoi(&buff[9]);
                    if (response.httpCode == 204) {
                        break;
                    }
                } else if (startsWith(buff, "Content-Length:")) {
                    contentLength = atoi(&buff[16]);
                    if (contentLength == 0) {
                        break;
                    }
                    response.payload.reserve(contentLength + 1);
                } else if (buff[0] == '\0') {
                    // End of headers
                    EOH = true;
                    M5S_DBG("<EOH>\n");
                    eventsSendLog("");
                }
            } else {
                // Read response content
                readSize = client.readBytes(buff, min(buffSize - 1, availableSize));
                buff[readSize] = '\0';
                M5S_DBG(buff);
                eventsSendLog(buff, log_raw);
                response.payload += buff;
                totatlReadSize += readSize;
                if (totatlReadSize >= contentLength) {
                    break;
                }
            }
        } else {
            if ((millis() - lastAvailableMillis) > 5000) {
                response = {504, "Response timeout"};
                break;
            }
            delay(100);
        }
    }
    client.stop();

    M5S_DBG("\n< [%d] HEAP: %d\n", ts, ESP.getFreeHeap());

    return response;
}


/**
 * Call Spotify API
 *
 * @param method
 * @param endpoint
 * @return
 */
HTTP_response_t sptfApiRequest(const char *method, const char *endpoint, const char *content) {
    uint32_t ts = micros();
    M5S_DBG("\n> [%d] sptfApiRequest(%s, %s, %s)\n", ts, method, endpoint, content);

    char headers[512];
    snprintf(headers, sizeof(headers),
             "%s /v1/me/player%s HTTP/1.1\r\n"
             "Host: api.spotify.com\r\n"
             "Authorization: Bearer %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             method, endpoint, access_token.c_str(), strlen(content)
    );

    return httpRequest("api.spotify.com", 443, headers, content);
}


/**
 * Get Spotify token
 *
 * @param code          Either an authorization code or a refresh token
 * @param grant_type    [gt_authorization_code|gt_refresh_token]
 */
void sptfGetToken(const String &code, GrantTypes grant_type) {
    uint32_t ts = micros();
    M5S_DBG("\n> [%d] sptfGetToken(%s, %s)\n", ts, code.c_str(), grant_type == gt_authorization_code ? "authorization" : "refresh");

    bool success = false;

    char requestContent[512];
    if (grant_type == gt_authorization_code) {
        snprintf(requestContent, sizeof(requestContent),
                 "grant_type=authorization_code"
                 "&redirect_uri=http%%3A%%2F%%2Fm5spot.local%%2Fcallback%%2F"
                 "&code=%s",
                 code.c_str()
        );
    } else {
        snprintf(requestContent, sizeof(requestContent),
                 "grant_type=refresh_token&refresh_token=%s",
                 code.c_str()
        );
    }

    uint8_t basicAuthSize = sizeof(SPTF_CLIENT_ID) + sizeof(SPTF_CLIENT_SECRET);
    char basicAuth[basicAuthSize];
    snprintf(basicAuth, basicAuthSize, "%s:%s", SPTF_CLIENT_ID, SPTF_CLIENT_SECRET);

    char requestHeaders[768];
    snprintf(requestHeaders, sizeof(requestHeaders),
             "POST /api/token HTTP/1.1\r\n"
             "Host: accounts.spotify.com\r\n"
             "Authorization: Basic %s\r\n"
             "Content-Length: %d\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Connection: close\r\n\r\n",
             b64Encode(basicAuth).c_str(), strlen(requestContent)
    );

    HTTP_response_t response = httpRequest("accounts.spotify.com", 443, requestHeaders, requestContent);

    if (response.httpCode == 200) {

        DynamicJsonBuffer jsonInBuffer(572);
        //StaticJsonBuffer<572> jsonInBuffer;
        JsonObject &json = jsonInBuffer.parseObject(response.payload);

        if (json.success()) {
            access_token = json["access_token"].as<String>();
            if (access_token != "") {
                token_lifetime_ms = (json["expires_in"].as<uint32_t>() - 300) * 1000;
                token_millis = millis();
                success = true;
                if (json.containsKey("refresh_token")) {
                    refresh_token = json["refresh_token"].as<String>();
                    writeRefreshToken();
                }
            }
        } else {
            M5S_DBG("  [%d] Unable to parse response payload:\n  %s\n", ts, response.payload.c_str());
            eventsSendError(500, "Unable to parse response payload", response.payload.c_str());
        }
    } else {
        M5S_DBG("  [%d] %d - %s\n", ts, response.httpCode, response.payload.c_str());
        eventsSendError(response.httpCode, "Spotify error", response.payload.c_str());
    }

    if (success) {
        sptfAction = CurrentlyPlaying;
    }

    getting_token = false;
}


/**
 * Get information about the Spotify user's current playback
 */
void sptfCurrentlyPlaying() {
    uint32_t ts = micros();
    M5S_DBG("\n> [%d] sptfCurrentlyPlaying()\n", ts);

    last_curplay_millis = millis();
    next_curplay_millis = 0;

    HTTP_response_t response = sptfApiRequest("GET", "/currently-playing");

    if (response.httpCode == 200) {

        DynamicJsonBuffer jsonBuffer(5120);

        JsonObject &json = jsonBuffer.parse(response.payload);

        if (json.success()) {
            sptf_is_playing = json["is_playing"];
            uint32_t progress_ms = json["progress_ms"];
            uint32_t duration_ms = json["item"]["duration_ms"];

            // Check if current song is about to end
            if (sptf_is_playing) {
                uint32_t remaining_ms = duration_ms - progress_ms;
                if (remaining_ms < SPTF_POLLING_DELAY) {
                    // Refresh at the end of current song,
                    // without considering remaining polling delay
                    next_curplay_millis = millis() + remaining_ms + 200;
                }
            }

            // Get song ID
            const char *id = json["item"]["id"];
            static char previousId[32] = {0};

            // If song has changed, refresh display
            if (strcmp(id, previousId) != 0) {
                strncpy(previousId, id, sizeof(previousId));

                // Display album art
                sptfDisplayAlbumArt(json["item"]["album"]["images"][1]["url"]);

                // Display song name
                M5.Lcd.fillRect(0, 0, 320, 30, 0xffffff);
                M5.Lcd.setTextColor(BLACK);
                M5.Lcd.setTextFont(2);
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextDatum(TC_DATUM);
                M5.Lcd.drawString(json["item"]["name"].as<String>(), 160, 2);

                // Display artists names
                JsonArray &arr = json["item"]["artists"];
                String artists;
                artists.reserve(150);
                bool first = true;
                for (auto &a : arr) {
                    if (first) {
                        artists += a["name"].as<String>();
                        first = false;
                    } else {
                        artists += ", ";
                        artists += a["name"].as<String>();
                    }
                }
                M5.Lcd.setTextFont(1);
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextDatum(BC_DATUM);
                M5.Lcd.drawString(artists, 160, 28);

                // Display progress bar background
                M5.Lcd.fillRect(0, 235, 320, 5, WHITE);

            }

            M5.Lcd.fillRect(0, 235, ceil((float) 320 * ((float) progress_ms / duration_ms)), 5, sptf_green);

        } else {
            M5S_DBG("  [%d] Unable to parse response payload:\n  %s\n", ts, response.payload.c_str());
            eventsSendError(500, "Unable to parse response payload", response.payload.c_str());
        }
    } else if (response.httpCode == 204) {
        // No content
    } else {
        M5S_DBG("  [%d] %d - %s\n", ts, response.httpCode, response.payload.c_str());
        eventsSendError(response.httpCode, "Spotify error", response.payload.c_str());
    }

    M5S_DBG("< [%d] HEAP: %d\n", ts, ESP.getFreeHeap());
}

/**
 * Spotify next track
 */
void sptfNext() {
    HTTP_response_t response = sptfApiRequest("POST", "/next");
    if (response.httpCode == 204) {
        next_curplay_millis = millis() + 200;
    } else {
        eventsSendError(response.httpCode, "Spotify error", response.payload.c_str());
    }
    sptfAction = CurrentlyPlaying;
};


/**
 * Spotify previous track
 */
void sptfPrevious() {
    HTTP_response_t response = sptfApiRequest("POST", "/previous");
    if (response.httpCode == 204) {
        next_curplay_millis = millis() + 200;
    } else {
        eventsSendError(response.httpCode, "Spotify error", response.payload.c_str());
    }
    sptfAction = CurrentlyPlaying;
};


/**
 * Spotify toggle pause/play
 */
void sptfToggle() {
    HTTP_response_t response = sptfApiRequest("PUT", sptf_is_playing ? "/pause" : "/play");
    if (response.httpCode == 204) {
        sptf_is_playing = !sptf_is_playing;
        next_curplay_millis = millis() + 200;
    } else {
        eventsSendError(response.httpCode, "Spotify error", response.payload.c_str());
    }
    sptfAction = CurrentlyPlaying;
};


#ifdef WITH_APDS9960
/**
 * Interrupt routine
 */
void IRAM_ATTR interruptRoutine() {
    isr_flag = 1;
}

/**
 * Gesture handler
 */
void handleGesture() {

    HTTP_response_t response;

    if (apds.isGestureAvailable()) {
        switch (apds.readGesture()) {
            case DIR_UP:
                Serial.println("> Gesture UP");
                response = sptfApiRequest("PUT", sptf_is_playing ? "/pause" : "/play");
                if (response.httpCode == 204) {
                    sptf_is_playing = !sptf_is_playing;
                }
                break;
            case DIR_DOWN:
                Serial.println("> Gesture DOWN");
                response = sptfApiRequest("PUT", sptf_is_playing ? "/pause" : "/play");
                if (response.httpCode == 204) {
                    sptf_is_playing = !sptf_is_playing;
                }
                break;
            case DIR_LEFT:
                Serial.println("> Gesture LEFT");
                response = sptfApiRequest("POST", "/previous");
                if (response.httpCode == 204) {
                    next_curplay_millis = millis() + 200;
                }
                break;
            case DIR_RIGHT:
                Serial.println("> Gesture RIGHT");
                response = sptfApiRequest("POST", "/next");
                if (response.httpCode == 204) {
                    next_curplay_millis = millis() + 200;
                }
                break;
            case DIR_NEAR:
                Serial.println("> Gesture NEAR");
                break;
            case DIR_FAR:
                Serial.println("> Gesture FAR");
                break;
            default:
                Serial.println("> Gesture NONE");
        }
    }
}
#endif

/**
 * Display bytes in a pretty format
 *
 * @param Bytes value
 * @return Bytes value prettified
 */
String prettyBytes(uint32_t bytes) {

    const char *suffixes[7] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    uint8_t s = 0;
    double count = bytes;

    while (count >= 1024 && s < 7) {
        s++;
        count /= 1024;
    }
    if (count - floor(count) == 0.0) {
        return String((int) count) + suffixes[s];
    } else {
        return String(round(count * 10.0) / 10.0, 1) + suffixes[s];
    };
}

/**
 * Display error message and stop execution
 *
 * @param errMsg
 */
void m5sEpitaph(const char *errMsg) {
    M5.Lcd.setFreeFont(&FreeSans12pt7b);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(CC_DATUM);
    M5.Lcd.fillScreen(RED);
    M5.Lcd.drawString(errMsg, 160, 120);
    while (true) {
        ArduinoOTA.handle();
        yield();
    }
}