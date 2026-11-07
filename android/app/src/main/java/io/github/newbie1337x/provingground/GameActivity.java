package io.github.newbie1337x.provingground;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** Runs the game: SDL's activity loads librexruntime.so (which holds SDL) and libmain.so. */
public class GameActivity extends SDLActivity {
    private static final String TAG = "ProvingGround";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Saves, settings, logs and the shader cache go to Android/data/<package>/files, which
        // the runtime reaches as XDG_DATA_HOME (it adds ProvingGroundRecomp/ itself).
        File files = getExternalFilesDir(null);
        if (files == null) files = getFilesDir();
        try {
            Os.setenv("XDG_DATA_HOME", files.getAbsolutePath(), true);
            Os.setenv("HOME", files.getAbsolutePath(), true);
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // The last one is the library whose SDL_main runs.
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        String folder = getIntent().getStringExtra(SetupActivity.EXTRA_GAME_FOLDER);
        if (folder != null) args.add("--game_data_root=" + folder);
        // Developer runs: one more argument per line of ProvingGroundRecomp/args.txt (adb push),
        // e.g. an input replay with its own --user_data_root so the player's saves stay untouched.
        File files = getExternalFilesDir(null);
        File extra = files == null ? null : new File(files, "ProvingGroundRecomp/args.txt");
        if (extra != null && extra.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(extra))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) args.add(line);
                }
            } catch (IOException e) {
                Log.e(TAG, "Cannot read " + extra, e);
            }
            Log.i(TAG, "Arguments: " + args);
        }
        return args.toArray(new String[0]);
    }
}
