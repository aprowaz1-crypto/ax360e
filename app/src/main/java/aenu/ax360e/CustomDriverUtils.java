package aenu.ax360e;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

public class CustomDriverUtils {

    private static final String TAG = "CustomDriverUtils";
    private static final String DRIVER_DIR_NAME = "custom_drivers";
    // Old stub library logic has been completely removed (libadrenotools migration)
    private static volatile String lastDriverError = "";
    // Decompression bomb limits to prevent OOM from malicious zip files
    private static final long MAX_TOTAL_UNCOMPRESSED_SIZE = 100 * 1024 * 1024; // 100MB total
    private static final long MAX_SINGLE_FILE_SIZE = 50 * 1024 * 1024; // 50MB per file
    // Saved LD_LIBRARY_PATH value before we prepend custom driver directories,
    // so clearDriverEnv() can restore it rather than unconditionally unsetting it.
    private static volatile String savedLdLibraryPath = null;

    public static File getDriverDirectory(Context context) {
        return context.getDir(DRIVER_DIR_NAME, Context.MODE_PRIVATE);
    }

    public static boolean installDriver(Context context, Uri zipUri) {
        setLastDriverError("");
        File dir = getDriverDirectory(context);
        File stagingDir = new File(dir.getParentFile(), "staging");

        // Clean up any previous staging directory
        if (stagingDir.exists()) {
            deleteRecursive(stagingDir);
        }
        stagingDir.mkdirs();

        String driverSoName = null;
        try {
            InputStream is = context.getContentResolver().openInputStream(zipUri);
            if (is == null) {
                Log.e(TAG, "Failed to open input stream from URI");
                setLastDriverError("Could not open the selected driver package.");
                deleteRecursive(stagingDir);
                return false;
            }

            try (InputStream isGuard = is; ZipInputStream zis = new ZipInputStream(isGuard)) {
                ZipEntry entry;
                String canonicalStagingPath = stagingDir.getCanonicalPath();
                long totalBytesExtracted = 0;
                while ((entry = zis.getNextEntry()) != null) {
                    if (!entry.isDirectory()) {
                        // Check for decompression bomb - individual file size
                        if (entry.getSize() > MAX_SINGLE_FILE_SIZE) {
                            Log.e(TAG, "Zip entry too large: " + entry.getName()
                                    + " (" + entry.getSize() + " bytes, max " + MAX_SINGLE_FILE_SIZE + ")");
                            setLastDriverError("Driver archive contains an oversized file: " + entry.getName());
                            deleteRecursive(stagingDir);
                            return false;
                        }

                        File file = new File(stagingDir, entry.getName());
                        String canonicalFilePath = file.getCanonicalPath();
                        if (!canonicalFilePath.startsWith(canonicalStagingPath + File.separator)) {
                            throw new SecurityException("Entry is outside of the target dir: " + entry.getName());
                        }

                        // Create parent directories if needed
                        File parentDir = file.getParentFile();
                        if (parentDir != null) parentDir.mkdirs();

                        try (FileOutputStream fos = new FileOutputStream(file)) {
                            byte[] buffer = new byte[4096];
                            int length;
                            long fileBytesWritten = 0;
                            while ((length = zis.read(buffer)) > 0) {
                                fileBytesWritten += length;
                                totalBytesExtracted += length;
                                // Check limits during extraction (handles unknown sizes)
                                if (fileBytesWritten > MAX_SINGLE_FILE_SIZE) {
                                    Log.e(TAG, "File exceeded max size during extraction: " + entry.getName());
                                    setLastDriverError("Driver file exceeded extraction limit: " + entry.getName());
                                    deleteRecursive(stagingDir);
                                    return false;
                                }
                                if (totalBytesExtracted > MAX_TOTAL_UNCOMPRESSED_SIZE) {
                                    Log.e(TAG, "Total extraction exceeded max size: " + totalBytesExtracted + " bytes");
                                    setLastDriverError("Driver archive is too large when extracted.");
                                    deleteRecursive(stagingDir);
                                    return false;
                                }
                                fos.write(buffer, 0, length);
                            }
                        }
                        if (entry.getName().endsWith(".so") && entry.getName().contains("vulkan")) {
                            driverSoName = entry.getName();
                        }
                        if (entry.getName().equals("meta.json")) {
                            // meta.json is handled by the extraction loop above
                        }
                    }
                    zis.closeEntry();
                }

                if (driverSoName != null) {
                    // Validation succeeded, atomically swap staging into place
                    if (dir.exists()) {
                        deleteRecursive(dir);
                    }
                    if (!stagingDir.renameTo(dir)) {
                        Log.e(TAG, "Failed to move staging directory to driver directory");
                        setLastDriverError("Could not move the driver into app storage.");
                        deleteRecursive(stagingDir);
                        return false;
                    }

                    // Ensure ALL extracted .so files are executable (bundled deps need this too)
                    if (!chmodSoFilesRecursive(dir)) {
                        Log.e(TAG, "Failed to make driver .so files executable");
                        setLastDriverError("Driver libraries could not be marked executable.");
                        deleteRecursive(dir);
                        return false;
                    }

                    // Generate ICD manifest AFTER rename so paths point to final location
                    generateIcdManifest(dir, driverSoName);
                    return true;
                } else {
                    Log.e(TAG, "No vulkan .so file found in zip.");
                    setLastDriverError("No Vulkan driver library was found in the package.");
                    deleteRecursive(stagingDir);
                    return false;
                }
            }
        } catch (IOException | JSONException | SecurityException | NullPointerException e) {
            Log.e(TAG, "Failed to install driver", e);
            setLastDriverError(buildFailureReason(e));
            deleteRecursive(stagingDir);
            return false;
        }
    }

    public static String getLastDriverError() {
        return lastDriverError;
    }

    /**
     * Returns the directory where the custom driver is installed.
     * This is the preferred path for libadrenotools-based loading.
     */
    public static String getCustomDriverDirectory(Context context) {
        File dir = getDriverDirectory(context);
        if (isDriverInstalled(context)) {
            return dir.getAbsolutePath();
        }
        return null;
    }

    private static void generateIcdManifest(File dir, String soName) throws IOException, JSONException {
        File soFile = new File(dir, soName);
        File jsonFile = new File(dir, "vk_icd.json");

        JSONObject icdObj = new JSONObject();
        icdObj.put("library_path", soFile.getAbsolutePath());
        icdObj.put("api_version", "1.3.0"); // Turnip usually supports 1.3.x

        JSONObject rootObj = new JSONObject();
        rootObj.put("file_format_version", "1.0.0");
        rootObj.put("ICD", icdObj);

        try (FileWriter writer = new FileWriter(jsonFile)) {
            writer.write(rootObj.toString(4));
        }
    }

    public static void setupDriverEnv(Context context) {
        File dir = getDriverDirectory(context);
        File icdFile = new File(dir, "vk_icd.json");
        if (icdFile.exists()) {
            try {
                // Confirm a valid driver .so exists before mutating the process environment.
                // On Android the system Vulkan loader ignores VK_ICD_FILENAMES,
                // so we export the direct .so path for native code to dlopen.
                String soPath = findDriverSoPath(dir);
                if (soPath == null) {
                    Log.w(TAG, "vk_icd.json exists but no valid vulkan .so found; skipping env setup");
                    return;
                }

                // Old manual stub library generation has been completely removed.
                // libadrenotools handles dependency resolution internally.

                // === WARNING: Mutating LD_LIBRARY_PATH at runtime is dangerous on Android ===
                // The dynamic linker typically reads LD_LIBRARY_PATH once early in process startup.
                // Changing it later can lead to inconsistent symbol resolution.
                //
                // AGGRESSIVE CLEANUP: With libadrenotools we strongly prefer avoiding this mutation.
                String currentPath = System.getenv("LD_LIBRARY_PATH");
                savedLdLibraryPath = currentPath;

                StringBuilder newPath = new StringBuilder();
                newPath.append(dir.getAbsolutePath());
                if (currentPath != null && !currentPath.isEmpty()) {
                    newPath.append(":").append(currentPath);
                }

                // With libadrenotools we strongly prefer NOT mutating LD_LIBRARY_PATH.
                boolean shouldMutateLdPath = false;

                if (shouldMutateLdPath) {
                    try {
                        Os.setenv("LD_LIBRARY_PATH", newPath.toString(), true);
                        Log.w(TAG, "LD_LIBRARY_PATH mutated (strongly discouraged).");
                    } catch (ErrnoException e) {
                        Log.e(TAG, "Failed to set LD_LIBRARY_PATH", e);
                    }
                } else {
                    Log.i(TAG, "Skipping LD_LIBRARY_PATH mutation (recommended with libadrenotools)");
                }

                // Set ICD env vars (used by desktop Vulkan loaders, informational on Android)
                Os.setenv("VK_ICD_FILENAMES", icdFile.getAbsolutePath(), true);
                Os.setenv("VK_DRIVER_FILES", icdFile.getAbsolutePath(), true);

                Os.setenv("CUSTOM_DRIVER_PATH", soPath, true);

                // Also expose the directory containing the driver.
                // The new adreno_driver loader (libadrenotools path) prefers working with directories.
                Os.setenv("CUSTOM_DRIVER_DIR", dir.getAbsolutePath(), true);

                Log.i(TAG, "Custom GPU driver environment variables set. Driver: " + soPath);
            } catch (ErrnoException e) {
                Log.e(TAG, "Failed to set custom driver env variables", e);
            }
        }
    }

    public static boolean isDriverInstalled(Context context) {
        File dir = getDriverDirectory(context);
        File icdFile = new File(dir, "vk_icd.json");
        return icdFile.exists() && findDriverSoPath(dir) != null;
    }

    public static void clearDriverEnv() {
        try {
            Os.unsetenv("CUSTOM_DRIVER_PATH");
            Os.unsetenv("CUSTOM_DRIVER_DIR");
            Os.unsetenv("VK_ICD_FILENAMES");
            Os.unsetenv("VK_DRIVER_FILES");

            // Best-effort restore of LD_LIBRARY_PATH (we avoid touching it with the libadrenotools path).
            if (savedLdLibraryPath != null) {
                Os.setenv("LD_LIBRARY_PATH", savedLdLibraryPath, true);
            } else {
                Os.unsetenv("LD_LIBRARY_PATH");
            }
            savedLdLibraryPath = null;
        } catch (ErrnoException e) {
            Log.w(TAG, "Failed to clear custom driver env vars", e);
        }
    }

    /**
     * Find the Vulkan .so path inside the installed driver directory.
     * Returns null if no valid driver .so is found.
     */
    private static String findDriverSoPath(File dir) {
        // First try parsing vk_icd.json for library_path
        File icdFile = new File(dir, "vk_icd.json");
        if (icdFile.exists()) {
            try (FileInputStream fis = new FileInputStream(icdFile)) {
                byte[] data = new byte[(int) icdFile.length()];
                fis.read(data);
                JSONObject root = new JSONObject(new String(data));
                JSONObject icd = root.getJSONObject("ICD");
                String path = icd.getString("library_path");
                if (new File(path).exists()) {
                    return path;
                }
            } catch (IOException | JSONException e) {
                Log.w(TAG, "Failed to parse vk_icd.json for library_path", e);
            }
        }
        // Fallback: scan directory for vulkan .so files
        return findSoRecursive(dir);
    }

    private static String findSoRecursive(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files) {
            if (f.isDirectory()) {
                String result = findSoRecursive(f);
                if (result != null) return result;
            } else if (f.getName().endsWith(".so") && f.getName().contains("vulkan")) {
                return f.getAbsolutePath();
            }
        }
        return null;
    }

    // Old stub methods (ensureAndroidStubLibraries, copyFile, extractLibraryFromApk)
    // were completely removed during the libadrenotools migration.

    public static void removeDriver(Context context) {
        Log.i(TAG, "Removing custom driver and clearing related environment");
        clearDriverEnv();

        File dir = getDriverDirectory(context);
        deleteRecursive(dir);
    }

    private static boolean chmodSoFilesRecursive(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return true;
        boolean success = true;
        for (File f : files) {
            if (f.isDirectory()) {
                if (!chmodSoFilesRecursive(f)) success = false;
            } else if (f.getName().endsWith(".so")) {
                try {
                    Os.chmod(f.getAbsolutePath(), 0700);
                } catch (ErrnoException e) {
                    Log.w(TAG, "chmod failed for " + f.getAbsolutePath() + ": " + e);
                    if (!f.setExecutable(true, true)) {
                        Log.e(TAG, "setExecutable also failed for " + f.getAbsolutePath());
                        success = false;
                    }
                }
            }
        }
        return success;
    }

    private static void deleteRecursive(File fileOrDirectory) {
        if (fileOrDirectory.isDirectory()) {
            File[] children = fileOrDirectory.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        fileOrDirectory.delete();
    }

    private static void setLastDriverError(String error) {
        lastDriverError = error == null ? "" : error;
    }

    private static String buildFailureReason(Exception exception) {
        if (exception == null) {
            return "Unknown driver installation failure.";
        }
        String message = exception.getMessage();
        if (message == null || message.trim().isEmpty()) {
            return exception.getClass().getSimpleName();
        }
        return message.trim();
    }
}
