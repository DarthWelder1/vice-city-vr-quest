package com.miamivr.quest;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * A deliberately tiny ADB bridge for Vice City save slots.
 *
 * Android's scoped external storage lets the release app create its saves but
 * prevents the ADB shell from reading those app-owned files.  Making the APK
 * debuggable merely to transfer saves would weaken the release.  This provider
 * instead exposes exactly eight files and accepts only calls from the app
 * itself, root, or Android's shell UID used by `adb shell content`.
 */
public final class SaveProvider extends ContentProvider {
    private static final int ROOT_UID = 0;
    private static final int SHELL_UID = 2000;

    @Override
    public boolean onCreate() {
        return true;
    }

    private void enforceAdbOrAppCaller() {
        final int caller = Binder.getCallingUid();
        if (caller != ROOT_UID && caller != SHELL_UID && caller != Process.myUid()) {
            throw new SecurityException("Vice City saves are available only through ADB");
        }
    }

    private int parseSlot(Uri uri) throws FileNotFoundException {
        if (uri == null || uri.getPathSegments().size() != 2 ||
                !"slot".equals(uri.getPathSegments().get(0))) {
            throw new FileNotFoundException("Expected content://.../slot/1 through /slot/8");
        }
        final int slot;
        try {
            slot = Integer.parseInt(uri.getPathSegments().get(1));
        } catch (NumberFormatException error) {
            throw new FileNotFoundException("Invalid save slot");
        }
        if (slot < 1 || slot > 8) {
            throw new FileNotFoundException("Save slot must be between 1 and 8");
        }
        return slot;
    }

    private File saveFile(int slot) throws FileNotFoundException {
        if (getContext() == null) {
            throw new FileNotFoundException("Application context is unavailable");
        }
        final File externalRoot = getContext().getExternalFilesDir(null);
        if (externalRoot == null) {
            throw new FileNotFoundException("External files directory is unavailable");
        }
        // The current Quest frontend stores 1.b..8.b in gamedata.  Keep this
        // bridge aligned with that on-disk contract; the transfer tools hide
        // the platform-specific name from users.
        final File gameData = new File(externalRoot, "gamedata");
        if (!gameData.isDirectory() && !gameData.mkdirs()) {
            throw new FileNotFoundException("Cannot create the game-data directory");
        }
        return new File(gameData, slot + ".b");
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        enforceAdbOrAppCaller();
        final File file = saveFile(parseSlot(uri));
        final int flags;
        if ("r".equals(mode)) {
            flags = ParcelFileDescriptor.MODE_READ_ONLY;
        } else if ("w".equals(mode) || "wt".equals(mode)) {
            flags = ParcelFileDescriptor.MODE_WRITE_ONLY |
                    ParcelFileDescriptor.MODE_CREATE |
                    ParcelFileDescriptor.MODE_TRUNCATE;
        } else {
            throw new FileNotFoundException("Supported modes are r and w");
        }
        return ParcelFileDescriptor.open(file, flags);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        enforceAdbOrAppCaller();
        try {
            final int slot = parseSlot(uri);
            final File file = saveFile(slot);
            final String[] columns = projection == null ?
                    new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE } :
                    projection;
            final MatrixCursor cursor = new MatrixCursor(columns, 1);
            final MatrixCursor.RowBuilder row = cursor.newRow();
            for (String column : columns) {
                if (OpenableColumns.DISPLAY_NAME.equals(column)) {
                    row.add("GTAVCsf" + slot + ".b");
                } else if (OpenableColumns.SIZE.equals(column)) {
                    row.add(file.isFile() ? file.length() : 0L);
                } else {
                    row.add(null);
                }
            }
            return cursor;
        } catch (FileNotFoundException error) {
            throw new IllegalArgumentException(error.getMessage(), error);
        }
    }

    @Override
    public String getType(Uri uri) {
        enforceAdbOrAppCaller();
        return "application/octet-stream";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Use openFile in write mode");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Save deletion is not exposed");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
            String[] selectionArgs) {
        throw new UnsupportedOperationException("Use openFile in write mode");
    }
}
