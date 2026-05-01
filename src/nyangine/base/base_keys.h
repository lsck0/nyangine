/**
 * @file base_keys.h
 *
 * Those are from:
 *  - https://github.com/libsdl-org/NYA/blob/main/include/SDL3/SDL_scancode.h
 *  - https://github.com/libsdl-org/NYA/blob/main/include/SDL3/SDL_keycode.h
 * */
#pragma once

#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

#define NYA_KEY_EXTENDED_MASK      (1u << 29)
#define NYA_KEY_SCANCODE_MASK      (1u << 30)
#define NYA_SCANCODE_TO_KEYCODE(X) ((X) | NYA_KEY_SCANCODE_MASK)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCANCODES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    NYA_SCANCODE_UNKNOWN = 0,

    NYA_SCANCODE_A = 4,
    NYA_SCANCODE_B = 5,
    NYA_SCANCODE_C = 6,
    NYA_SCANCODE_D = 7,
    NYA_SCANCODE_E = 8,
    NYA_SCANCODE_F = 9,
    NYA_SCANCODE_G = 10,
    NYA_SCANCODE_H = 11,
    NYA_SCANCODE_I = 12,
    NYA_SCANCODE_J = 13,
    NYA_SCANCODE_K = 14,
    NYA_SCANCODE_L = 15,
    NYA_SCANCODE_M = 16,
    NYA_SCANCODE_N = 17,
    NYA_SCANCODE_O = 18,
    NYA_SCANCODE_P = 19,
    NYA_SCANCODE_Q = 20,
    NYA_SCANCODE_R = 21,
    NYA_SCANCODE_S = 22,
    NYA_SCANCODE_T = 23,
    NYA_SCANCODE_U = 24,
    NYA_SCANCODE_V = 25,
    NYA_SCANCODE_W = 26,
    NYA_SCANCODE_X = 27,
    NYA_SCANCODE_Y = 28,
    NYA_SCANCODE_Z = 29,

    NYA_SCANCODE_1 = 30,
    NYA_SCANCODE_2 = 31,
    NYA_SCANCODE_3 = 32,
    NYA_SCANCODE_4 = 33,
    NYA_SCANCODE_5 = 34,
    NYA_SCANCODE_6 = 35,
    NYA_SCANCODE_7 = 36,
    NYA_SCANCODE_8 = 37,
    NYA_SCANCODE_9 = 38,
    NYA_SCANCODE_0 = 39,

    NYA_SCANCODE_RETURN    = 40,
    NYA_SCANCODE_ESCAPE    = 41,
    NYA_SCANCODE_BACKSPACE = 42,
    NYA_SCANCODE_TAB       = 43,
    NYA_SCANCODE_SPACE     = 44,

    NYA_SCANCODE_MINUS        = 45,
    NYA_SCANCODE_EQUALS       = 46,
    NYA_SCANCODE_LEFTBRACKET  = 47,
    NYA_SCANCODE_RIGHTBRACKET = 48,
    NYA_SCANCODE_BACKSLASH    = 49,
    NYA_SCANCODE_NONUSHASH    = 50,
    NYA_SCANCODE_SEMICOLON    = 51,
    NYA_SCANCODE_APOSTROPHE   = 52,
    NYA_SCANCODE_GRAVE        = 53,
    NYA_SCANCODE_COMMA        = 54,
    NYA_SCANCODE_PERIOD       = 55,
    NYA_SCANCODE_SLASH        = 56,

    NYA_SCANCODE_CAPSLOCK = 57,

    NYA_SCANCODE_F1  = 58,
    NYA_SCANCODE_F2  = 59,
    NYA_SCANCODE_F3  = 60,
    NYA_SCANCODE_F4  = 61,
    NYA_SCANCODE_F5  = 62,
    NYA_SCANCODE_F6  = 63,
    NYA_SCANCODE_F7  = 64,
    NYA_SCANCODE_F8  = 65,
    NYA_SCANCODE_F9  = 66,
    NYA_SCANCODE_F10 = 67,
    NYA_SCANCODE_F11 = 68,
    NYA_SCANCODE_F12 = 69,

    NYA_SCANCODE_PRINTSCREEN = 70,
    NYA_SCANCODE_SCROLLLOCK  = 71,
    NYA_SCANCODE_PAUSE       = 72,
    NYA_SCANCODE_INSERT      = 73,
    NYA_SCANCODE_HOME        = 74,
    NYA_SCANCODE_PAGEUP      = 75,
    NYA_SCANCODE_DELETE      = 76,
    NYA_SCANCODE_END         = 77,
    NYA_SCANCODE_PAGEDOWN    = 78,
    NYA_SCANCODE_RIGHT       = 79,
    NYA_SCANCODE_LEFT        = 80,
    NYA_SCANCODE_DOWN        = 81,
    NYA_SCANCODE_UP          = 82,

    NYA_SCANCODE_NUMLOCKCLEAR = 83,
    NYA_SCANCODE_KP_DIVIDE    = 84,
    NYA_SCANCODE_KP_MULTIPLY  = 85,
    NYA_SCANCODE_KP_MINUS     = 86,
    NYA_SCANCODE_KP_PLUS      = 87,
    NYA_SCANCODE_KP_ENTER     = 88,
    NYA_SCANCODE_KP_1         = 89,
    NYA_SCANCODE_KP_2         = 90,
    NYA_SCANCODE_KP_3         = 91,
    NYA_SCANCODE_KP_4         = 92,
    NYA_SCANCODE_KP_5         = 93,
    NYA_SCANCODE_KP_6         = 94,
    NYA_SCANCODE_KP_7         = 95,
    NYA_SCANCODE_KP_8         = 96,
    NYA_SCANCODE_KP_9         = 97,
    NYA_SCANCODE_KP_0         = 98,
    NYA_SCANCODE_KP_PERIOD    = 99,

    NYA_SCANCODE_NONUSBACKSLASH = 100,
    NYA_SCANCODE_APPLICATION    = 101,
    NYA_SCANCODE_POWER          = 102,

    NYA_SCANCODE_KP_EQUALS      = 103,
    NYA_SCANCODE_F13            = 104,
    NYA_SCANCODE_F14            = 105,
    NYA_SCANCODE_F15            = 106,
    NYA_SCANCODE_F16            = 107,
    NYA_SCANCODE_F17            = 108,
    NYA_SCANCODE_F18            = 109,
    NYA_SCANCODE_F19            = 110,
    NYA_SCANCODE_F20            = 111,
    NYA_SCANCODE_F21            = 112,
    NYA_SCANCODE_F22            = 113,
    NYA_SCANCODE_F23            = 114,
    NYA_SCANCODE_F24            = 115,
    NYA_SCANCODE_EXECUTE        = 116,
    NYA_SCANCODE_HELP           = 117,
    NYA_SCANCODE_MENU           = 118,
    NYA_SCANCODE_SELECT         = 119,
    NYA_SCANCODE_STOP           = 120,
    NYA_SCANCODE_AGAIN          = 121,
    NYA_SCANCODE_UNDO           = 122,
    NYA_SCANCODE_CUT            = 123,
    NYA_SCANCODE_COPY           = 124,
    NYA_SCANCODE_PASTE          = 125,
    NYA_SCANCODE_FIND           = 126,
    NYA_SCANCODE_MUTE           = 127,
    NYA_SCANCODE_VOLUMEUP       = 128,
    NYA_SCANCODE_VOLUMEDOWN     = 129,
    NYA_SCANCODE_KP_COMMA       = 133,
    NYA_SCANCODE_KP_EQUALSAS400 = 134,

    NYA_SCANCODE_INTERNATIONAL1 = 135,

    NYA_SCANCODE_INTERNATIONAL2 = 136,
    NYA_SCANCODE_INTERNATIONAL3 = 137,
    NYA_SCANCODE_INTERNATIONAL4 = 138,
    NYA_SCANCODE_INTERNATIONAL5 = 139,
    NYA_SCANCODE_INTERNATIONAL6 = 140,
    NYA_SCANCODE_INTERNATIONAL7 = 141,
    NYA_SCANCODE_INTERNATIONAL8 = 142,
    NYA_SCANCODE_INTERNATIONAL9 = 143,
    NYA_SCANCODE_LANG1          = 144,
    NYA_SCANCODE_LANG2          = 145,
    NYA_SCANCODE_LANG3          = 146,
    NYA_SCANCODE_LANG4          = 147,
    NYA_SCANCODE_LANG5          = 148,
    NYA_SCANCODE_LANG6          = 149,
    NYA_SCANCODE_LANG7          = 150,
    NYA_SCANCODE_LANG8          = 151,
    NYA_SCANCODE_LANG9          = 152,

    NYA_SCANCODE_ALTERASE   = 153,
    NYA_SCANCODE_SYSREQ     = 154,
    NYA_SCANCODE_CANCEL     = 155,
    NYA_SCANCODE_CLEAR      = 156,
    NYA_SCANCODE_PRIOR      = 157,
    NYA_SCANCODE_RETURN2    = 158,
    NYA_SCANCODE_SEPARATOR  = 159,
    NYA_SCANCODE_OUT        = 160,
    NYA_SCANCODE_OPER       = 161,
    NYA_SCANCODE_CLEARAGAIN = 162,
    NYA_SCANCODE_CRSEL      = 163,
    NYA_SCANCODE_EXSEL      = 164,

    NYA_SCANCODE_KP_00              = 176,
    NYA_SCANCODE_KP_000             = 177,
    NYA_SCANCODE_THOUSANDSSEPARATOR = 178,
    NYA_SCANCODE_DECIMALSEPARATOR   = 179,
    NYA_SCANCODE_CURRENCYUNIT       = 180,
    NYA_SCANCODE_CURRENCYSUBUNIT    = 181,
    NYA_SCANCODE_KP_LEFTPAREN       = 182,
    NYA_SCANCODE_KP_RIGHTPAREN      = 183,
    NYA_SCANCODE_KP_LEFTBRACE       = 184,
    NYA_SCANCODE_KP_RIGHTBRACE      = 185,
    NYA_SCANCODE_KP_TAB             = 186,
    NYA_SCANCODE_KP_BACKSPACE       = 187,
    NYA_SCANCODE_KP_A               = 188,
    NYA_SCANCODE_KP_B               = 189,
    NYA_SCANCODE_KP_C               = 190,
    NYA_SCANCODE_KP_D               = 191,
    NYA_SCANCODE_KP_E               = 192,
    NYA_SCANCODE_KP_F               = 193,
    NYA_SCANCODE_KP_XOR             = 194,
    NYA_SCANCODE_KP_POWER           = 195,
    NYA_SCANCODE_KP_PERCENT         = 196,
    NYA_SCANCODE_KP_LESS            = 197,
    NYA_SCANCODE_KP_GREATER         = 198,
    NYA_SCANCODE_KP_AMPERSAND       = 199,
    NYA_SCANCODE_KP_DBLAMPERSAND    = 200,
    NYA_SCANCODE_KP_VERTICALBAR     = 201,
    NYA_SCANCODE_KP_DBLVERTICALBAR  = 202,
    NYA_SCANCODE_KP_COLON           = 203,
    NYA_SCANCODE_KP_HASH            = 204,
    NYA_SCANCODE_KP_SPACE           = 205,
    NYA_SCANCODE_KP_AT              = 206,
    NYA_SCANCODE_KP_EXCLAM          = 207,
    NYA_SCANCODE_KP_MEMSTORE        = 208,
    NYA_SCANCODE_KP_MEMRECALL       = 209,
    NYA_SCANCODE_KP_MEMCLEAR        = 210,
    NYA_SCANCODE_KP_MEMADD          = 211,
    NYA_SCANCODE_KP_MEMSUBTRACT     = 212,
    NYA_SCANCODE_KP_MEMMULTIPLY     = 213,
    NYA_SCANCODE_KP_MEMDIVIDE       = 214,
    NYA_SCANCODE_KP_PLUSMINUS       = 215,
    NYA_SCANCODE_KP_CLEAR           = 216,
    NYA_SCANCODE_KP_CLEARENTRY      = 217,
    NYA_SCANCODE_KP_BINARY          = 218,
    NYA_SCANCODE_KP_OCTAL           = 219,
    NYA_SCANCODE_KP_DECIMAL         = 220,
    NYA_SCANCODE_KP_HEXADECIMAL     = 221,

    NYA_SCANCODE_LCTRL  = 224,
    NYA_SCANCODE_LSHIFT = 225,
    NYA_SCANCODE_LALT   = 226,
    NYA_SCANCODE_LGUI   = 227,
    NYA_SCANCODE_RCTRL  = 228,
    NYA_SCANCODE_RSHIFT = 229,
    NYA_SCANCODE_RALT   = 230,
    NYA_SCANCODE_RGUI   = 231,

    NYA_SCANCODE_MODE = 257,

    NYA_SCANCODE_SLEEP = 258,
    NYA_SCANCODE_WAKE  = 259,

    NYA_SCANCODE_CHANNEL_INCREMENT = 260,
    NYA_SCANCODE_CHANNEL_DECREMENT = 261,

    NYA_SCANCODE_MEDIA_PLAY           = 262,
    NYA_SCANCODE_MEDIA_PAUSE          = 263,
    NYA_SCANCODE_MEDIA_RECORD         = 264,
    NYA_SCANCODE_MEDIA_FAST_FORWARD   = 265,
    NYA_SCANCODE_MEDIA_REWIND         = 266,
    NYA_SCANCODE_MEDIA_NEXT_TRACK     = 267,
    NYA_SCANCODE_MEDIA_PREVIOUS_TRACK = 268,
    NYA_SCANCODE_MEDIA_STOP           = 269,
    NYA_SCANCODE_MEDIA_EJECT          = 270,
    NYA_SCANCODE_MEDIA_PLAY_PAUSE     = 271,
    NYA_SCANCODE_MEDIA_SELECT         = 272,

    NYA_SCANCODE_AC_NEW        = 273,
    NYA_SCANCODE_AC_OPEN       = 274,
    NYA_SCANCODE_AC_CLOSE      = 275,
    NYA_SCANCODE_AC_EXIT       = 276,
    NYA_SCANCODE_AC_SAVE       = 277,
    NYA_SCANCODE_AC_PRINT      = 278,
    NYA_SCANCODE_AC_PROPERTIES = 279,

    NYA_SCANCODE_AC_SEARCH    = 280,
    NYA_SCANCODE_AC_HOME      = 281,
    NYA_SCANCODE_AC_BACK      = 282,
    NYA_SCANCODE_AC_FORWARD   = 283,
    NYA_SCANCODE_AC_STOP      = 284,
    NYA_SCANCODE_AC_REFRESH   = 285,
    NYA_SCANCODE_AC_BOOKMARKS = 286,

    NYA_SCANCODE_SOFTLEFT = 287,

    NYA_SCANCODE_SOFTRIGHT = 288,

    NYA_SCANCODE_CALL    = 289,
    NYA_SCANCODE_ENDCALL = 290,

    NYA_SCANCODE_RESERVED = 400,

    NYA_SCANCODE_COUNT = 512
} NYA_Scancode;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * KEYCODES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u32                         NYA_Keycode;
NYA_API NYA_EXTERN NYA_ConstCString nya_keycode_to_cstring(NYA_Keycode keycode);

#define NYA_KEY_UNKNOWN              0x00000000u /**< 0 */
#define NYA_KEY_RETURN               0x0000000du /**< '\r' */
#define NYA_KEY_ESCAPE               0x0000001bu /**< '\x1B' */
#define NYA_KEY_BACKSPACE            0x00000008u /**< '\b' */
#define NYA_KEY_TAB                  0x00000009u /**< '\t' */
#define NYA_KEY_SPACE                0x00000020u /**< ' ' */
#define NYA_KEY_EXCLAIM              0x00000021u /**< '!' */
#define NYA_KEY_DBLAPOSTROPHE        0x00000022u /**< '"' */
#define NYA_KEY_HASH                 0x00000023u /**< '#' */
#define NYA_KEY_DOLLAR               0x00000024u /**< '$' */
#define NYA_KEY_PERCENT              0x00000025u /**< '%' */
#define NYA_KEY_AMPERSAND            0x00000026u /**< '&' */
#define NYA_KEY_APOSTROPHE           0x00000027u /**< '\'' */
#define NYA_KEY_LEFTPAREN            0x00000028u /**< '(' */
#define NYA_KEY_RIGHTPAREN           0x00000029u /**< ')' */
#define NYA_KEY_ASTERISK             0x0000002au /**< '*' */
#define NYA_KEY_PLUS                 0x0000002bu /**< '+' */
#define NYA_KEY_COMMA                0x0000002cu /**< ',' */
#define NYA_KEY_MINUS                0x0000002du /**< '-' */
#define NYA_KEY_PERIOD               0x0000002eu /**< '.' */
#define NYA_KEY_SLASH                0x0000002fu /**< '/' */
#define NYA_KEY_0                    0x00000030u /**< '0' */
#define NYA_KEY_1                    0x00000031u /**< '1' */
#define NYA_KEY_2                    0x00000032u /**< '2' */
#define NYA_KEY_3                    0x00000033u /**< '3' */
#define NYA_KEY_4                    0x00000034u /**< '4' */
#define NYA_KEY_5                    0x00000035u /**< '5' */
#define NYA_KEY_6                    0x00000036u /**< '6' */
#define NYA_KEY_7                    0x00000037u /**< '7' */
#define NYA_KEY_8                    0x00000038u /**< '8' */
#define NYA_KEY_9                    0x00000039u /**< '9' */
#define NYA_KEY_COLON                0x0000003au /**< ':' */
#define NYA_KEY_SEMICOLON            0x0000003bu /**< ';' */
#define NYA_KEY_LESS                 0x0000003cu /**< '<' */
#define NYA_KEY_EQUALS               0x0000003du /**< '=' */
#define NYA_KEY_GREATER              0x0000003eu /**< '>' */
#define NYA_KEY_QUESTION             0x0000003fu /**< '?' */
#define NYA_KEY_AT                   0x00000040u /**< '@' */
#define NYA_KEY_LEFTBRACKET          0x0000005bu /**< '[' */
#define NYA_KEY_BACKSLASH            0x0000005cu /**< '\\' */
#define NYA_KEY_RIGHTBRACKET         0x0000005du /**< ']' */
#define NYA_KEY_CARET                0x0000005eu /**< '^' */
#define NYA_KEY_UNDERSCORE           0x0000005fu /**< '_' */
#define NYA_KEY_GRAVE                0x00000060u /**< '`' */
#define NYA_KEY_A                    0x00000061u /**< 'a' */
#define NYA_KEY_B                    0x00000062u /**< 'b' */
#define NYA_KEY_C                    0x00000063u /**< 'c' */
#define NYA_KEY_D                    0x00000064u /**< 'd' */
#define NYA_KEY_E                    0x00000065u /**< 'e' */
#define NYA_KEY_F                    0x00000066u /**< 'f' */
#define NYA_KEY_G                    0x00000067u /**< 'g' */
#define NYA_KEY_H                    0x00000068u /**< 'h' */
#define NYA_KEY_I                    0x00000069u /**< 'i' */
#define NYA_KEY_J                    0x0000006au /**< 'j' */
#define NYA_KEY_K                    0x0000006bu /**< 'k' */
#define NYA_KEY_L                    0x0000006cu /**< 'l' */
#define NYA_KEY_M                    0x0000006du /**< 'm' */
#define NYA_KEY_N                    0x0000006eu /**< 'n' */
#define NYA_KEY_O                    0x0000006fu /**< 'o' */
#define NYA_KEY_P                    0x00000070u /**< 'p' */
#define NYA_KEY_Q                    0x00000071u /**< 'q' */
#define NYA_KEY_R                    0x00000072u /**< 'r' */
#define NYA_KEY_S                    0x00000073u /**< 's' */
#define NYA_KEY_T                    0x00000074u /**< 't' */
#define NYA_KEY_U                    0x00000075u /**< 'u' */
#define NYA_KEY_V                    0x00000076u /**< 'v' */
#define NYA_KEY_W                    0x00000077u /**< 'w' */
#define NYA_KEY_X                    0x00000078u /**< 'x' */
#define NYA_KEY_Y                    0x00000079u /**< 'y' */
#define NYA_KEY_Z                    0x0000007au /**< 'z' */
#define NYA_KEY_LEFTBRACE            0x0000007bu /**< '{' */
#define NYA_KEY_PIPE                 0x0000007cu /**< '|' */
#define NYA_KEY_RIGHTBRACE           0x0000007du /**< '}' */
#define NYA_KEY_TILDE                0x0000007eu /**< '~' */
#define NYA_KEY_DELETE               0x0000007fu /**< '\x7F' */
#define NYA_KEY_PLUSMINUS            0x000000b1u /**< '\xB1' */
#define NYA_KEY_CAPSLOCK             0x40000039u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CAPSLOCK) */
#define NYA_KEY_F1                   0x4000003au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F1) */
#define NYA_KEY_F2                   0x4000003bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F2) */
#define NYA_KEY_F3                   0x4000003cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F3) */
#define NYA_KEY_F4                   0x4000003du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F4) */
#define NYA_KEY_F5                   0x4000003eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F5) */
#define NYA_KEY_F6                   0x4000003fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F6) */
#define NYA_KEY_F7                   0x40000040u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F7) */
#define NYA_KEY_F8                   0x40000041u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F8) */
#define NYA_KEY_F9                   0x40000042u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F9) */
#define NYA_KEY_F10                  0x40000043u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F10) */
#define NYA_KEY_F11                  0x40000044u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F11) */
#define NYA_KEY_F12                  0x40000045u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F12) */
#define NYA_KEY_PRINTSCREEN          0x40000046u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PRINTSCREEN) */
#define NYA_KEY_SCROLLLOCK           0x40000047u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SCROLLLOCK) */
#define NYA_KEY_PAUSE                0x40000048u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PAUSE) */
#define NYA_KEY_INSERT               0x40000049u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_INSERT) */
#define NYA_KEY_HOME                 0x4000004au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_HOME) */
#define NYA_KEY_PAGEUP               0x4000004bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PAGEUP) */
#define NYA_KEY_END                  0x4000004du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_END) */
#define NYA_KEY_PAGEDOWN             0x4000004eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PAGEDOWN) */
#define NYA_KEY_RIGHT                0x4000004fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RIGHT) */
#define NYA_KEY_LEFT                 0x40000050u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_LEFT) */
#define NYA_KEY_DOWN                 0x40000051u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_DOWN) */
#define NYA_KEY_UP                   0x40000052u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_UP) */
#define NYA_KEY_NUMLOCKCLEAR         0x40000053u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_NUMLOCKCLEAR) */
#define NYA_KEY_KP_DIVIDE            0x40000054u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_DIVIDE) */
#define NYA_KEY_KP_MULTIPLY          0x40000055u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MULTIPLY) */
#define NYA_KEY_KP_MINUS             0x40000056u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MINUS) */
#define NYA_KEY_KP_PLUS              0x40000057u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_PLUS) */
#define NYA_KEY_KP_ENTER             0x40000058u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_ENTER) */
#define NYA_KEY_KP_1                 0x40000059u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_1) */
#define NYA_KEY_KP_2                 0x4000005au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_2) */
#define NYA_KEY_KP_3                 0x4000005bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_3) */
#define NYA_KEY_KP_4                 0x4000005cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_4) */
#define NYA_KEY_KP_5                 0x4000005du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_5) */
#define NYA_KEY_KP_6                 0x4000005eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_6) */
#define NYA_KEY_KP_7                 0x4000005fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_7) */
#define NYA_KEY_KP_8                 0x40000060u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_8) */
#define NYA_KEY_KP_9                 0x40000061u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_9) */
#define NYA_KEY_KP_0                 0x40000062u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_0) */
#define NYA_KEY_KP_PERIOD            0x40000063u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_PERIOD) */
#define NYA_KEY_APPLICATION          0x40000065u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_APPLICATION) */
#define NYA_KEY_POWER                0x40000066u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_POWER) */
#define NYA_KEY_KP_EQUALS            0x40000067u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_EQUALS) */
#define NYA_KEY_F13                  0x40000068u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F13) */
#define NYA_KEY_F14                  0x40000069u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F14) */
#define NYA_KEY_F15                  0x4000006au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F15) */
#define NYA_KEY_F16                  0x4000006bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F16) */
#define NYA_KEY_F17                  0x4000006cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F17) */
#define NYA_KEY_F18                  0x4000006du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F18) */
#define NYA_KEY_F19                  0x4000006eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F19) */
#define NYA_KEY_F20                  0x4000006fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F20) */
#define NYA_KEY_F21                  0x40000070u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F21) */
#define NYA_KEY_F22                  0x40000071u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F22) */
#define NYA_KEY_F23                  0x40000072u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F23) */
#define NYA_KEY_F24                  0x40000073u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_F24) */
#define NYA_KEY_EXECUTE              0x40000074u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_EXECUTE) */
#define NYA_KEY_HELP                 0x40000075u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_HELP) */
#define NYA_KEY_MENU                 0x40000076u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MENU) */
#define NYA_KEY_SELECT               0x40000077u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SELECT) */
#define NYA_KEY_STOP                 0x40000078u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_STOP) */
#define NYA_KEY_AGAIN                0x40000079u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AGAIN) */
#define NYA_KEY_UNDO                 0x4000007au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_UNDO) */
#define NYA_KEY_CUT                  0x4000007bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CUT) */
#define NYA_KEY_COPY                 0x4000007cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_COPY) */
#define NYA_KEY_PASTE                0x4000007du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PASTE) */
#define NYA_KEY_FIND                 0x4000007eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_FIND) */
#define NYA_KEY_MUTE                 0x4000007fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MUTE) */
#define NYA_KEY_VOLUMEUP             0x40000080u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_VOLUMEUP) */
#define NYA_KEY_VOLUMEDOWN           0x40000081u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_VOLUMEDOWN) */
#define NYA_KEY_KP_COMMA             0x40000085u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_COMMA) */
#define NYA_KEY_KP_EQUALSAS400       0x40000086u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_EQUALSAS400) */
#define NYA_KEY_ALTERASE             0x40000099u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_ALTERASE) */
#define NYA_KEY_SYSREQ               0x4000009au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SYSREQ) */
#define NYA_KEY_CANCEL               0x4000009bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CANCEL) */
#define NYA_KEY_CLEAR                0x4000009cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CLEAR) */
#define NYA_KEY_PRIOR                0x4000009du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_PRIOR) */
#define NYA_KEY_RETURN2              0x4000009eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RETURN2) */
#define NYA_KEY_SEPARATOR            0x4000009fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SEPARATOR) */
#define NYA_KEY_OUT                  0x400000a0u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_OUT) */
#define NYA_KEY_OPER                 0x400000a1u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_OPER) */
#define NYA_KEY_CLEARAGAIN           0x400000a2u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CLEARAGAIN) */
#define NYA_KEY_CRSEL                0x400000a3u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CRSEL) */
#define NYA_KEY_EXSEL                0x400000a4u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_EXSEL) */
#define NYA_KEY_KP_00                0x400000b0u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_00) */
#define NYA_KEY_KP_000               0x400000b1u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_000) */
#define NYA_KEY_THOUSANDSSEPARATOR   0x400000b2u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_THOUSANDSSEPARATOR) */
#define NYA_KEY_DECIMALSEPARATOR     0x400000b3u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_DECIMALSEPARATOR) */
#define NYA_KEY_CURRENCYUNIT         0x400000b4u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CURRENCYUNIT) */
#define NYA_KEY_CURRENCYSUBUNIT      0x400000b5u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CURRENCYSUBUNIT) */
#define NYA_KEY_KP_LEFTPAREN         0x400000b6u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_LEFTPAREN) */
#define NYA_KEY_KP_RIGHTPAREN        0x400000b7u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_RIGHTPAREN) */
#define NYA_KEY_KP_LEFTBRACE         0x400000b8u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_LEFTBRACE) */
#define NYA_KEY_KP_RIGHTBRACE        0x400000b9u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_RIGHTBRACE) */
#define NYA_KEY_KP_TAB               0x400000bau /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_TAB) */
#define NYA_KEY_KP_BACKSPACE         0x400000bbu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_BACKSPACE) */
#define NYA_KEY_KP_A                 0x400000bcu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_A) */
#define NYA_KEY_KP_B                 0x400000bdu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_B) */
#define NYA_KEY_KP_C                 0x400000beu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_C) */
#define NYA_KEY_KP_D                 0x400000bfu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_D) */
#define NYA_KEY_KP_E                 0x400000c0u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_E) */
#define NYA_KEY_KP_F                 0x400000c1u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_F) */
#define NYA_KEY_KP_XOR               0x400000c2u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_XOR) */
#define NYA_KEY_KP_POWER             0x400000c3u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_POWER) */
#define NYA_KEY_KP_PERCENT           0x400000c4u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_PERCENT) */
#define NYA_KEY_KP_LESS              0x400000c5u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_LESS) */
#define NYA_KEY_KP_GREATER           0x400000c6u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_GREATER) */
#define NYA_KEY_KP_AMPERSAND         0x400000c7u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_AMPERSAND) */
#define NYA_KEY_KP_DBLAMPERSAND      0x400000c8u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_DBLAMPERSAND) */
#define NYA_KEY_KP_VERTICALBAR       0x400000c9u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_VERTICALBAR) */
#define NYA_KEY_KP_DBLVERTICALBAR    0x400000cau /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_DBLVERTICALBAR) */
#define NYA_KEY_KP_COLON             0x400000cbu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_COLON) */
#define NYA_KEY_KP_HASH              0x400000ccu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_HASH) */
#define NYA_KEY_KP_SPACE             0x400000cdu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_SPACE) */
#define NYA_KEY_KP_AT                0x400000ceu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_AT) */
#define NYA_KEY_KP_EXCLAM            0x400000cfu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_EXCLAM) */
#define NYA_KEY_KP_MEMSTORE          0x400000d0u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMSTORE) */
#define NYA_KEY_KP_MEMRECALL         0x400000d1u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMRECALL) */
#define NYA_KEY_KP_MEMCLEAR          0x400000d2u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMCLEAR) */
#define NYA_KEY_KP_MEMADD            0x400000d3u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMADD) */
#define NYA_KEY_KP_MEMSUBTRACT       0x400000d4u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMSUBTRACT) */
#define NYA_KEY_KP_MEMMULTIPLY       0x400000d5u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMMULTIPLY) */
#define NYA_KEY_KP_MEMDIVIDE         0x400000d6u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_MEMDIVIDE) */
#define NYA_KEY_KP_PLUSMINUS         0x400000d7u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_PLUSMINUS) */
#define NYA_KEY_KP_CLEAR             0x400000d8u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_CLEAR) */
#define NYA_KEY_KP_CLEARENTRY        0x400000d9u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_CLEARENTRY) */
#define NYA_KEY_KP_BINARY            0x400000dau /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_BINARY) */
#define NYA_KEY_KP_OCTAL             0x400000dbu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_OCTAL) */
#define NYA_KEY_KP_DECIMAL           0x400000dcu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_DECIMAL) */
#define NYA_KEY_KP_HEXADECIMAL       0x400000ddu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_KP_HEXADECIMAL) */
#define NYA_KEY_LCTRL                0x400000e0u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_LCTRL) */
#define NYA_KEY_LSHIFT               0x400000e1u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_LSHIFT) */
#define NYA_KEY_LALT                 0x400000e2u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_LALT) */
#define NYA_KEY_LGUI                 0x400000e3u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_LGUI) */
#define NYA_KEY_RCTRL                0x400000e4u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RCTRL) */
#define NYA_KEY_RSHIFT               0x400000e5u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RSHIFT) */
#define NYA_KEY_RALT                 0x400000e6u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RALT) */
#define NYA_KEY_RGUI                 0x400000e7u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_RGUI) */
#define NYA_KEY_MODE                 0x40000101u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MODE) */
#define NYA_KEY_SLEEP                0x40000102u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SLEEP) */
#define NYA_KEY_WAKE                 0x40000103u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_WAKE) */
#define NYA_KEY_CHANNEL_INCREMENT    0x40000104u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CHANNEL_INCREMENT) */
#define NYA_KEY_CHANNEL_DECREMENT    0x40000105u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CHANNEL_DECREMENT) */
#define NYA_KEY_MEDIA_PLAY           0x40000106u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_PLAY) */
#define NYA_KEY_MEDIA_PAUSE          0x40000107u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_PAUSE) */
#define NYA_KEY_MEDIA_RECORD         0x40000108u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_RECORD) */
#define NYA_KEY_MEDIA_FAST_FORWARD   0x40000109u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_FAST_FORWARD) */
#define NYA_KEY_MEDIA_REWIND         0x4000010au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_REWIND) */
#define NYA_KEY_MEDIA_NEXT_TRACK     0x4000010bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_NEXT_TRACK) */
#define NYA_KEY_MEDIA_PREVIOUS_TRACK 0x4000010cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_PREVIOUS_TRACK) */
#define NYA_KEY_MEDIA_STOP           0x4000010du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_STOP) */
#define NYA_KEY_MEDIA_EJECT          0x4000010eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_EJECT) */
#define NYA_KEY_MEDIA_PLAY_PAUSE     0x4000010fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_PLAY_PAUSE) */
#define NYA_KEY_MEDIA_SELECT         0x40000110u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_MEDIA_SELECT) */
#define NYA_KEY_AC_NEW               0x40000111u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_NEW) */
#define NYA_KEY_AC_OPEN              0x40000112u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_OPEN) */
#define NYA_KEY_AC_CLOSE             0x40000113u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_CLOSE) */
#define NYA_KEY_AC_EXIT              0x40000114u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_EXIT) */
#define NYA_KEY_AC_SAVE              0x40000115u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_SAVE) */
#define NYA_KEY_AC_PRINT             0x40000116u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_PRINT) */
#define NYA_KEY_AC_PROPERTIES        0x40000117u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_PROPERTIES) */
#define NYA_KEY_AC_SEARCH            0x40000118u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_SEARCH) */
#define NYA_KEY_AC_HOME              0x40000119u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_HOME) */
#define NYA_KEY_AC_BACK              0x4000011au /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_BACK) */
#define NYA_KEY_AC_FORWARD           0x4000011bu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_FORWARD) */
#define NYA_KEY_AC_STOP              0x4000011cu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_STOP) */
#define NYA_KEY_AC_REFRESH           0x4000011du /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_REFRESH) */
#define NYA_KEY_AC_BOOKMARKS         0x4000011eu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_AC_BOOKMARKS) */
#define NYA_KEY_SOFTLEFT             0x4000011fu /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SOFTLEFT) */
#define NYA_KEY_SOFTRIGHT            0x40000120u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_SOFTRIGHT) */
#define NYA_KEY_CALL                 0x40000121u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_CALL) */
#define NYA_KEY_ENDCALL              0x40000122u /**< NYA_SCANCODE_TO_KEYCODE(NYA_SCANCODE_ENDCALL) */
#define NYA_KEY_LEFT_TAB             0x20000001u /**< Extended key Left Tab */
#define NYA_KEY_LEVEL5_SHIFT         0x20000002u /**< Extended key Level 5 Shift */
#define NYA_KEY_MULTI_KEY_COMPOSE    0x20000003u /**< Extended key Multi-key Compose */
#define NYA_KEY_LMETA                0x20000004u /**< Extended key Left Meta */
#define NYA_KEY_RMETA                0x20000005u /**< Extended key Right Meta */
#define NYA_KEY_LHYPER               0x20000006u /**< Extended key Left Hyper */
#define NYA_KEY_RHYPER               0x20000007u /**< Extended key Right Hyper */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MODIFIER FLAGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u16 NYA_KeyModFlag;

#define NYA_KEYMOD_NONE   0x0000u                                 /**< no modifier is applicable. */
#define NYA_KEYMOD_LSHIFT 0x0001u                                 /**< the left Shift key is down. */
#define NYA_KEYMOD_RSHIFT 0x0002u                                 /**< the right Shift key is down. */
#define NYA_KEYMOD_LEVEL5 0x0004u                                 /**< the Level 5 Shift key is down. */
#define NYA_KEYMOD_LCTRL  0x0040u                                 /**< the left Ctrl (Control) key is down. */
#define NYA_KEYMOD_RCTRL  0x0080u                                 /**< the right Ctrl (Control) key is down. */
#define NYA_KEYMOD_LALT   0x0100u                                 /**< the left Alt key is down. */
#define NYA_KEYMOD_RALT   0x0200u                                 /**< the right Alt key is down. */
#define NYA_KEYMOD_LGUI   0x0400u                                 /**< the left GUI key (often the Windows key) is down. */
#define NYA_KEYMOD_RGUI   0x0800u                                 /**< the right GUI key (often the Windows key) is down. */
#define NYA_KEYMOD_NUM    0x1000u                                 /**< the Num Lock key (may be located on an extended keypad) is down. */
#define NYA_KEYMOD_CAPS   0x2000u                                 /**< the Caps Lock key is down. */
#define NYA_KEYMOD_MODE   0x4000u                                 /**< the !AltGr key is down. */
#define NYA_KEYMOD_SCROLL 0x8000u                                 /**< the Scroll Lock key is down. */
#define NYA_KEYMOD_CTRL   (NYA_KEYMOD_LCTRL | NYA_KEYMOD_RCTRL)   /**< Any Ctrl key is down. */
#define NYA_KEYMOD_SHIFT  (NYA_KEYMOD_LSHIFT | NYA_KEYMOD_RSHIFT) /**< Any Shift key is down. */
#define NYA_KEYMOD_ALT    (NYA_KEYMOD_LALT | NYA_KEYMOD_RALT)     /**< Any Alt key is down. */
#define NYA_KEYMOD_GUI    (NYA_KEYMOD_LGUI | NYA_KEYMOD_RGUI)     /**< Any GUI key is down. */
