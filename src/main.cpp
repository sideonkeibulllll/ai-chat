#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <Preferences.h>
#include "xfont.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_IRQ 36

#define SD_MISO 19
#define SD_MOSI 23
#define SD_SCLK 18
#define SD_CS 5

#define BACKLIGHT_PIN 21
#define BOOT_BUTTON_PIN 0
#define LDR_PIN 34

#define LED_RED 4
#define LED_GREEN 16
#define LED_BLUE 17

#define VDB_BUFFER_SIZE (SCREEN_WIDTH * 10)

#define BACKLIGHT_MAX 255
#define BACKLIGHT_MIN 10

#define WIFI_MAX_SCAN_RESULTS 6
#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASS_LEN 64

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI;
SPIClass sdSPI(VSPI);
XPT2046_Touchscreen* touchscreen = nullptr;

XFont *xFont = nullptr;

static lv_color_t buf1[VDB_BUFFER_SIZE];
static lv_color_t buf2[VDB_BUFFER_SIZE];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static int16_t lastTouchX = 0;
static int16_t lastTouchY = 0;

static bool sdCardDetected = false;

static TaskHandle_t lvglTaskHandle = nullptr;
static volatile bool lvglTaskRunning = false;

typedef enum {
    BACKLIGHT_MODE_MANUAL = 0,
    BACKLIGHT_MODE_AUTO = 1,
    BACKLIGHT_MODE_OFF = 2
} backlight_mode_t;

typedef enum {
    APP_NONE = 0,
    APP_FILE_BROWSER = 1,
    APP_SETTINGS = 2
} active_app_t;

typedef enum {
    WIFI_MODE_IDLE = 0,
    WIFI_MODE_CONNECTING,
    WIFI_MODE_CONNECTED,
    WIFI_MODE_FAILED
} wifi_app_mode_t;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int32_t rssi;
    wifi_auth_mode_t encryption;
    bool isOpen;
} wifi_scan_result_t;

static backlight_mode_t backlightMode = BACKLIGHT_MODE_MANUAL;
static uint8_t backlightLevel = BACKLIGHT_MAX;
static volatile bool bootButtonPressed = false;
static uint32_t lastActivityMs = 0;
static uint32_t bootButtonDebounceMs = 0;

static lv_obj_t* sidebar = nullptr;
static lv_obj_t* toggleBtn = nullptr;
static lv_obj_t* btnFile = nullptr;
static lv_obj_t* btnSettings = nullptr;
static bool sidebarOpen = false;

static active_app_t currentApp = APP_NONE;
static lv_obj_t* appScreen = nullptr;

static wifi_scan_result_t wifiScanResults[WIFI_MAX_SCAN_RESULTS];
static int wifiScanCount = 0;
static wifi_app_mode_t wifiAppState = WIFI_MODE_IDLE;
static char selectedSSID[WIFI_MAX_SSID_LEN];
static char wifiPassword[WIFI_MAX_PASS_LEN];
static bool wifiConnecting = false;
static int wifiConnectAttempts = 0;

static lv_obj_t* wifiList = nullptr;
static lv_obj_t* wifiStatusLabel = nullptr;
static lv_obj_t* wifiPasswordOverlay = nullptr;
static lv_obj_t* wifiTextarea = nullptr;
static lv_obj_t* wifiKeyboard = nullptr;
static lv_obj_t* wifiBtnScan = nullptr;

static lv_obj_t* settingsPerfLabel = nullptr;
static lv_obj_t* settingsBacklightSlider = nullptr;
static lv_obj_t* settingsBacklightLabel = nullptr;
static lv_obj_t* settingsModeLabel = nullptr;

static String currentPath = "/";
static lv_obj_t* fileList = nullptr;
static lv_obj_t* filePathLabel = nullptr;

void IRAM_ATTR bsp_display_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, false);
    tft.endWrite();
    
    lv_disp_flush_ready(disp);
}

void bsp_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    if (!touchscreen) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    
    if (touchscreen->tirqTouched() && touchscreen->touched()) {
        TS_Point p = touchscreen->getPoint();
        
        int16_t x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
        int16_t y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
        
        x = constrain(x, 0, SCREEN_WIDTH - 1);
        y = constrain(y, 0, SCREEN_HEIGHT - 1);
        
        lastTouchX = x;
        lastTouchY = y;
        
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
        
        lastActivityMs = millis();
    } else {
        data->point.x = lastTouchX;
        data->point.y = lastTouchY;
        data->state = LV_INDEV_STATE_REL;
    }
}

void bsp_backlight_set(uint8_t level) {
    ledcWrite(0, level);
    Serial.printf("[BL] Set backlight: %d\n", level);
}

void rgbLedSet(uint8_t r, uint8_t g, uint8_t b) {
    digitalWrite(LED_RED, r > 0 ? LOW : HIGH);
    digitalWrite(LED_GREEN, g > 0 ? LOW : HIGH);
    digitalWrite(LED_BLUE, b > 0 ? LOW : HIGH);
}

void rgbLedOff() {
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
}

void updateSettingsUI();

void setBacklightMode(backlight_mode_t mode) {
    backlightMode = mode;
    
    switch (mode) {
        case BACKLIGHT_MODE_MANUAL:
            bsp_backlight_set(backlightLevel > 0 ? backlightLevel : BACKLIGHT_MAX);
            break;
        case BACKLIGHT_MODE_AUTO:
            {
                uint16_t ldr = analogRead(LDR_PIN);
                uint8_t level = map(constrain(ldr, 100, 4000), 100, 4000, BACKLIGHT_MIN, BACKLIGHT_MAX);
                backlightLevel = level;
                bsp_backlight_set(level);
            }
            break;
        case BACKLIGHT_MODE_OFF:
            bsp_backlight_set(0);
            break;
    }
    
    updateSettingsUI();
    
    Serial.printf("[Power] Backlight: %s (%d)\n",
        mode == BACKLIGHT_MODE_MANUAL ? "MANUAL" :
        mode == BACKLIGHT_MODE_AUTO ? "AUTO" : "OFF", backlightLevel);
}

void cycleBacklightMode() {
    backlight_mode_t newMode;
    
    switch (backlightMode) {
        case BACKLIGHT_MODE_MANUAL:
            newMode = BACKLIGHT_MODE_AUTO;
            break;
        case BACKLIGHT_MODE_AUTO:
            newMode = BACKLIGHT_MODE_OFF;
            break;
        default:
            newMode = BACKLIGHT_MODE_MANUAL;
            backlightLevel = BACKLIGHT_MAX;
            break;
    }
    
    setBacklightMode(newMode);
}

void IRAM_ATTR bootButtonISR() {
    bootButtonPressed = true;
}

void handleBootButton() {
    if (bootButtonPressed) {
        bootButtonPressed = false;
        
        uint32_t now = millis();
        if (now - bootButtonDebounceMs < 500) {
            return;
        }
        bootButtonDebounceMs = now;
        
        cycleBacklightMode();
        lastActivityMs = millis();
    }
}

void lvglTaskEntry(void* arg) {
    lvglTaskRunning = true;
    while (lvglTaskRunning) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(NULL);
}

void startLvglTask() {
    if (lvglTaskHandle != nullptr) return;
    lvglTaskRunning = true;
    xTaskCreatePinnedToCore(lvglTaskEntry, "LVGL_Task", 8192, nullptr, 5, &lvglTaskHandle, 1);
}

void initSDCard() {
    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sdSPI)) {
        sdCardDetected = (SD.cardType() != CARD_NONE);
        if (sdCardDetected) {
            Serial.printf("[SD] Card: %llu MB\n", SD.cardSize() / (1024 * 1024));
        }
    }
}

bool initDisplay() {
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, VDB_BUFFER_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = bsp_display_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    return true;
}

bool initTouch() {
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    touchscreen = new XPT2046_Touchscreen(TOUCH_CS, TOUCH_IRQ);
    if (!touchscreen->begin(touchSPI)) return false;
    touchscreen->setRotation(1);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = bsp_touch_read;
    lv_indev_drv_register(&indev_drv);
    return true;
}

void initRGBLed() {
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    rgbLedOff();
}

void closeCurrentApp();

void toggle_sidebar_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (sidebarOpen) {
            lv_obj_set_pos(sidebar, -60, 20);
            lv_label_set_text(lv_obj_get_child(toggleBtn, 0), LV_SYMBOL_RIGHT);
            if (btnFile) lv_obj_add_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
            if (btnSettings) lv_obj_add_flag(btnSettings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(sidebar, 0, 20);
            lv_label_set_text(lv_obj_get_child(toggleBtn, 0), LV_SYMBOL_LEFT);
            if (btnFile) lv_obj_clear_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
            if (btnSettings) lv_obj_clear_flag(btnSettings, LV_OBJ_FLAG_HIDDEN);
        }
        sidebarOpen = !sidebarOpen;
    }
}

void settings_app_cb(lv_event_t *e);
void file_browse_cb(lv_event_t *e);

void createGlobalUI() {
    toggleBtn = lv_btn_create(lv_layer_top());
    lv_obj_set_size(toggleBtn, 50, 20);
    lv_obj_align(toggleBtn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(toggleBtn, toggle_sidebar_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(toggleBtn, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_border_width(toggleBtn, 0, 0);
    lv_obj_set_style_radius(toggleBtn, 0, 0);
    
    lv_obj_t *arrow = lv_label_create(toggleBtn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_center(arrow);
    lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
    
    sidebar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(sidebar, 60, SCREEN_HEIGHT - 20);
    lv_obj_set_pos(sidebar, -60, 20);
    lv_obj_set_style_bg_color(sidebar, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 5, 0);
    
    btnFile = lv_btn_create(sidebar);
    lv_obj_set_size(btnFile, 50, 30);
    lv_obj_align(btnFile, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_add_event_cb(btnFile, file_browse_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnFile, lv_color_make(0x60, 0x40, 0x00), 0);
    lv_obj_t* fileIcon = lv_label_create(btnFile);
    lv_label_set_text(fileIcon, LV_SYMBOL_LIST);
    lv_obj_center(fileIcon);
    lv_obj_set_style_text_color(fileIcon, lv_color_white(), 0);
    lv_obj_add_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
    
    btnSettings = lv_btn_create(sidebar);
    lv_obj_set_size(btnSettings, 50, 30);
    lv_obj_align(btnSettings, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_add_event_cb(btnSettings, settings_app_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnSettings, lv_color_make(0x00, 0x60, 0x80), 0);
    lv_obj_t* settingsIcon = lv_label_create(btnSettings);
    lv_label_set_text(settingsIcon, LV_SYMBOL_SETTINGS);
    lv_obj_center(settingsIcon);
    lv_obj_set_style_text_color(settingsIcon, lv_color_white(), 0);
    lv_obj_add_flag(btnSettings, LV_OBJ_FLAG_HIDDEN);
}

void closeCurrentApp() {
    if (appScreen) {
        lv_obj_del(appScreen);
        appScreen = nullptr;
    }
    if (wifiPasswordOverlay) {
        wifiPasswordOverlay = nullptr;
    }
    if (wifiKeyboard) {
        wifiKeyboard = nullptr;
    }
    wifiList = nullptr;
    wifiStatusLabel = nullptr;
    wifiTextarea = nullptr;
    wifiBtnScan = nullptr;
    fileList = nullptr;
    filePathLabel = nullptr;
    settingsPerfLabel = nullptr;
    settingsBacklightSlider = nullptr;
    settingsBacklightLabel = nullptr;
    settingsModeLabel = nullptr;
    wifiAppState = WIFI_MODE_IDLE;
    currentApp = APP_NONE;
    Serial.println("[App] Closed");
}

void updateSettingsUI() {
    if (settingsPerfLabel) {
        lv_mem_monitor_t mem_mon;
        lv_mem_monitor(&mem_mon);
        lv_label_set_text_fmt(settingsPerfLabel, 
            "CPU: %dMHz\nHeap: %uKB\nLVGL: %u%%",
            getCpuFrequencyMhz(), ESP.getFreeHeap() / 1024, mem_mon.used_pct);
    }
    
    if (settingsBacklightLabel) {
        const char* modeStr = backlightMode == BACKLIGHT_MODE_MANUAL ? "M" :
                              backlightMode == BACKLIGHT_MODE_AUTO ? "A" : "O";
        lv_label_set_text_fmt(settingsBacklightLabel, "Backlight: %d [%s]", backlightLevel, modeStr);
    }
    
    if (settingsBacklightSlider && backlightMode == BACKLIGHT_MODE_MANUAL) {
        lv_slider_set_value(settingsBacklightSlider, backlightLevel, LV_ANIM_OFF);
    }
    
    if (settingsModeLabel) {
        const char* modeStr = backlightMode == BACKLIGHT_MODE_MANUAL ? "MANUAL" :
                              backlightMode == BACKLIGHT_MODE_AUTO ? "AUTO" : "OFF";
        lv_label_set_text_fmt(settingsModeLabel, "Mode: %s", modeStr);
    }
}

void settings_backlight_slider_cb(lv_event_t *e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    backlightLevel = (uint8_t)value;
    backlightMode = BACKLIGHT_MODE_MANUAL;
    bsp_backlight_set(backlightLevel);
    updateSettingsUI();
}

void settings_mode_btn_cb(lv_event_t *e) {
    cycleBacklightMode();
}

void settings_back_cb(lv_event_t *e) {
    closeCurrentApp();
}

void wifi_connect_cb(lv_event_t *e);
void wifi_cancel_cb(lv_event_t *e);
void wifi_item_cb(lv_event_t *e);
void doWiFiScan();
void wifi_rescan_cb(lv_event_t *e);
void tryAutoConnectWiFi();

void settings_app_cb(lv_event_t *e) {
    if (currentApp != APP_NONE) {
        closeCurrentApp();
    }
    
    currentApp = APP_SETTINGS;
    
    appScreen = lv_obj_create(lv_layer_top());
    lv_obj_set_size(appScreen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(appScreen, lv_color_make(0x10, 0x10, 0x20), 0);
    lv_obj_move_background(appScreen);
    
    lv_obj_t* title = lv_label_create(appScreen);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    lv_obj_t* perfTitle = lv_label_create(appScreen);
    lv_label_set_text(perfTitle, "--- System Info ---");
    lv_obj_set_style_text_color(perfTitle, lv_color_make(0xFF, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(perfTitle, &lv_font_montserrat_12, 0);
    lv_obj_align(perfTitle, LV_ALIGN_TOP_LEFT, 10, 30);
    
    settingsPerfLabel = lv_label_create(appScreen);
    lv_obj_set_style_text_color(settingsPerfLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(settingsPerfLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(settingsPerfLabel, LV_ALIGN_TOP_LEFT, 10, 50);
    
    lv_obj_t* blTitle = lv_label_create(appScreen);
    lv_label_set_text(blTitle, "--- Backlight ---");
    lv_obj_set_style_text_color(blTitle, lv_color_make(0xFF, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(blTitle, &lv_font_montserrat_12, 0);
    lv_obj_align(blTitle, LV_ALIGN_TOP_LEFT, 10, 90);
    
    settingsBacklightLabel = lv_label_create(appScreen);
    lv_obj_set_style_text_color(settingsBacklightLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(settingsBacklightLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(settingsBacklightLabel, LV_ALIGN_TOP_LEFT, 10, 110);
    
    settingsBacklightSlider = lv_slider_create(appScreen);
    lv_obj_set_width(settingsBacklightSlider, 200);
    lv_obj_align(settingsBacklightSlider, LV_ALIGN_TOP_LEFT, 10, 135);
    lv_slider_set_range(settingsBacklightSlider, 0, 255);
    lv_slider_set_value(settingsBacklightSlider, backlightLevel, LV_ANIM_OFF);
    lv_obj_add_event_cb(settingsBacklightSlider, settings_backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    settingsModeLabel = lv_label_create(appScreen);
    lv_obj_set_style_text_color(settingsModeLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(settingsModeLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(settingsModeLabel, LV_ALIGN_TOP_LEFT, 10, 160);
    
    lv_obj_t* btnMode = lv_btn_create(appScreen);
    lv_obj_set_size(btnMode, 100, 25);
    lv_obj_align(btnMode, LV_ALIGN_TOP_LEFT, 10, 180);
    lv_obj_add_event_cb(btnMode, settings_mode_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnMode, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_t* lblMode = lv_label_create(btnMode);
    lv_label_set_text(lblMode, "Mode [M/A/O]");
    lv_obj_center(lblMode);
    
    lv_obj_t* wifiTitle = lv_label_create(appScreen);
    lv_label_set_text(wifiTitle, "--- WiFi ---");
    lv_obj_set_style_text_color(wifiTitle, lv_color_make(0xFF, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(wifiTitle, &lv_font_montserrat_12, 0);
    lv_obj_align(wifiTitle, LV_ALIGN_TOP_LEFT, 10, 215);
    
    wifiStatusLabel = lv_label_create(appScreen);
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text_fmt(wifiStatusLabel, "Connected: %s", WiFi.localIP().toString().c_str());
        lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
    } else {
        lv_label_set_text(wifiStatusLabel, "Not connected");
        lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0xFF, 0x00), 0);
    }
    lv_obj_set_style_text_font(wifiStatusLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(wifiStatusLabel, LV_ALIGN_TOP_LEFT, 10, 235);
    
    wifiBtnScan = lv_btn_create(appScreen);
    lv_obj_set_size(wifiBtnScan, 80, 25);
    lv_obj_align(wifiBtnScan, LV_ALIGN_TOP_LEFT, 10, 255);
    lv_obj_add_event_cb(wifiBtnScan, wifi_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(wifiBtnScan, lv_color_make(0x00, 0x60, 0x80), 0);
    lv_obj_t* lblScan = lv_label_create(wifiBtnScan);
    lv_label_set_text(lblScan, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(lblScan);
    
    wifiList = lv_list_create(appScreen);
    lv_obj_set_size(wifiList, SCREEN_WIDTH - 20, 60);
    lv_obj_align(wifiList, LV_ALIGN_TOP_MID, 0, 285);
    lv_obj_set_style_bg_color(wifiList, lv_color_make(0x20, 0x20, 0x20), 0);
    
    lv_obj_t* btnBack = lv_btn_create(appScreen);
    lv_obj_set_size(btnBack, 80, 25);
    lv_obj_align(btnBack, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_add_event_cb(btnBack, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnBack, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_t* lblBack = lv_label_create(btnBack);
    lv_label_set_text(lblBack, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lblBack);
    
    updateSettingsUI();
    
    if (WiFi.status() != WL_CONNECTED) {
        tryAutoConnectWiFi();
    }
}

void tryAutoConnectWiFi() {
    const char* autoSSID = "YIYAN_cat";
    const char* autoPass = "27276868999";
    
    if (wifiStatusLabel) {
        lv_label_set_text(wifiStatusLabel, "Auto-connecting...");
    }
    
    Serial.printf("[WiFi] Auto-connecting to %s...\n", autoSSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(autoSSID, autoPass);
    
    wifiAppState = WIFI_MODE_CONNECTING;
    wifiConnecting = true;
    wifiConnectAttempts = 0;
    strncpy(selectedSSID, autoSSID, WIFI_MAX_SSID_LEN - 1);
    strncpy(wifiPassword, autoPass, WIFI_MAX_PASS_LEN - 1);
}

void wifi_connect_cb(lv_event_t *e) {
    if (wifiTextarea) {
        const char* pass = lv_textarea_get_text(wifiTextarea);
        strncpy(wifiPassword, pass, WIFI_MAX_PASS_LEN - 1);
        wifiPassword[WIFI_MAX_PASS_LEN - 1] = '\0';
    }
    
    if (wifiPasswordOverlay) {
        lv_obj_del(wifiPasswordOverlay);
        wifiPasswordOverlay = nullptr;
    }
    if (wifiKeyboard) {
        wifiKeyboard = nullptr;
    }
    wifiTextarea = nullptr;
    
    if (wifiStatusLabel) {
        lv_label_set_text(wifiStatusLabel, "Connecting...");
    }
    wifiAppState = WIFI_MODE_CONNECTING;
    wifiConnecting = true;
    wifiConnectAttempts = 0;
    WiFi.mode(WIFI_STA);
    WiFi.begin(selectedSSID, wifiPassword);
}

void wifi_cancel_cb(lv_event_t *e) {
    if (wifiPasswordOverlay) {
        lv_obj_del(wifiPasswordOverlay);
        wifiPasswordOverlay = nullptr;
    }
    if (wifiKeyboard) {
        wifiKeyboard = nullptr;
    }
    wifiTextarea = nullptr;
}

void wifi_item_cb(lv_event_t *e) {
    lv_obj_t* btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    
    if (idx >= 0 && idx < wifiScanCount) {
        strncpy(selectedSSID, wifiScanResults[idx].ssid, WIFI_MAX_SSID_LEN - 1);
        selectedSSID[WIFI_MAX_SSID_LEN - 1] = '\0';
        
        if (wifiScanResults[idx].isOpen) {
            wifiPassword[0] = '\0';
            if (wifiStatusLabel) lv_label_set_text(wifiStatusLabel, "Connecting...");
            wifiAppState = WIFI_MODE_CONNECTING;
            wifiConnecting = true;
            WiFi.mode(WIFI_STA);
            WiFi.begin(selectedSSID, nullptr);
        } else {
            wifiPasswordOverlay = lv_obj_create(lv_layer_top());
            lv_obj_set_size(wifiPasswordOverlay, SCREEN_WIDTH - 20, 180);
            lv_obj_align(wifiPasswordOverlay, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(wifiPasswordOverlay, lv_color_make(0x30, 0x30, 0x50), 0);
            lv_obj_set_style_border_width(wifiPasswordOverlay, 2, 0);
            lv_obj_set_style_border_color(wifiPasswordOverlay, lv_color_make(0x00, 0xFF, 0x00), 0);
            lv_obj_move_foreground(wifiPasswordOverlay);
            
            lv_obj_t* label = lv_label_create(wifiPasswordOverlay);
            lv_label_set_text_fmt(label, "Password: %s", selectedSSID);
            lv_obj_set_style_text_color(label, lv_color_make(0x00, 0xFF, 0x00), 0);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 5);
            
            wifiTextarea = lv_textarea_create(wifiPasswordOverlay);
            lv_obj_set_size(wifiTextarea, SCREEN_WIDTH - 50, 30);
            lv_obj_align(wifiTextarea, LV_ALIGN_TOP_MID, 0, 25);
            lv_textarea_set_one_line(wifiTextarea, true);
            lv_textarea_set_max_length(wifiTextarea, WIFI_MAX_PASS_LEN);
            lv_textarea_set_placeholder_text(wifiTextarea, "Enter password...");
            
            wifiKeyboard = lv_keyboard_create(wifiPasswordOverlay);
            lv_obj_set_size(wifiKeyboard, SCREEN_WIDTH - 30, 80);
            lv_obj_align(wifiKeyboard, LV_ALIGN_TOP_MID, 0, 60);
            lv_keyboard_set_textarea(wifiKeyboard, wifiTextarea);
            lv_keyboard_set_mode(wifiKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
            
            lv_obj_t* btnConnect = lv_btn_create(wifiPasswordOverlay);
            lv_obj_set_size(btnConnect, 70, 25);
            lv_obj_align(btnConnect, LV_ALIGN_BOTTOM_LEFT, 10, -5);
            lv_obj_add_event_cb(btnConnect, wifi_connect_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_bg_color(btnConnect, lv_color_make(0x00, 0x80, 0x00), 0);
            lv_obj_t* lblConn = lv_label_create(btnConnect);
            lv_label_set_text(lblConn, LV_SYMBOL_OK " OK");
            lv_obj_center(lblConn);
            
            lv_obj_t* btnCancel = lv_btn_create(wifiPasswordOverlay);
            lv_obj_set_size(btnCancel, 70, 25);
            lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_RIGHT, -10, -5);
            lv_obj_add_event_cb(btnCancel, wifi_cancel_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_bg_color(btnCancel, lv_color_make(0x80, 0x00, 0x00), 0);
            lv_obj_t* lblCancel = lv_label_create(btnCancel);
            lv_label_set_text(lblCancel, LV_SYMBOL_CLOSE " X");
            lv_obj_center(lblCancel);
        }
    }
}

void doWiFiScan() {
    if (wifiStatusLabel) {
        lv_label_set_text(wifiStatusLabel, "Scanning...");
    }
    
    if (wifiList) {
        lv_obj_clean(wifiList);
    }
    
    lv_timer_handler();
    
    Serial.println("[WiFi] Resetting WiFi module...");
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(500);
    
    Serial.println("[WiFi] Starting scan...");
    Serial.printf("[WiFi] WiFi mode: %s\n", WiFi.getMode() == WIFI_MODE_STA ? "STA" : "OTHER");
    Serial.printf("[WiFi] Free heap: %u\n", ESP.getFreeHeap());
    
    int n = WiFi.scanNetworks(false, true, false, 10000);
    
    unsigned long start = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - start < 15000) {
        delay(100);
    }
    
    n = WiFi.scanComplete();
    
    Serial.printf("[WiFi] Scan result: %d\n", n);
    
    wifiScanCount = 0;
    
    if (n > 0) {
        wifiScanCount = min(n, WIFI_MAX_SCAN_RESULTS);
        
        for (int i = 0; i < wifiScanCount; i++) {
            strncpy(wifiScanResults[i].ssid, WiFi.SSID(i).c_str(), WIFI_MAX_SSID_LEN - 1);
            wifiScanResults[i].ssid[WIFI_MAX_SSID_LEN - 1] = '\0';
            wifiScanResults[i].rssi = WiFi.RSSI(i);
            wifiScanResults[i].encryption = WiFi.encryptionType(i);
            wifiScanResults[i].isOpen = (wifiScanResults[i].encryption == WIFI_AUTH_OPEN);
        }
        
        for (int i = 0; i < wifiScanCount - 1; i++) {
            for (int j = i + 1; j < wifiScanCount; j++) {
                if (wifiScanResults[j].rssi > wifiScanResults[i].rssi) {
                    wifi_scan_result_t tmp = wifiScanResults[i];
                    wifiScanResults[i] = wifiScanResults[j];
                    wifiScanResults[j] = tmp;
                }
            }
        }
    }
    
    WiFi.scanDelete();
    
    if (wifiList) {
        lv_obj_clean(wifiList);
        for (int i = 0; i < wifiScanCount; i++) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s (%ddBm)%s", 
                wifiScanResults[i].ssid, wifiScanResults[i].rssi,
                wifiScanResults[i].isOpen ? " [Open]" : "");
            lv_obj_t* btn = lv_list_add_btn(wifiList, LV_SYMBOL_WIFI, buf);
            lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_text_color(btn, lv_color_white(), 0);
            lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        }
    }
    
    if (wifiStatusLabel) {
        lv_label_set_text_fmt(wifiStatusLabel, "Found %d networks", wifiScanCount);
    }
    
    wifiAppState = WIFI_MODE_IDLE;
}

void wifi_rescan_cb(lv_event_t *e) {
    doWiFiScan();
}

void file_back_cb(lv_event_t *e) {
    closeCurrentApp();
}

void file_item_cb(lv_event_t *e) {
    lv_obj_t* btn = lv_event_get_target(e);
    const char* name = (const char*)lv_event_get_user_data(e);
    
    if (name && strcmp(name, "..") == 0) {
        int lastSlash = currentPath.lastIndexOf('/');
        if (lastSlash > 0) {
            currentPath = currentPath.substring(0, lastSlash);
        } else {
            currentPath = "/";
        }
    } else if (name) {
        String newPath = currentPath.endsWith("/") ? 
            currentPath + name : currentPath + "/" + name;
        
        File f = SD.open(newPath);
        if (f && f.isDirectory()) {
            currentPath = newPath;
            f.close();
        } else {
            f.close();
            return;
        }
    }
    
    if (filePathLabel) {
        lv_label_set_text(filePathLabel, currentPath.c_str());
    }
    
    if (!fileList) return;
    
    lv_obj_clean(fileList);
    
    if (currentPath != "/") {
        lv_obj_t* btnUp = lv_list_add_btn(fileList, LV_SYMBOL_UP, "..");
        lv_obj_set_style_bg_color(btnUp, lv_color_make(0x40, 0x40, 0x40), 0);
        lv_obj_set_style_text_color(btnUp, lv_color_white(), 0);
        lv_obj_add_event_cb(btnUp, file_item_cb, LV_EVENT_CLICKED, (void*)"..");
    }
    
    File dir = SD.open(currentPath);
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        while (file) {
            const char* fname = file.name();
            const char* icon = file.isDirectory() ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            
            char buf[48];
            if (file.isDirectory()) {
                snprintf(buf, sizeof(buf), "%s/", fname);
            } else {
                snprintf(buf, sizeof(buf), "%s (%luB)", fname, file.size());
            }
            
            lv_obj_t* btnItem = lv_list_add_btn(fileList, icon, buf);
            lv_obj_set_style_bg_color(btnItem, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_text_color(btnItem, lv_color_white(), 0);
            
            char* nameCopy = strdup(fname);
            lv_obj_add_event_cb(btnItem, file_item_cb, LV_EVENT_CLICKED, nameCopy);
            
            file = dir.openNextFile();
        }
    }
    dir.close();
}

void file_browse_cb(lv_event_t *e) {
    if (!sdCardDetected) {
        Serial.println("[File] No SD card");
        return;
    }
    
    if (currentApp != APP_NONE) {
        closeCurrentApp();
    }
    
    currentApp = APP_FILE_BROWSER;
    currentPath = "/";
    
    appScreen = lv_obj_create(lv_layer_top());
    lv_obj_set_size(appScreen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(appScreen, lv_color_make(0x10, 0x10, 0x20), 0);
    lv_obj_move_background(appScreen);
    
    lv_obj_t* title = lv_label_create(appScreen);
    lv_label_set_text(title, LV_SYMBOL_LIST " File Browser");
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    filePathLabel = lv_label_create(appScreen);
    lv_label_set_text(filePathLabel, "/");
    lv_obj_set_style_text_color(filePathLabel, lv_color_make(0xFF, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(filePathLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(filePathLabel, LV_ALIGN_TOP_MID, 0, 28);
    
    fileList = lv_list_create(appScreen);
    lv_obj_set_size(fileList, SCREEN_WIDTH - 20, 150);
    lv_obj_align(fileList, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(fileList, lv_color_make(0x20, 0x20, 0x20), 0);
    
    lv_obj_t* btnBack = lv_btn_create(appScreen);
    lv_obj_set_size(btnBack, 80, 25);
    lv_obj_align(btnBack, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_add_event_cb(btnBack, file_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnBack, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_t* lblBack = lv_label_create(btnBack);
    lv_label_set_text(lblBack, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lblBack);
    
    File dir = SD.open("/");
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        while (file) {
            const char* fname = file.name();
            const char* icon = file.isDirectory() ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            
            char buf[48];
            if (file.isDirectory()) {
                snprintf(buf, sizeof(buf), "%s/", fname);
            } else {
                snprintf(buf, sizeof(buf), "%s (%luB)", fname, file.size());
            }
            
            lv_obj_t* btnItem = lv_list_add_btn(fileList, icon, buf);
            lv_obj_set_style_bg_color(btnItem, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_text_color(btnItem, lv_color_white(), 0);
            
            char* nameCopy = strdup(fname);
            lv_obj_add_event_cb(btnItem, file_item_cb, LV_EVENT_CLICKED, nameCopy);
            
            file = dir.openNextFile();
        }
    }
    dir.close();
}

void updateWifiConnection() {
    if (wifiAppState != WIFI_MODE_CONNECTING || !wifiConnecting) return;
    
    wl_status_t status = WiFi.status();
    
    if (status == WL_CONNECTED) {
        wifiConnecting = false;
        wifiAppState = WIFI_MODE_CONNECTED;
        if (wifiStatusLabel) {
            lv_label_set_text_fmt(wifiStatusLabel, "Connected: %s", WiFi.localIP().toString().c_str());
            lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
        }
        rgbLedSet(0, 255, 0);
        
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid", selectedSSID);
        prefs.putString("pass", wifiPassword);
        prefs.end();
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
        wifiConnecting = false;
        wifiAppState = WIFI_MODE_FAILED;
        if (wifiStatusLabel) {
            lv_label_set_text(wifiStatusLabel, "Connection failed!");
            lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0x00, 0x00), 0);
        }
        rgbLedSet(255, 0, 0);
    } else {
        wifiConnectAttempts++;
        if (wifiConnectAttempts > 50) {
            wifiConnecting = false;
            wifiAppState = WIFI_MODE_FAILED;
            WiFi.disconnect();
            if (wifiStatusLabel) {
                lv_label_set_text(wifiStatusLabel, "Connection timeout!");
                lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0x00, 0x00), 0);
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=================================");
    Serial.println("LVGL Settings Demo");
    Serial.println("=================================");
    
    initRGBLed();
    
    backlightMode = BACKLIGHT_MODE_MANUAL;
    backlightLevel = BACKLIGHT_MAX;
    lastActivityMs = millis();
    
    lv_init();
    initDisplay();
    
    ledcSetup(0, 5000, 8);
    ledcAttachPin(BACKLIGHT_PIN, 0);
    bsp_backlight_set(BACKLIGHT_MAX);
    
    Serial.printf("[BL] PWM initialized on GPIO %d\n", BACKLIGHT_PIN);
    
    initTouch();
    initSDCard();
    
    xFont = new XFont(true);
    
    lv_obj_t* blankScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(blankScreen, lv_color_black(), 0);
    lv_scr_load(blankScreen);
    
    createGlobalUI();
    startLvglTask();
    
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOOT_BUTTON_PIN), bootButtonISR, FALLING);
    
    Serial.println("=================================");
    Serial.println("System ready.");
    Serial.println("BOOT: MANUAL->AUTO->OFF");
    Serial.println("=================================");
}

void loop() {
    handleBootButton();
    
    if (currentApp == APP_SETTINGS) {
        updateWifiConnection();
        
        static unsigned long lastPerfUpdate = 0;
        if (millis() - lastPerfUpdate > 1000) {
            updateSettingsUI();
            lastPerfUpdate = millis();
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
}
