#ifndef M5SPOT_MAIN_H
#define M5SPOT_MAIN_H

#define min(X, Y) (((X)<(Y))?(X):(Y))
#define startsWith(STR, SEARCH) (strncmp(STR, SEARCH, strlen(SEARCH)) == 0)

//@formatter:off
#ifdef DEBUG_M5SPOT
#define M5S_DBG(...) Serial.printf( __VA_ARGS__ )
#else
#define M5S_DBG(...)
#endif
//@formatter:on

typedef struct {
    int httpCode;
    String payload;
} HTTP_response_t;

enum SptfActions {
    Iddle, GetToken, CurrentlyPlaying, Next, Previous, Toggle
};

enum GrantTypes {
    gt_authorization_code, gt_refresh_token
};

enum EventsLogTypes {
    log_line, log_raw
};

typedef struct {
    const char *ssid;
    const char *passphrase;
} APlist_t;


/*
 * Function declarations
 */
//@formatter:off
void progressBar(uint8_t y, uint8_t val, uint16_t width = 200, uint16_t height = 7, uint16_t color = WHITE);

void eventsSendLog(const char *logData, EventsLogTypes type = log_line);
void eventsSendInfo(const char *msg, const char* payload = "");
void eventsSendError(int code, const char *msg, const char *payload = "");

HTTP_response_t httpRequest(const char *host, uint16_t port, const char *headers, const char *content = "");
HTTP_response_t sptfApiRequest(const char *method, const char *endpoint, const char *content = "");
void sptfGetToken(const String &code, GrantTypes grant_type = gt_refresh_token);
void sptfCurrentlyPlaying();
void sptfNext();
void sptfPrevious();
void sptfToggle();
void sptfDisplayAlbumArt(String url);

void writeRefreshToken();
void deleteRefreshToken();
String readRefreshToken();

void handleGesture();
void IRAM_ATTR interruptRoutine();

void m5sEpitaph(const char *errMsg);
String b64Encode(String str);
String prettyBytes(uint32_t bytes);
//@formatter:on

#endif // M5SPOT_MAIN_H
