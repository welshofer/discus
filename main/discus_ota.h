#pragma once

/* Start the background updater. Polls the agent's firmware manifest and
 * applies any image whose version differs from the running one. */
void discus_ota_start(void);

/* Call once the device has proven it can reach the agent. Until this runs a
 * freshly-OTA'd image sits in "pending verify" and the bootloader will roll
 * back to the previous slot on the next reboot. */
void discus_ota_confirm_running(void);
