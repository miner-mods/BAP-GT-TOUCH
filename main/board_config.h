/**
 * @file board_config.h
 * @brief Miner-Mods build options.
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// 1 = Waveshare ESP32-S3-Touch-LCD-4.3B (backlight/touch reset via CH422G expander)
// 0 = original Bitaxe Turbo Touch v1.1 board (GPIO15 PWM backlight)
#define MM_BOARD_WAVESHARE_43B 1

// 1 = no BAP cable: the screen joins WiFi itself (password entered on the
//     touchscreen, saved on the board) and reads the miners over WiFi only.
#define MM_STANDALONE_WIFI 1

#endif // BOARD_CONFIG_H
