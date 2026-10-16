package io.github.shredder565.retropixels;

import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;

/**
 * Android's keys as the controls know them: controller buttons in the
 * browser's standard gamepad numbering (0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT,
 * 7 RT, 8 View/Select, 9 Menu/Start, 10 L3, 11 R3, 12-15 D-pad up, down,
 * left, right), and keyboard keys as the browser's lower-cased key names
 * ("x", "enter", "shift", " ", "arrowup"), as RetroPixels Native saves them.
 */
final class KeyNames {
    private KeyNames() {
    }

    static final String[] PAD_NAMES = {"A", "B", "X", "Y", "LB / L1", "RB / R1", "LT / L2", "RT / R2", "View / Select", "Menu / Start",
        "L3 (left stick)", "R3 (right stick)", "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →"};

    /** Whether the event comes from a game controller (not a keyboard or the phone's own keys). */
    static boolean fromPad(InputEvent e) {
        int s = e.getSource();
        if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            return true;
        }
        InputDevice d = e.getDevice();
        if (d == null || d.isVirtual()) {
            return false;
        }
        int ds = d.getSources();
        return (ds & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (ds & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** A controller key's standard gamepad button, or -1. */
    static int padButton(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return 0;
            case KeyEvent.KEYCODE_BUTTON_B: return 1;
            case KeyEvent.KEYCODE_BUTTON_X: return 2;
            case KeyEvent.KEYCODE_BUTTON_Y: return 3;
            case KeyEvent.KEYCODE_BUTTON_L1: return 4;
            case KeyEvent.KEYCODE_BUTTON_R1: return 5;
            case KeyEvent.KEYCODE_BUTTON_L2: return 6;
            case KeyEvent.KEYCODE_BUTTON_R2: return 7;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 8;
            case KeyEvent.KEYCODE_BUTTON_START: return 9;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 10;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 11;
            case KeyEvent.KEYCODE_DPAD_UP: return 12;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 13;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 14;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 15;
            default: return -1;
        }
    }

    /** A keyboard key's name, or null for keys the controls don't use. */
    static String keyName(KeyEvent e) {
        int k = e.getKeyCode();
        if (k >= KeyEvent.KEYCODE_A && k <= KeyEvent.KEYCODE_Z) {
            return String.valueOf((char) ('a' + k - KeyEvent.KEYCODE_A));
        }
        if (k >= KeyEvent.KEYCODE_0 && k <= KeyEvent.KEYCODE_9) {
            return String.valueOf((char) ('0' + k - KeyEvent.KEYCODE_0));
        }
        if (k >= KeyEvent.KEYCODE_NUMPAD_0 && k <= KeyEvent.KEYCODE_NUMPAD_9) {
            return String.valueOf((char) ('0' + k - KeyEvent.KEYCODE_NUMPAD_0));
        }
        if (k >= KeyEvent.KEYCODE_F1 && k <= KeyEvent.KEYCODE_F12) {
            return "f" + (k - KeyEvent.KEYCODE_F1 + 1);
        }
        switch (k) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return "enter";
            case KeyEvent.KEYCODE_SHIFT_LEFT:
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return "shift";
            case KeyEvent.KEYCODE_CTRL_LEFT:
            case KeyEvent.KEYCODE_CTRL_RIGHT: return "control";
            case KeyEvent.KEYCODE_ALT_LEFT:
            case KeyEvent.KEYCODE_ALT_RIGHT: return "alt";
            case KeyEvent.KEYCODE_SPACE: return " ";
            case KeyEvent.KEYCODE_TAB: return "tab";
            case KeyEvent.KEYCODE_DEL: return "backspace";
            case KeyEvent.KEYCODE_FORWARD_DEL: return "delete";
            case KeyEvent.KEYCODE_INSERT: return "insert";
            case KeyEvent.KEYCODE_MOVE_HOME: return "home";
            case KeyEvent.KEYCODE_MOVE_END: return "end";
            case KeyEvent.KEYCODE_PAGE_UP: return "pageup";
            case KeyEvent.KEYCODE_PAGE_DOWN: return "pagedown";
            case KeyEvent.KEYCODE_DPAD_UP: return "arrowup";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "arrowdown";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "arrowleft";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "arrowright";
            case KeyEvent.KEYCODE_COMMA: return ",";
            case KeyEvent.KEYCODE_PERIOD: return ".";
            case KeyEvent.KEYCODE_SLASH: return "/";
            case KeyEvent.KEYCODE_SEMICOLON: return ";";
            case KeyEvent.KEYCODE_APOSTROPHE: return "'";
            case KeyEvent.KEYCODE_LEFT_BRACKET: return "[";
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return "]";
            case KeyEvent.KEYCODE_BACKSLASH: return "\\";
            case KeyEvent.KEYCODE_MINUS: return "-";
            case KeyEvent.KEYCODE_EQUALS: return "=";
            case KeyEvent.KEYCODE_GRAVE: return "`";
            default: return null;
        }
    }

    /** How a key name is shown ("Space", "Enter", "Up"). */
    static String keyLabel(String k) {
        if (k.equals(" ")) {
            return "Space";
        }
        if (k.startsWith("arrow")) {
            k = k.substring(5);
        }
        return k.isEmpty() ? k : Character.toUpperCase(k.charAt(0)) + k.substring(1);
    }
}
