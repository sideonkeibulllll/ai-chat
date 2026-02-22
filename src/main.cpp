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

#define IDLE_TIMEOUT_MS 30000
#define SLEEP_TIMEOUT_MS 300000
#define CPU_DOWN_TIMEOUT_MS 30000
#define SLEEP_DELAY_MS 120000
#define AUTO_ADJUST_INTERVAL 1000

#define WIFI_MAX_SCAN_RESULTS 8
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
    POWER_MODE_HIGH = 0,
    POWER_MODE_BALANCED = 1,
    POWER_MODE_LOW = 2
} power_cpu_mode_t;

typedef enum {
    BACKLIGHT_MODE_MANUAL = 0,
    BACKLIGHT_MODE_AUTO = 1,
    BACKLIGHT_MODE_OFF = 2
} backlight_mode_t;

typedef enum {
    POWER_STATE_ACTIVE = 0,
    POWER_STATE_IDLE = 1,
    POWER_STATE_SLEEP = 2
} power_state_t;

typedef enum {
    APP_NONE = 0,
    APP_WIFI_CONFIG = 1,
    APP_FILE_BROWSER = 2
} active_app_t;

typedef struct {
    power_cpu_mode_t cpuMode;
    backlight_mode_t backlightMode;
    power_state_t state;
    uint8_t backlightLevel;
    uint16_t ldrValue;
    uint32_t idleTimeMs;
    uint32_t lastActivityMs;
    uint32_t bootPressCount;
} power_status_t;

static power_status_t powerStatus;
static volatile bool bootButtonPressed = false;
static uint8_t autoMinLevel = BACKLIGHT_MIN;
static uint8_t autoMaxLevel = BACKLIGHT_MAX;
static uint32_t lastAutoAdjustMs = 0;

static lv_obj_t* sidebar = nullptr;
static lv_obj_t* toggleBtn = nullptr;
static lv_obj_t* perfLabel = nullptr;
static lv_obj_t* btnWiFi = nullptr;
static lv_obj_t* btnFile = nullptr;
static bool sidebarOpen = false;

static active_app_t currentApp = APP_NONE;
static lv_obj_t* appScreen = nullptr;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int32_t rssi;
    wifi_auth_mode_t encryption;
    bool isOpen;
} wifi_scan_result_t;

static wifi_scan_result_t wifiScanResults[WIFI_MAX_SCAN_RESULTS];
static int wifiScanCount = 0;
static char selectedSSID[WIFI_MAX_SSID_LEN];
static char wifiPassword[WIFI_MAX_PASS_LEN];
static bool wifiConnecting = false;
static int wifiConnectAttempts = 0;

static lv_obj_t* wifiList = nullptr;
static lv_obj_t* wifiStatusLabel = nullptr;
static lv_obj_t* wifiPasswordOverlay = nullptr;
static lv_obj_t* wifiTextarea = nullptr;

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
        
        powerStatus.lastActivityMs = millis();
    } else {
        data->point.x = lastTouchX;
        data->point.y = lastTouchY;
        data->state = LV_INDEV_STATE_REL;
    }
}

void bsp_backlight_set(uint8_t level) {
    ledcWrite(0, level);
    powerStatus.backlightLevel = level;
}

void setCpuMode(power_cpu_mode_t mode) {
    powerStatus.cpuMode = mode;
    uint32_t targetFreq = 240;
    
    switch (mode) {
        case POWER_MODE_HIGH: targetFreq = 240; break;
        case POWER_MODE_BALANCED: targetFreq = 160; break;
        case POWER_MODE_LOW: targetFreq = 80; break;
    }
    
    if (getCpuFrequencyMhz() != targetFreq) {
        setCpuFrequencyMhz(targetFreq);
    }
}

uint8_t calculateBacklightFromLDR(uint16_t ldrValue) {
    uint16_t minLDR = 100;
    uint16_t maxLDR = 4000;
    ldrValue = constrain(ldrValue, minLDR, maxLDR);
    uint16_t normalized = map(ldrValue, minLDR, maxLDR, 0, 255);
    return map(normalized, 0, 255, autoMinLevel, autoMaxLevel);
}

void updateBacklightAuto() {
    if (powerStatus.idleTimeMs < IDLE_TIMEOUT_MS) return;
    powerStatus.ldrValue = analogRead(LDR_PIN);
    uint8_t newLevel = calculateBacklightFromLDR(powerStatus.ldrValue);
    if (newLevel != powerStatus.backlightLevel) {
        bsp_backlight_set(newLevel);
    }
}

void cycleBacklightMode() {
    backlight_mode_t newMode;
    
    switch (powerStatus.backlightMode) {
        case BACKLIGHT_MODE_MANUAL:
            newMode = BACKLIGHT_MODE_AUTO;
            break;
        case BACKLIGHT_MODE_AUTO:
            newMode = BACKLIGHT_MODE_OFF;
            break;
        default:
            newMode = BACKLIGHT_MODE_MANUAL;
            powerStatus.backlightLevel = BACKLIGHT_MAX;
            break;
    }
    
    powerStatus.backlightMode = newMode;
    
    switch (newMode) {
        case BACKLIGHT_MODE_MANUAL:
            bsp_backlight_set(powerStatus.backlightLevel > 0 ? powerStatus.backlightLevel : BACKLIGHT_MAX);
            break;
        case BACKLIGHT_MODE_AUTO:
            updateBacklightAuto();
            break;
        case BACKLIGHT_MODE_OFF:
            bsp_backlight_set(0);
            break;
    }
    
    Serial.printf("[Power] Backlight mode: %s\n",
        newMode == BACKLIGHT_MODE_MANUAL ? "MANUAL" :
        newMode == BACKLIGHT_MODE_AUTO ? "AUTO" : "OFF");
}

void enterSleep() {
    Serial.println("[Power] Entering SLEEP...");
    powerStatus.state = POWER_STATE_SLEEP;
    bsp_backlight_set(0);
    Serial.flush();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BUTTON_PIN, LOW);
    esp_light_sleep_start();
    powerStatus.state = POWER_STATE_ACTIVE;
    powerStatus.lastActivityMs = millis();
    setCpuMode(POWER_MODE_HIGH);
    if (powerStatus.backlightMode == BACKLIGHT_MODE_MANUAL) {
        bsp_backlight_set(powerStatus.backlightLevel);
    }
    Serial.println("[Power] Wakeup complete");
}

void updateIdleState() {
    uint32_t idleMs = powerStatus.idleTimeMs;
    if (powerStatus.state == POWER_STATE_SLEEP) return;
    
    if (idleMs >= SLEEP_TIMEOUT_MS) {
        enterSleep();
    } else if (idleMs >= SLEEP_DELAY_MS && powerStatus.backlightMode == BACKLIGHT_MODE_OFF) {
        enterSleep();
    } else if (idleMs >= CPU_DOWN_TIMEOUT_MS && powerStatus.backlightMode == BACKLIGHT_MODE_OFF) {
        setCpuMode(POWER_MODE_BALANCED);
    } else if (idleMs >= IDLE_TIMEOUT_MS && powerStatus.state == POWER_STATE_ACTIVE) {
        powerStatus.state = POWER_STATE_IDLE;
    } else if (idleMs < IDLE_TIMEOUT_MS && powerStatus.state == POWER_STATE_IDLE) {
        powerStatus.state = POWER_STATE_ACTIVE;
    }
}

void powerUpdate() {
    uint32_t now = millis();
    powerStatus.idleTimeMs = now - powerStatus.lastActivityMs;
    
    if (bootButtonPressed) {
        bootButtonPressed = false;
        powerStatus.bootPressCount++;
        cycleBacklightMode();
        powerStatus.lastActivityMs = millis();
    }
    
    if (powerStatus.backlightMode == BACKLIGHT_MODE_AUTO) {
        if (now - lastAutoAdjustMs >= AUTO_ADJUST_INTERVAL) {
            lastAutoAdjustMs = now;
            updateBacklightAuto();
        }
    }
    
    updateIdleState();
}

void IRAM_ATTR bootButtonISR() {
    bootButtonPressed = true;
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
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
}

void updatePerfLabel() {
    if (perfLabel == nullptr) return;
    lv_mem_monitor_t mem_mon;
    lv_mem_monitor(&mem_mon);
    const char* modeStr = powerStatus.backlightMode == BACKLIGHT_MODE_MANUAL ? "M" :
                          powerStatus.backlightMode == BACKLIGHT_MODE_AUTO ? "A" : "O";
    lv_label_set_text_fmt(perfLabel, "CPU:%dMHz\nHeap:%uKB\nLVGL:%u%%\nBL:%d[%s]",
        getCpuFrequencyMhz(), ESP.getFreeHeap() / 1024, mem_mon.used_pct,
        powerStatus.backlightLevel, modeStr);
}

void closeCurrentApp();

void toggle_sidebar_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (sidebarOpen) {
            lv_obj_set_pos(sidebar, -70, 20);
            lv_label_set_text(lv_obj_get_child(toggleBtn, 0), LV_SYMBOL_RIGHT);
            if (perfLabel) lv_obj_add_flag(perfLabel, LV_OBJ_FLAG_HIDDEN);
            if (btnWiFi) lv_obj_add_flag(btnWiFi, LV_OBJ_FLAG_HIDDEN);
            if (btnFile) lv_obj_add_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(sidebar, 0, 20);
            lv_label_set_text(lv_obj_get_child(toggleBtn, 0), LV_SYMBOL_LEFT);
            if (perfLabel) {
                lv_obj_clear_flag(perfLabel, LV_OBJ_FLAG_HIDDEN);
                updatePerfLabel();
            }
            if (btnWiFi) lv_obj_clear_flag(btnWiFi, LV_OBJ_FLAG_HIDDEN);
            if (btnFile) lv_obj_clear_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
        }
        sidebarOpen = !sidebarOpen;
    }
}

void wifi_scan_cb(lv_event_t *e);
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
    lv_obj_set_size(sidebar, 70, SCREEN_HEIGHT - 20);
    lv_obj_set_pos(sidebar, -70, 20);
    lv_obj_set_style_bg_color(sidebar, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 5, 0);
    
    perfLabel = lv_label_create(sidebar);
    lv_obj_set_style_text_color(perfLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(perfLabel, &lv_font_montserrat_10, 0);
    lv_obj_align(perfLabel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(perfLabel, "Loading...");
    lv_obj_add_flag(perfLabel, LV_OBJ_FLAG_HIDDEN);
    
    btnWiFi = lv_btn_create(sidebar);
    lv_obj_set_size(btnWiFi, 60, 30);
    lv_obj_align(btnWiFi, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_event_cb(btnWiFi, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnWiFi, lv_color_make(0x00, 0x60, 0x80), 0);
    lv_obj_t* wifiIcon = lv_label_create(btnWiFi);
    lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
    lv_obj_center(wifiIcon);
    lv_obj_set_style_text_color(wifiIcon, lv_color_white(), 0);
    lv_obj_add_flag(btnWiFi, LV_OBJ_FLAG_HIDDEN);
    
    btnFile = lv_btn_create(sidebar);
    lv_obj_set_size(btnFile, 60, 30);
    lv_obj_align(btnFile, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_add_event_cb(btnFile, file_browse_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnFile, lv_color_make(0x60, 0x40, 0x00), 0);
    lv_obj_t* fileIcon = lv_label_create(btnFile);
    lv_label_set_text(fileIcon, LV_SYMBOL_LIST);
    lv_obj_center(fileIcon);
    lv_obj_set_style_text_color(fileIcon, lv_color_white(), 0);
    lv_obj_add_flag(btnFile, LV_OBJ_FLAG_HIDDEN);
}

void closeCurrentApp() {
    if (appScreen) {
        lv_obj_del(appScreen);
        appScreen = nullptr;
    }
    wifiList = nullptr;
    wifiStatusLabel = nullptr;
    wifiPasswordOverlay = nullptr;
    wifiTextarea = nullptr;
    fileList = nullptr;
    filePathLabel = nullptr;
    currentApp = APP_NONE;
    Serial.println("[App] Closed");
}

void wifi_back_cb(lv_event_t *e) {
    closeCurrentApp();
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
        wifiTextarea = nullptr;
    }
    
    if (wifiStatusLabel) {
        lv_label_set_text(wifiStatusLabel, "Connecting...");
    }
    wifiConnecting = true;
    wifiConnectAttempts = 0;
    WiFi.mode(WIFI_STA);
    WiFi.begin(selectedSSID, wifiPassword);
}

void wifi_cancel_cb(lv_event_t *e) {
    if (wifiPasswordOverlay) {
        lv_obj_del(wifiPasswordOverlay);
        wifiPasswordOverlay = nullptr;
        wifiTextarea = nullptr;
    }
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
            wifiConnecting = true;
            WiFi.mode(WIFI_STA);
            WiFi.begin(selectedSSID, nullptr);
        } else {
            wifiPasswordOverlay = lv_obj_create(appScreen);
            lv_obj_set_size(wifiPasswordOverlay, SCREEN_WIDTH - 40, 140);
            lv_obj_align(wifiPasswordOverlay, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(wifiPasswordOverlay, lv_color_make(0x30, 0x30, 0x50), 0);
            lv_obj_move_foreground(wifiPasswordOverlay);
            
            lv_obj_t* label = lv_label_create(wifiPasswordOverlay);
            lv_label_set_text_fmt(label, "Password for: %s", selectedSSID);
            lv_obj_set_style_text_color(label, lv_color_make(0x00, 0xFF, 0x00), 0);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 5);
            
            wifiTextarea = lv_textarea_create(wifiPasswordOverlay);
            lv_obj_set_size(wifiTextarea, SCREEN_WIDTH - 80, 30);
            lv_obj_align(wifiTextarea, LV_ALIGN_TOP_MID, 0, 25);
            lv_textarea_set_one_line(wifiTextarea, true);
            lv_textarea_set_max_length(wifiTextarea, WIFI_MAX_PASS_LEN);
            
            lv_obj_t* btnConnect = lv_btn_create(wifiPasswordOverlay);
            lv_obj_set_size(btnConnect, 70, 25);
            lv_obj_align(btnConnect, LV_ALIGN_BOTTOM_LEFT, 10, -10);
            lv_obj_add_event_cb(btnConnect, wifi_connect_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_bg_color(btnConnect, lv_color_make(0x00, 0x80, 0x00), 0);
            lv_obj_t* lblConn = lv_label_create(btnConnect);
            lv_label_set_text(lblConn, "Connect");
            lv_obj_center(lblConn);
            
            lv_obj_t* btnCancel = lv_btn_create(wifiPasswordOverlay);
            lv_obj_set_size(btnCancel, 70, 25);
            lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
            lv_obj_add_event_cb(btnCancel, wifi_cancel_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_bg_color(btnCancel, lv_color_make(0x80, 0x00, 0x00), 0);
            lv_obj_t* lblCancel = lv_label_create(btnCancel);
            lv_label_set_text(lblCancel, "Cancel");
            lv_obj_center(lblCancel);
        }
    }
}

void wifi_scan_cb(lv_event_t *e) {
    if (currentApp != APP_NONE) {
        closeCurrentApp();
    }
    
    currentApp = APP_WIFI_CONFIG;
    appScreen = lv_obj_create(lv_layer_top());
    lv_obj_set_size(appScreen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(appScreen, lv_color_make(0x10, 0x10, 0x20), 0);
    lv_obj_move_background(appScreen);
    
    lv_obj_t* title = lv_label_create(appScreen);
    lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi Config");
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    wifiStatusLabel = lv_label_create(appScreen);
    lv_label_set_text(wifiStatusLabel, "Scanning...");
    lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0xFF, 0x00), 0);
    lv_obj_set_style_text_font(wifiStatusLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(wifiStatusLabel, LV_ALIGN_TOP_MID, 0, 28);
    
    wifiList = lv_list_create(appScreen);
    lv_obj_set_size(wifiList, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 90);
    lv_obj_align(wifiList, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(wifiList, lv_color_make(0x20, 0x20, 0x20), 0);
    
    lv_obj_t* btnBack = lv_btn_create(appScreen);
    lv_obj_set_size(btnBack, 80, 30);
    lv_obj_align(btnBack, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_add_event_cb(btnBack, wifi_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btnBack, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_t* lblBack = lv_label_create(btnBack);
    lv_label_set_text(lblBack, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lblBack);
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);
    
    int n = WiFi.scanNetworks(false, true);
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
    
    lv_obj_clean(wifiList);
    for (int i = 0; i < wifiScanCount; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%ddBm)%s", 
            wifiScanResults[i].ssid, wifiScanResults[i].rssi,
            wifiScanResults[i].isOpen ? " [Open]" : "");
        lv_obj_t* btn = lv_list_add_btn(wifiList, LV_SYMBOL_WIFI, buf);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    
    if (wifiStatusLabel) {
        lv_label_set_text_fmt(wifiStatusLabel, "Found %d networks", wifiScanCount);
    }
    
    WiFi.scanDelete();
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
            
            char buf[64];
            if (file.isDirectory()) {
                snprintf(buf, sizeof(buf), "%s/", fname);
            } else {
                snprintf(buf, sizeof(buf), "%s (%luB)", fname, file.size());
            }
            
            lv_obj_t* btn = lv_list_add_btn(fileList, icon, buf);
            lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_text_color(btn, lv_color_white(), 0);
            
            char* nameCopy = strdup(fname);
            lv_obj_add_event_cb(btn, file_item_cb, LV_EVENT_CLICKED, nameCopy);
            
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
    lv_obj_set_size(fileList, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 90);
    lv_obj_align(fileList, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(fileList, lv_color_make(0x20, 0x20, 0x20), 0);
    
    lv_obj_t* btnBack = lv_btn_create(appScreen);
    lv_obj_set_size(btnBack, 80, 30);
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
            
            char buf[64];
            if (file.isDirectory()) {
                snprintf(buf, sizeof(buf), "%s/", fname);
            } else {
                snprintf(buf, sizeof(buf), "%s (%luB)", fname, file.size());
            }
            
            lv_obj_t* btn = lv_list_add_btn(fileList, icon, buf);
            lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_text_color(btn, lv_color_white(), 0);
            
            char* nameCopy = strdup(fname);
            lv_obj_add_event_cb(btn, file_item_cb, LV_EVENT_CLICKED, nameCopy);
            
            file = dir.openNextFile();
        }
    }
    dir.close();
}

void updateWifiConnection() {
    if (!wifiConnecting) return;
    
    wl_status_t status = WiFi.status();
    
    if (status == WL_CONNECTED) {
        wifiConnecting = false;
        if (wifiStatusLabel) {
            lv_label_set_text_fmt(wifiStatusLabel, "Connected: %s", WiFi.localIP().toString().c_str());
            lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0x00, 0xFF, 0x00), 0);
        }
        digitalWrite(LED_GREEN, LOW);
        
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid", selectedSSID);
        prefs.putString("pass", wifiPassword);
        prefs.end();
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
        wifiConnecting = false;
        if (wifiStatusLabel) {
            lv_label_set_text(wifiStatusLabel, "Connection failed!");
            lv_obj_set_style_text_color(wifiStatusLabel, lv_color_make(0xFF, 0x00, 0x00), 0);
        }
        digitalWrite(LED_RED, LOW);
    } else {
        wifiConnectAttempts++;
        if (wifiConnectAttempts > 100) {
            wifiConnecting = false;
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
    Serial.println("LVGL WiFi & File Browser Demo");
    Serial.println("=================================");
    
    initRGBLed();
    
    powerStatus.cpuMode = POWER_MODE_HIGH;
    powerStatus.backlightMode = BACKLIGHT_MODE_MANUAL;
    powerStatus.state = POWER_STATE_ACTIVE;
    powerStatus.backlightLevel = BACKLIGHT_MAX;
    powerStatus.lastActivityMs = millis();
    
    lv_init();
    initDisplay();
    
    ledcSetup(0, 5000, 8);
    ledcAttachPin(BACKLIGHT_PIN, 0);
    bsp_backlight_set(BACKLIGHT_MAX);
    
    initTouch();
    initSDCard();
    
    pinMode(LDR_PIN, INPUT);
    
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
    Serial.println("Sidebar: WiFi & File buttons");
    Serial.println("=================================");
}

void loop() {
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    
    powerUpdate();
    
    if (now - lastUpdate > 5000) {
        if (currentApp == APP_NONE) {
            Serial.printf("[Power] State:%s CPU:%dMHz BL:%d\n",
                powerStatus.state == POWER_STATE_ACTIVE ? "ACT" : "IDL",
                getCpuFrequencyMhz(), powerStatus.backlightLevel);
        }
        lastUpdate = now;
    }
    
    if (sidebarOpen && perfLabel && currentApp == APP_NONE) {
        static unsigned long lastPerfUpdate = 0;
        if (now - lastPerfUpdate > 1000) {
            updatePerfLabel();
            lastPerfUpdate = now;
        }
    }
    
    if (currentApp == APP_WIFI_CONFIG) {
        updateWifiConnection();
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
}
