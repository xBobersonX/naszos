#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

/* Dzwieki z glosniczka komputera (PC Speaker). Nuty sa odtwarzane w tle:
 * sound_play() tylko dopisuje je do kolejki, a przerwanie zegara przelacza
 * kolejne - program nie czeka, az melodia sie skonczy. */

struct note {
    uint16_t freq;               /* Hz; 0 = pauza */
    uint16_t ms;
};

#define SOUND_TRACKS 4             /* tyle sciezek (glosow) melodii naraz */

void sound_play(const struct note *notes, int count);      /* sciezka 0, dzwiek "8-bit" */
/* Nuty na wybranej sciezce, fala WAVE_* z audio.h, glosnosc 0-100 */
void sound_play_track(int track, const struct note *notes, int count, int wave, int volume);
void sound_beep(uint16_t freq, uint16_t ms);
void sound_stop(void);
void sound_wait(void);           /* czeka, az kolejka sie skonczy */
void sound_set_enabled(int on);
int  sound_enabled(void);
void sound_tick(void);           /* wolane z przerwania zegara */

/* Gotowe efekty */
void sound_startup(void);
void sound_shutdown(void);
void sound_eat(void);
void sound_game_over(void);
void sound_record(void);
void sound_melody(void);

#endif
