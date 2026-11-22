#ifndef POWER_H
#define POWER_H

/* Zapisuja pliki na dysk i restartuja / wylaczaja komputer */
void system_reboot(void) __attribute__((noreturn));
void system_shutdown(void) __attribute__((noreturn));

#endif
