/* pad_map: see pad_map.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad_map.h"

/* the mapping fields the table keeps, by the host_pad button bit they feed */
static const struct { const char *key; int bit; } roles[] = {
    { "a", 0 }, { "b", 1 }, { "x", 2 }, { "y", 3 }, { "leftshoulder", 4 }, { "rightshoulder", 5 },
    { "back", 6 }, { "start", 7 }, { "leftstick", 8 }, { "rightstick", 9 }, { "lefttrigger", 10 },
    { "righttrigger", 11 }, { "dpup", 12 }, { "dpright", 13 }, { "dpdown", 14 }, { "dpleft", 15 },
};

static int parse_src(const char *v, pad_src *s)
{
    memset(s, 0, sizeof *s);
    if (*v == '+') {
        s->half = 1;
        v++;
    } else if (*v == '-') {
        s->half = 2;
        v++;
    }
    if (*v == 'b' && !s->half) {
        s->kind = PAD_SRC_BUTTON;
        s->index = (int16_t)atoi(v + 1);
    } else if (*v == 'a') {
        s->kind = PAD_SRC_AXIS;
        s->index = (int16_t)atoi(v + 1);
        s->invert = strchr(v, '~') != NULL;
    } else if (*v == 'h' && !s->half) {
        const char *dot = strchr(v, '.');
        if (!dot)
            return 0;
        s->kind = PAD_SRC_HAT;
        s->index = (int16_t)atoi(v + 1);
        s->mask = (uint8_t)atoi(dot + 1);
    } else {
        memset(s, 0, sizeof *s);
        return 0;
    }
    return 1;
}

static unsigned hex4(const char *p)
{
    char b[5];
    memcpy(b, p, 4);
    b[4] = 0;
    return (unsigned)strtoul(b, NULL, 16);
}

static void parse_map(const char *text, pad_map *m)
{
    char buf[512];
    char *tok, *save = NULL;
    snprintf(buf, sizeof buf, "%s", text);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        size_t k;
        pad_src s;
        if (!colon)
            continue;
        *colon = 0;
        if (!parse_src(colon + 1, &s))
            continue;
        if (!strcmp(tok, "leftx")) m->lx = s;
        else if (!strcmp(tok, "lefty")) m->ly = s;
        else if (!strcmp(tok, "rightx")) m->rx = s;
        else if (!strcmp(tok, "righty")) m->ry = s;
        else
            for (k = 0; k < sizeof roles / sizeof *roles; k++)
                if (!strcmp(tok, roles[k].key))
                    m->btn[roles[k].bit] = s;
    }
}

int pad_map_find(const char *const *db, size_t count, unsigned vendor, unsigned product, int version, pad_map *m)
{
    size_t i;
    const char *first = NULL;
    for (i = 0; i < count; i++) {
        const char *e = db[i];
        if (hex4(e) != vendor || hex4(e + 4) != product)
            continue;
        if (!first)
            first = e;
        if (version >= 0 && (int)hex4(e + 8) == version) {
            first = e;
            break;
        }
    }
    if (!first)
        return 0;
    {
        const char *name = first + 13, *bar = strchr(name, '|');
        int n;
        memset(m, 0, sizeof *m);
        if (!bar)
            return 0;
        n = (int)(bar - name);
        snprintf(m->name, sizeof m->name, "%.*s", n < 63 ? n : 63, name);
        parse_map(bar + 1, m);
    }
    return 1;
}

/* an axis source as -1..1, or for a half axis 0..1 */
static float axis_value(const pad_src *s, const pad_raw *r)
{
    float v;
    if (s->index < 0 || s->index >= r->naxes)
        return 0;
    v = r->axes[s->index];
    if (s->invert)
        v = -v;
    if (s->half == 1)
        return v > 0 ? v : 0;
    if (s->half == 2)
        return v < 0 ? -v : 0;
    return v;
}

/* a source as an amount 0..1 (a button pressed or not; a whole axis from its far end) */
static float src_amount(const pad_src *s, const pad_raw *r)
{
    switch (s->kind) {
    case PAD_SRC_BUTTON:
        return s->index >= 0 && s->index < r->nbuttons && r->buttons[s->index] ? 1.0f : 0.0f;
    case PAD_SRC_HAT:
        return s->index >= 0 && s->index < r->nhats && (r->hats[s->index] & s->mask) ? 1.0f : 0.0f;
    case PAD_SRC_AXIS: {
        float v = axis_value(s, r);
        return s->half ? v : (v + 1.0f) * 0.5f;
    }
    }
    return 0;
}

static float stick(const pad_src *s, const pad_raw *r)
{
    float v;
    if (s->kind != PAD_SRC_AXIS)
        return 0;
    v = axis_value(s, r);
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

void pad_map_apply(const pad_map *m, const pad_raw *r, host_pad *o)
{
    int b;
    unsigned bits = 0;
    o->x = stick(&m->lx, r);
    o->y = stick(&m->ly, r);
    o->rx = stick(&m->rx, r);
    o->ry = stick(&m->ry, r);
    o->lt = src_amount(&m->btn[10], r);
    o->rt = src_amount(&m->btn[11], r);
    for (b = 0; b < 16; b++) {
        if (m->btn[b].kind == PAD_SRC_NONE)
            continue;
        if (b == 10 || b == 11) {
            if ((b == 10 ? o->lt : o->rt) > 0.5f)
                bits |= 1u << b;
        } else if (m->btn[b].kind == PAD_SRC_AXIS) {
            /* a button on an axis: pressed past half way along it (a half axis from its centre) */
            if (axis_value(&m->btn[b], r) > 0.5f)
                bits |= 1u << b;
        } else if (src_amount(&m->btn[b], r) > 0.5f)
            bits |= 1u << b;
    }
    o->buttons = bits;
}

int pad_style_of_vendor(unsigned vendor)
{
    return vendor == 0x054c ? HOST_PAD_PLAYSTATION : vendor == 0x057e ? HOST_PAD_NINTENDO
         : vendor == 0x045e ? HOST_PAD_XBOX : HOST_PAD_GENERIC;
}

uint64_t pad_hash(const char *text)
{
    uint64_t h = 1469598103934665603ull;
    for (; *text; text++)
        h = (h ^ (unsigned char)*text) * 1099511628211ull;
    return h ? h : 1;
}
