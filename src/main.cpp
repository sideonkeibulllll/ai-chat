#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "xfont.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_IRQ 36

#define BUTTON_X 160
#define BUTTON_Y 185
#define BUTTON_W 140
#define BUTTON_H 45

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI;
XPT2046_Touchscreen* touchscreen = nullptr;

XFont *xFont;

int touchX = 0;
int touchY = 0;
bool buttonPressed = false;
int touchCount = 0;
bool lastTouchState = false;

void drawButton(bool pressed) {
    uint16_t bgColor = pressed ? TFT_DARKGREY : TFT_BLUE;
    uint16_t borderColor = pressed ? TFT_YELLOW : TFT_WHITE;
    uint16_t textColor = pressed ? TFT_YELLOW : TFT_WHITE;
    
    tft.fillRoundRect(BUTTON_X - BUTTON_W/2, BUTTON_Y - BUTTON_H/2, BUTTON_W, BUTTON_H, 10, bgColor);
    tft.drawRoundRect(BUTTON_X - BUTTON_W/2, BUTTON_Y - BUTTON_H/2, BUTTON_W, BUTTON_H, 10, borderColor);
    
    tft.setTextColor(textColor);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString("Touch Test", BUTTON_X, BUTTON_Y);
}

void drawMainScreen() {
    tft.fillScreen(TFT_BLACK);
    
    tft.fillRect(0, 0, SCREEN_WIDTH, 35, TFT_NAVY);
    tft.setTextColor(TFT_YELLOW);
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(1);
    tft.drawString("ESP32 Display Demo", SCREEN_WIDTH/2, 18);
    
    xFont->DrawChineseEx(SCREEN_WIDTH/2 - 40, 60, "你好", TFT_GREEN);
    
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("hello world", SCREEN_WIDTH/2, 100);
    
    tft.setTextSize(1);
    tft.setTextColor(TFT_ORANGE);
    tft.drawString("Chinese + English Display", SCREEN_WIDTH/2, 130);
    
    drawButton(false);
    
    tft.setTextColor(TFT_SILVER);
    tft.setTextSize(1);
    tft.setTextDatum(BL_DATUM);
    tft.drawString("Touch count: 0", 10, SCREEN_HEIGHT - 8);
}

void updateTouchCount(int count) {
    tft.fillRect(0, SCREEN_HEIGHT - 18, 130, 18, TFT_BLACK);
    tft.setTextColor(TFT_SILVER);
    tft.setTextSize(1);
    tft.setTextDatum(BL_DATUM);
    tft.drawString("Touch count: " + String(count), 10, SCREEN_HEIGHT - 8);
}

bool isInsideButton(int x, int y) {
    return (x >= BUTTON_X - BUTTON_W/2 && x <= BUTTON_X + BUTTON_W/2 &&
            y >= BUTTON_Y - BUTTON_H/2 && y <= BUTTON_Y + BUTTON_H/2);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=================================");
    Serial.println("ESP32 Display Demo Starting...");
    Serial.println("=================================");
    
    Serial.println("Initializing TFT display...");
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    Serial.println("Initializing touch (custom SPI)...");
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    touchscreen = new XPT2046_Touchscreen(TOUCH_CS, TOUCH_IRQ);
    touchscreen->begin(touchSPI);
    touchscreen->setRotation(1);
    Serial.println("Touch init: OK");
    
    Serial.println("Initializing Chinese font...");
    xFont = new XFont(true);
    
    Serial.println("Drawing main screen...");
    drawMainScreen();
    
    Serial.println("=================================");
    Serial.println("Setup complete! Touch the button to test.");
    Serial.println("=================================");
}

void loop() {
    if (touchscreen == nullptr) {
        delay(100);
        return;
    }
    
    bool currentTouchState = touchscreen->tirqTouched() && touchscreen->touched();
    
    if (currentTouchState && !lastTouchState) {
        TS_Point p = touchscreen->getPoint();
        
        touchX = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
        touchY = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
        
        touchX = constrain(touchX, 0, SCREEN_WIDTH - 1);
        touchY = constrain(touchY, 0, SCREEN_HEIGHT - 1);
        
        if (isInsideButton(touchX, touchY)) {
            buttonPressed = true;
            drawButton(true);
            touchCount++;
            updateTouchCount(touchCount);
        }
    } else if (!currentTouchState && lastTouchState) {
        if (buttonPressed) {
            buttonPressed = false;
            drawButton(false);
        }
    }
    
    lastTouchState = currentTouchState;
    delay(20);
}
