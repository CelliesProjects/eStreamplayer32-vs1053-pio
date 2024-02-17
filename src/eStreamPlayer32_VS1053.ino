#include <Arduino.h>
#include <FFat.h>
#include <WiFi.h>
#include <AsyncTCP.h>          /* use the esphome.io fork*/
#include <ESPAsyncWebServer.h> /* use the esphome.io fork*/
#include <ESP32_VS1053_Stream.h>

#include "secrets.h" /* Untracked file containing the WiFi credentials*/

#include "playList.h"
#include "index_htm_gz.h"
#include "icons.h"

const char PROGRAM_NAME[] = "eStreamPlayer";

void startNextItem();
void playlistHasEnded();
void websocketEventHandler(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleSingleFrame(AsyncWebSocketClient *client, uint8_t *data, size_t len);
void handleMultiFrame(AsyncWebSocketClient *client, uint8_t *data, size_t len, AwsFrameInfo *info);
String percentEncode(const char *plaintext);

#ifdef ST7789_TFT
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

struct st7789Message
{
    enum action
    {
        SYSTEM_MESSAGE,
        PROGRESS_BAR,
        CLEAR_SCREEN,
        SHOW_STATION,
        SHOW_TITLE,
        SHOW_IPADDRESS
    };
    action action;
    char str[256];
    size_t value1 = 0;
    size_t value2 = 0;
};
static QueueHandle_t st7789Queue = NULL;
#endif

struct playerMessage
{
    enum action
    {
        SET_VOLUME,
        CONNECTTOHOST,
        STOPSONG,
        SETTONE
    };
    action action;
    char url[PLAYLIST_MAX_URL_LENGTH];
    size_t value = 0;
};
static QueueHandle_t playerQueue = NULL;

static SemaphoreHandle_t spiMutex;
static playList_t playList;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

static const char *FAVORITES_FOLDER = "/"; /* if this is a folder use a closing slash */
static const char *VOLUME_HEADER = "volume";
static const char *MESSAGE_HEADER = "message";
static const char *CURRENT_HEADER = "currentPLitem";

static auto _playerVolume = VS1053_INITIALVOLUME;
static size_t _savedPosition = 0;
static size_t _currentSize = 0;
static bool _paused = false;
static bool _codecRunning = false;

constexpr const auto NUMBER_OF_PRESETS = sizeof(preset) / sizeof(source);

#ifdef ST7789_TFT
//****************************************************************************************
//                                   S T 7 7 8 9 _ T A S K                             *
//****************************************************************************************

double map_range(const double input,
                 const double input_start, const double input_end,
                 const double output_start, const double output_end)
{
    const double input_range = input_end - input_start;
    const double output_range = output_end - output_start;
    return (input - input_start) * (output_range / input_range) + output_start;
}

void st7789Task(void *parameter)
{
    const auto BACKGROUND_COLOR = ST77XX_YELLOW;
    const auto LEDC_CHANNEL = 0;
    const auto LEDC_MAX_PWM_VALUE = (1 << SOC_LEDC_TIMER_BIT_WIDE_NUM) - 1;

    ledcSetup(LEDC_CHANNEL, 1220, SOC_LEDC_TIMER_BIT_WIDE_NUM);
    ledcAttachPin(TFT_BACKLITE, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, LEDC_MAX_PWM_VALUE >> (SOC_LEDC_TIMER_BIT_WIDE_NUM / 3));

    // turn on the TFT / I2C power supply
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    delay(10);

    xSemaphoreTake(spiMutex, portMAX_DELAY);
    static Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
    tft.init(135, 240, SPI_MODE0);
    tft.setRotation(1);
    tft.setFont(&FreeSansBold9pt7b);
    tft.fillScreen(BACKGROUND_COLOR);
    tft.setTextColor(ST77XX_BLACK, BACKGROUND_COLOR);
    tft.setTextSize(1);
    tft.setCursor(2, 14);
    tft.setTextWrap(false);
    tft.printf("%s\n%s\nbooting...", PROGRAM_NAME, GIT_VERSION);
    xSemaphoreGive(spiMutex);

    while (!_codecRunning)
    {
        delay(100);
    }

    static GFXcanvas16 canvas(240, 20);
    static int16_t strX, strY;
    static uint16_t strWidth, strHeight;

    static char streamTitle[264];    
    static bool streamTitleIsEmpty;
    static int16_t streamTitleOffset = 0;

    while (1)
    {
        static st7789Message msg = {};
        if (xQueueReceive(st7789Queue, &msg, pdTICKS_TO_MS(25)) == pdTRUE)
        {
            xSemaphoreTake(spiMutex, portMAX_DELAY);
            switch (msg.action)
            {
            case st7789Message::SYSTEM_MESSAGE:
                tft.setFont();
                tft.setTextSize(1);
                tft.setCursor(1, 1);
                tft.print(msg.str);
                break;
            case st7789Message::PROGRESS_BAR:
            {
                const int HEIGHT_IN_PIXELS = 20;
                const int HEIGHT_OFFSET = 64;
                int16_t FILLED_AREA = map_range(msg.value1, 0, msg.value2, 0, tft.width() + 1);
                tft.fillRect(0, HEIGHT_OFFSET, FILLED_AREA, HEIGHT_IN_PIXELS, ST77XX_BLUE);
                tft.fillRect(FILLED_AREA, HEIGHT_OFFSET, tft.width() - FILLED_AREA, HEIGHT_IN_PIXELS, ST77XX_WHITE);
                break;
            }
            case st7789Message::CLEAR_SCREEN:
                streamTitle[0] = 0;
                streamTitleOffset = 0;
                tft.fillScreen(BACKGROUND_COLOR);
                break;
            case st7789Message::SHOW_STATION:
                tft.setTextSize(1);
                tft.setTextColor(ST77XX_BLACK);
                tft.setFont(&FreeSansBold9pt7b);
                tft.setCursor(5, 34);
                tft.print(msg.str);
                break;
            case st7789Message::SHOW_TITLE:
                streamTitleOffset = 0;
                streamTitleIsEmpty = msg.str[0] ? false : true;
                snprintf(streamTitle, sizeof(streamTitle), "%s", msg.str);
                tft.setTextSize(1);
                tft.setFont(&FreeSansBold9pt7b);
                tft.getTextBounds(streamTitle, 0, 0, &strX, &strY, &strWidth, &strHeight);
                break;
            case st7789Message::SHOW_IPADDRESS:
            {
                tft.setFont(&FreeSansBold18pt7b);
                tft.setTextSize(1);
                tft.setTextColor(ST77XX_BLUE);
                static int16_t xpos;
                static int16_t ypos;
                static uint16_t height;
                static uint16_t width;
                tft.getTextBounds(WiFi.localIP().toString().c_str(), 0, 0, &xpos, &ypos, &width, &height);
                tft.setCursor((tft.width() / 2) - (width / 2), 65);
                tft.print(WiFi.localIP().toString().c_str());
                break;
            }
            default:
                log_w("unhandled st7789 msg type");
            }
            xSemaphoreGive(spiMutex);
        }

        if (streamTitle[0] || streamTitleIsEmpty)
        {
            const auto Y_POSITION = 64;

            canvas.fillScreen(0);
            canvas.setCursor(canvas.width() - streamTitleOffset, canvas.height() - 6);
            canvas.setFont(&FreeSansBold9pt7b);
            canvas.setTextSize(1);
            canvas.setTextWrap(false);
            canvas.print(streamTitle);

            xSemaphoreTake(spiMutex, portMAX_DELAY);
            tft.drawRGBBitmap(0, Y_POSITION, canvas.getBuffer(), canvas.width(), canvas.height());
            xSemaphoreGive(spiMutex);

            streamTitleOffset = (streamTitleOffset < (canvas.width() + strWidth)) ? streamTitleOffset + 2 : 0;
            streamTitleIsEmpty = false;
        }
    }
}
#endif
//****************************************************************************************
//                                   P L A Y E R _ T A S K                               *
//****************************************************************************************

void playerTask(void *parameter)
{
    log_i("Starting VS1053 codec...");

#if defined(VS1053_RST_PIN)

    /* the RESET or ENABLE pin is not exposed on the ESP32-S3 BOX */
    /* so we use a gpio pin to connect to XRST on the VS1053 */
    /* and set this pin to high to enable the codec chip */

    pinMode(VS1053_RST_PIN, OUTPUT);
    digitalWrite(VS1053_RST_PIN, HIGH);
#endif

    static ESP32_VS1053_Stream audio;

    xSemaphoreTake(spiMutex, portMAX_DELAY);
    if (!audio.startDecoder(VS1053_CS_PIN, VS1053_DCS_PIN, VS1053_DREQ_PIN) || !audio.isChipConnected())
    {
        log_e("VS1053 board could not init\nSystem halted");
        while (true)
            delay(100);
    }
    xSemaphoreGive(spiMutex);

    log_d("Heap: %d", ESP.getHeapSize());
    log_d("Free: %d", ESP.getFreeHeap());
    log_d("PSRAM: %d", ESP.getPsramSize());
    log_d("Free: %d", ESP.getFreePsram());

    log_i("Ready to rock!");

    _codecRunning = true;

    while (true)
    {
        playerMessage msg;
        if (xQueueReceive(playerQueue, &msg, pdMS_TO_TICKS(5)) == pdTRUE)
        {
            xSemaphoreTake(spiMutex, portMAX_DELAY);
            switch (msg.action)
            {
            case playerMessage::SET_VOLUME:
                audio.setVolume(msg.value);
                break;
            case playerMessage::CONNECTTOHOST:
                audio.stopSong();
                _paused = false;
                ws.textAll("status\nplaying\n");

#ifdef ST7789_TFT
                {
                    auto offset = msg.value;

                    st7789Message msg;
                    if (!offset)
                    {
                        msg.action = st7789Message::CLEAR_SCREEN;
                        xQueueSend(st7789Queue, &msg, portMAX_DELAY);
                    }
                    playListItem item;
                    playList.get(playList.currentItem(), item);

                    switch (item.type)
                    {
                    case HTTP_FILE:
                        snprintf(msg.str, sizeof(msg.str), "%s", item.url.substring(item.url.lastIndexOf("/") + 1).c_str());
                        break;
                    case HTTP_FOUND:
                        snprintf(msg.str, sizeof(msg.str), "%s", item.name.c_str());
                        break;
                    case HTTP_FAVORITE:
                        snprintf(msg.str, sizeof(msg.str), "%s", item.name.c_str());
                        break;
                    case HTTP_PRESET:
                        snprintf(msg.str, sizeof(msg.str), "%s", preset[item.index].name.c_str());
                        break;
                    default:
                        log_w("unhandled type");
                    }

                    msg.action = st7789Message::SHOW_STATION;
                    xQueueSend(st7789Queue, &msg, portMAX_DELAY);
                }
#endif

                xSemaphoreGive(spiMutex); // connecttohost() does not touch the spi bus

                if (!audio.connecttohost(msg.url, LIBRARY_USER, LIBRARY_PWD, msg.value))
                    startNextItem();

                xSemaphoreTake(spiMutex, portMAX_DELAY);

                _currentSize = audio.size();
                _savedPosition = audio.position();
                break;
            case playerMessage::STOPSONG:
                audio.stopSong();
                break;
            default:
                log_e("error: unhandled audio action: %i", msg.action);
            }
            xSemaphoreGive(spiMutex);
        }

        constexpr const auto MAX_UPDATE_FREQ_HZ = 3;
        constexpr const auto UPDATE_INTERVAL_MS = 1000 / MAX_UPDATE_FREQ_HZ;
        static unsigned long savedTime = millis();

        if (audio.size() && millis() - savedTime > UPDATE_INTERVAL_MS && audio.position() != _savedPosition)
        {
            log_d("Buffer status: %s", audio.bufferStatus());

#ifdef ST7789_TFT
            st7789Message msg;
            msg.value1 = audio.position();
            msg.value2 = audio.size();
            msg.action = st7789Message::PROGRESS_BAR;
            xQueueSend(st7789Queue, &msg, portMAX_DELAY);
#endif

            ws.printfAll("progress\n%i\n%i\n", audio.position(), audio.size());
            savedTime = millis();
            _savedPosition = audio.position();
        }
        xSemaphoreTake(spiMutex, portMAX_DELAY);
        audio.loop();
        xSemaphoreGive(spiMutex);
    }
}

//****************************************************************************************
//                                   H E L P E R - R O U T I N E S                       *
//****************************************************************************************

inline __attribute__((always_inline)) void updateCurrentItemOnClients()
{
    ws.printfAll("%s\n%i\n", CURRENT_HEADER, playList.currentItem());
}

void startItem(uint8_t const index, size_t offset = 0)
{
    updateCurrentItemOnClients();
    audio_showstreamtitle("");

    playerMessage msg;
    msg.action = playerMessage::CONNECTTOHOST;
    msg.value = offset;
    snprintf(msg.url, sizeof(msg.url), "%s", playList.url(index).c_str());
    xQueueSend(playerQueue, &msg, portMAX_DELAY);

    playListItem item;
    playList.get(index, item);
    switch (item.type)
    {
    case HTTP_FILE:
    {
        char name[item.url.length() - item.url.lastIndexOf('/')];
        auto pos = item.url.lastIndexOf('/') + 1;
        auto cnt = 0;
        while (pos < item.url.length())
            name[cnt++] = item.url.charAt(pos++);
        name[cnt] = 0;
        audio_showstation(name);
    }
    break;
    case HTTP_PRESET:
        audio_showstation(preset[item.index].name.c_str());
        break;
    default:
        audio_showstation(item.name.c_str());
    }
}

void startNextItem()
{
    if (playList.currentItem() < playList.size() - 1)
    {
        playList.setCurrentItem(playList.currentItem() + 1);
        startItem(playList.currentItem());
        return;
    }
    playlistHasEnded();
}

void playlistHasEnded()
{
    playList.setCurrentItem(PLAYLIST_STOPPED);
    audio_showstreamtitle("Search API provided by: <a href=\"https://www.radio-browser.info/\" target=\"_blank\"><span style=\"white-space:nowrap;\">radio-browser.info</span></a>");
    updateCurrentItemOnClients();
    char versionString[50];
    snprintf(versionString, sizeof(versionString), "%s %s", PROGRAM_NAME, GIT_VERSION);
    audio_showstation(versionString);

#ifdef ST7789_TFT
    {
        st7789Message msg;
        msg.action = st7789Message::CLEAR_SCREEN;
        xQueueSend(st7789Queue, &msg, portMAX_DELAY);
        msg.action = st7789Message::SHOW_IPADDRESS;
        xQueueSend(st7789Queue, &msg, portMAX_DELAY);
    }
#endif
}

void upDatePlaylistOnClients()
{
    {
        String s;
        ws.textAll(playList.toString(s));
    }
    updateCurrentItemOnClients();
}

bool saveItemToFavorites(AsyncWebSocketClient *client, const char *filename, const playListItem &item)
{
    if (!strlen(filename))
    {
        log_e("ERROR! no filename");
        return false;
    }
    switch (item.type)
    {
    case HTTP_FILE:
        log_d("file (wont save)%s", item.url.c_str());
        return false;
    case HTTP_PRESET:
        log_d("preset (wont save) %s %s", preset[item.index].name.c_str(), preset[item.index].url.c_str());
        return false;
    case HTTP_FOUND:
    case HTTP_FAVORITE:
    {
        char path[strlen(FAVORITES_FOLDER) + strlen(filename) + 1];
        snprintf(path, sizeof(path), "%s%s", FAVORITES_FOLDER, filename);
        File file = FFat.open(path, FILE_WRITE);
        if (!file)
        {
            log_e("failed to open '%s' for writing", filename);
            client->printf("%s\nERROR: Could not open '%s' for writing!", MESSAGE_HEADER, filename);
            return false;
        }
        char url[item.url.length() + 2];
        snprintf(url, sizeof(url), "%s\n", item.url.c_str());
        const auto bytesWritten = file.print(url);
        file.close();
        if (bytesWritten < strlen(url))
        {
            log_e("ERROR! Saving '%s' failed - disk full?", filename);
            client->printf("%s\nERROR: Could not completely save '%s' to favorites!", MESSAGE_HEADER, filename);
            return false;
        }
        client->printf("%s\nSaved '%s' to favorites!", MESSAGE_HEADER, filename);
        return true;
    }
    break;
    default:
    {
        log_w("Unhandled item.type.");
        return false;
    }
    }
}

void handleFavoriteToPlaylist(AsyncWebSocketClient *client, const char *filename, const bool startNow)
{
    if (PLAYLIST_MAX_ITEMS == playList.size())
    {
        log_e("ERROR! Could not add %s to playlist", filename);
        client->printf("%s\nERROR: Could not add '%s' to playlist", MESSAGE_HEADER, filename);
        return;
    }
    char path[strlen(FAVORITES_FOLDER) + strlen(filename) + 1];
    snprintf(path, sizeof(path), "%s%s", FAVORITES_FOLDER, filename);
    File file = FFat.open(path);
    if (!file)
    {
        log_e("ERROR! Could not open %s", filename);
        client->printf("%s\nERROR: Could not add '%s' to playlist", MESSAGE_HEADER, filename);
        return;
    }
    char url[file.size() + 1];
    auto cnt = 0;
    char ch = (char)file.read();
    while (ch != '\n' && file.available())
    {
        url[cnt++] = ch;
        ch = (char)file.read();
    }
    url[cnt] = 0;
    file.close();
    const auto previousSize = playList.size();
    playList.add({HTTP_FAVORITE, filename, url, 0});

    log_d("favorite to playlist: %s -> %s", filename, url);
    client->printf("%s\nAdded '%s' to playlist", MESSAGE_HEADER, filename);

    if (startNow || playList.currentItem() == PLAYLIST_STOPPED)
    {
        playList.setCurrentItem(previousSize);
        startItem(playList.currentItem());
    }
}

const String &favoritesToString(String &s)
{
    s = "favorites\n";
    File folder = FFat.open(FAVORITES_FOLDER);
    if (!folder)
    {
        log_e("ERROR! Could not open favorites folder");
        return s;
    }
    File file = folder.openNextFile();
    while (file)
    {
        if (!file.isDirectory() && file.size() < PLAYLIST_MAX_URL_LENGTH)
        {
            s.concat(file.name());
            s.concat("\n");
        }
        file = folder.openNextFile();
    }
    return s;
}

const String &favoritesToCStruct(String &s)
{
    File folder = FFat.open(FAVORITES_FOLDER);
    if (!folder)
    {
        s = "ERROR! Could not open folder " + String(FAVORITES_FOLDER);
        return s;
    }
    s = "const source preset[] = {\n";
    File file = folder.openNextFile();
    while (file)
    {
        if (!file.isDirectory() && file.size() < PLAYLIST_MAX_URL_LENGTH)
        {
            s.concat("    {\"");
            s.concat(file.name());
            s.concat("\", \"");
            char ch = (char)file.read();
            while (file.available() && ch != '\n')
            {
                s.concat(ch);
                ch = (char)file.read();
            }
            s.concat("\"},\n");
        }
        file = folder.openNextFile();
    }
    s.concat("};\n");
    return s;
}

//****************************************************************************************
//                                   S E T U P                                           *
//****************************************************************************************

const char *HEADER_MODIFIED_SINCE = "If-Modified-Since";

static inline __attribute__((always_inline)) bool htmlUnmodified(const AsyncWebServerRequest *request, const char *date)
{
    return request->hasHeader(HEADER_MODIFIED_SINCE) && request->header(HEADER_MODIFIED_SINCE).equals(date);
}

// cppcheck-suppress unusedFunction
void setup()
{
    SPI.setHwCs(true);
    SPI.begin(SPI_CLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    spiMutex = xSemaphoreCreateMutex(); // outside the ST77*9 area so the SPI can continue to lock/unlock for vs1053 when running on another board

#ifdef ST7789_TFT
    st7789Queue = xQueueCreate(5, sizeof(struct st7789Message));

    if (!st7789Queue)
    {
        log_e("FATAL error! could not create feather queue HALTED!");
        while (1)
            delay(100);
    }
    log_i("starting tft");

    const BaseType_t result1 = xTaskCreatePinnedToCore(
        st7789Task,           /* Function to implement the task */
        "st7789Task",         /* Name of the task */
        8000,                 /* Stack size in BYTES! */
        NULL,                 /* Task input parameter */
        tskIDLE_PRIORITY + 1, /* Priority of the task */
        NULL,                 /* Task handle. */
        1                     /* Core where the task should run */
    );

    if (result1 != pdPASS)
    {
        log_e("ERROR! Could not create st7789Task. System halted.");
        while (true)
            delay(100);
    }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S2) && ARDUHAL_LOG_LEVEL != ARDUHAL_LOG_LEVEL_NONE
    delay(3000);
    Serial.setDebugOutput(true);
#endif

    log_i("\n\n\t\t\t\t%s version: %s\n", PROGRAM_NAME, GIT_VERSION);

    [[maybe_unused]] const uint32_t idf = ESP_IDF_VERSION_PATCH + ESP_IDF_VERSION_MINOR * 10 + ESP_IDF_VERSION_MAJOR * 100;
    [[maybe_unused]] const uint32_t ard = ESP_ARDUINO_VERSION_PATCH + ESP_ARDUINO_VERSION_MINOR * 10 + ESP_ARDUINO_VERSION_MAJOR * 100;
    log_i("ESP32 IDF Version %d.%d.%d", idf / 100 % 10, idf / 10 % 10, idf % 10);
    log_i("ESP32 Arduino Version %d.%d.%d", ard / 100 % 10, ard / 10 % 10, ard % 10);
    log_i("CPU: %iMhz", getCpuFrequencyMhz());
    log_i("Found %i presets", NUMBER_OF_PRESETS);

    /* check if a ffat partition is defined and halt the system if it is not defined*/
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat"))
    {
        log_e("FATAL ERROR! No FFat partition defined. System is halted.\nCheck 'Tools>Partition Scheme' in the Arduino IDE and select a partition table with a FFat partition.");
        while (true)
            delay(1000); /* system is halted */
    }

    /* partition is defined - try to mount it */
    if (FFat.begin(0, "", 2)) // see: https://github.com/lorol/arduino-esp32fs-plugin#notes-for-fatfs
        log_i("FFat mounted");

    /* partition is present, but does not mount so now we just format it */
    else
    {
        log_i("Formatting FFat...");
        if (!FFat.format(true, (char *)"ffat") || !FFat.begin(0, "", 2))
        {
            log_e("FFat error while formatting. Halting.");
            while (true)
                delay(1000); /* system is halted */
        }
    }

    btStop();

    static IPAddress localip, gateway, subnet, primarydns;
    localip.fromString(STATIC_IP);
    gateway.fromString(GATEWAY);
    subnet.fromString(SUBNET);
    primarydns.fromString(PRIMARY_DNS);

    WiFi.useStaticBuffers(true);

    if (SET_STATIC_IP && !WiFi.config(localip, gateway, subnet, primarydns))
    {
        log_e("Setting static IP failed");
    }

    WiFi.begin(SSID_NAME, SSID_PASSWORD);
    WiFi.setSleep(false);

    log_i("Connecting to %s...", SSID_NAME);

    playerQueue = xQueueCreate(5, sizeof(struct playerMessage));

    if (!playerQueue)
    {
        log_e("Could not create queue. System halted.");
        while (true)
            delay(100);
    }

    WiFi.waitForConnectResult();

    if (!WiFi.isConnected())
    {
        log_e("Could not connect to Wifi! System halted! Check 'system_setup.h'!");
        while (true)
            delay(1000); /* system is halted */
    }

    log_i("WiFi connected - IP %s", WiFi.localIP().toString().c_str());

    configTzTime(TIMEZONE, NTP_POOL);

    struct tm timeinfo
    {
    };

    log_i("Waiting for NTP sync...");

    while (!getLocalTime(&timeinfo, 0))
        delay(10);

    log_i("Synced");

    //****************************************************************************************
    //                                   W E B S E R V E R                                   *
    //****************************************************************************************

    time_t bootTime;
    time(&bootTime);
    static char modifiedDate[30];
    strftime(modifiedDate, sizeof(modifiedDate), "%a, %d %b %Y %X GMT", gmtime(&bootTime));

    static const char *HTML_MIMETYPE{"text/html"};
    static const char *HEADER_LASTMODIFIED{"Last-Modified"};
    static const char *HEADER_CONTENT_ENCODING{"Content-Encoding"};
    static const char *GZIP_CONTENT_ENCODING{"gzip"};

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, HTML_MIMETYPE, index_htm_gz, index_htm_gz_len);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->addHeader(HEADER_CONTENT_ENCODING, GZIP_CONTENT_ENCODING);
        request->send(response); });

    server.on("/scripturl", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->println(SCRIPT_URL);
        if (strlen(LIBRARY_USER) || strlen(LIBRARY_PWD)) {
            response->println(LIBRARY_USER);
            response->println(LIBRARY_PWD);
        }
        request->send(response); });

    server.on("/stations", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        auto i = 0;
        while (i < NUMBER_OF_PRESETS)
            response->printf("%s\n", preset[i++].name.c_str());
        request->send(response); });

    server.on("/favorites", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        AsyncResponseStream* const response = request->beginResponseStream("text/plain");
        response->addHeader("Cache-Control", "no-cache,no-store,must-revalidate,max-age=0");
        String s;
        response->print(favoritesToCStruct(s));
        request->send(response); });

    static const char *SVG_MIMETYPE{"image/svg+xml"};

    server.on("/radioicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, radioicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/playicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, playicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/libraryicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, libraryicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/favoriteicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, favoriteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/streamicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, pasteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/deleteicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, deleteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/addfoldericon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, addfoldericon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/emptyicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, emptyicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/starticon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, starticon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/pauseicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, pauseicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/searchicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, searchicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.on("/nosslicon.svg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, nosslicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response); });

    server.onNotFound([](AsyncWebServerRequest *request)
                      {
        log_e("404 - Not found: 'http://%s%s'", request->host().c_str(), request->url().c_str());
        request->send(404); });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    playlistHasEnded();

    server.begin();
    ws.onEvent(websocketEventHandler);
    server.addHandler(&ws);
    log_i("Webserver started");

    const BaseType_t result = xTaskCreatePinnedToCore(
        playerTask,                                 /* Function to implement the task */
        "playerTask",                               /* Name of the task */
        8000,                                       /* Stack size in BYTES! */
        NULL,                                       /* Task input parameter */
        (tskIDLE_PRIORITY + 2) | portPRIVILEGE_BIT, /* Priority of the task */
        NULL,                                       /* Task handle. */
        0                                           /* Core where the task should run */
    );

    if (result != pdPASS)
    {
        log_e("ERROR! Could not create playerTask. System halted.");
        while (true)
            delay(100);
    }

    playerMessage msg;
    msg.action = playerMessage::SET_VOLUME;
    msg.value = VS1053_INITIALVOLUME;
    xQueueSend(playerQueue, &msg, portMAX_DELAY);

    vTaskDelete(NULL); // this deletes both setup() and loop() - see ~/.arduino15/packages/esp32/hardware/esp32/1.0.6/cores/esp32/main.cpp
}

//****************************************************************************************
//                                   L O O P                                             *
//****************************************************************************************

// cppcheck-suppress unusedFunction
void loop()
{
}

//*****************************************************************************************
//                                  E V E N T S                                           *
//*****************************************************************************************

#define MAX_STATION_NAME_LENGTH 200
static char showstation[MAX_STATION_NAME_LENGTH];
void audio_showstation(const char *info)
{
    playListItem item;
    playList.get(playList.currentItem(), item);
    snprintf(showstation, sizeof(showstation), "showstation\n%s\n%s", info, typeStr[item.type]);
    log_d("%s", showstation);
    ws.textAll(showstation);
}

#define MAX_METADATA_LENGTH 255
static char streamtitle[MAX_METADATA_LENGTH];
void audio_showstreamtitle(const char *info)
{
    snprintf(streamtitle, sizeof(streamtitle), "streamtitle\n%s", percentEncode(info).c_str());
    log_d("%s", streamtitle);
    ws.textAll(streamtitle);

#ifdef ST7789_TFT
    if (playList.currentItem() == PLAYLIST_STOPPED)
        return;

    st7789Message msg;
    snprintf(msg.str, sizeof(msg.str), "%s", info);
    msg.action = st7789Message::SHOW_TITLE;
    xQueueSend(st7789Queue, &msg, portMAX_DELAY);
#endif
}

void audio_eof_stream(const char *info)
{
    log_d("%s", info);
    startNextItem();
}
