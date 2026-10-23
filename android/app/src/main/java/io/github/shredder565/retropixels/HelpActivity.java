package io.github.shredder565.retropixels;

import android.app.Activity;
import android.os.Bundle;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Help and About. */
public final class HelpActivity extends Activity {
    static String text(String version) {
        return "RetroPixels " + version + " for Android\n\n"
                + "PLAYING\n"
                + "Open game picks a game file: .nes .sfc .smc .gb .gbc .gba .z64 .n64 .v64 .md .gen .smd .sms .a26 .a78 .bin, "
                + "also inside .zip and .7z files. Files other apps open with RetroPixels, or share with it, play too. "
                + "The game is copied into the app, so it plays again from Recent games (the last ten).\n"
                + "Arcade games (Pac-Man, Galaxian, Space Invaders, Donkey Kong, TMNT, TMNT: Turtles in Time, The Simpsons): open the game's "
                + ".zip ROM set and keep it zipped (a .7z works too); the game is found from the names of the files inside. "
                + "If chip files are missing or the wrong size, it says which; clone sets need a full (non-merged) set.\n"
                + "Settings > Open .bin files as picks the system for .bin files instead of guessing.\n\n"
                + "TOUCH CONTROLS\n"
                + "The D-pad (the Nintendo 64's stick, with its D-pad above it) is on the left, the buttons on the right, named as on "
                + "that system's pad; shoulder buttons are at the top corners and Select / Start (Coin / Start, Pause / Reset...) "
                + "at the bottom. The ≡ button opens the menu; hold » to fast-forward. Atari 2600 paddle games: slide a finger "
                + "across the picture to turn the paddle. The controls hide while a controller is used and come back when the "
                + "screen is touched (Settings changes this, their size and how see-through they are).\n\n"
                + "CONTROLLERS AND KEYBOARDS\n"
                + "Bluetooth and USB controllers work as soon as they are used, up to four (one per player). The buttons are where "
                + "RetroPixels puts them on an Xbox controller: on Nintendo systems the bottom button is B and the right one A, as on "
                + "the original pads; Genesis X / A / B = A / B / C; Nintendo 64: A = A, X = B, LT = Z, left stick = the stick, "
                + "right stick = the C buttons. Arcade: View / Select = Coin, Menu / Start = Start. "
                + "Keyboards use the same keys as RetroPixels Native (arrows move; X / Z for A / B, Enter = Start, Shift = Select, "
                + "5 = Coin, 1 = Start in arcade games). Change them in Settings > Controls (one set per system). "
                + "Shortcuts: Back, Esc or the controller's Home / Mode button opens the menu; click the right stick (or F2) to save a "
                + "state, the left stick (or F4) to load it; F3 picks the slot; hold Tab to fast-forward.\n\n"
                + "THE MENU\n"
                + "Pause, fast-forward, save state and load state (four slots per game, and Undo last load), reset, volume and mute, "
                + "machine settings (an arcade board's DIP switches, kept per game; reset after a change), the player you play as "
                + "(TMNT, Turtles in Time, The Simpsons), the battery save (export as .sav, or import a .sav / .srm from another "
                + "emulator: the game restarts and the old save is kept as .bak), importing a save state file, the controls, "
                + "sharp or smooth pixels, and quitting to the start screen.\n\n"
                + "SAVES\n"
                + "Battery saves are written as soon as they change and when the app goes to the background; the game pauses there. "
                + "Saves are kept inside the app, per system and game, the way RetroPixels Native keeps them "
                + "(saves/<system>/<game>.sav, .state and .slotN.state). Uninstalling the app removes them; export a battery save "
                + "first to keep it.\n\n"
                + "RECORDING\n"
                + "Settings > Record games automatically (on unless you turn it off) records every game from start to end, "
                + "as RetroPixels.exe and RetroPixels Native do: only the game's picture (sharp, scaled up to about 720 lines, "
                + "without the touch controls) and its sound, at the game's own speed, so pausing leaves no gap. Nothing needs "
                + "allowing: the app makes the video itself, with the phone's own video encoder. The game going to the "
                + "background or closing ends the video (coming back starts a new one); recordings under five seconds "
                + "aren't kept. Videos are named <game> <date time>.mp4 and go to Movies/RetroPixels, where the gallery "
                + "finds them. On Android 7 to 9 the app asks once for storage access for that folder; without it they go "
                + "to Android/data/io.github.shredder565.retropixels/files/Movies/RetroPixels (removed with the app). "
                + "If the phone's encoder can't record, the game plays on and the app says so once.\n\n"
                + "SUPER NINTENDO CHIP PROGRAMS\n"
                + "DSP-3, DSP-4, ST010, ST011 and ST018 games need your own dump of the chip's program (dsp3.rom, dsp4.rom, "
                + "st010.rom, st011.rom, st018.rom). The first time, RetroPixels asks for the file, checks it and keeps it. "
                + "None are included.\n\n"
                + "ARCHIPELAGO\n"
                + "Archipelago patch files (.apsm, .apred, .apemerald and others) open: the first time, RetroPixels asks for the "
                + "original ROM the game's setup guide names, checks it and keeps it. Connecting to an Archipelago server isn't "
                + "in the Android app: the SNI and BizHawk clients run on a computer with RetroPixels Native.\n\n"
                + "NOT IN THE ANDROID APP\n"
                + "GameCube and Wii games (RetroPixels plays them through Dolphin on Windows; Dolphin has its own Android app), "
                + "MAME and Archipelago connections.\n\n"
                + "ABOUT\n"
                + "RetroPixels' own code was written for this project: its NES, Super Nintendo, Game Boy / Color, Game Boy Advance, "
                + "Nintendo 64, Genesis, Master System, Atari 2600, Atari 7800 and arcade emulators, with replacements for the "
                + "consoles' start-up programs, so no Nintendo or Sega code is used. No games, BIOS files or chip programs are "
                + "included. Nintendo, Sega and Atari are trademarks of their owners, named only to say what the app works with; "
                + "RetroPixels isn't connected with or endorsed by any of them.";
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout col = Ui.column(this);
        col.addView(Ui.heading(this, "Help"));
        ScrollView sv = new ScrollView(this);
        TextView t = Ui.text(this, text(Ui.version(this)), 15, Ui.FG);
        t.setTextIsSelectable(true);
        sv.addView(t);
        col.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(col);
    }
}
