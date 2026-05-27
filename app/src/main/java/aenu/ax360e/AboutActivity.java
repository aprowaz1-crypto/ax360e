// SPDX-License-Identifier: WTFPL
package aenu.ax360e;

import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

public class AboutActivity extends AppCompatActivity {


    public static String get_update_log(){
        final String log="\n"
                +"0.2(2025-09-30)\n"
                + " *首个正式版本\n"
                +"0.3(2025-10-03)\n"
                + " *修正虚拟键盘闪退\n"
                + " *修正摇杆无效\n"
                + " *优化stfs格式识别（无后缀名即可）\n"
                + " *加入设置界面（暂不可用）\n"
                +"0.4(2025-10-07)\n"
                + " *完善设置\n"
                + " *添加虚拟键盘编辑\n"
                + " *多语言支持\n"
                +"0.6(2025-10-29)\n"
                + " *将apu,gpu,kernel等部分切换到xenia canary\n"
                +"0.8(2025-11-21)\n"
                + " *修正默认配置\n"
                + " *更新设置\n"
                +"0.9(2025-12-13)\n"
                + " *修正按键映射\n"
                + " *优化界面\n"
                +"0.10(2026-01-04)\n"
                + " *虚拟键盘优化\n"
                + " *将vfs部分切换到xenia canary\n"
                + " *设置完善\n"
                +"0.11(2026-01-09)\n"
                + " *修正了a64后端的部分实现\n"
                +"0.12(2026-01-31)\n"
                + " *部分修正\n"
                //+ " *创建快捷方式\n"
                +"0.13(2026-02-11)\n"
                + " *部分修正\n"
                +"0.14(2026-03-11)\n"
                + " *部分优化与修正\n"
                + " *修复手柄摇杆\n"
                + " *修复xex格式支持\n"
                + " *添加zar格式支持\n"
                + " \n";

        return log;
    }

    TextView text;
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_about);
        text=findViewById(R.id.about_text);
        findViewById(R.id.gratitude).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                text.setText(R.string.gratitude_content);
            }
        });
        findViewById(R.id.update_log).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                text.setText(get_update_log());
            }
        });

        findViewById(R.id.open_source_licenses).setOnClickListener(new View.OnClickListener() {
            void show_licenses_dialog(){
                AlertDialog.Builder ab=new AlertDialog.Builder(AboutActivity.this);
                ab.setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface p1, int p2) {
                        p1.cancel();
                    }
                });
                WebView wv=new WebView(AboutActivity.this);
                wv.loadUrl("file:///android_asset/licenses.html");
                ab.setView(wv);
                ab.create().show();
            }
            @Override
            public void onClick(View v) {
                show_licenses_dialog();
            }
        });

        String info = Emulator.get.simple_device_info();

        // === Deepened driver status integration (p3-4): TurnipDriverInfo + native detailed + build flag ===
        String driverSection = "\n\n════════════════════════════════\n"
                + getString(R.string.driver_status_section) + "\n"
                + "════════════════════════════════\n";

        try {
            TurnipDriverInfo driverInfo = TurnipDriverInfo.detect(this);

            // Prominent one-line summary in the main about text (discoverable, nicely formatted)
            if (driverInfo.isInstalled()) {
                driverSection += driverInfo.getFormattedInfo() + "\n";

                // Add device compatibility snippet when available
                String compat = driverInfo.getDeviceCompatibilityReport();
                if (compat != null && !compat.trim().isEmpty()) {
                    driverSection += "\n" + compat.trim() + "\n";
                }
            } else {
                driverSection += getString(R.string.driver_summary_no_driver) + "\n"
                        + "Install via Settings → Custom Drivers for best Adreno performance.\n";
            }

            // Native detailed status (now richer post p3-4 native enhancement)
            try {
                String detailed = Emulator.nativeGetDetailedDriverStatus();
                if (detailed != null && !detailed.isEmpty() && !detailed.contains("No custom driver status")) {
                    driverSection += "\n[Native Loader Details]\n" + detailed + "\n";
                }
            } catch (Throwable ignored) {}

            // Build capability (new native)
            try {
                boolean buildSupports = Emulator.nativeSupportsLibadrenotoolsBuild();
                driverSection += "\n" + (buildSupports
                        ? getString(R.string.build_supports_libadrenotools)
                        : getString(R.string.build_no_libadrenotools)) + "\n";
            } catch (Throwable ignored) {}

            // Quick hint for user (p3-6 updated)
            driverSection += "\n(Tap Driver Info / Troubleshooting buttons above for the full rich report + copy + live refresh. See toolbar and empty-state badges too.)";
        } catch (Exception e) {
            driverSection += "[Driver status detection failed: " + e.getMessage() + "]";
        }

        info += driverSection;

        text.setText(info);
        text.setTextIsSelectable(true);
        text.setLongClickable(true);

        // === Wire up the new dedicated Driver Info action buttons (prominent at top of screen) ===
        setupDriverActionButtons();
    }

    private void setupDriverActionButtons() {
        // "Driver Info" - opens the rich formatted dialog (re-uses polished UX from Settings)
        View driverInfoBtn = findViewById(R.id.driver_info_btn);
        if (driverInfoBtn != null) {
            driverInfoBtn.setOnClickListener(v -> showDriverInfoDialog());
        }

        // Troubleshooting quick action dialog with actionable tips
        View troubleshootBtn = findViewById(R.id.driver_troubleshoot_btn);
        if (troubleshootBtn != null) {
            troubleshootBtn.setOnClickListener(v -> showTroubleshootingDialog());
        }

        // Quick link/action to open full Custom Driver settings (where the pref for Driver Info also lives)
        View settingsBtn = findViewById(R.id.driver_settings_btn);
        if (settingsBtn != null) {
            settingsBtn.setOnClickListener(v -> {
                try {
                    Intent intent = new Intent(AboutActivity.this, EmulatorSettings.class);
                    startActivity(intent);
                } catch (Exception e) {
                    Toast.makeText(this, "Could not open Settings", Toast.LENGTH_SHORT).show();
                }
            });
        }
    }

    /**
     * Shows the rich, emoji-sectioned full driver report dialog.
     * Matches the high-quality implementation used in EmulatorSettings and MainActivity toolbar.
     * Includes Copy + Refresh for excellent live UX.
     */
    private void showDriverInfoDialog() {
        try {
            final TurnipDriverInfo[] currentInfo = new TurnipDriverInfo[1];
            currentInfo[0] = TurnipDriverInfo.detect(this);

            final String[] currentReport = new String[1];
            currentReport[0] = currentInfo[0].getRichDriverReport(this);

            final ScrollView scrollView = new ScrollView(this);
            final TextView contentView = new TextView(this);
            contentView.setText(currentReport[0]);
            contentView.setTextIsSelectable(true);
            contentView.setPadding(32, 24, 32, 24);
            contentView.setTextSize(14f);
            scrollView.addView(contentView, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));

            new AlertDialog.Builder(this)
                    .setTitle(getString(R.string.turnip_driver_info_title))
                    .setView(scrollView)
                    .setPositiveButton(getString(R.string.copy_to_clipboard), (dialog, which) -> {
                        try {
                            android.content.ClipboardManager clipboard =
                                    (android.content.ClipboardManager) getSystemService(android.content.Context.CLIPBOARD_SERVICE);
                            android.content.ClipData clip = android.content.ClipData.newPlainText("Turnip Driver Info", currentReport[0]);
                            clipboard.setPrimaryClip(clip);
                            Toast.makeText(this, getString(R.string.driver_info_copied), Toast.LENGTH_SHORT).show();
                        } catch (Exception e) {
                            Toast.makeText(this, getString(R.string.copy_failed), Toast.LENGTH_SHORT).show();
                        }
                    })
                    .setNeutralButton(getString(R.string.refresh), (dialog, which) -> {
                        ((androidx.appcompat.app.AlertDialog) dialog).dismiss();
                        new Handler(Looper.getMainLooper()).post(this::showDriverInfoDialog);
                    })
                    .setNegativeButton(getString(R.string.close), null)
                    .create().show();
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.driver_status_unavailable), Toast.LENGTH_SHORT).show();
        }
    }

    private void showTroubleshootingDialog() {
        try {
            new AlertDialog.Builder(this)
                    .setTitle(getString(R.string.driver_troubleshooting_title))
                    .setMessage(getString(R.string.driver_troubleshooting_content))
                    .setPositiveButton(getString(R.string.view_full_driver_report), (d, w) -> showDriverInfoDialog())
                    .setNegativeButton(getString(R.string.close), null)
                    .setNeutralButton(getString(R.string.open_driver_settings), (d, w) -> {
                        try {
                            startActivity(new Intent(this, EmulatorSettings.class));
                        } catch (Exception ignored) {}
                    })
                    .show();
        } catch (Exception e) {
            // Fallback
            Toast.makeText(this, getString(R.string.driver_troubleshooting_content), Toast.LENGTH_LONG).show();
        }
    }
}
