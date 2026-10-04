/* DirectInput -> XInput layout mapping, on synthetic device states (no pad needed).
   Build: gcc -I helper/src -o tests/.tmp/pad_map_test tests/pad_map_test.c helper/src/pad_map.c */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pad.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } else printf("PASS %s\n", msg); } while (0)

static NgDiState rest(unsigned mask)
{
    NgDiState s;
    memset(&s, 0, sizeof(s));
    s.axisMask = mask;
    return s;
}

int main(void)
{
    NgPadMap ps, sw, gen;
    ng_pad_default_map(ng_pad_layout_for_vendor(0x054C), 0x054C, &ps);
    ng_pad_default_map(ng_pad_layout_for_vendor(0x057E), 0x057E, &sw);
    ng_pad_default_map(ng_pad_layout_for_vendor(0x046D), 0x046D, &gen);
    CHECK(ng_pad_layout_for_vendor(0x054C) == NG_LAYOUT_PLAYSTATION && ng_pad_layout_for_vendor(0x057E) == NG_LAYOUT_SWITCH, "layout by vendor");

    Pad p;
    NgDiState s = rest(0x3F);   // X Y Z Rx Ry Rz, with the PlayStation triggers resting at -32768
    s.axis[3] = s.axis[4] = -32768;
    ng_pad_map(&ps, &s, &p);
    CHECK(!p.a && !p.b && !p.y && !p.z && !p.zoomIn && !p.zoomOut && !p.music && p.lx == 0 && p.ly == 0 && p.rx == 0 && p.ry == 0, "PlayStation at rest: nothing pressed");

    s.btn[1] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.a && !p.b, "PlayStation Cross = A (jump)"); s.btn[1] = 0;
    s.btn[2] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.b && !p.a, "PlayStation Circle = B"); s.btn[2] = 0;
    s.btn[0] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.b, "PlayStation Square = X = B (punch)"); s.btn[0] = 0;
    s.btn[3] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.y && !p.b, "PlayStation Triangle = Y (pick up)"); s.btn[3] = 0;
    s.btn[4] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.zoomOut && !p.zoomIn, "L1 = LB (zoom out)"); s.btn[4] = 0;
    s.btn[5] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.zoomIn && !p.zoomOut, "R1 = RB (zoom in)"); s.btn[5] = 0;
    s.btn[6] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.z, "L2 button = trigger (Z)"); s.btn[6] = 0;
    s.btn[8] = 1; ng_pad_map(&ps, &s, &p); CHECK(p.music, "Share = Back (music)"); s.btn[8] = 0;
    s.axis[4] = 32767; ng_pad_map(&ps, &s, &p); CHECK(p.z, "R2 analog axis pulled = Z"); s.axis[4] = -32768;
    s.axis[3] = -20000; ng_pad_map(&ps, &s, &p); CHECK(!p.z, "L2 analog barely moved = not Z"); s.axis[3] = -32768;

    s.axis[0] = 32767; ng_pad_map(&ps, &s, &p); CHECK(p.lx > 0.99f, "left stick right = +1"); s.axis[0] = 0;
    s.axis[1] = -32768; ng_pad_map(&ps, &s, &p); CHECK(p.ly > 0.99f, "left stick up (negative on DirectInput) = +1 like XInput"); s.axis[1] = 0;
    s.axis[2] = -32768; ng_pad_map(&ps, &s, &p); CHECK(p.rx < -0.99f, "right stick left (Z axis) = -1"); s.axis[2] = 0;
    s.axis[5] = -32768; ng_pad_map(&ps, &s, &p); CHECK(p.ry > 0.99f, "right stick up (Rz axis) = +1"); s.axis[5] = 0;
    s.axis[0] = 5000; ng_pad_map(&ps, &s, &p); CHECK(p.lx == 0, "stick inside the deadzone = 0"); s.axis[0] = 0;

    NgDiState g = rest(0x27);   // a generic pad: X Y Z Rz only, no analog triggers, rest at 0
    ng_pad_map(&gen, &g, &p);
    CHECK(!p.z && p.lx == 0 && p.ly == 0, "generic pad at rest (axes at 0): no phantom trigger");
    g.btn[7] = 1; ng_pad_map(&gen, &g, &p); CHECK(p.z, "generic R2 button = Z");

    NgDiState w = rest(0x27);
    w.btn[0] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.a && !p.b, "Switch Pro B (bottom) = A (jump), by position"); w.btn[0] = 0;
    w.btn[1] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.b && !p.a, "Switch Pro A (right) = B"); w.btn[1] = 0;
    w.btn[2] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.b, "Switch Pro Y (left) = X = B"); w.btn[2] = 0;
    w.btn[3] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.y, "Switch Pro X (top) = Y (pick up)"); w.btn[3] = 0;
    w.btn[6] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.z, "Switch Pro ZL = Z"); w.btn[6] = 0;
    w.btn[8] = 1; ng_pad_map(&sw, &w, &p); CHECK(p.music, "Switch Pro Minus = Back"); w.btn[8] = 0;

    NgPadMap m = ps;
    int bad = ng_pad_apply_ini("# my pad\na = 3\nlayout=switch\nrb=0\nrx=-rz\nnonsense\nfoo=1\nly=q\n", 0x054C, &m);
    CHECK(bad == 3, "ini: three lines it doesn't understand are counted");
    CHECK(m.a == 2 && m.rb == -1 && m.rx == (5 | NG_AXIS_INVERT), "ini: a=3 -> button index 2, rb=0 turns it off, rx=-rz inverted Rz");
    CHECK(m.b == 1 && m.x == 2, "ini: layout=switch applies before the other lines");
    s = rest(0x3F); s.btn[5] = 1; ng_pad_map(&m, &s, &p);
    CHECK(!p.zoomIn, "ini: a disabled button does nothing");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
