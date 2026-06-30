# Design: Persistent Virtual-Media Mounts

|                     |                                                                                                                    |
| ------------------- | ------------------------------------------------------------------------------------------------------------------ |
| **Component** | `remote-media` (`xyz.openbmc_project.VirtualMedia`)                                                            |
| **Status**    | Implemented                                                                                                        |
| **Scope**     | Legacy-mode mounts only                                                                                            |
| **Source**    | `src/persistence.hpp`, `src/state_machine.hpp`, `src/main.cpp`, `xyz.openbmc_project.VirtualMedia.service` |

## 1. Motivation

A virtual-media mount in `remote-media` is ephemeral: it exists only for the
lifetime of the service process. When the BMC reboots (firmware update, power
cycle, watchdog, crash-restart) every active mount is lost and an operator or
higher-level orchestrator must re-issue the `Mount` call.

For platform-image use cases (e.g. a BMC that boots or repairs the host from a
CD/USB image it hosts itself), that mount needs to survive a reboot with **no
external actor** re-establishing it. This design adds opt-in, reboot-durable
mounts that the service restores automatically at start-up.

## 2. Goals and non-goals

### Goals

- **G1** — Survive BMC reboot: a designated mount is automatically re-created at
  service start without any external `Mount` call.
- **G2** — Opt-in per slot: only slots explicitly marked as durable are
  persisted; all other slots keep today's ephemeral behavior with zero change.
- **G3** — Never store secrets on disk.
- **G4** — Robust to power loss and corruption: a crash mid-write or a mangled
  state file must never brick service start-up.
- **G5** — Tolerate a `file://` image whose backing filesystem is not mounted
  yet at boot.
- **G6** — Keep the persistence logic decoupled from the mount state machine so
  it can be unit-tested and evolved independently.

### Non-goals

- Persisting **remote** (`https://` / `smb://`) mounts in this phase, with or
  without credentials. Authenticated ones would also require storing
  credentials (see G3).
- Persisting **Proxy-mode** mounts (those are driven by an external nbd client
  and have no server-side image to restore).
- Cross-BMC / redundant replication of mount state.
- A Redfish/D-Bus API to toggle persistence at runtime **in this phase**:
  eligibility here is structural (config-driven), not a live property. A future
  phase moves remote (`https://` / `smb://`) intent to the operator-visible,
  per-insert Redfish `EjectPolicy: Persistent` property; the structural `file://`
  opt-in stays.

## 3. Design overview

Two hook points wrap the existing mount state machine, plus a start-up replay
driver:

1. **Save hook** — when a slot reaches `Active`, if it is eligible, write a
   small JSON descriptor to a durable directory.
2. **Replay driver** — at service start, enumerate saved descriptors and drive
   each back through the normal mount path, with bounded retry while a
   `file://` image's backing filesystem is not mounted yet.
3. **Clear** — whenever a saved descriptor is discovered to be stale or invalid,
   delete it.

The persistence module (`persistence.hpp`) is deliberately **standalone**: it
has no dependency on `MountPointStateMachine` and speaks only in slot names and
a `SavedMount` POD. The FSM calls into it through three narrow entry points
(save, clear, enumerate). This satisfies G6 and keeps the on-disk concerns
(atomicity, validation, layout) in one file.

```mermaid
flowchart LR
    subgraph FSM [state_machine.hpp]
      A[ActiveState::onEnter] -->|eligible| W[writeState]
    end
    subgraph MAIN [main.cpp]
      R[replayPersistedMounts] --> E[enumerateSavedSlots]
      R --> D[driveReplayMount]
      D -->|MountEvent| FSM
    end
    subgraph P [persistence.hpp]
      W --> DISK[(/var/lib/virtual-media/persist)]
      E --> DISK
      C[clearState] --> DISK
    end
    D -. stale/invalid .-> C
```

## 4. Detailed design

### 4.1 Eligibility

A mount is persisted only if **both** hold at the instant it becomes `Active`:

1. **Slot opt-in** — the slot has a non-empty `AllowedLocalMountPoints` array in
   `/etc/virtual-media.json`. This existing field is reused as the single
   structural signal marking a slot as a "durable platform image slot". No new
   config key is introduced. Slots that leave it empty stay ephemeral. The gate
   is also what makes `file://` legal at all (`isLocalMountPointAllowed()`
   fails closed on an empty list), so it both designates a platform image slot
   and enforces the local-path allow-list.
2. **Local source** — the image URL is `file://` with no embedded `userinfo@`.

```text
persistable = slotOptedIn && file:// && no userinfo
```

Remote `https://` / `smb://` mounts are **not** persisted in this phase, even
when unauthenticated. `AllowedLocalMountPoints` is local-path policy, not
remote-URL intent; remote persistence will be gated on Redfish
`EjectPolicy: Persistent` (see §10). Replaying an HTTPS mount would also need
its `VerifyCertificate` setting, which the descriptor does not carry.

The FSM hook and `loadState()` share `persistence::isPersistableUrl()`, so a
remote URL is never written, and one left on disk by an older build is dropped
rather than replayed.

### 4.2 Data model

One JSON file per persisted slot:

```text
/var/lib/virtual-media/persist/<slot>.json
```

```json
{
  "SchemaVersion": 1,
  "Slot": "USB0",
  "ImageURL": "file:///var/lib/platform-images/boot.iso",
  "RW": false
}
```

- `SchemaVersion` gates forward/backward compatibility; `loadState()` drops
  files whose version it does not recognize instead of mis-parsing them.
- `ImageURL` is the only source-identifying field; by construction it never
  contains credentials.
- The `SavedMount` POD (`{ slot, imageUrl, rw }`) is the in-memory shape passed
  across the module boundary.

### 4.3 Storage semantics

- **Location & ownership** — the systemd unit declares
  `StateDirectory=virtual-media/persist`, so systemd provisions
  `/var/lib/virtual-media/persist` as `0700`, owned by the service user, on
  first start and preserves it across reboots and package upgrades.
- **Durable writes (G4)** — `writeState()` writes `<slot>.json.tmp` and
  `fsync(2)`s it, `rename(2)`s it over the target, then `fsync`s the state
  directory. The first `fsync` puts the new bytes on disk before the name
  points at them (`flush()`/`close()` alone only reach the page cache);
  `rename` swaps the directory entry atomically; the directory `fsync` makes
  that swap durable. A power loss mid-save therefore leaves either the old or
  the new descriptor, never a half-written one.
- **Best-effort** — a write failure logs and returns `false`; it never fails
  the mount the operator just requested. Persistence is an enhancement, not a
  precondition of mounting.

### 4.4 Save path

The hook is `ActiveState::onEnter` (`state_machine.hpp`). On every transition to
`Active` it re-evaluates eligibility and, if eligible, calls `writeState()`.
Re-writing on the replay path is intentional and harmless: the content matches
what was just loaded, and it self-heals a partially written file from a prior
boot.

```mermaid
sequenceDiagram
    participant Op as Operator/Orchestrator
    participant FSM as MountPointStateMachine
    participant P as persistence

    Op->>FSM: Mount(url, rw, creds?)
    FSM->>FSM: Activating -> WaitingForGadget -> Active
    Note over FSM: ActiveState::onEnter
    alt slot opted-in AND non-secret source
        FSM->>P: writeState(slot, url, rw)
        P-->>FSM: ok (best-effort)
    else ephemeral
        Note over FSM: no write
    end
```

### 4.5 Replay path

`main.cpp` posts `replayPersistedMounts()` onto the event loop immediately after
all slots are registered on D-Bus. For each descriptor from
`enumerateSavedSlots()` it re-checks eligibility synchronously and then drives
the mount on a coroutine:

- Slot no longer in config → `clearState`, skip.
- Slot lost its `AllowedLocalMountPoints` opt-in → `clearState`, skip.
- Slot is not legacy (`Mode` is proxy) → `clearState`, skip. A proxy slot would
  otherwise start an nbd-client listener from a saved image URL.
- Otherwise → seed the FSM target and fire a synthetic `MountEvent`.
- If that mount returns to `ReadyState` with "Local file is not a valid mount
  point" (the path is outside a narrowed allow-list) → `clearState`, stop.
  Retrying would fail the same way on every boot.

Note: a missing `file://` backing image is **not** cleared here. The backing
filesystem may be mounted after virtual-media starts (see §7), so a missing file
at replay time is usually a boot-time race, not a deleted image. Clearing it was
destructive — it permanently wiped a valid persisted mount on every reboot that
lost the race. Instead every eligible slot is handed to `driveReplayMount()`,
which waits/retries for the file and, if it never appears, **retains** the
descriptor for a future boot.

```mermaid
flowchart TD
    A[Service start] --> B[enumerateSavedSlots]
    B --> C{For each saved slot}
    C --> D{Slot in config?}
    D -- no --> X[clearState + skip]
    D -- yes --> E{Opt-in present?}
    E -- no --> X
    E -- yes --> M{Legacy mode?}
    M -- no --> X
    M -- yes --> G[spawn coroutine: driveReplayMount]
    G --> P{file:// present?}
    P -- no --> J
    P -- yes --> Q[emit MountEvent]
    Q --> H{Reached Active?}
    H -- yes --> I[Restored]
    H -- not allowed --> Y[clearState + stop]
    H -- no --> J{Attempts left?}
    J -- yes --> K[wait backoff] --> G
    J -- no --> L[Give up, RETAIN descriptor]
```

## 5. Failure handling and resilience

### 5.1 Self-healing reads (G4)

`loadState()` deletes and skips any descriptor that fails JSON parse, has a
mismatched `SchemaVersion`, an empty `ImageURL`, or a non-persistable scheme. A
single bad file therefore can never block start-up, and `enumerateSavedSlots()`
only ever returns known-good entries.

### 5.2 Startup readiness (G5)

Replay runs the instant the event loop starts, so it can race resources that
are not ready yet at boot. Only `file://` mounts are persisted, and such an
image may live on a filesystem mounted *after* virtual-media — e.g. the BF ARM
OS `host-settings` partition that backs `/media/bf_arm_os`, mounted by a
separate unit (see §7).

Two mechanisms address this:

1. **Mount ordering (platform)** — where a persisted `file://` image lives on a
   platform partition, the service is ordered after that partition's mount unit
   via a platform drop-in (see §7). This is the *deterministic* fix for the
   local-file race.
2. **Bounded retry** — `driveReplayMount()` polls the FSM for the outcome and
   retries failures, riding out a not-yet-mounted local filesystem that
   ordering alone might miss. It is the safety net, not the primary fix.

| Parameter               | Value  | Meaning                                      |
| ----------------------- | ------ | -------------------------------------------- |
| `kReplayMaxAttempts`  | 5      | Attempts per slot.                           |
| `kReplayRetryDelay`   | 5 s    | Backoff between attempts.                    |
| `kReplaySettlePolls`  | 120    | Polls per attempt.                           |
| `kReplayPollInterval` | 100 ms | Poll interval (~12 s settle budget/attempt). |

Behavior:

- Each attempt guards that the FSM is in `ReadyState` before emitting (so
  `emitMountEvent` can't throw on a stuck state), and re-seeds `target` (entering
  `ReadyState` after a failure clears it).
- For a `file://` image, each attempt first checks the file exists, resolving
  the path with `getImagePathFromFileUrl()` so it checks the same path the
  mount later opens. If it is missing, replay logs at info (this is the
  expected boot race), backs off, and retries **without** spawning nbdkit on a
  missing path — this is how it waits for a late-mounting backing filesystem.
- Polling distinguishes `ActiveState` (success), `ReadyState` (failed → retry,
  except an allow-list rejection, which clears the descriptor), and "still
  activating" (keep waiting within budget).
- After all attempts fail, the descriptor is **retained** so a later boot or a
  manual remount can still recover it — a transient race never discards a valid
  config.

## 6. Security considerations (G3)

- **No secrets on disk** — enforced structurally: only `file://` URLs without
  userinfo are ever handed to `writeState()`.
- **Owner-only state directory** — `0700`, root-owned, via `StateDirectoryMode`.

## 7. Deployment

Generic unit (`remote-media`):

```ini
[Unit]
After=dbus.service bmc-boot-complete.service
Requires=dbus.service

[Service]
StateDirectory=virtual-media/persist
StateDirectoryMode=0700
```

### Platform mount ordering (BF ARM OS)

The persisted image path `file:///media/bf_arm_os/...` resolves through a
symlink (`/media/bf_arm_os` → `/var/lib/bf_arm_os`) to the `host-settings`
ext4 partition. That partition is mounted by a dedicated oneshot unit,
`bf-arm-os-mount.service` (which runs the idempotent
`bf-arm-os-updater-mount.sh` early, ordered `After=local-fs.target`). Both
consumers depend on it:

- The **BF ARM OS updater** service `Requires=`/`After=` it (its old
  `ExecStartPre` mount was removed and centralised here).
- **virtual-media** orders after it via a platform-layer systemd drop-in
  (`xyz.openbmc_project.VirtualMedia.service.d/virtual-media-override.conf`):

  ```ini
  [Unit]
  Wants=bf-arm-os-mount.service
  After=bf-arm-os-mount.service
  ```

  `Wants` (not `Requires`) is deliberate: on builds without that mount unit, or
  if the mount fails, virtual-media still starts and the retry/self-heal in
  `driveReplayMount()` covers the gap.

Opt a slot in via `/etc/virtual-media.json` (`Mode`: `0` = proxy, `1` = legacy;
persistence applies to **legacy** only):

```json
{
  "MountPoints": {
    "USB0": {
      "Mode": 1,
      "NBDDevice": "nbd0",
      "UnixSocket": "usb0.sock",
      "AllowedLocalMountPoints": [
        "/var/lib/platform-images"
      ]
    }
  }
}
```

## 8. Testing

The persistence logic is decoupled from the mount state machine (G6) and is
exercised directly by a GoogleTest suite that links none of the daemon's
libraries (no systemd/udev/D-Bus objects) and runs in milliseconds.

### 8.1 Unit-test guide

The suite lives in `tests/test_persistence.cpp` and targets every function in
`src/persistence.hpp`. It is gated behind the `BUILD_TESTS` CMake option, which
defaults to `ON` for native builds (so the CI unit-test job's plain `cmake .` +
`ctest` builds and runs it) and `OFF` when cross-compiling (Yocto image builds,
which do not ship GoogleTest).

**Requirements:** a C++23 compiler, GoogleTest (`libgtest-dev` / `gtest` pkg),
and the header-only dependencies that `persistence.hpp` pulls in —
`nlohmann/json` plus the Boost and `sdbusplus` headers reached through
`utils.hpp` (the shared URL-scheme helpers live there). These are all present
in the OpenBMC SDK sysroot; only the headers are needed, not the libraries.

**Build and run with CMake/CTest:**

```bash
cd remote-media
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

**Quick one-off compile** (add the SDK sysroot so the Boost/`sdbusplus` headers
resolve; a bare host without those headers cannot build the suite):

```bash
cd remote-media
g++ -std=c++23 -Wall -I src --sysroot="${SDKTARGETSYSROOT}" \
    tests/test_persistence.cpp \
    -lgtest -lgtest_main -lpthread -o /tmp/test_persistence
/tmp/test_persistence
```

Every filesystem-touching test redirects the state directory to a unique tmp
path via `persistence::setStateDir()` in its `SetUp()`, so the tests never read
or write the real `/var/lib/virtual-media/persist` and can run in parallel.

### 8.2 Unit-test coverage

The standalone module is the primary unit-test surface (redirect the directory
with `setStateDir()` to a tmp path):

- **URL predicates** — the shared `utils::isFileUrl` / `utils::isHttpsUrl` /
  `utils::isCifsUrl` match only their scheme; `utils::hasEmbeddedUrlCredentials`
  flags `scheme://user[:pass]@host` but ignores an `@` in a path/query/fragment;
  `persistence::isPersistableUrl` accepts only `file://` without userinfo and
  rejects `https://` / `smb://`.
- **Round-trip** — `writeState` then `loadState` returns an equal `SavedMount`
  (both `rw=true` and `rw=false`), and the on-disk JSON carries the expected
  `SchemaVersion` / `Slot` / `ImageURL` / `RW`.
- **Atomicity** — the `.tmp` sidecar does not survive a successful `writeState`,
  and a stray `.tmp` is ignored by `enumerateSavedSlots`.
- **Self-heal** — corrupt JSON, wrong `SchemaVersion`, empty `ImageURL`,
  non-persistable schemes, and URL-embedded credentials are dropped from disk
  and not returned; a missing file yields `std::nullopt` without throwing.
- **Clear / overwrite** — `clearState` removes an existing file and is a no-op
  success when absent; re-persisting a slot overwrites the previous descriptor.
- **Enumerate** — only `<slot>.json` files are considered; mixed valid/invalid
  sets return only the valid ones; a missing directory returns an empty list.

### 8.3 Integration-level tests (FSM + replay)

- Eligible `file://` mounts are saved on `Active`; remote and non-opted-in
  mounts are not.
- Replay restores an eligible mount; a missing `file://` image is retried and
  retained if it never appears, while a lost-opt-in slot is cleared.
- A failed mount is retried and the descriptor is retained after the cap; a
  non-legacy slot or an image outside `AllowedLocalMountPoints` is cleared.

## 9. Alternatives considered

- **A new per-slot boolean config key** instead of reusing
  `AllowedLocalMountPoints`. Rejected: it adds a second source of truth for
  "durable slot" and drifts from the existing allow-list semantics — and a bare
  behavior toggle is easy to misconfigure (flag on without an allow-list, or
  vice versa). If a dedicated key were ever unavoidable, it should be a
  **slot-role** name (e.g. `PlatformImageSlot: true` / `DurablePlatformSlot: true`)
  rather than a behavior toggle like `PersistAcrossBmcReboot`. For remote
  intent the correct long-term answer is Redfish `EjectPolicy: Persistent`, not
  a config knob.
- **Encrypting stored credentials** to allow authenticated remote persistence.
  Rejected (for now): key management on the BMC is out of scope and conflicts
  with G3; local `file://` sources cover the platform-image use case.
- **URL-embedded credentials** (`https://user:pass@...`). Rejected: violates
  G3; must fail closed in `isPersistableUrl()` / `writeState()`.
- **Blocking, synchronous replay in `App`'s constructor.** Rejected: it would
  stall service start on slow/absent networks. Posting onto the event loop and
  driving each mount on a coroutine keeps start-up non-blocking and enables the
  retry loop.

## 10. Future work

- Optional credential persistence backed by a BMC secret store, gated per slot.
- Configurable retry/back-off (currently compile-time constants).
- A Redfish surface to inspect which slots are persisted and their last replay
  outcome.
- Gate remote (`https://` / `smb://`) persistence on Redfish
  `EjectPolicy: Persistent` in bmcweb (persisted via
  `phosphor-settings-manager`, same pattern as `PowerRestorePolicy`).
  `AllowedLocalMountPoints` remains the Phase 1 opt-in for local `file://`.
- With remote persistence, extend startup readiness to remote sources: SMB's
  blocking cifs `mount(2)` and HTTPS's `nbdkit` + curl both need the network up
  when replay runs, so retry must ride out DHCP, DNS and slow-server delays.

## Appendix A — API reference

Persistence symbols live in `namespace persistence` (`src/persistence.hpp`):

| Symbol                                | Purpose                                                 |
| ------------------------------------- | ------------------------------------------------------- |
| `writeState(slot, imageUrl, rw)`    | Durably save a descriptor (best-effort).                |
| `clearState(slot)`                  | Remove a descriptor.                                    |
| `loadState(slot)`                   | Read + validate one descriptor (self-heals on failure). |
| `enumerateSavedSlots()`             | Return all valid descriptors at start-up.               |
| `isPersistableUrl(url)`             | True for `file://` without userinfo.                    |
| `setStateDir(path)`                 | Redirect the state directory (tests).                   |
| `SavedMount { slot, imageUrl, rw }` | POD returned by load/enumerate.                         |

Shared URL-scheme helpers live in `namespace utils` (`src/utils.hpp`) and are
used directly by both the FSM and persistence:

| Symbol                                 | Purpose                                                          |
| -------------------------------------- | ---------------------------------------------------------------- |
| `matchesUrlScheme(scheme, url)`      | Case-sensitive scheme-prefix check.                              |
| `isFileUrl / isHttpsUrl / isCifsUrl` | URL scheme predicates (`file://` / `https://` / `smb://`). |
| `hasEmbeddedUrlCredentials(url)`     | Detects `scheme://user[:pass]@host` userinfo.                   |

## Appendix B — Replay tuning constants

Defined in `src/main.cpp` (`App`):

| Constant                | Default    |
| ----------------------- | ---------- |
| `kReplayMaxAttempts`  | `5`      |
| `kReplayRetryDelay`   | `5 s`    |
| `kReplaySettlePolls`  | `120`    |
| `kReplayPollInterval` | `100 ms` |
