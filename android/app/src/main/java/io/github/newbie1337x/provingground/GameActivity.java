package io.github.newbie1337x.provingground;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

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
        String folder = getIntent().getStringExtra(SetupActivity.EXTRA_GAME_FOLDER);
        if (folder == null) return new String[0];
        return new String[] {"--game_data_root=" + folder};
    }
}
