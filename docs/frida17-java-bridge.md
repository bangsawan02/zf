# Frida 17 + Java Bridge di Termux

Semua ini diverifikasi di perangkat ini: Termux, Android 13 (arm64-v8a),
Frida 17.24.3, OnyxZygisk, tanpa PC.

## TL;DR

Frida 17 **menghapus** global `Java`. Bridge-nya sekarang package npm terpisah
dan harus di-bundle. Hasil bundle ~236 KB, tapi transport Frida menolak payload
di atas ~128 KB. Solusinya: bundle di-push ke file di target, lalu kirim loader
kecil yang membacanya dan melakukan `eval`.

```sh
# 1. dependency (Termux repo tidak punya; npm adalah cara termux-first)
mkdir -p ~/jswork && cd ~/jswork
npm install frida-java-bridge@7.0.13

# 2. tulis hook
cat > hook.ts <<'EOF'
import Java from "frida-java-bridge";
Java.perform(() => {
  console.log("package = " +
    Java.use("android.app.ActivityThread").currentApplication().getPackageName());
});
EOF

# 3. jalankan (compile + push + eval)
python3 ~/.config/opencode/skills/frida-termux/references/jbrun.py hook.ts 27044
```

## 1. Install `frida-java-bridge`

```sh
cd ~/jswork
npm install --no-audit --no-fund frida-java-bridge@7.0.13
```

Versioning bridge **terpisah** dari Frida: Frida 17.24.3 berpasangan dengan
bridge 7.x, bukan 17.24.3. `npm install frida-java-bridge@17.24.3` akan gagal
dengan `No matching version found`.

## 2. `frida.Compiler` — API resmi Frida 17

`frida-compile` (CLI) dan `frida.Compiler` (Python) melakukan hal yang sama.
Bedanya: `frida.Compiler` bisa dipanggil dari dalam skrip Python, dan
opsinya lebih eksplisit.

```python
import frida

compiler = frida.Compiler()
result = compiler.build(
    "hook.ts",
    compression="terser",        # "none" | "terser"
    source_maps="omitted",       # "included" | "omitted"
    bundle_format="iife",        # "esm" | "iife"
    type_check="none",           # "full" | "none"
    project_root=".",            # tempat node_modules/frida-java-bridge
)
```

### Dua jebakan

| Gejala | Cause | Fix |
|---|---|---|
| `InvalidArgumentError: compilation failed` (tanpa detail) | `type_check="full"` (default) menolak TS | `type_check="none"` |
| `compilation failed` juga saat file JS biasa | `project_root` salah, `frida-java-bridge` tidak ketemu | `project_root` = folder yang punya `node_modules/frida-java-bridge` |

`project_root` searched berurutan oleh `jbrun.py`:
1. folder script
2. `~/jswork`
3. `~/node_modules`
4. root dari `npm root -g`

## 3. Limit transport ~128 KB

Bukan bundle-nya yang salah — **ukuran payload**-nya:

```
100 000 bytes  -> OK
150 000 bytes  -> TransportError: the connection is closed
```

`session.create_script()` mengirim source lewat socket, dan kegagalannya
tidak informatif. Gejalanya: attach berhasil, `create_script` langsung
"connection is closed".

Workaround: payload yang dikirim hanya loader kecil; bundle besar ada di file.

```
[host]  compile -> 236 KB bundle
[host]  push bundle ke /data/local/tmp/re.zyg.fri/taxsee/_frida_script.js
[host]  kirim loader ~300 byte:
          var f = new File(path, 'r');
          var code = f.readText();
          f.close();
          (0, eval)(code);
[target] eval bundle -> java bridge aktif
```

File harus **world-readable** karena proses app berjalan sebagai uid-nya
sendiri:

```sh
su -c "cat src.js > /data/local/tmp/re.zyg.fri/<app>/_frida_script.js"
su -c "chmod 666 /data/local/tmp/re.zyg.fri/<app>/_frida_script.js"
```

## 4. `jbrun.py` — wrapper siap pakai

```sh
python3 ~/.config/opencode/skills/frida-termux/references/jbrun.py <script.ts> [port]
python3 ~/.config/opencode/skills/frida-termux/references/jbrun.py --compile-only <script.ts>
```

- compile dengan `frida.Compiler` (type_check off, minify)
- push ke `/data/local/tmp/re.zyg.fri/taxsee/_frida_script.js`
- attach ke `127.0.0.1:<port>`, kirim loader, print messages live

## 5. `probe.py` — attach script biasa

Untuk script tanpa java bridge (payload < 128 KB, tidak perlu compile):

```sh
python3 ~/.config/opencode/skills/frida-termux/references/probe.py 27044
python3 ~/.config/opencode/skills/frida-termux/references/probe.py 27044 --scan SSL_write
python3 ~/.config/opencode/skills/frida-termux/references/probe.py 27044 myscript.js
```

## 6. `frida_mcp_lite.py cli` — mode shell

MCP server yang sama bisa dipanggil tanpa client MCP:

```sh
cd ~/.config/opencode/skills/frida-termux/scripts
python3 frida_mcp_lite.py tools
python3 frida_mcp_lite.py cli frida_version
python3 frida_mcp_lite.py cli frida_attach '{"process":"Gadget","device":"127.0.0.1:27044"}'
python3 frida_mcp_lite.py cli frida_exec '{"process":"Gadget","device":"127.0.0.1:27044","code":"console.log(1)"}'
```

Device bisa ditulis sebagai `host:port` eksplisit — `127.0.0.1:27042` (bank),
`127.0.0.1:27043` (chrome), `127.0.0.1:27044` (taxsee).

## 7. Catatan Frida 17 di Android

| Topik | Detail |
|---|---|
| `Build.VERSION.SDK_INT` | Di Android 13 ini **field statis int biasa**, bukan objek field. `Build.VERSION.SDK_INT.value` → `undefined`. Pakai `Java.use("android.os.Build").VERSION.SDK_INT` langsung. |
| `Java.enumerateLoadedClassesSync()` | Mengembalikan array of **string**, bukan objek. Tidak punya `.name`. |
| `Java.use("java.lang.Process").myPid()` | Tidak tersedia sebagai static method. Pakai `Process.id` (native Frida). |
| `console.log` | Masuk logcat dengan **tag milik script**, bukan `Frida`. Lihat tabel di `PORTS.md`. |

## 8. Contoh: hook native + java bridge bersamaan

```ts
import Java from "frida-java-bridge";

console.log("=== TAXSEE DEBUG ===");

function tryIt(label: string, fn: () => any) {
  try {
    console.log(label + " = " + fn());
  } catch (e: any) {
    console.log(label + " !! " + (e && e.message ? e.message : String(e)));
  }
}

Java.perform(() => {
  tryIt("package", () =>
    Java.use("android.app.ActivityThread").currentApplication().getPackageName());
  tryIt("model", () => Java.use("android.os.Build").MODEL.value);
});

setTimeout(() => {
  const m = Process.findModuleByName("libssl.so");
  if (!m) return;
  const addr = m.findExportByName("SSL_write");
  console.log("SSL_write @ " + addr);
  Interceptor.attach(addr, {
    onEnter(args) {
      const len = args[2].toInt32();
      let preview = "";
      try {
        const buf = new Uint8Array(args[1].readByteArray(Math.min(len, 100)));
        preview = String.fromCharCode.apply(null, buf).replace(/[^\x20-\x7e]/g, ".");
      } catch (e) {}
      console.log("[SSL_write] len=" + len + " " + preview);
    }
  });
}, 2000);
```

Output nyata dari Taxsee:

```
package = com.taxsee.driver
model = M2102J20SG
SSL_write @ 0x6f3bbcda94
[SSL_write] len=657  GET /Services/Driver.svc/api/v4/order-group/all/orders?appName=TAXSEE&imei=&udid=c8fa630248d42272&rt...
[SSL_write] len=1073 GET /Services/Driver.svc/UpdateEx?reason=normal&ga=1&distance=0.0&advertId=e4b52bc2-d1d0-4463-a80f-6...
[SSL_write] len=644  GET /Services/Driver.svc/api/v3/menu/main?appName=TAXSEE&imei=&udid=c8fa630248d42272&rt=155244.397...
```

## 9. Gotcha operational

- **Satu koneksi per gadget.** Gadget hanya menerima satu client. Jangan
  jalankan `probe.py` dan `jbrun.py` bersamaan ke port yang sama — yang kedua
  dapat "connection is closed". Kill dulu yang lama.
- **`on_load: "wait"`** menahan app sampai ada client connect. Kalau mau app
  jalan normal tapi tetap bisa attach, pakai `on_load: "resume"`.
- **Restart app setelah ganti config gadget.** Config dibaca saat injeksi.
- **`enumerate_processes()` bisa hang** kalau gadget disuspensi (`wait`).
  Attach by name `"Gadget"` sebagai gantinya.
- `frida-java-bridge` harus ada di `node_modules` **di folder tempat
  `frida.Compiler` dijalankan** (atau `project_root` yang menunjuk ke sana).
