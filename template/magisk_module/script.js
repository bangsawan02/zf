// Default Frida script for ZygiskFrida
console.log("[Frida] Script starting...");

// Function to log specifically to Android Logcat
function logToLogcat(message) {
    console.log(message);
    try {
        Java.perform(function() {
            var Log = Java.use("android.util.Log");
            Log.i("ZygiskFrida", "[JS] " + message);
        });
    } catch (e) {
        // Fallback for non-Java context
    }
}

logToLogcat("------------------------------------------");
logToLogcat("ZygiskFrida Script Loaded Successfully!");
logToLogcat("Current Process: " + Process.id);
logToLogcat("Current Thread: " + Process.getCurrentThreadId());
logToLogcat("Script Location: " + (typeof __filename !== 'undefined' ? __filename : 'unknown'));
logToLogcat("------------------------------------------");

// Hook example
Java.perform(function() {
    logToLogcat("Frida is inside Java.perform context");
    try {
        var ActivityThread = Java.use("android.app.ActivityThread");
        var app = ActivityThread.currentApplication();
        if (app) {
            logToLogcat("Application Name: " + app.getPackageName());
        }
    } catch (e) {
        logToLogcat("Failed to get application info: " + e.message);
    }
});
