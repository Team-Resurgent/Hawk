// Hawk — WiFi UDP debug logging. Connects STA and mirrors every ESP_LOG line
// as a UDP broadcast (port 5556). Lets a PC on the same LAN watch the
// communicator's behavior while it's plugged into a far-away Xbox controller.
#pragma once
void wifi_log_start(void);
