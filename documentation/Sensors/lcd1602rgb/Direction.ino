
#include <Wire.h>
#include "Waveshare_LCD1602_RGB.h"


Waveshare_LCD1602_RGB lcd(16,2);  //16 characters and 2 lines of show
int r,g,b,t=0;
void setup() {
    // initialize
    lcd.init();
    
    lcd.setCursor(0,0);
    lcd.send_string("Waveshare");
    lcd.setCursor(0,1);
    lcd.send_string("Hello,World!");
}

void loop() {
    r = (abs(sin(3.14*t/180)))*255;
    g = (abs(sin(3.14*(t + 60)/180)))*255;
    b = (abs(sin(3.14*(t + 120)/180)))*255;
    t = t + 3;
    lcd.setRGB(r,g,b);
    delay(150);
}
