package aenu.ax360e;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Handler;
import android.os.Message;
import android.os.ParcelFileDescriptor;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.preference.PreferenceManager;
import android.util.Log;
import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.documentfile.provider.DocumentFile;

import java.io.File;

// Created by aenu on 2025/7/29.
// SPDX-License-Identifier: WTFPL
public class EmulatorActivity extends Activity implements SurfaceHolder.Callback, View.OnGenericMotionListener {

    static final int DELAY_ON_CREATE=0xaeae0001;
    public static final String EXTRA_GAME_URI="game_uri";
    static SurfaceView sf=null;
    private static final float STICK_DEADZONE = 0.10f;
    private static final float HAT_THRESHOLD = 0.50f;
    private static final float TRIGGER_DEADZONE = 0.02f;
    private static final int NO_ACTIVE_CONTROLLER = -1;
    private static final int TRIGGER_MAX_VALUE = 0xFF;

    private SparseIntArray keysMap = new SparseIntArray();
    private Vibrator vibrator=null;
    private VibrationEffect vibrationEffect=null;
    private int activeControllerDeviceId = NO_ACTIVE_CONTROLLER;
    private int hatXState = 0;
    private int hatYState = 0;
    static boolean started=false;
    Dialog delay_dialog=null;
    final Handler delay_on_create=new Handler(new Handler.Callback(){
        @Override
        public boolean handleMessage(@NonNull Message msg) {

            if(msg.what!=DELAY_ON_CREATE) return false;
            if(delay_dialog!=null){
                delay_dialog.dismiss();
                delay_dialog=null;
            }
            on_create();
            return true;
        }
    });
    private static final long MIN_RAM_BYTES = 4L * 1024 * 1024 * 1024; // 4 GB

    void on_create(){
        // Check minimum RAM as proxy for address space availability
        // Xbox 360 emulation requires 4GB+ virtual address space (xenia maps physical_membase_
        // at mapping_base_ + 0x100000000). While this checks physical RAM rather than VA space,
        // devices with <4GB RAM are unlikely to provide sufficient address space for emulation.
        android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am != null) {
            android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            if (mi.totalMem < MIN_RAM_BYTES) {
                long totalMB = mi.totalMem / (1024 * 1024);
                new AlertDialog.Builder(this)
                    .setTitle(R.string.insufficient_ram_title)
                    .setMessage(getString(R.string.insufficient_ram_message, totalMB))
                    .setPositiveButton(R.string.insufficient_ram_continue_anyway, (d, w) -> continueOnCreate())
                    .setNegativeButton(R.string.insufficient_ram_exit, (d, w) -> finish())
                    .setCancelable(false)
                    .show();
                return;
            }
        }
        continueOnCreate();
    }

    private String gameUri; // Store for environment resolution
    private PerformanceMonitor performanceMonitor;
    private MemoryPressureManager memoryPressureManager;
    private Handler metricsHandler;
    private Runnable metricsUpdateRunnable;
    private android.widget.TextView performanceOverlay;

    private void continueOnCreate(){
        String uri=getIntent().getStringExtra(EXTRA_GAME_URI);
        this.gameUri = uri; // Store for later use
        aenu.emulator.Emulator.Path path=aenu.emulator.Emulator.Path.from(uri,-1);
        Emulator.get.setup_context(this);
        Uri gameDirUri = MainActivity.load_pref_game_dir(this);
        if (gameDirUri != null) {
            Emulator.get.setup_document_file_tree(DocumentFile.fromTreeUri(this, gameDirUri));
        }
        Emulator.get.setup_game_path(path);
        Emulator.get.setup_launch_args(new String[]{
                "--storage_root="+Application.get_app_data_dir().getAbsolutePath(),
                "--config="+Application.get_global_config_file().getAbsolutePath(),
                // log_file cvar is not defined on AX360E - logging goes to logcat via log_to_logcat
        });

        Emulator.get.setup_uri_info_list_file(Application.get_uri_info_list_file().getAbsolutePath());
        setContentView(R.layout.activity_emulator);
        sf = (SurfaceView) findViewById(R.id.surface_view);
        sf.getHolder().addCallback(EmulatorActivity.this);

        sf.setFocusable(true);
        sf.setFocusableInTouchMode(true);
        sf.requestFocus();
        sf.setOnGenericMotionListener(this);

        load_key_map_and_vibrator();

        // Initialize performance overlay
        performanceOverlay = findViewById(R.id.performance_overlay);
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
        boolean showOverlay = prefs.getBoolean("Performance|show_performance_overlay", false);
        performanceOverlay.setVisibility(showOverlay ? android.view.View.VISIBLE : android.view.View.GONE);

        if (prefs.getBoolean("Performance|force_max_clocks", false)) {
            applyClockLocks(true);
        }

        // Initialize performance monitoring
        performanceMonitor = new PerformanceMonitor(this);
        memoryPressureManager = new MemoryPressureManager(this);
        metricsHandler = new Handler();
        metricsUpdateRunnable = new Runnable() {
            @Override
            public void run() {
                if (started && Emulator.get != null) {
                    performanceMonitor.updateMetrics();
                    performanceMonitor.pushMetricsToNative();
                    updateMemoryPressure();
                    updatePerformanceOverlay();
                }
                metricsHandler.postDelayed(this, 2000); // Update every 2 seconds
            }
        };
        metricsHandler.postDelayed(metricsUpdateRunnable, 2000);
    }

    private void updatePerformanceOverlay() {
        if (performanceOverlay.getVisibility() != android.view.View.VISIBLE) {
            return;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("FPS: ").append(String.format("%.1f", performanceMonitor.getCurrentFps())).append("\n");
        sb.append("Frame: ").append(String.format("%.1f", 1000.0f / Math.max(performanceMonitor.getCurrentFps(), 1.0f))).append("ms\n");
        sb.append("Memory: ").append(performanceMonitor.getFormattedMemoryUsage()).append("\n");
        sb.append("Thermal: ").append(performanceMonitor.getFormattedThermalStatus()).append("\n");
        sb.append("State: ").append(performanceMonitor.getPerformanceState().name());

        performanceOverlay.setText(sb.toString());
    }

    private void updateMemoryPressure() {
        MemoryPressureManager.MemoryPressure pressure = memoryPressureManager.getMemoryPressure();
        Emulator.get.update_memory_pressure(
            pressure.level.getValue(),
            pressure.availableMB,
            pressure.thermalLevel
        );
    }

    void vibrator(){
        if(vibrator!=null) {
            vibrator.vibrate(vibrationEffect);
        }
    }

    void load_key_map_and_vibrator() {
        final SharedPreferences sPrefs = PreferenceManager.getDefaultSharedPreferences(this);
        keysMap.clear();
        for (int i = 0; i < KeyMapConfig.KEY_NAMEIDS.length; i++) {
            String keyName = Integer.toString(KeyMapConfig.KEY_NAMEIDS[i]);
            int keyCode = sPrefs.getInt(keyName, KeyMapConfig.DEFAULT_KEYMAPPERS[i]);
            keysMap.put(keyCode, KeyMapConfig.KEY_VALUES[i]);
        }
        if(sPrefs.getBoolean("enable_vibrator",false)){
            vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);
            vibrationEffect = VibrationEffect.createOneShot(25, VibrationEffect.DEFAULT_AMPLITUDE);
        }
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if(!Application.should_delay_load()){
            on_create();
            return;
        }

        delay_dialog=ProgressTask.create_progress_dialog( this,getString(R.string.loading));
        delay_dialog.show();
        new Thread() {
            @Override
            public void run() {
                try {
                    Thread.sleep(500);
                    Emulator.load_library();
                    Thread.sleep(100);
                    delay_on_create.sendEmptyMessage(DELAY_ON_CREATE);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
            }
        }.start();
        return;
    }
    @Override
    public void onBackPressed()
    {

        if(delay_dialog!=null)
            return;

        AlertDialog.Builder ab=new AlertDialog.Builder(this);
        ab.setPositiveButton(R.string.quit, new DialogInterface.OnClickListener(){

            @Override
            public void onClick(DialogInterface p1, int p2)
            {
                p1.cancel();
                finish();
            }


        });

        /*ab.setNegativeButton("TE", new DialogInterface.OnClickListener(){

                @Override
                public void onClick(DialogInterface p1, int p2)
                {
                    if(Emulator.get.is_running())
                         Emulator.get.pause();
                     else if(Emulator.get.is_paused())
                         Emulator.get.resume();
                }


        });*/
        //if(Emulator.get.is_running())
        //Emulator.get.pause();
        ab.create().show();
    }

    /*@Override
    protected void onPause()
    {
        super.onPause();
        if(started)
            if(Emulator.get.is_running())
                Emulator.get.pause();;
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        if(started)
            if(Emulator.get.is_paused())
                Emulator.get.resume();
    }*/

    @Override
    protected void onPause() {
        super.onPause();
        // Stop performance overlay updates when activity is paused
        if (metricsHandler != null && metricsUpdateRunnable != null) {
            metricsHandler.removeCallbacks(metricsUpdateRunnable);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Resume performance overlay updates when activity is resumed
        if (metricsHandler != null && metricsUpdateRunnable != null && performanceOverlay != null && performanceOverlay.getVisibility() == View.VISIBLE) {
            metricsHandler.postDelayed(metricsUpdateRunnable, 2000);
        }
    }

    @Override
    protected void onDestroy()
    {
        // Stop performance monitoring
        if (metricsHandler != null && metricsUpdateRunnable != null) {
            metricsHandler.removeCallbacks(metricsUpdateRunnable);
        }

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
        if (prefs.getBoolean("Performance|force_max_clocks", false)) {
            // Restore synchronously or at least try to start the restore before we die
            applyClockLocks(false);
            try { Thread.sleep(200); } catch (Exception ignored) {} 
        }

        releaseControllerState();
        super.onDestroy();
        if (isFinishing()) {
            // The emulator's native state cannot be cleanly restarted in the same process.
            // Kill the :emu process so the next launch starts fresh.
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }

    final int KEY_NO_MAPPED = -1;

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (isGameControllerSource(event.getSource()) && !acceptControllerEvent(event)) {
            return true;
        }

        if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) {
            sendTrigger(VirtualControl.KEY_CODE_TRIGGER_L, true, TRIGGER_MAX_VALUE);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
            sendTrigger(VirtualControl.KEY_CODE_TRIGGER_R, true, TRIGGER_MAX_VALUE);
            return true;
        }

        int gameKey = keysMap.get(keyCode, KEY_NO_MAPPED);
        if (gameKey == KEY_NO_MAPPED) return super.onKeyDown(keyCode, event);
        if (event.getRepeatCount() == 0){
            vibrator();
            Emulator.get.key_event(gameKey, true,VirtualControl.KEY_VALUE_UNUSED);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (isGameControllerSource(event.getSource()) && !acceptControllerEvent(event)) {
            return true;
        }

        if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) {
            sendTrigger(VirtualControl.KEY_CODE_TRIGGER_L, false, 0);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
            sendTrigger(VirtualControl.KEY_CODE_TRIGGER_R, false, 0);
            return true;
        }

        int gameKey = keysMap.get(keyCode, KEY_NO_MAPPED);
        if (gameKey != KEY_NO_MAPPED) {
            Emulator.get.key_event(gameKey, false,VirtualControl.KEY_VALUE_UNUSED);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }
    @Override
    public void surfaceCreated(@NonNull SurfaceHolder holder) {

        if(!started){
            started=true;

            // Resolve and apply launch environment (driver + Turnip vars)
            LaunchEnvironmentResolver envResolver = new LaunchEnvironmentResolver(this);
            envResolver.resolveAndApply(gameUri);

            Emulator.get.setup_surface(holder.getSurface());
            try {
                Emulator.get.boot();
            } catch (aenu.emulator.Emulator.BootException e) {
                throw new RuntimeException(e);
            }
        }
        else{
            Emulator.get.setup_surface(holder.getSurface());
            Emulator.get.notify_surface_changed();
            if(Emulator.get.is_paused())
                Emulator.get.resume();
        }


    }

    @Override
    public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
        if(!started) return;
        if(width==0||height==0) return;
        Emulator.get.change_surface(width,height);
    }

    @Override
    public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
        if(!started) return;
        releaseControllerState();
        Emulator.get.setup_surface(null);
    }

    private static boolean isGameControllerSource(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private boolean acceptControllerEvent(InputEvent event) {
        if (!isGameControllerSource(event.getSource())) {
            return false;
        }
        final int deviceId = event.getDeviceId();
        if (activeControllerDeviceId == NO_ACTIVE_CONTROLLER) {
            activeControllerDeviceId = deviceId;
            return true;
        }
        if (activeControllerDeviceId == deviceId) {
            return true;
        }
        if (InputDevice.getDevice(activeControllerDeviceId) == null) {
            activeControllerDeviceId = deviceId;
            return true;
        }
        return false;
    }

    private static float clamp(float value, float min, float max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    private static float normalizeTriggerValue(float value) {
        float normalized = value;
        if (normalized < 0.0f) {
            normalized = (normalized + 1.0f) * 0.5f;
        }
        return clamp(normalized, 0.0f, 1.0f);
    }

    private static float axisWithFallback(MotionEvent event, int primaryAxis, int fallbackAxis) {
        float primary = event.getAxisValue(primaryAxis);
        float fallback = event.getAxisValue(fallbackAxis);
        return Math.abs(fallback) > Math.abs(primary) ? fallback : primary;
    }

    private static float applyDeadzone(float value, float deadzone) {
        float abs = Math.abs(value);
        if (abs <= deadzone) {
            return 0.0f;
        }
        float normalized = (abs - deadzone) / (1.0f - deadzone);
        return value < 0.0f ? -normalized : normalized;
    }

    private static short scaleStick(float value) {
        float clamped = clamp(value, -1.0f, 1.0f);
        if (clamped < 0.0f) {
            return (short) Math.round(clamped * 32768.0f);
        }
        return (short) Math.round(clamped * 32767.0f);
    }

    private void sendStickAxis(int negativeKey, int positiveKey, float value) {
        final short zero = 0;
        if (value < 0.0f) {
            Emulator.get.key_event(positiveKey, false, zero);
            Emulator.get.key_event(negativeKey, true, scaleStick(value));
        } else if (value > 0.0f) {
            Emulator.get.key_event(negativeKey, false, zero);
            Emulator.get.key_event(positiveKey, true, scaleStick(value));
        } else {
            Emulator.get.key_event(negativeKey, false, zero);
            Emulator.get.key_event(positiveKey, false, zero);
        }
    }

    private void sendTrigger(int triggerKey, boolean pressed, int value) {
        Emulator.get.key_event(triggerKey, pressed, (short) value);
    }

    private void sendTriggerAxis(int triggerKey, float value) {
        float normalized = applyDeadzone(normalizeTriggerValue(value), TRIGGER_DEADZONE);
        int triggerValue = Math.round(normalized * TRIGGER_MAX_VALUE);
        sendTrigger(triggerKey, triggerValue > 0, triggerValue);
    }

    private void updateHatDpad(MotionEvent event) {
        int hatX = 0;
        int hatY = 0;
        float rawHatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float rawHatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (rawHatX <= -HAT_THRESHOLD) {
            hatX = -1;
        } else if (rawHatX >= HAT_THRESHOLD) {
            hatX = 1;
        }
        if (rawHatY <= -HAT_THRESHOLD) {
            hatY = -1;
        } else if (rawHatY >= HAT_THRESHOLD) {
            hatY = 1;
        }

        if (hatX == hatXState && hatY == hatYState) {
            return;
        }

        boolean hadPressed = hatXState != 0 || hatYState != 0;

        hatXState = hatX;
        hatYState = hatY;

        boolean leftPressed = hatXState < 0;
        boolean rightPressed = hatXState > 0;
        boolean upPressed = hatYState < 0;
        boolean downPressed = hatYState > 0;

        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, leftPressed, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, rightPressed, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, upPressed, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, downPressed, VirtualControl.KEY_VALUE_UNUSED);

        if ((leftPressed || rightPressed || upPressed || downPressed) && !hadPressed) {
            vibrator();
        }
    }

    private void releaseControllerState() {
        final short zero = 0;
        hatXState = 0;
        hatYState = 0;
        activeControllerDeviceId = NO_ACTIVE_CONTROLLER;

        if (Emulator.get == null) {
            return;
        }

        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, false, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, false, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, false, VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, false, VirtualControl.KEY_VALUE_UNUSED);

        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_LEFT, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_UP, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_RIGHT, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_DOWN, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_LEFT, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_UP, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_RIGHT, false, zero);
        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_DOWN, false, zero);

        sendTrigger(VirtualControl.KEY_CODE_TRIGGER_L, false, 0);
        sendTrigger(VirtualControl.KEY_CODE_TRIGGER_R, false, 0);
    }

    @Override
    public boolean onGenericMotion(View v, MotionEvent event) {
        if (!isGameControllerSource(event.getSource())) {
            return super.onGenericMotionEvent(event);
        }
        if (!acceptControllerEvent(event)) {
            return true;
        }
        if (event.getAction() == MotionEvent.ACTION_CANCEL) {
            releaseControllerState();
            return true;
        }

        updateHatDpad(event);

        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            float leftX = applyDeadzone(event.getAxisValue(MotionEvent.AXIS_X), STICK_DEADZONE);
            float leftY = applyDeadzone(event.getAxisValue(MotionEvent.AXIS_Y), STICK_DEADZONE);
            float rightX = applyDeadzone(axisWithFallback(event, MotionEvent.AXIS_RX, MotionEvent.AXIS_Z), STICK_DEADZONE);
            float rightY = applyDeadzone(axisWithFallback(event, MotionEvent.AXIS_RY, MotionEvent.AXIS_RZ), STICK_DEADZONE);

            sendStickAxis(VirtualControl.KEY_CODE_LTHUMB_LEFT, VirtualControl.KEY_CODE_LTHUMB_RIGHT, leftX);
            sendStickAxis(VirtualControl.KEY_CODE_LTHUMB_DOWN, VirtualControl.KEY_CODE_LTHUMB_UP, -leftY);
            sendStickAxis(VirtualControl.KEY_CODE_RTHUMB_LEFT, VirtualControl.KEY_CODE_RTHUMB_RIGHT, rightX);
            sendStickAxis(VirtualControl.KEY_CODE_RTHUMB_DOWN, VirtualControl.KEY_CODE_RTHUMB_UP, -rightY);

            float lTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
            sendTriggerAxis(VirtualControl.KEY_CODE_TRIGGER_L, lTrigger);
            sendTriggerAxis(VirtualControl.KEY_CODE_TRIGGER_R, rTrigger);

            return true;
        }

        return true;
    }

    private void applyClockLocks(boolean enable) {
        new Thread(() -> {
            if (enable) {
                Log.i("ax360e", "Applying Max Clock Locks (Root required)");
                // Snapdragon 8 Elite (sun) clock locking script
                String cmd = "setprop persist.vendor.power.mode 2; " +
                             "stop vendor.thermal-hal; " +
                             "stop perf2-hal-1-0; " +
                             "for i in 0 1 2 3 4 5 6 7; do " +
                             "  if [ -f /sys/devices/system/cpu/cpu$i/cpufreq/scaling_max_freq ]; then " +
                             "    echo performance > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_governor; " +
                             "    cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_max_freq > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_min_freq; " +
                             "  fi; " +
                             "done; " +
                             "echo 1 > /sys/class/thermal/thermal_message/sconfig"; // Odin-specific high perf
                Utils.runShell(cmd, true);
            } else {
                Log.i("ax360e", "Restoring Thermal Controls");
                String cmd = "setprop persist.vendor.power.mode 0; " +
                             "start vendor.thermal-hal; " +
                             "start perf2-hal-1-0; " +
                             "for i in 0 1 2 3 4 5 6 7; do " +
                             "  echo schedutil > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_governor; " +
                             "  echo 0 > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_min_freq; " +
                             "done; " +
                             "echo 0 > /sys/class/thermal/thermal_message/sconfig";
                Utils.runShell(cmd, true);
            }
        }).start();
    }

}
