/**
 * @file miners.h
 * @brief Miner-Mods dual-miner support: reads two AxeOS/HexOS miners over WiFi
 *        and presents them on the screen as one combined miner.
 */
#ifndef MINERS_H
#define MINERS_H

#include <stdbool.h>
#include "esp_err.h"

// Miner addresses on the local network (reserved in the router)
#define MINER_COUNT 2
#define MINER_0_IP "192.168.5.94"   // Chinese GT 801
#define MINER_1_IP "192.168.5.157"  // Gekko GT 801

#define MINERS_POLL_INTERVAL_MS 3000
#define MINERS_STALE_MS 45000   // only call a miner offline after 45 s of no answers

// Start the background task that polls both miners.
void miners_start(void);

// True when at least one miner answered over WiFi recently. While true, the
// screen ignores mining data arriving over the BAP cable so the combined
// numbers aren't overwritten by the single attached miner.
bool miners_wifi_active(void);

// Send a frequency (MHz) / core voltage (mV) change to every miner.
esp_err_t miners_set_freq_voltage(int frequency_mhz, int core_voltage_mv);

#endif // MINERS_H
