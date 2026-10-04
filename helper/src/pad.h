// The controller state the helper's input uses, and the DirectInput side of it: reading PlayStation / Switch Pro /
// generic pads and mapping them to the same layout XInput gives (A jump, B/X punch, Y pick up, triggers Z, bumpers
// zoom, Back music).
#ifndef NG64_PAD_H
#define NG64_PAD_H

typedef struct { float lx, ly, rx, ry; int a, b, z, zoomIn, zoomOut, y, music, songNext, songPrev; } Pad;

// One DirectInput device's raw state: axes -32768..32767 in DirectInput's order (X, Y, Z, Rx, Ry, Rz, slider 0, 1),
// buttons 0-based. axisMask has bit i set for the axes the device really has.
typedef struct { long axis[8]; unsigned axisMask; unsigned char btn[32]; } NgDiState;

// Where each XInput control lives on a DirectInput device. Buttons are 0-based indexes (-1: none). An axis is
// index | NG_AXIS_INVERT (up is negative on DirectInput, positive on XInput), or -1.
#define NG_AXIS_INVERT 0x100
typedef struct {
    int a, b, x, y, lb, rb, lt, rt, back;
    int lx, ly, rx, ry;
    int ltAxis, rtAxis;   // analog triggers (rest at -32768); pressed past a quarter of the travel
} NgPadMap;

enum { NG_LAYOUT_PLAYSTATION, NG_LAYOUT_SWITCH };

// the layout for a device by USB vendor id (0x054C Sony, 0x057E Nintendo; anything else gets the PlayStation-style
// numbering most generic pads use)
int ng_pad_layout_for_vendor(unsigned vid);
void ng_pad_default_map(int layout, unsigned vid, NgPadMap *m);
// the user's controller.ini on top of a map: "layout=", button numbers (1-based) for a b x y lb rb lt rt back,
// axis names (X Y Z RX RY RZ S1 S2, a leading - inverts) for lx ly rx ry, and ltaxis / rtaxis. Returns the number of
// lines it didn't understand.
int ng_pad_apply_ini(const char *text, unsigned vid, NgPadMap *m);
void ng_pad_map(const NgPadMap *m, const NgDiState *s, Pad *out);

// DirectInput devices (not the XInput-compatible ones, which XInput already reads). Returns 1 when a pad is
// connected. logfn gets a line whenever a device appears or goes.
void ng64_dinput_init(void (*logfn)(const char *), const char *iniPath);
int ng64_dinput_read(Pad *pad);   // merges into pad, which may already hold the XInput state

#endif
