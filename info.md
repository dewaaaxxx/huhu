# Project Info — LYN8BP Mod Menu

  > **Last updated:** 2026-05-27
  > Dokumen ini adalah referensi lengkap untuk agent AI. Baca ini dulu sebelum mulai kerja.

  ---

  ## Credentials & Tokens

  > Semua credential disimpan di Replit Secrets / scratchpad session. Jangan hardcode di sini.

  | Item | Value |
  |------|-------|
  | **GitHub Repo** | https://github.com/Kztutorial99/LYN8BP |
  | **GitHub Token** | _(lihat Replit Secrets: GITHUB_TOKEN)_ |
  | **Vercel Token** | _(lihat Replit Secrets: VERCEL_TOKEN)_ |
  | **Vercel Project ID** | prj_o6tYBc0gBDaIm74KXpXwRywA0nMO |
  | **Vercel Panel URL** | https://lyn8bp.vercel.app |
  | **Encrypt Key** | _(env: ENCRYPT_KEY — default di validate/route.ts)_ |
  | **WS Token** | _(env: WS_TOKEN — default di validate/route.ts)_ |
  | **Mod Version** | 1.0 |

  ---

  ## Aturan Penting

  - Panel source SELALU ada di `panel/` folder di root repo — **BUKAN** `.migration-backup/panel/`
  - Vercel rootDirectory = `panel/` — edit di `panel/` = auto-deploy ke Vercel
  - GitHub Actions build Android APK setiap push ke `main`
  - Push file ke GitHub harus **sequential** (satu per satu), bukan parallel — tiap commit ubah SHA semua file

  ---

  ## Stack

  ### License Panel (Next.js + Neon DB)
  - **Framework:** Next.js (App Router)
  - **DB:** PostgreSQL via Neon (`DATABASE_URL` env var)
  - **DB Client:** `@neondatabase/serverless` (neon tagged template)
  - **Deploy:** Vercel (auto-deploy dari `panel/` folder)
  - **API Validate Endpoint:** https://lyn8bp.vercel.app/api/validate

  ### C++ Mod Menu (Android)
  - **Target:** 8 Ball Pool (Android, arm64-v8a)
  - **Build:** GitHub Actions NDK r25c
  - **UI:** ImGui (custom mobile menu)
  - **Crypto:** XOR + Base64 (custom impl di keylogin.h — cocok dengan panel keygen.ts)

  ---

  ## Database Schema (`panel/lib/db.ts`)

  ```sql
  CREATE TABLE licenses (
    id          SERIAL PRIMARY KEY,
    key         VARCHAR(64) UNIQUE NOT NULL,
    status      VARCHAR(16) NOT NULL DEFAULT 'active',   -- active|banned|expired
    game_type   VARCHAR(32) NOT NULL DEFAULT '8ball',
    max_devices INT NOT NULL DEFAULT 1,                  -- 0 = unlimited
    note        TEXT DEFAULT '',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at  TIMESTAMPTZ,                             -- NULL = Lifetime
    hwid        TEXT DEFAULT '',
    features    TEXT DEFAULT ''                          -- comma-separated: "autoplay,aim_line,esp"
  );
  ```

  ### DB Functions (`panel/lib/db.ts`)
  | Function | Description |
  |----------|-------------|
  | `getAllLicenses()` | Ambil semua license, order by created_at DESC |
  | `getLicenseByKey(key)` | Cari license by key string |
  | `createLicense(data)` | Buat license baru (key, game_type, max_devices, note, expires_at, features) |
  | `updateLicenseStatus(id, status)` | Update status: active/banned/expired |
  | `updateLicenseHwid(key, hwid)` | Set HWID by key string |
  | `resetLicenseHwid(id)` | Clear HWID by numeric ID |
  | `updateLicenseFeatures(id, features)` | Update features string by ID |
  | `deleteLicense(id)` | Hapus license |
  | `extendLicense(id, days)` | Extend expiry N hari |
  | `getStats()` | Count total/active/expired/banned |
  | `initDB()` | Create table + ALTER TABLE migration (dipanggil dari /api/setup) |

  ---

  ## Panel API Routes

  ### `GET /api/licenses`
  Returns: `{ licenses: License[], stats: { total, active, expired, banned } }`

  ### `POST /api/licenses`
  Body: `{ key?, game_type, max_devices, note, expires_at, features: string[] }`
  Returns: `{ license: License }`

  ### `PATCH /api/licenses/[id]`
  Handles multiple operation modes:
  ```json
  { "reset_hwid": true }          // Clear HWID — device bisa login ulang
  { "features": "autoplay,esp" }  // Update feature flags (comma-separated string)
  { "extend_days": 30 }           // Extend expiry by N days (1–3650)
  { "status": "banned" }          // Change status: active|banned|expired
  ```

  ### `DELETE /api/licenses/[id]`
  Hapus license.

  ### `POST /api/validate`
  Dipanggil oleh C++ mod. Encrypted payload (XOR+Base64).

  **Request body:**
  ```json
  {
    "token": "<WS_TOKEN>",
    "data": "<encrypted: { license_key, hwid, game_type, version }>"
  }
  ```

  **Response success (encrypted, lalu di-decrypt di C++):**
  ```json
  {
    "status": "success",
    "data": {
      "auth_token": "0wQRlDkgoQlf",
      "expiry_date": "2026-12-31 23:59:59",
      "version": "1.0",
      "license_key": "LYN8BP-XXXX-XXXX",
      "max_devices": 1,
      "active_devices": 1,
      "features": ["autoplay", "aim_line", "ball_predict"]
    }
  }
  ```

  **Device limit logic:**
  - `max_devices == 0` = unlimited (no HWID check)
  - `max_devices == 1` = single device lock (HWID binding)
  - `max_devices > 1` = multi-device (HWID binding ke device pertama)

  ---

  ## Feature Flags System

  ### Available Features
  | ID | Label | Color |
  |----|-------|-------|
  | `autoplay` | AutoPlay Bot | Indigo |
  | `aim_line` | Extended Aim Line | Green |
  | `ball_predict` | Ball Prediction | Amber |
  | `auto_queue` | Auto Queue | Blue |
  | `anti_detect` | Anti Detection | Red |
  | `esp` | ESP Overlay | Purple |

  ### Behavior
  - Disimpan di DB sebagai comma-separated string: `"autoplay,aim_line,esp"`
  - API validate mengembalikan sebagai JSON array: `["autoplay","aim_line","esp"]`
  - **Empty features = semua fitur aktif (backward compatible dengan key lama)**

  ### C++ Usage (`app/src/main/jni/mod/keylogin.h`)
  ```cpp
  // Global vars (diisi setelah Login() dan RefreshLicenseStatus()):
  static std::vector<std::string> g_Features;

  // Helper — empty g_Features = semua fitur ON:
  inline bool HasFeature(const std::string& feat) {
      if (g_Features.empty()) return true;
      return std::find(g_Features.begin(), g_Features.end(), feat) != g_Features.end();
  }

  // Contoh pemakaian di menu.h:
  if (HasFeature("autoplay"))     { /* tampilkan AutoPlay tab */ }
  if (HasFeature("aim_line"))     { /* tampilkan Aim Line settings */ }
  if (HasFeature("ball_predict")) { /* tampilkan Ball Prediction */ }
  if (HasFeature("auto_queue"))   { /* tampilkan Auto Queue */ }
  if (HasFeature("anti_detect"))  { /* tampilkan Anti Detection */ }
  if (HasFeature("esp"))          { /* tampilkan ESP Overlay */ }
  ```

  ---

  ## Key Source Files

  ### Panel (Next.js)
  | File | Purpose |
  |------|---------|
  | `panel/lib/db.ts` | DB schema + semua query function |
  | `panel/lib/keygen.ts` | Key generator + XOR+Base64 crypto helpers |
  | `panel/app/dashboard/page.tsx` | Admin dashboard UI (semua fitur) |
  | `panel/app/api/licenses/route.ts` | GET list + POST create |
  | `panel/app/api/licenses/[id]/route.ts` | PATCH (reset_hwid/features/extend/status) + DELETE |
  | `panel/app/api/validate/route.ts` | C++ mod validation endpoint |
  | `panel/app/api/setup/route.ts` | Run initDB() migration — hit sekali setelah schema change |

  ### C++ Mod
  | File | Purpose |
  |------|---------|
  | `app/src/main/jni/menu.h` | Main ImGui menu (semua tab + UI) |
  | `app/src/main/jni/mod/keylogin.h` | License validation, g_Features, HasFeature() |
  | `app/src/main/jni/game/inc/AutoPlay.h` | AutoPlay shoot engine (8-ball) |
  | `app/src/main/jni/game/inc/AimLock9Ball.h` | 9-Ball AimLock scan engine |
  | `app/src/main/jni/game/inc/AutoAim.h` | Manual aim-assist (solid/stripe) |
  | `app/src/main/jni/game/inc/Prediction.fast.h` | Shot simulation engine |
  | `app/src/main/jni/game/inc/NumberUtils.h` | Angle/double precision utils |
  | `app/src/main/jni/game/inc/GameConstants.h` | Table, pocket, physics constants |

  ---

  ## Dashboard UI Features (panel/app/dashboard/page.tsx)

  ### Action Buttons per Key
  | Button | Color | Action |
  |--------|-------|--------|
  | ✏️ Extend | Blue | Perpanjang expiry (hari / bulan) |
  | 🔶 Reset HWID | Amber | Clear HWID, device bisa login ulang di perangkat baru |
  | ⭐ Features | Green | Buka FeaturesModal, toggle feature on/off per key |
  | ⛔ Status | Red/Green | Toggle active/banned/expired |
  | 🗑️ Delete | Dark Red | Hapus key |

  ### Create Key Modal
  - Key format: `LYN8BP-XXXX-XXXX` (auto-generate) atau custom key
  - Max Devices: min 0 (unlimited) — tidak ada batas atas
  - Expiry: datetime picker + quick presets (7d / 30d / 90d / 1y / Lifetime)
  - Features: checkboxes untuk tiap fitur (kosong = semua aktif)

  ### FeaturesModal
  - Toggle individual feature per key
  - Tombol: "Reset Semua" / "Aktifkan Semua" / "Simpan Fitur"
  - Perubahan langsung tersimpan ke DB

  ---

  ## Menu Tab Structure (menu.h)

  | Tab | Content |
  |-----|---------|
  | 0 — Aim | Draw Lines, Draw Pockets, Enemy Lines, Auto Play, 9-Ball AimLock |
  | 1 — User | Device info, Key display, Expiry countdown |
  | 2 — Settings | Theme picker, Line Thickness, Menu Size, Font Scale, Save Config |

  ---

  ## Persistent Config Keys (C++)

  | Key | Type | Description |
  |-----|------|-------------|
  | `bESP_DrawPredictionLine` | bool | Draw prediction/aim lines |
  | `bESP_DrawPocketsShotState` | bool | Draw pocket indicators |
  | `bEnemyLine` | bool | Show enemy aim lines |
  | `bAutoPlay` | bool | AutoPlay 8-ball on/off |
  | `b9BallAimLock` | bool | 9-Ball AimLock on/off |
  | `bManualPower` | bool | AutoPlay manual power override |
  | `bAutoPlayFixedPower` | bool | AutoPlay fixed power mode |
  | `fAutoPlayPower` | float | AutoPlay power value (0–1000) |
  | `fAutoPlayShotDelayMax` | float | AutoPlay shot delay max (seconds) |
  | `iLineThickness` | int | ESP line thickness (1–10) |
  | `iMenuSizeOffset` | int | Menu scale offset (0–20) |
  | `iTheme` | int | Theme index (0=Dark Red, 1=Dark Glass, 2=Dark Neon) |
  | `fFontScale` | float | ImGui font scale |

  ---

  ## History — Bug Fixes & Features

  | Commit/Build | Perubahan | Description |
  |-------------|-----------|-------------|
  | #31/#32 | AutoPlay.h reset | Reset loop state saat game mulai/berakhir |
  | #31/#32 | menu.h sync format | Sinkronisasi format display AutoPlay |
  | #31/#32 | UpdateKey clear | Clear HWID/key saat logout |
  | #31/#32 | DrawMinimizedIcon | Fix icon render saat menu minimized |
  | 2026-05-27 | Panel key format | LYN8BP-XXXX-XXXX format |
  | 2026-05-27 | Extend modal | Extend expiry dengan hari/bulan tabs |
  | 2026-05-27 | HWID Reset | Tombol reset HWID per key di dashboard |
  | 2026-05-27 | Unlimited devices | max_devices: 0 = unlimited, no cap |
  | 2026-05-27 | Feature flags | Per-key feature toggle (panel + API + C++) |
  | #16 | BUG-1 | Power override re-validates shot before firing |
  | #16 | BUG-2 | ScanFast tries fallback powers {666,466,266,100} |
  | #14 | BUG-3 | No cache-skip for actual shot (isAuto=false) |
  | #14 | BUG-4 | SceneSnapshot captures real pocket positions |
  | #13 | BUG-5 | normalizeDoublePrecision maxLen 7→9 |

  ---

  ## Build & Deploy

  - **Android Build:** GitHub Actions → NDK r25c → push ke `main` = auto build APK
  - **Panel Deploy:** Vercel → push ke `panel/` di `main` = auto deploy (1-2 menit)
  - **DB Migration:** Hit GET `/api/setup` setelah schema change (jalankan `initDB()` + ALTER TABLE)
  