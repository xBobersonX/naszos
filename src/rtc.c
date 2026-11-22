/* Zegar czasu rzeczywistego (RTC) w ukladzie CMOS. Dziala na baterii, wiec
 * liczy czas takze przy wylaczonym komputerze. Rejestry czytamy przez porty
 * 0x70 (numer rejestru) i 0x71 (wartosc). Wartosci zwykle sa w kodzie BCD
 * (np. 0x59 = 59), a godzina moze byc w trybie 12-godzinnym - rejestr B
 * mowi, jak jest naprawde. */

#include "rtc.h"
#include "io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECONDS  0x00
#define REG_MINUTES  0x02
#define REG_HOURS    0x04
#define REG_DAY      0x07
#define REG_MONTH    0x08
#define REG_YEAR     0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80   /* zegar wlasnie aktualizuje rejestry - nie czytac */
#define STATUS_B_24H      0x02
#define STATUS_B_BINARY   0x04
#define HOUR_PM           0x80

const char *const rtc_day_short[7] = { "Nd", "Pn", "Wt", "Sr", "Cz", "Pt", "So" };
const char *const rtc_day_name[7] = {
    "Niedziela", "Poniedzialek", "Wtorek", "Sroda", "Czwartek", "Piatek", "Sobota",
};
const char *const rtc_month_name[12] = {
    "Styczen", "Luty", "Marzec", "Kwiecien", "Maj", "Czerwiec",
    "Lipiec", "Sierpien", "Wrzesien", "Pazdziernik", "Listopad", "Grudzien",
};
const char *const rtc_month_genitive[12] = {
    "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
    "lipca", "sierpnia", "wrzesnia", "pazdziernika", "listopada", "grudnia",
};

struct raw { uint8_t sec, min, hour, day, month, year; };

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void read_raw(struct raw *r)
{
    /* Aktualizacja trwa ok. 2 ms; limit na wypadek braku zegara RTC */
    for (int i = 0; i < 100000 && (cmos_read(REG_STATUS_A) & STATUS_A_UPDATING); i++)
        ;
    r->sec   = cmos_read(REG_SECONDS);
    r->min   = cmos_read(REG_MINUTES);
    r->hour  = cmos_read(REG_HOURS);
    r->day   = cmos_read(REG_DAY);
    r->month = cmos_read(REG_MONTH);
    r->year  = cmos_read(REG_YEAR);
}

static uint8_t from_bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

void rtc_read(struct rtc_time *t)
{
    /* Przerwanie zegara tez czyta RTC (pasek statusu) - gdyby wcisnelo sie
     * miedzy zapis numeru rejestru a odczyt wartosci, dostalibysmy zly rejestr */
    unsigned long flags = irq_save();

    /* Czytamy, az dwa odczyty z rzedu beda identyczne - wtedy mamy pewnosc,
     * ze zegar nie zmienil wartosci w trakcie (np. 23:59:59 -> 00:00:00) */
    struct raw a, b;
    read_raw(&b);
    do {
        a = b;
        read_raw(&b);
    } while (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
             a.day != b.day || a.month != b.month || a.year != b.year);

    uint8_t status_b = cmos_read(REG_STATUS_B);
    irq_restore(flags);

    int pm = b.hour & HOUR_PM;
    b.hour &= ~HOUR_PM;
    if (!(status_b & STATUS_B_BINARY)) {
        b.sec   = from_bcd(b.sec);
        b.min   = from_bcd(b.min);
        b.hour  = from_bcd(b.hour);
        b.day   = from_bcd(b.day);
        b.month = from_bcd(b.month);
        b.year  = from_bcd(b.year);
    }
    if (!(status_b & STATUS_B_24H)) {    /* tryb 12h: 12 AM = 0:00, 1 PM = 13:00 */
        if (b.hour == 12)
            b.hour = 0;
        if (pm)
            b.hour += 12;
    }

    t->year    = (uint16_t)(2000 + b.year);
    t->month   = b.month;
    t->day     = b.day;
    t->hour    = b.hour;
    t->minute  = b.min;
    t->second  = b.sec;
    t->weekday = (uint8_t)rtc_weekday(t->year, t->month, t->day);
}

/* Algorytm Sakamoto - dzien tygodnia dla daty w kalendarzu gregorianskim */
int rtc_weekday(int year, int month, int day)
{
    static const int offset[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3)
        year--;
    return (year + year / 4 - year / 100 + year / 400 + offset[month - 1] + day) % 7;
}

int rtc_days_in_month(int year, int month)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

static uint8_t to_bcd(uint8_t v)
{
    return (uint8_t)((v / 10) << 4 | v % 10);
}

void rtc_write(const struct rtc_time *t)
{
    unsigned long flags = irq_save();
    uint8_t status_b = cmos_read(REG_STATUS_B);
    int binary = status_b & STATUS_B_BINARY;
    uint8_t hour = t->hour;
    int pm = 0;
    if (!(status_b & STATUS_B_24H)) {    /* tryb 12h: 0:00 = 12 AM, 13:00 = 1 PM */
        pm = hour >= 12;
        hour = (uint8_t)(hour % 12 ? hour % 12 : 12);
    }
    uint8_t v[6] = { t->second, t->minute, hour, t->day, t->month, (uint8_t)(t->year % 100) };
    static const uint8_t regs[6] = { REG_SECONDS, REG_MINUTES, REG_HOURS, REG_DAY, REG_MONTH, REG_YEAR };

    outb(CMOS_INDEX, REG_STATUS_B);      /* SET: zatrzymaj zegar na czas zapisu */
    outb(CMOS_DATA, status_b | 0x80);
    for (int i = 0; i < 6; i++) {
        uint8_t value = binary ? v[i] : to_bcd(v[i]);
        if (i == 2 && pm)
            value |= HOUR_PM;
        outb(CMOS_INDEX, regs[i]);
        outb(CMOS_DATA, value);
    }
    outb(CMOS_INDEX, REG_STATUS_B);
    outb(CMOS_DATA, status_b & ~0x80);
    irq_restore(flags);
}

/* Liczba dni od 1970-01-01 dla daty (algorytm "days from civil") */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_from_unix(int64_t secs, struct rtc_time *t)
{
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    /* Odwrotnosc days_from_civil */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int y = (int)(yoe + era * 400 + (m <= 2));
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)(rem / 3600);
    t->minute = (uint8_t)(rem / 60 % 60);
    t->second = (uint8_t)(rem % 60);
    t->weekday = (uint8_t)rtc_weekday(y, m, d);
}

int64_t rtc_to_unix(const struct rtc_time *t)
{
    return days_from_civil(t->year, t->month, t->day) * 86400 +
           t->hour * 3600 + t->minute * 60 + t->second;
}

/* Ostatnia niedziela miesiaca (dzien) */
static int last_sunday(int year, int month)
{
    int d = rtc_days_in_month(year, month);
    return d - rtc_weekday(year, month, d);
}

/* Czas polski: zima UTC+1, lato UTC+2 (od ostatniej niedzieli marca
 * do ostatniej niedzieli pazdziernika, zmiana o 1:00 UTC) */
int rtc_poland_offset(int64_t utc)
{
    struct rtc_time t;
    rtc_from_unix(utc, &t);
    int64_t start = days_from_civil(t.year, 3, last_sunday(t.year, 3)) * 86400 + 3600;
    int64_t end = days_from_civil(t.year, 10, last_sunday(t.year, 10)) * 86400 + 3600;
    return utc >= start && utc < end ? 7200 : 3600;
}
