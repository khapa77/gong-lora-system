#include "mp3handler.h"
#include "config.h"
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>

static HardwareSerial    mp3Serial(2);
static DFRobotDFPlayerMini player;
static uint8_t           curVol = DEFAULT_VOLUME;
static bool              ready  = false;

void mp3_setup() {
    mp3Serial.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
    pinMode(MP3_BUSY, INPUT_PULLUP);
    delay(500);

    // begin(serial, isACK=false, doReset=true)
    if (!player.begin(mp3Serial, false, true)) {
        Serial.println("[MP3] DFPlayer init failed! Check wiring/SD card.");
        return;
    }
    ready = true;
    player.volume(curVol);
    player.EQ(DFPLAYER_EQ_NORMAL);
    Serial.printf("[MP3] Ready. Volume=%d\n", curVol);
}

void mp3_play(uint8_t track) {
    if (!ready) { Serial.println("[MP3] Not ready"); return; }
    Serial.printf("[MP3] Playing track %d\n", track);
    player.play(track);
}

void mp3_stop() {
    if (!ready) return;
    player.stop();
    Serial.println("[MP3] Stopped");
}

void mp3_setVolume(uint8_t vol) {
    if (!ready) return;
    if (vol > 30) vol = 30;
    curVol = vol;
    player.volume(vol);
    Serial.printf("[MP3] Volume=%d\n", vol);
}

bool mp3_isBusy() {
    return digitalRead(MP3_BUSY) == LOW;
}

void mp3_loop() {
    if (!ready) return;
    if (player.available()) {
        player.readType(); // consume any response from DFPlayer
    }
}

