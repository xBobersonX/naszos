#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month;      /* 1-12 */
    uint8_t  day;        /* 1-31 */
    uint8_t  hour, minute, second;
    uint8_t  weekday;    /* 0 = niedziela ... 6 = sobota */
};

void rtc_read(struct rtc_time *t);
void rtc_write(const struct rtc_time *t);            /* ustawia zegar */

/* Czas "uniksowy": sekundy od 1 stycznia 1970 (tak liczy internet) */
void    rtc_from_unix(int64_t secs, struct rtc_time *t);
int64_t rtc_to_unix(const struct rtc_time *t);
int     rtc_poland_offset(int64_t utc);              /* 3600 zima, 7200 lato */

int rtc_weekday(int year, int month, int day);      /* 0 = niedziela */
int rtc_days_in_month(int year, int month);

/* Polskie nazwy (bez ogonkow - tryb tekstowy VGA ich nie ma) */
extern const char *const rtc_day_short[7];          /* "Nd", "Pn", ... */
extern const char *const rtc_day_name[7];           /* "Niedziela", ... */
extern const char *const rtc_month_name[12];        /* "Styczen", ... */
extern const char *const rtc_month_genitive[12];    /* "stycznia", ... */

#endif
