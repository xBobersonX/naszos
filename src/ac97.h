#ifndef AC97_H
#define AC97_H

#include <stdint.h>

/* Karta dzwiekowa Intel AC'97 (QEMU: -device AC97). Gra bez przerwy
 * probki 16-bitowe stereo, 48000 razy na sekunde. Probki dostarcza funkcja
 * fill() - wolana z przerwania, gdy karta konczy kolejny kawalek. */

#define AC97_RATE 48000

typedef void (*ac97_fill_fn)(int16_t *stereo, unsigned frames);

int ac97_init(ac97_fill_fn fill);     /* 1 = karta znaleziona i gra */

#endif
