/* Schowek systemowy: kopia tekstu w pamieci jadra */
#include "clipboard.h"
#include "heap.h"
#include "string.h"

static char *clip;
static unsigned clip_len;

void clipboard_set(const char *text, unsigned len)
{
    char *c = kmalloc(len + 1);
    if (!c)
        return;
    memcpy(c, text, len);
    c[len] = '\0';
    if (clip)
        kfree(clip);
    clip = c;
    clip_len = len;
}

const char *clipboard_get(unsigned *len)
{
    if (len)
        *len = clip ? clip_len : 0;
    return clip ? clip : "";
}
