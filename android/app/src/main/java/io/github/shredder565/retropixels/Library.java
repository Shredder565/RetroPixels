package io.github.shredder565.retropixels;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The games the player opened: each is copied into the app's own storage
 * (games/<file name>), where the engine reads it, and listed in the recent
 * games (the last ten). Saves are kept by the game's name, apart from the
 * copies, so a game dropping off the list keeps its saves.
 */
final class Library {
    static final long MAX_GAME = 1L << 30; // .zip / .7z sets; the engine checks the rest

    private Library() {
    }

    static File gamesDir(Context c) {
        File d = new File(c.getFilesDir(), "games");
        d.mkdirs();
        return d;
    }

    /** The file's name as the app that gave it says (with its extension, which tells the system). */
    static String displayName(Context c, Uri uri) {
        String name = null;
        if ("content".equals(uri.getScheme())) {
            Cursor cur = null;
            try {
                cur = c.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null);
                if (cur != null && cur.moveToFirst() && !cur.isNull(0)) {
                    name = cur.getString(0);
                }
            } catch (RuntimeException ignored) {
                // no name: use the last part of the address
            } finally {
                if (cur != null) {
                    cur.close();
                }
            }
        }
        if (name == null || name.isEmpty()) {
            name = uri.getLastPathSegment();
        }
        if (name == null || name.isEmpty()) {
            name = "game";
        }
        int slash = name.lastIndexOf('/');
        if (slash >= 0) {
            name = name.substring(slash + 1);
        }
        return safeFileName(name);
    }

    static String safeFileName(String s) {
        s = s.replaceAll("[\\\\/:*?\"<>|\\x00-\\x1f]", "_").trim();
        if (s.isEmpty() || s.equals(".") || s.equals("..")) {
            s = "game";
        }
        if (s.length() > 150) {
            int dot = s.lastIndexOf('.');
            String ext = dot > 0 && s.length() - dot <= 10 ? s.substring(dot) : "";
            s = s.substring(0, 150 - ext.length()) + ext;
        }
        return s;
    }

    /** Copies what the uri holds to a file (at most limit bytes). */
    static void copy(Context c, Uri uri, File to, long limit) throws IOException {
        ContentResolver cr = c.getContentResolver();
        InputStream in = cr.openInputStream(uri);
        if (in == null) {
            throw new IOException("couldn't open that file");
        }
        File tmp = new File(to.getPath() + ".part");
        OutputStream out = new FileOutputStream(tmp);
        try {
            byte[] b = new byte[1 << 16];
            long total = 0;
            int n;
            while ((n = in.read(b)) > 0) {
                total += n;
                if (total > limit) {
                    throw new IOException("that file is far too big to be a game");
                }
                out.write(b, 0, n);
            }
        } finally {
            try {
                in.close();
            } catch (IOException ignored) {
                // read already
            }
            out.close();
        }
        if (!tmp.renameTo(to)) {
            tmp.delete();
            throw new IOException("couldn't keep a copy of the file");
        }
    }

    /** Copies a game from another app into the library; returns its file. */
    static File importGame(Context c, Uri uri) throws IOException {
        String name = displayName(c, uri);
        File f = new File(gamesDir(c), name);
        copy(c, uri, f, MAX_GAME);
        return f;
    }

    /** The game's name for saves: its file name without the extension (gameNameOf in the native app). */
    static String gameName(String fileName) {
        int dot = fileName.lastIndexOf('.');
        return dot > 0 ? fileName.substring(0, dot) : fileName;
    }

    /** Puts a game first in the recent games, and lets go of the copies of games no longer listed. */
    static void addRecent(Context c, File game, String label) {
        Prefs p = new Prefs(c);
        List<Prefs.Recent> out = new ArrayList<Prefs.Recent>();
        out.add(new Prefs.Recent(game.getPath(), game.getName(), label));
        for (Prefs.Recent r : p.recent()) {
            if (!r.path.equals(game.getPath()) && out.size() < Prefs.MAX_RECENT) {
                out.add(r);
            }
        }
        p.setRecent(out);
        tidy(c, out);
    }

    static void removeRecent(Context c, String path) {
        Prefs p = new Prefs(c);
        List<Prefs.Recent> out = new ArrayList<Prefs.Recent>();
        for (Prefs.Recent r : p.recent()) {
            if (!r.path.equals(path)) {
                out.add(r);
            }
        }
        p.setRecent(out);
        tidy(c, out);
    }

    private static void tidy(Context c, List<Prefs.Recent> keep) {
        Set<String> paths = new HashSet<String>();
        for (Prefs.Recent r : keep) {
            paths.add(r.path);
        }
        File[] files = gamesDir(c).listFiles();
        if (files == null) {
            return;
        }
        for (File f : files) {
            if (!paths.contains(f.getPath())) {
                f.delete();
            }
        }
    }
}
