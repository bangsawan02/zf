# Multi-Target Config (v2)

Satu `config.json` untuk banyak aplikasi. Tidak perlu copy-paste blok target,
tidak perlu satu file per aplikasi.

## Bentuk paling singkat

```json
{
  "version": 2,
  "defaults": {
    "enabled": true,
    "injected_libraries": [
      { "path": "/data/local/tmp/re.zyg.fri/libgadget.so" }
    ]
  },
  "targets": {
    "com.example.bank": "hook",
    "com.example.shop": true,
    "com.example.family.*": "hook"
  }
}
```

`targets` boleh berupa **object map** (key = nama paket / pola wildcard) atau
**array** seperti config v1. Nilai tiap entry boleh:

| Nilai | Arti |
|---|---|
| `true` | pakai `defaults` apa adanya, `enabled: true` |
| `false` | match tapi jangan inject (mematikan paket tertentu di dalam wildcard) |
| `"hook"` | pakai profil bernama `hook` |
| `{ ... }` | override per-target (field v1 + `hot_reload`, `stealth`, dst) |

Bentuk array juga menerima string polos:

```json
"targets": [
  "com.example.one",
  { "app_name": "com.example.two", "start_up_delay_ms": 5000 },
  { "app_name": ["com.example.three", "com.example.three:*"], "profile": "hook" }
]
```

## Urutan prioritas

Per-field, dari yang paling lemah: `defaults` → `profile` → `target`.

- **Skalar** (`enabled`, `start_up_delay_ms`, `hot_reload`, `reload_interval_ms`, `mode`): tulisan terakhir menang.
- **`injected_libraries`**: diganti utuh (tidak digabung).
- **`child_gating`** dan **`stealth`**: digabung per-field. Target cukup menulis
  field yang berubah.
- Target pertama yang match dipakai; urutan entry penting.
- Key tak dikenal hanya memunculkan `LOGW`, tidak menggagalkan config.

## Profil dan pewarisan (`extends`)

```json
"profiles": {
  "base":  { "stealth": { "thread_alias": "RenderThread" } },
  "hook":  { "extends": "base", "hot_reload": true },
  "quiet": { "extends": ["base"], "stealth": { "rename_threads": false } }
}
```

`extends` menerima satu nama atau array nama. Kedalaman maksimum 12 level;
siklus dideteksi dan dilaporkan sebagai error.

## Exclude (denylist)

Dicek **sebelum** target matching, jadi wildcard `*` pun tidak akan menyentuh
paket di sini:

```json
"exclude": ["com.android.*", "com.google.android.*", "com.example.protected"]
```

## Wildcard

`app_name` mendukung:

- `*` — sembarang urutan karakter (termasuk kosong)
- `?` — tepat satu karakter

Contoh: `com.example.*`, `com.*.whatsapp`, `id.co.bank?mobile`.

## Field baru per target

```json
{
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
}
```

Detailnya di [`stealth_and_hotreload.md`](stealth_and_hotreload.md).

## `zfctl` — editor config tanpa tangan

```sh
./tools/zfctl list
./tools/zfctl add com.example.app --profile hook --delay 3000
./tools/zfctl add 'com.example.family.*' --no-stealth
./tools/zfctl set com.example.app start_up_delay_ms=5000
./tools/zfctl rm com.example.app
./tools/zfctl validate
./tools/zfctl status
./tools/zfctl reload com.example.app   # butuh root
```

`zfctl add` otomatis membuat `config.json` v2 lengkap dengan `defaults`
(termasuk stealth + hot reload) bila file belum ada. Path default bisa
di-override lewat env `ZYGISKFRIDA_DIR`.

## Kompatibilitas v1

Config tanpa `"version"` tetap dibaca dengan skema lama. Target v1 boleh
menambahkan `hot_reload`, `reload_interval_ms`, dan `stealth`; field yang tidak
ditulis memakai default. Untuk migrasi: tambahkan `"version": 2`, pindahkan
bagian berulang ke `defaults`, lalu ganti `targets` menjadi object map.
