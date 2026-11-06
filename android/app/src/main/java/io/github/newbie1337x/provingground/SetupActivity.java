package io.github.newbie1337x.provingground;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * Picks the game folder (the extracted disc: default.xex with DATA and Content) and starts the
 * game with it. The runtime reads files by path, so the folder chosen in the system picker is
 * turned back into a path on a local volume, which needs "All files access". A copy in the
 * app's own folder (Android/data/&lt;package&gt;/files/game) is found without any permission.
 */
public class SetupActivity extends Activity {
    static final String EXTRA_GAME_FOLDER = "game_folder";
    private static final String PREFS = "setup";
    private static final String KEY_FOLDER = "game_folder";
    private static final int REQUEST_PICK_FOLDER = 1;

    private String message;
    private boolean pickAfterPermission;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        File app = appGameFolder();
        if (app != null) app.mkdirs();  // So the folder shows up for copying over USB.
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pickAfterPermission && hasAllFilesAccess()) {
            pickAfterPermission = false;
            openPicker();
            return;
        }
        showScreen(currentGameFolder());
    }

    // --- Folder detection ---

    private File appGameFolder() {
        File files = getExternalFilesDir(null);
        return files != null ? new File(files, "game") : null;
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    /** The saved folder if it still holds the game, else the app's own folder if it does. */
    private File currentGameFolder() {
        String saved = prefs().getString(KEY_FOLDER, null);
        if (saved != null && hasAllFilesAccess()) {
            File folder = resolveGameFolder(new File(saved));
            if (folder != null) return folder;
        }
        return resolveGameFolder(appGameFolder());
    }

    /** The child of folder named name in any case (the game looks its files up that way). */
    private static File findChild(File folder, String name) {
        File[] children = folder.listFiles();
        if (children == null) return null;
        for (File child : children) {
            if (child.getName().equalsIgnoreCase(name)) return child;
        }
        return null;
    }

    /** Same test as the PC launcher: default.xex plus DATA/SCRIPTS/ENGINE/engine_params.qb.xen. */
    private static boolean isGameFolder(File folder) {
        File xex = findChild(folder, "default.xex");
        if (xex == null || !xex.isFile()) return false;
        File path = folder;
        for (String part : new String[] {"DATA", "SCRIPTS", "ENGINE", "engine_params.qb.xen"}) {
            path = findChild(path, part);
            if (path == null) return false;
        }
        return true;
    }

    /** The folder itself, or the one game folder directly inside it (as on PC). */
    private static File resolveGameFolder(File folder) {
        if (folder == null || !folder.isDirectory()) return null;
        if (isGameFolder(folder)) return folder;
        File[] children = folder.listFiles();
        if (children != null) {
            for (File child : children) {
                if (child.isDirectory() && isGameFolder(child)) return child;
            }
        }
        return null;
    }

    /**
     * content://com.android.externalstorage.documents/tree/primary%3AGames%2FTHPG becomes
     * /storage/emulated/0/Games/THPG; a "&lt;volume-uuid&gt;:path" tree becomes /storage/&lt;uuid&gt;/path.
     * Returns null for folders that are not on a local volume (cloud providers, apps).
     */
    private static File treeUriToPath(Uri uri) {
        if (uri == null || !"com.android.externalstorage.documents".equals(uri.getAuthority())) {
            return null;
        }
        String id = DocumentsContract.getTreeDocumentId(uri);
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String relative = id.substring(colon + 1);
        File root = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory()
                                                       : new File("/storage/" + volume);
        return relative.isEmpty() ? root : new File(root, relative);
    }

    // --- Actions ---

    private void startGame(File game) {
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra(EXTRA_GAME_FOLDER, game.getAbsolutePath());
        startActivity(intent);
    }

    private void chooseFolder() {
        if (!hasAllFilesAccess()) {
            // The picker only gives a content URI; reading the game by path needs this permission.
            pickAfterPermission = true;
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            return;
        }
        openPicker();
    }

    private void openPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(intent, REQUEST_PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_FOLDER || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        File folder = treeUriToPath(uri);
        if (folder == null) {
            message = "That folder is not on the phone's storage or SD card. Choose a folder on "
                    + "the device (cloud folders cannot be read).";
            return;
        }
        File game = resolveGameFolder(folder);
        if (game == null) {
            message = "No game in " + folder.getAbsolutePath() + ".\n\nChoose the folder that has "
                    + "default.xex and the DATA folder (with DATA/SCRIPTS/ENGINE/engine_params.qb.xen).";
            return;
        }
        prefs().edit().putString(KEY_FOLDER, game.getAbsolutePath()).apply();
        message = null;
        startGame(game);
    }

    // --- Screen ---

    private void showScreen(File game) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        layout.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView text = new TextView(this);
        text.setTextSize(16);
        StringBuilder sb = new StringBuilder();
        if (message != null) sb.append(message).append("\n\n");
        if (game != null) {
            sb.append("Game folder:\n").append(game.getAbsolutePath()).append('\n');
        } else {
            File app = appGameFolder();
            sb.append("Choose the folder with your extracted Xbox 360 game (default.xex with the "
                    + "DATA and Content folders). Proving Ground needs \"All files access\" to "
                    + "read it.\n\nOr copy the game, without any permission, to:\n")
              .append(app != null ? app.getAbsolutePath() : "(app folder unavailable)")
              .append('\n');
        }
        text.setText(sb.toString());
        layout.addView(text);

        if (game != null) {
            Button play = new Button(this);
            play.setText("Play");
            play.setOnClickListener(v -> startGame(game));
            layout.addView(play);
        }

        Button choose = new Button(this);
        choose.setText(game != null ? "Change game folder" : "Choose game folder");
        choose.setOnClickListener(v -> {
            message = null;
            chooseFolder();
        });
        layout.addView(choose);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        setContentView(scroll);
    }
}
