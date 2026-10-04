// DirectInput -> XInput layout mapping. No Windows calls in here, so it's tested on its own (tests/pad_map_test.c).
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include "pad.h"

int ng_pad_layout_for_vendor(unsigned vid)
{
    return vid == 0x057E ? NG_LAYOUT_SWITCH : NG_LAYOUT_PLAYSTATION;
}

// Face buttons go by position, so the bottom button jumps whichever pad it is: PlayStation Cross = Xbox A = Switch B.
// DirectInput button order (0-based): PlayStation / generic: Square Cross Circle Triangle L1 R1 L2 R2 Share Options;
// Switch Pro: B A Y X L R ZL ZR Minus Plus. Sticks: X,Y left, Z,Rz right; on a PlayStation pad L2/R2 are also the
// analog Rx / Ry.
void ng_pad_default_map(int layout, unsigned vid, NgPadMap *m)
{
    if (layout == NG_LAYOUT_SWITCH) {
        m->a = 0; m->b = 1; m->x = 2; m->y = 3;
    } else {
        m->a = 1; m->b = 2; m->x = 0; m->y = 3;
    }
    m->lb = 4; m->rb = 5; m->lt = 6; m->rt = 7; m->back = 8;
    m->lx = 0; m->ly = 1 | NG_AXIS_INVERT;
    m->rx = 2; m->ry = 5 | NG_AXIS_INVERT;
    m->ltAxis = m->rtAxis = -1;
    if (vid == 0x054C) { m->ltAxis = 3; m->rtAxis = 4; }
}

static int axis_from_name(const char *v)
{
    int inv = 0;
    if (*v == '-') { inv = NG_AXIS_INVERT; v++; }
    static const char *names[] = { "x", "y", "z", "rx", "ry", "rz", "s1", "s2" };
    for (int i = 0; i < 8; i++)
        if (!strcasecmp(v, names[i])) return i | inv;
    if (!strcasecmp(v, "none") || !strcmp(v, "0")) return -1;
    return -2;   // not understood
}

int ng_pad_apply_ini(const char *text, unsigned vid, NgPadMap *m)
{
    int bad = 0;
    // layout= first, so it's the base the other lines adjust whatever order they're in
    for (const char *l = text; *l; l += strcspn(l, "\n") + (l[strcspn(l, "\n")] ? 1 : 0)) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncasecmp(l, "layout", 6)) {
            const char *v = memchr(l, '=', strcspn(l, "\n"));
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                if (!strncasecmp(v, "switch", 6)) ng_pad_default_map(NG_LAYOUT_SWITCH, vid, m);
                else if (!strncasecmp(v, "playstation", 11) || !strncasecmp(v, "generic", 7)) ng_pad_default_map(NG_LAYOUT_PLAYSTATION, vid, m);
                else bad++;
            }
        }
    }
    struct { const char *key; int *dst; int isAxis; } keys[] = {
        { "a", &m->a, 0 }, { "b", &m->b, 0 }, { "x", &m->x, 0 }, { "y", &m->y, 0 },
        { "lb", &m->lb, 0 }, { "rb", &m->rb, 0 }, { "lt", &m->lt, 0 }, { "rt", &m->rt, 0 }, { "back", &m->back, 0 },
        { "lx", &m->lx, 1 }, { "ly", &m->ly, 1 }, { "rx", &m->rx, 1 }, { "ry", &m->ry, 1 },
        { "ltaxis", &m->ltAxis, 1 }, { "rtaxis", &m->rtAxis, 1 },
    };
    for (const char *l = text; *l; l += strcspn(l, "\n") + (l[strcspn(l, "\n")] ? 1 : 0)) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '#' || *l == ';' || *l == '\n' || *l == '\r' || !*l) continue;
        if (!strncasecmp(l, "layout", 6)) continue;
        const char *eq = memchr(l, '=', strcspn(l, "\n"));
        if (!eq) { bad++; continue; }
        size_t kl = (size_t)(eq - l);
        while (kl && (l[kl - 1] == ' ' || l[kl - 1] == '\t')) kl--;
        char tok[16];   // the value: up to the end of the word
        const char *vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') vp++;
        size_t tl = strcspn(vp, " \t\r\n#;");
        if (tl >= sizeof(tok)) tl = sizeof(tok) - 1;
        memcpy(tok, vp, tl);
        tok[tl] = 0;
        const char *v = tok;
        int found = 0;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strlen(keys[i].key) != kl || strncasecmp(l, keys[i].key, kl)) continue;
            found = 1;
            if (keys[i].isAxis) {
                int ax = axis_from_name(v);
                if (ax == -2) bad++; else *keys[i].dst = ax;
            } else {
                int n = atoi(v);   // 1-based, as in Windows' Game Controllers dialog; 0 turns it off
                if (n < 0 || n > 32) bad++; else *keys[i].dst = n - 1;
            }
        }
        if (!found) bad++;
    }
    return bad;
}

static int button(const NgPadMap *m, const NgDiState *s, int idx)
{
    return idx >= 0 && idx < 32 && s->btn[idx];
}

static float stick(const NgDiState *s, int spec)
{
    if (spec < 0) return 0;
    int i = spec & 7;
    if (!(s->axisMask & (1u << i))) return 0;
    float v = s->axis[i] / 32768.0f;
    const float dz = 0.24f;   // XInput's own stick deadzones (7849/32767 and 8689/32767), so the feel matches
    float a = v < 0 ? -v : v;
    if (a <= dz) return 0;
    a = (a - dz) / (1.0f - dz);
    if (a > 1) a = 1;
    v = v < 0 ? -a : a;
    return (spec & NG_AXIS_INVERT) ? -v : v;
}

static int trigger(const NgPadMap *m, const NgDiState *s, int spec)
{
    if (spec < 0) return 0;
    int i = spec & 7;
    return (s->axisMask & (1u << i)) && s->axis[i] > -16384;   // rest is -32768, full pull 32767
}

void ng_pad_map(const NgPadMap *m, const NgDiState *s, Pad *out)
{
    memset(out, 0, sizeof(*out));
    out->lx = stick(s, m->lx);
    out->ly = stick(s, m->ly);
    out->rx = stick(s, m->rx);
    out->ry = stick(s, m->ry);
    out->a = button(m, s, m->a);
    out->b = button(m, s, m->b) || button(m, s, m->x);
    out->y = button(m, s, m->y);
    out->z = button(m, s, m->lt) || button(m, s, m->rt) || trigger(m, s, m->ltAxis) || trigger(m, s, m->rtAxis);
    out->zoomIn = button(m, s, m->rb);
    out->zoomOut = button(m, s, m->lb);
    out->music = button(m, s, m->back);
}
