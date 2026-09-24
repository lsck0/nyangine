/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // TEST: scancode enum values are defined
  nya_assert(NYA_SCANCODE_A == 4);
  nya_assert(NYA_SCANCODE_Z == 29);
  nya_assert(NYA_SCANCODE_0 == 39);
  nya_assert(NYA_SCANCODE_9 == 38);
  nya_assert(NYA_SCANCODE_RETURN == 40);
  nya_assert(NYA_SCANCODE_ESCAPE == 41);
  nya_assert(NYA_SCANCODE_SPACE == 44);
  nya_assert(NYA_SCANCODE_F1 == 58);
  nya_assert(NYA_SCANCODE_F12 == 69);
  nya_assert(NYA_SCANCODE_UP == 82);
  nya_assert(NYA_SCANCODE_DOWN == 81);
  nya_assert(NYA_SCANCODE_LEFT == 80);
  nya_assert(NYA_SCANCODE_RIGHT == 79);

  // TEST: keycode macros are defined
  nya_assert(NYA_KEY_A == 0x00000061u);
  nya_assert(NYA_KEY_Z == 0x0000007au);
  nya_assert(NYA_KEY_0 == 0x00000030u);
  nya_assert(NYA_KEY_9 == 0x00000039u);
  nya_assert(NYA_KEY_RETURN == 0x0000000du);
  nya_assert(NYA_KEY_ESCAPE == 0x0000001bu);
  nya_assert(NYA_KEY_SPACE == 0x00000020u);
  nya_assert(NYA_KEY_F1 == 0x4000003au);
  nya_assert(NYA_KEY_F12 == 0x40000045u);
  nya_assert(NYA_KEY_UP == 0x40000052u);
  nya_assert(NYA_KEY_DOWN == 0x40000051u);
  nya_assert(NYA_KEY_LEFT == 0x40000050u);
  nya_assert(NYA_KEY_RIGHT == 0x4000004fu);

  // TEST: modifier flags are defined
  nya_assert(NYA_KEYMOD_NONE == 0x0000u);
  nya_assert(NYA_KEYMOD_LSHIFT == 0x0001u);
  nya_assert(NYA_KEYMOD_RSHIFT == 0x0002u);
  nya_assert(NYA_KEYMOD_LCTRL == 0x0040u);
  nya_assert(NYA_KEYMOD_RCTRL == 0x0080u);
  nya_assert(NYA_KEYMOD_LALT == 0x0100u);
  nya_assert(NYA_KEYMOD_RALT == 0x0200u);
  nya_assert(NYA_KEYMOD_LGUI == 0x0400u);
  nya_assert(NYA_KEYMOD_RGUI == 0x0800u);

  // TEST: combined modifier flags
  nya_assert(NYA_KEYMOD_CTRL == (NYA_KEYMOD_LCTRL | NYA_KEYMOD_RCTRL));
  nya_assert(NYA_KEYMOD_SHIFT == (NYA_KEYMOD_LSHIFT | NYA_KEYMOD_RSHIFT));
  nya_assert(NYA_KEYMOD_ALT == (NYA_KEYMOD_LALT | NYA_KEYMOD_RALT));
  nya_assert(NYA_KEYMOD_GUI == (NYA_KEYMOD_LGUI | NYA_KEYMOD_RGUI));

  // TEST: scancode mask macros
  nya_assert(NYA_KEY_EXTENDED_MASK == (1u << 29));
  nya_assert(NYA_KEY_SCANCODE_MASK == (1u << 30));
  nya_assert(NYA_SCANCODE_TO_KEYCODE(4) == (4 | NYA_KEY_SCANCODE_MASK));

  // TEST: scancode constants for different key groups Letter keys A-Z
  nya_assert(NYA_SCANCODE_A == 4);
  nya_assert(NYA_SCANCODE_B == 5);
  nya_assert(NYA_SCANCODE_Q == 20);
  nya_assert(NYA_SCANCODE_Z == 29);

  // Number keys 0-9
  nya_assert(NYA_SCANCODE_1 == 30);
  nya_assert(NYA_SCANCODE_5 == 34);
  nya_assert(NYA_SCANCODE_0 == 39);

  // Function keys
  nya_assert(NYA_SCANCODE_F1 == 58);
  nya_assert(NYA_SCANCODE_F10 == 67);
  nya_assert(NYA_SCANCODE_F24 == 115);

  // Navigation keys
  nya_assert(NYA_SCANCODE_HOME == 74);
  nya_assert(NYA_SCANCODE_END == 77);
  nya_assert(NYA_SCANCODE_PAGEUP == 75);
  nya_assert(NYA_SCANCODE_PAGEDOWN == 78);
  nya_assert(NYA_SCANCODE_INSERT == 73);
  nya_assert(NYA_SCANCODE_DELETE == 76);

  // TEST: special key codes
  nya_assert(NYA_KEY_UNKNOWN == 0x00000000u);
  nya_assert(NYA_KEY_BACKSPACE == 0x00000008u);
  nya_assert(NYA_KEY_TAB == 0x00000009u);
  nya_assert(NYA_KEY_DELETE == 0x0000007fu);
  nya_assert(NYA_KEY_PLUSMINUS == 0x000000b1u);

  // TEST: keypad scancodes
  nya_assert(NYA_SCANCODE_KP_0 == 98);
  nya_assert(NYA_SCANCODE_KP_9 == 97);
  nya_assert(NYA_SCANCODE_KP_ENTER == 88);
  nya_assert(NYA_SCANCODE_KP_PLUS == 87);
  nya_assert(NYA_SCANCODE_KP_DIVIDE == 84);
  nya_assert(NYA_SCANCODE_KP_MULTIPLY == 85);

  // TEST: modifier key scancodes
  nya_assert(NYA_SCANCODE_LCTRL == 224);
  nya_assert(NYA_SCANCODE_RCTRL == 228);
  nya_assert(NYA_SCANCODE_LSHIFT == 225);
  nya_assert(NYA_SCANCODE_RSHIFT == 229);
  nya_assert(NYA_SCANCODE_LALT == 226);
  nya_assert(NYA_SCANCODE_RALT == 230);
  nya_assert(NYA_SCANCODE_LGUI == 227);
  nya_assert(NYA_SCANCODE_RGUI == 231);

  // TEST: extended keys
  nya_assert(NYA_KEY_LEFT_TAB == 0x20000001u);
  nya_assert(NYA_KEY_LEVEL5_SHIFT == 0x20000002u);
  nya_assert(NYA_KEY_LMETA == 0x20000004u);
  nya_assert(NYA_KEY_RMETA == 0x20000005u);

  // TEST: extended modifier state flags
  nya_assert(NYA_KEYMOD_NUM == 0x1000u);
  nya_assert(NYA_KEYMOD_CAPS == 0x2000u);
  nya_assert(NYA_KEYMOD_MODE == 0x4000u);
  nya_assert(NYA_KEYMOD_SCROLL == 0x8000u);

  // TEST: media keys
  nya_assert(NYA_SCANCODE_MEDIA_PLAY == 262);
  nya_assert(NYA_SCANCODE_MEDIA_PAUSE == 263);
  nya_assert(NYA_SCANCODE_MEDIA_NEXT_TRACK == 267);
  nya_assert(NYA_SCANCODE_MEDIA_PREVIOUS_TRACK == 268);
  nya_assert(NYA_KEY_MEDIA_PLAY == 0x40000106u);

  // TEST: count value
  nya_assert(NYA_SCANCODE_COUNT == 512);

  // TEST: comparing scan codes Scan codes should be sequential in some areas
  nya_assert(NYA_SCANCODE_B == NYA_SCANCODE_A + 1);
  nya_assert(NYA_SCANCODE_C == NYA_SCANCODE_B + 1);

  // TEST: keycode to string function exists
  NYA_ConstCString name = nya_keycode_to_cstring(NYA_KEY_A);
  // Function exists and returns non-null
  nya_assert(name != nullptr);

  return 0;
}
