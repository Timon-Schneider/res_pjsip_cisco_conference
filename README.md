# res_pjsip_cisco_conference

An Asterisk PJSIP module that makes the **hardware Conference button** on Cisco CP-8xxx IP phones work with FreePBX / Asterisk 21.

---

## The problem

When the user presses the physical Conference key, Cisco CP-8xxx phones (CP-8811, CP-8841, CP-8851, CP-8861 …) send a `REFER` request carrying a proprietary body:

```
REFER sip:pbx SIP/2.0
Content-Type: application/x-cisco-remotecc-request+xml

<?xml version="1.0" encoding="UTF-8"?>
<x-cisco-remotecc-request>
  <softkeyeventmsg>
    <softkeyevent>Conference</softkeyevent>
    <dialogid>
      <callid>…</callid>
      <localtag>…</localtag>
      <remotetag>…</remotetag>
    </dialogid>
    <consultdialogid>
      <callid>…</callid>
      <localtag>…</localtag>
      <remotetag>…</remotetag>
    </consultdialogid>
  </softkeyeventmsg>
</x-cisco-remotecc-request>
```

Asterisk has no built-in handler for this content type and responds `501 Not Implemented`, leaving the conference button dead.

This module intercepts those `REFER` messages, uses the embedded SIP dialog identifiers to locate the three call legs already in memory, and asynchronously redirects all three into a `ConfBridge` room.

---

## Requirements

| Component | Version tested |
|---|---|
| OS | Debian / Ubuntu (FreePBX 17 ISO) |
| FreePBX | 17 |
| Asterisk | 21.5.0 |
| Asterisk source (headers only) | 21.12.2 |
| GCC | 12+ (system default) |
| Cisco phones | CP-8811 |

---

## Installation

### Step 1 — Install build tools

```bash
apt-get update
apt-get install -y gcc make wget tar \
    libssl-dev libncurses5-dev uuid-dev \
    libjansson-dev libxml2-dev libsqlite3-dev \
    libedit-dev binutils
```

### Step 2 — Download the Asterisk source tree

The source tree is needed **for headers only** — you do not recompile Asterisk.
Use the closest available version to what FreePBX installed (21.12.2 works fine with 21.5.0):

```bash
cd /usr/src
wget https://downloads.asterisk.org/pub/telephony/asterisk/asterisk-21.12.2.tar.gz
tar xzf asterisk-21.12.2.tar.gz
```

Run `./configure` to generate `autoconfig.h` and unpack the bundled pjproject headers:

```bash
cd /usr/src/asterisk-21.12.2
./configure --with-pjproject-bundled
```

> You do **not** need to run `make`.

### Step 3 — Create `buildopts.h`

Asterisk enforces a build-option checksum between a module and the running binary.
Extract the checksum from the already-installed `res_pjsip.so`:

Write the header:

```bash
BUILDSUM=$(strings /usr/lib/x86_64-linux-gnu/asterisk/modules/res_pjsip.so \
    | grep -E "^[a-f0-9]{32}$" | head -1)
echo "Found checksum: $BUILDSUM"

cat > /usr/src/asterisk-21.12.2/include/asterisk/buildopts.h <<EOF
#ifndef _ASTERISK_BUILDOPTS_H
#define _ASTERISK_BUILDOPTS_H

#if defined(HAVE_COMPILER_ATTRIBUTE_WEAKREF)
#define __ref_undefined __attribute__((weakref));
#else
#define __ref_undefined ;
#endif

#define AST_BUILDOPT_SUM "${BUILDSUM}"

#endif /* _ASTERISK_BUILDOPTS_H */
EOF
```

Verify:

```bash
cat /usr/src/asterisk-21.12.2/include/asterisk/buildopts.h
```

### Step 4 — Copy the source file

```bash
cp res_pjsip_cisco_conference.c /usr/src/asterisk-21.12.2/res/
```

OR create the source file:
```bash
nano /usr/src/asterisk-21.12.2/res/res_pjsip_cisco_conference.c
```

### Step 5 — Compile

```bash
ASTSRC=/usr/src/asterisk-21.12.2
MODDIR=/usr/lib/x86_64-linux-gnu/asterisk/modules
PJROOT=${ASTSRC}/third-party/pjproject/source

gcc -fPIC -shared -g -O2 \
  -DASTERISK_REGISTER_FILE \
  -D_GNU_SOURCE \
  -DAST_MODULE_SELF_SYM=__local_ast_module_self \
  -DAST_MODULE=\"res_pjsip_cisco_conference\" \
  -I${ASTSRC}/include \
  -I${PJROOT}/pjsip/include \
  -I${PJROOT}/pjlib/include \
  -I${PJROOT}/pjlib-util/include \
  -I${PJROOT}/pjmedia/include \
  -I${PJROOT}/pjnath/include \
  -o ${MODDIR}/res_pjsip_cisco_conference.so \
  ${ASTSRC}/res/res_pjsip_cisco_conference.c \
  && echo "COMPILE OK"
```

A successful build prints `COMPILE OK` and may produce a few harmless warnings. No errors.

To reload the module immediately after a successful compile:

```bash
asterisk -rx "module unload res_pjsip_cisco_conference.so"
asterisk -rx "module load res_pjsip_cisco_conference.so"
```

### Step 6 — Load the module

```bash
asterisk -rx "module load res_pjsip_cisco_conference.so"
asterisk -rx "module show like cisco"
```

Expected output:

```
Module                             Description                              Use Count  Status      Support Level
res_pjsip_cisco_conference.so      Cisco x-cisco-remotecc Conference …      0          Running     extended
```

#### Auto-load on restart

> **Do not edit `/etc/asterisk/modules.conf` directly** — FreePBX regenerates it automatically and will overwrite any changes.

FreePBX's `modules.conf` uses `autoload=yes` by default, which means every `.so` placed in the modules directory loads automatically on startup. No further configuration is needed.

### Step 7 — Dialplan

Add to `/etc/asterisk/extensions_custom.conf`:

```ini
[cisco-conference]
exten => s,1,NoOp(Cisco Conference Room: ${CISCO_CONF_ROOM})
 same => n,Set(CONFBRIDGE(user,announce_only_user)=no)
 same => n,ConfBridge(${CISCO_CONF_ROOM},,,)
 same => n,Hangup()
```

Reload the dialplan:

```bash
asterisk -rx "dialplan reload"
```

Verify the context loaded — **if this shows nothing, the conference will not work**:

```bash
asterisk -rx "dialplan show cisco-conference"
```

Expected output:
```
[ Context 'cisco-conference' created by 'pbx_config' ]
  's' =>            1. NoOp(Cisco Conference Room: ${CISCO_CONF_ROOM}) [extensions_custom.conf:XX]
                    2. Set(CONFBRIDGE(user,announce_only_user)=no) [extensions_custom.conf:XX]
                    3. ConfBridge(${CISCO_CONF_ROOM},,,)          [extensions_custom.conf:XX]
                    4. Hangup()                                   [extensions_custom.conf:XX]
```

---

## How it works

```
Step 1: Phone 200 and Phone 220 are in a call.
        Phone 220 presses Conference (first press) — 200 is put on hold, 220 gets dial tone.

Step 2: Phone 220 dials Phone 210 (the consult call). 220 and 210 talk.

Step 3: Phone 220 presses Conference again (second press) — this triggers the REFER:

  Phone 220 → REFER  Content-Type: application/x-cisco-remotecc-request+xml
           ← 202 Accepted

Module (in the PJSIP receive thread):
  1. Checks Content-Type with pj_stricmp2() — ignores anything else
  2. Reads <softkeyevent>: non-Conference events get 200 OK and are discarded
  3. Parses <dialogid>       → callid / localtag / remotetag  (held dialog)
  4. Parses <consultdialogid>→ callid / localtag / remotetag  (consult dialog)
  5. Calls pjsip_ua_find_dialog() to locate each PJSIP dialog in memory
  6. Gets the Asterisk session/channel from each dialog via ast_sip_dialog_get_session()
  7. Calls ast_channel_bridge_peer() to find the bridge peers:
       ch_first  → the outgoing leg (Asterisk→220) for the held call  [to be dropped]
       ch_held   → bridge peer of ch_first  = PJSIP/200  (held party)
       ch_second → the incoming leg (220→Asterisk) for the consult call
       ch_consult→ bridge peer of ch_second = PJSIP/210  (consult party)
  8. Copies channel names + room into a cc_task struct
  9. Sends 202 Accepted
 10. Spawns a detached pthread

Worker thread (300 ms after 202 is sent):
  1. Re-fetches channels by name (ast_channel_get_by_name)
  2. Sets CISCO_CONF_ROOM on all three active channels
  3. ast_async_goto(ch_consult → cisco-conference)  ┐ no sleep
     ast_async_goto(ch_phone   → cisco-conference)  ┘ between these
     ast_async_goto(ch_held    → cisco-conference)
  4. 300 ms pause, then ast_softhangup(ch_drop)

All three phones enter ConfBridge(cc<timestamp>) and hear each other.
```

### Why the async_goto order matters

`ch_phone` (220) and `ch_consult` (210) are linked by a `Dial()` application.
If either leaves the bridge, `Dial()` hangs up the other — **unless** both already
have `AST_FLAG_ASYNC_GOTO` set.  By calling `ast_async_goto()` on `ch_consult`
then `ch_phone` with **no sleep between them**, both flags are set before any
Asterisk thread has a chance to run `Dial()`'s cleanup code.

### Why the module runs at priority 31 (APPLICATION − 1)

`res_pjsip_refer` registers at `PJSIP_MOD_PRIORITY_APPLICATION` (32).
This module registers at 31, so it sees the proprietary `REFER` first and
returns `PJ_TRUE` (consumed), preventing `res_pjsip_refer` from ever seeing it
and issuing `501 Not Implemented`.

---

## Troubleshooting

### Watch the live log

```bash
tail -f /var/log/asterisk/full | grep CiscoConf
```

A successful conference produces:

```
[…] NOTICE[…] res_pjsip_cisco_conference.c: CiscoConf: Conference REFER — dialog='<call-id>' consult='<call-id>'
[…] NOTICE[…] res_pjsip_cisco_conference.c: CiscoConf: room=cc1776259582 held=PJSIP/200-… phone=PJSIP/220-… consult=PJSIP/210-… drop=PJSIP/220-…
[…] NOTICE[…] res_pjsip_cisco_conference.c: CiscoConf: room='cc1776259582' held=PJSIP/200-… phone=PJSIP/220-… consult=PJSIP/210-…
```

### See the raw SIP REFER

```bash
asterisk -rx "pjsip set logger on"
# then attempt a conference and watch /var/log/asterisk/full
```

You should see the incoming `REFER` and the `SIP/2.0 202 Accepted` response.
If you see `501 Not Implemented` instead, the module is not loaded or not
registering at the right priority.

### Confirm the module is running

```bash
asterisk -rx "module show like cisco"
```

### One party drops out

This is almost always the `Dial()` race condition.  Check the log for which
channel did not receive its `async_goto`.  The critical requirement is that
`ast_async_goto(ch_consult, …)` and `ast_async_goto(ch_phone, …)` execute
back-to-back with **no `usleep` between them**.

---

### `buildopts.h` checksum mismatch

If Asterisk refuses to load the module with a checksum error, re-extract:

```bash
strings /usr/lib/x86_64-linux-gnu/asterisk/modules/res_pjsip.so \
    | grep -E '^[a-f0-9]{32}$' | head -1
```

Update `buildopts.h` and recompile.

---

### Channels sent to `invalid extension` / `cisco-conference,s,1`

```
WARNING: Channel '…' sent to invalid extension but no invalid handler: context,exten,priority=cisco-conference,s,1
```

The `[cisco-conference]` dialplan context is missing or was not reloaded after being added.
All three channels immediately hit their hangup handlers and the call tears down.

Fix: add the context to `/etc/asterisk/extensions_custom.conf` (Step 7) if not already done,
then reload and verify:

```bash
asterisk -rx "dialplan reload"
asterisk -rx "dialplan show cisco-conference"
```

---

### `autoconfig.h: No such file or directory` during compile

`autoconfig.h` is generated by `./configure` and is **never shipped** with the source tarball.
This error means `./configure` was not run, or it failed before producing the file.

Fix:

```bash
cd /usr/src/asterisk-21.12.2
# Make sure required libraries are installed first (Step 1)
./configure --with-pjproject-bundled
ls include/asterisk/autoconfig.h   # must exist before compiling
```

If `./configure` still fails, check its output for lines containing `error`, `not found`,
or `missing` and install the corresponding `-dev` packages.

---

### `pjsip.h: No such file or directory` during compile

`./configure` may not have fully unpacked pjproject.  Try:

```bash
cd /usr/src/asterisk-21.12.2
make third-party/pjproject/configure
```

Then recompile with all five `-I` pjproject paths as shown in Step 5.

---

### `dialog not found` in the log

The module could not match a PJSIP dialog to the Call-ID / tags from the XML.
This can happen if:
- The call was already torn down by the time the module processed the REFER
- The phone sent swapped `localtag`/`remotetag` values (the code tries both
  orderings automatically, so this should be rare)

Enable full PJSIP logging (`pjsip set logger on`) and compare the Call-ID and
tags in the raw REFER body against what `pjsip show channels` reports.

---

