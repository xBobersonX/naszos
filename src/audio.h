#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Dzwiek przez karte AC'97: mikser kilku glosow naraz (akordy, efekty
 * i nagrania WAV jednoczesnie). Kazdy glos to fala o danym ksztalcie:
 *   prostokat - ostry, jak w starych grach ("8-bit")
 *   trojkat   - lagodniejszy, jak flet
 *   sinus     - czysty ton
 *   pianino   - sinus z alikwotem, ktory powoli cichnie
 *   szum      - np. werbel, wybuch */

enum { WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SINE, WAVE_PIANO, WAVE_NOISE };

int  audio_init(void);                /* 1 = jest karta dzwiekowa */
int  audio_available(void);

/* Nuta: czestotliwosc (Hz), dlugosc (ms), ksztalt fali, glosnosc 0-100 */
void audio_note(uint16_t freq, uint16_t ms, int wave, int volume);

/* Nagranie: probki 16-bit (1 = mono, 2 = stereo) z dowolna czestotliwoscia.
 * Dane musza istniec, dopoki audio_pcm_playing() zwraca 1. */
void audio_play_pcm(const int16_t *data, uint32_t frames, int channels, uint32_t rate, int volume);
int  audio_pcm_playing(void);
uint32_t audio_pcm_position(void);    /* ktora probka gra (do paska postepu) */
void audio_stop_all(void);

#endif
