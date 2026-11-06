package io.github.newbie1337x.provingground;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * Finds the game folder (the extracted disc: default.xex next to DATA and Content) and starts
 * the game with it. Looked for, in order:
 *   1. Android/data/&lt;package&gt;/files/game on shared storage (no permission needed; copy it there
 *      from a PC over USB, or with adb push).
 *   2. ProvingGroundRecomp/game at the root of shared storage, with "All files access".
 * Without either, it shows where to put the files.
 */
public class SetupActivity extends Activity {
    static final String EXTRA_GAME_FOLDER = "game_folder";
    static final String SHARED_FOLDER = "ProvingGroundRecomp/game";

    @Override
    protected void onResume() {
        super.onResume();
        File game = findGameFolder();
        if (game != null) {
            Intent intent = new Intent(this, GameActivity.class);
            intent.putExtra(EXTRA_GAME_FOLDER, game.getAbsolutePath());
            startActivity(intent);
            finish();
            return;
        }
        showInstructions();
    }

    private File appGameFolder() {
        File files = getExternalFilesDir(null);
        return files != null ? new File(files, "game") : null;
    }

    private File sharedGameFolder() {
        return new File(Environment.getExternalStorageDirectory(), SHARED_FOLDER);
    }

    private static boolean hasGame(File folder) {
        return folder != null && new File(folder, "default.xex").isFile();
    }

    private File findGameFolder() {
        File app = appGameFolder();
        if (app != null) {
            app.mkdirs();  // So the folder shows up for copying over USB.
            if (hasGame(app)) return app;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && Environment.isExternalStorageManager()
                && hasGame(sharedGameFolder())) {
            return sharedGameFolder();
        }
        return null;
    }

    private void showInstructions() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        layout.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView text = new TextView(this);
        text.setTextSize(16);
        File app = appGameFolder();
        text.setText("Game files not found.\n\n"
                + "Copy your extracted Xbox 360 game (default.xex with the DATA and Content "
                + "folders) to one of:\n\n"
                + "  " + (app != null ? app.getAbsolutePath() : "(app folder unavailable)") + "\n"
                + "  (from a PC over USB, or: adb push <game> " + (app != null ? app.getAbsolutePath() : "") + ")\n\n"
                + "  " + sharedGameFolder().getAbsolutePath() + "\n"
                + "  (needs \"All files access\" below)\n");
        layout.addView(text);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            Button grant = new Button(this);
            grant.setText("Allow all files access");
            grant.setOnClickListener(v -> {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            });
            layout.addView(grant);
        }

        Button retry = new Button(this);
        retry.setText("Check again");
        retry.setOnClickListener(v -> onResume());
        layout.addView(retry);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        setContentView(scroll);
    }
}
