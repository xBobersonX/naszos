#ifndef SHELL_NET_H
#define SHELL_NET_H

/* Komendy powloki do sieci (src/shell_net.c) */
void cmd_siec(char *args);
void cmd_ping(char *args);
void cmd_dns(char *args);
void cmd_www(char *args);
void cmd_czas(char *args);
void cmd_pobierz(char *args);

#include <stdint.h>
struct rtc_time;
/* Ustawia zegar komputera wedlug serwera czasu. diff = o ile sie mylil (s) */
int net_clock_sync(int64_t *diff_secs, struct rtc_time *now);

#endif
