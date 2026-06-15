// Default Frida script for ZygiskFrida
console.log("[Frida] Script loading...");

Java.perform(function() {
    var Log = Java.use("android.util.Log");
    var TAG = "ZygiskFridaJS";
    
    Log.i(TAG, "------------------------------------------");
    Log.i(TAG, "Frida script injected and running!");
    Log.i(TAG, "Process: " + Process.id);
    Log.i(TAG, "------------------------------------------");
    
    console.log("[Frida] Found Android runtime, logs also sent to Logcat tag: " + TAG);
});

console.log("[Frida] Script initialization complete");
