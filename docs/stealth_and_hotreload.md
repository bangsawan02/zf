# Stealth & Hot Reload

Dua fitur yang berjalan di dalam proses target setelah gadget di-inject.

## 1. Hot reload script (tanpa restart aplikasi)

Gadget dibuka dengan mode `script`:

```json
{
  "interaction": { "type": "script", "path": "/data/local/tmp/re.zyg.fri/script.js" },
  "runtime": "v8"
}
```

Saat `preAppSpecialize`, modul menyalin config ke sandbox aplikasi
(`/data/data/<pkg>/.zygisk_frida/`), lalu:

1. Menulis `hotreload.js` di folder yang sama.
2. Mengubah `interaction.path` pada config hasil salinan agar menunjuk
   `hotreload.js`.
3. `hotreload.js` membaca `script.js`, menjalankannya lewat
   `Script.evaluate()`, lalu polling tiap `reload_interval_ms`.

Jadi edit `script.js` → simpan → script baru otomatis dieksekusi di proses yang
sedang hidup.

### Sinkronisasi script ke sandbox

Aplikasi **tidak boleh** membaca `/data/local/tmp` (SELinux `untrusted_app`),
jadi salinan di sandbox yang dibaca oleh watcher. Untuk push tanpa restart:

```sh
./tools/zfctl reload com.example.app     # root, via su
```

Perintah itu menyalin `script.js` dari `/data/local/tmp/re.zyg.fri/` ke
`/data/data/<pkg>/.zygisk_frida/script.js` dan memperbaiki uid/gid.

### Membersihkan hook lama

Sebelum evaluasi ulang, wrapper memanggil `Interceptor.detachAll()` dan (bila
ada) `globalThis.__zfOnUnload()`. Untuk hook Java yang tidak otomatis lepas,
definisikan pembersihan sendiri di script:

```js
let myHook = null;

globalThis.__zfOnUnload = function () {
    if (myHook !== null) {
        myHook.implementation = null;   // balikkan hook Java
        myHook = null;
    }
};
```

### Catatan

- Hot reload hanya aktif untuk `interaction.type = "script"`. Mode `listen`
  sudah bisa reload dari sisi client (`script.load()`).
- `reload_interval_ms` default 1500 ms. Watcher membandingkan isi file, jadi
  perubahan yang ukurannya sama tetap terdeteksi.
- Script yang memakai sintaks ES module dicoba lewat `Script.load()` sebagai
  fallback.
- Untuk menghemat I/O, naikkan interval (mis. 5000) bila script sangat besar.

## 2. Linker hiding & memory cloaking

Setelah semua library selesai di-inject, thread injeksi menjalankan
`stealth_apply()`:

| Opsi | Default | Efek |
|---|---|---|
| `stealth.enabled` | `false` | master switch — **opt-in per target** |
| `stealth.remap` | `false` | ganti semua mapping file-backed `lib*.so` dengan salinan anonim |
| `stealth.rename_threads` | `false` | rename thread `gum-js-loop`, `gmain`, `gdbus`, dll |
| `stealth.unlink_payload` | `false` | hapus file `.so` hasil lokalisasi dari sandbox |
| `stealth.remap_delay_ms` | `0` | tunggu gadget selesai init sebelum remap |
| `stealth.thread_alias` | `"Jit thread pool"` | nama pengganti thread |

> **Kenapa default-nya mati.** Remap dilakukan **setelah** gadget hidup, saat
> thread gum sudah berjalan: `mremap(MREMAP_FIXED)` menimpa segmen executable
> yang mungkin sedang dieksekusi. Di aplikasi ber-harden (SecShell/Tencent) itu
> berakhir `SIGSEGV` dengan `pc/lr/sp` tak termap. `unlink_payload` juga
> self-defeating untuk app yang respawn atau punya beberapa proses senama:
> proses berikutnya tidak punya apa pun untuk diinjeksi. Nyalakan hanya kalau
> target memang membutuhkannya, dan uji ulang setelah menyalakannya.


### Apa yang berubah di `/proc/self/maps`

Sebelum:

```
7a1c000000-7a1c021000 r-xp 00000000 fd:00 1234 /data/data/com.x/.zygisk_frida/libgadget.so
```

Sesudah:

```
7a1c000000-7a1c021000 r-xp 00000000 00:00 0
```

Caranya: untuk tiap segmen, modul `mmap()` region anonim, menyalin isinya,
mengembalikan permission asli, lalu `mremap(MREMAP_FIXED)` menimpa mapping
file-backed. Alamat virtual tidak berubah, jadi relokasi, GOT, dan handle
`dlopen` tetap valid. File `.so` juga di-unlink dari sandbox sehingga scanner
berbasis path tidak menemukan apa pun.

### Thread

Frida membuat thread dengan nama yang mudah dicari (`gum-js-loop`, `gmain`,
`gdbus`, `pool-spawner`). Modul menulis ulang `/proc/self/task/<tid>/comm` agar
terlihat seperti thread ART biasa.

### Batasan

- Modul Zygisk (`libzygiskfrida.so`) sendiri **tidak** di-remap: kode itu
  sedang dieksekusi oleh thread injeksi saat proses berjalan. Untuk target,
  Zygisk tetap memetakan modul sampai proses mati.
- Remap tidak menyembunyikan isi memori (string Frida masih ada di region
  anonim). Yang hilang adalah jejak path di `/proc/self/maps` dan file di
  disk.
- Bila aplikasi sangat sensitif dan mengalami crash saat remap, turunkan
  agresivitas:

  ```json
  "stealth": { "remap": false, "rename_threads": true, "unlink_payload": true }
  ```

- Semua operasi berjalan sebagai uid aplikasi, tanpa `ptrace`, tanpa root
  tambahan.

## Contoh config lengkap

```json
{
  "version": 2,
  "defaults": {
    "enabled": true,
    "injected_libraries": [
      { "path": "/data/local/tmp/re.zyg.fri/libgadget.so" }
    ],
    "hot_reload": true,
    "reload_interval_ms": 1500,
    "stealth": {
      "enabled": true,
      "remap": true,
      "rename_threads": true,
      "unlink_payload": true,
      "remap_delay_ms": 750,
      "thread_alias": "Jit thread pool"
    }
  },
  "targets": {
    "com.example.app": true,
    "com.example.heavy": { "stealth": { "remap": false } }
  }
}
```
