# ipagsi.c - what is in `out/`, how to run it, what is verified and what is open

Date 2026-11-06. `out/phone/wifi/` is the deliverable: the 9f2f0c8 sources plus the new
`ipagsi.c` and its hook-ups. `repo/phone/` is the untouched baseline; `out/ipagsi.diff` is the
unified diff between them (1267 added lines, 17 removed - almost all of it `ipagsi.c`).

Nothing here was compiled into a phone image or run on the phone: the two `ipagsi.c` checks below
are `gcc -fsyntax-only` on Linux, which validates the C but not the register values.

## Files touched

| file | change |
|---|---|
| `phone/wifi/ipagsi.c` | **new**, ~1080 lines: the GSI v4.5 / IPA v4.7 AP side described below |
| `phone/wifi/ipagsi.h` | the interface (unchanged) plus the inline QMAP helpers `qmap_frame()` / `qmap_deagg()` / `struct qmap_stats`, and the state strings `ipagsi_state()` returns |
| `phone/wifi/cell.c` | `x1hold=1`, `ipadata=1`, `ipacmd=1` option words; fills `struct ipagsi_call` from WDS Get Current Settings (0x2d) and calls `ipagsi_start()`; `ipagsi_stop()` when the call or the option goes away; `ipadata=<state>` in `cell.status` |
| `phone/wifi/wland.c` | `ipagsi_poll()` at the end of `net_idle()` |
| `phone/wifi/ipnet.c/.h` | a second, independent utun API: `ipnet_utun_open()` / `ipnet_utun_set()` (the WLAN's `tun_open()`/`tun_configure()` are untouched) |
| `phone/wifi/build.sh` | `ipagsi.c` added to the `ios-cc` list |

## The staged plan (`IPAGSI_PHASE`)

`IPAGSI_PHASE` is wrapped in `#ifndef`, so the compiler line picks the phase - no editing needed:

```
ios-cc ... -DIPAGSI_PHASE=2 ipagsi.c ...      # phase 2
```

**Phase 0 (`ipaphase=0` in cell.on, run time, any build).** The ws-n-ipa-plumb test merged in
(2026-11-06): no GSI or IPA access at all; ipagsi only opens and configures the utun and counts and
drops what the stack sends to it (`phase 0 utunN: tx ... pkts` every 10 s). Proven on the phone at
ws-n-ipa-plumb a5f1e36 (r5au1). Use it as the fallback when a phase 1/2 build misbehaves.

Two cell.c changes came in with it: ipagsi is only started when this boot's `ipa=1` bring-up had
INIT_DRIVER accepted (`ipa_ok`), and with `ipadata=1` the `x1hold=1` hold lasts only while
ipagsi is running, so a failed setup releases the call as described below (`x1hold=1` without
`ipadata=1` still holds the call by itself).

**Phase 1 (default, the one to run first).** GSI only. Check GSI (`GSI_STATUS`, `HW_PARAM_2`, the
current state of channels 0-7 and event rings 0-3), allocate + program + start event rings 2 and 3
and channels 2 (AP_MODEM_TX) and 7 (AP_MODEM_RX), allocate event ring 0 / channel 5 only when
`ipacmd=1`, pre-post 32 x 8192-byte RX buffers on channel 7 and ring its doorbell, open and
configure the utun, and dump the IPA registers we intend to use later. *No IPA register is
written, no immediate command is sent.* Packets cannot flow yet - the IPA still points endpoints 2
and 16 at the modem's own channels - but the only writes are to EE0 rings, channels and memory
that belong to us, so the modem's setup is not disturbed. The dump also runs in phase 2.

**Phase 2 (`-DIPAGSI_PHASE=2`).** Additionally reads/verifies the endpoint blocks 2, 16 (and 7 with
`ipacmd=1`) against what the modem programmed, warns about anything that would stop data flowing,
and starts all the channels. `IPAGSI_SET_AGGR` / `IPAGSI_SET_SEQ` / `IPAGSI_SET_CTRL` turn
individual endpoint registers into writes once their values are known good; while they are 0,
`ep_rmw()` logs `would be old -> new, left alone`. The channel-5 immediate commands (spec step 5)
are still **not** written: `ipagsi_tables()` logs that and leaves the modem's tables alone.

## How to run it (phone side, per the usual flow: commit, agent `build userland`, `files put`, restart wland)

`/var/local/cell.on`:

```
wds=1 datatest=1 ipadata=1            # the data path; implies wds=1 datatest=1
ipacmd=1                              # also allocate channel 5 + event ring 0 (table init is not run)
x1hold=1                              # keep the call up while ipagsi is running
```

`ipadata=1` implies `wds=1 datatest=1`; `ipacmd=1` implies `ipadata=1`. `ipagsi_start()` is driven
only by WDS Get Current Settings succeeding, so `wds=1` alone stays on the old datatest path.

What to expect in the log, in order: `GSI status ... hw_param_2 ...`, one `ch N cntxt0 ... state`
line for channels 0-7, `evt ring N pa ... cnt ... ok`, `ch N pa ... cnt ... evt ... tx|rx ok`,
`utun utunN up: ip ... gw ... mask ... mtu ...`, then the IPA dump (`ipa flavor ...`, one block per
endpoint, `src/dst rsrc grp0`, per-endpoint filter/router hash config), then `up (phase 1,
tables 0)` and a counters line every 10 s. `cell.status` shows `ipadata=up` (or `setup-N`,
`failed-N`, `stopped`). A failure at step N leaves the state at `failed-N` and releases the call
hold, because `ipagsi_running()` is only true when the state is `up`.

One setup attempt per wland run: `ipagsi_stop()` closes the polling and the utun but does not reset
GSI or the IPA, so a second `ipagsi_start()` in the same process is refused with a log. Restart
wland to try again.

After phase 1, read the dump and answer the open items below, then switch to phase 2.

## Verified - where the values come from

- Every GSI offset, field and opcode is from mainline `drivers/net/ipa/reg/gsi_reg-v4.5.c`
  (`CH_C_CNTXT_0` 0x0000f000 + 0x80*ch, `CH_C_QOS` 0xf05c, `CH_C_SCRATCH_0..3` 0xf060/64/68/6c,
  `GSI_STATUS` 0x12000, `CH_CMD` 0x12008, `EV_CH_CMD` 0x12010, `GENERIC_CMD` 0x12018,
  `HW_PARAM_2` 0x12040, `EV_CH_E_CNTXT_0` 0x10000 + 0x80*ev, doorbells 0x11000 / 0x11100) and
  `drivers/net/ipa/gsi.c` (both 5.15 and 6.7 checked). `GSI_EE_AP` is 0, so the `0x4000 * ee` term
  drops out and these are the EE0 offsets we use. GSI's register window is at physical
  `0x1e04000`, IPA's at `0x1e40000`.
- Order and semantics straight from `gsi.c`: program the context, then `ALLOCATE`, then ring the
  doorbell with the **address** of the last ring element (`gsi_channel_doorbell`,
  `gsi_evt_ring_doorbell`, 5.15); event-ring doorbell = the last element consumed,
  `((rp - 1) % cnt)`. QOS: `WRR_WEIGHT` 0 for data channels, field maximum 7 for the command
  channel, `MAX_PREFETCH` left clear, no doorbell engine from v4.0 on,
  `PREFETCH_MODE = GSI_ESCAPE_BUF_ONLY` (= 1, bits 13:10) on all but the command channel. Event
  ring moderation: `EV_MODT` 32 with `EV_MODC` 1 (mainline's `GSI_EVT_RING_INT_MODT`), `EV_MOD_CNT`
  0, MSI address and read-pointer-update registers 0. GPI scratch:
  `max_outstanding_tre = trans_tre_max * 16` (= `tlv_count * 16`) in `SCRATCH_2` 31:16,
  `outstanding_threshold = 2 * 16` in `SCRATCH_3` 31:16 with the low half kept.
- TRE layout from `gsi_trans.c`: 16 bytes - `addr` (8), **`len` u16 + `resv` u16**, `flags` u32 =
  entry type in 23:16 (`GSI_RE_XFER` 0, `GSI_RE_IMMD_CMD` 3) plus bit 0 CHAIN, bit 9 IEOT,
  bit 10 BEI. Every transaction we post is one TRE, so IEOT is always set and CHAIN never used.
  The endpoint a transfer belongs to comes from the channel, never from the TRE; `TLV_CMDTX` 20 /
  `TLV_MDM_TX` 16 / `TLV_MDM_RX` 9 are the TLV FIFO sizes from `ipa_data-v4.7.c`, used only to set
  `max_outstanding_tre`.
- IPA endpoint block offsets/fields from mainline `reg/ipa_reg-v4.7.c`: one 0x70-wide block per
  endpoint from `IPA_BASE + 0x0800` - ctrl 0x00, cfg 0x08, nat 0x0c, hdr 0x10, hdr_ext 0x14,
  hdr_mask 0x18, mode 0x20 (`ENDP_MODE` 2:0, `DEST_PIPE_INDEX` 8:4), aggr 0x24 (`AGGR_EN` 1:0,
  `BYTE_LIMIT` 10:5, `TIME_LIMIT` 16:12, `PKT_LIMIT` 22:17, `SW_EOF_ACTIVE` 23, `AGGR_GRAN_SEL`
  27), hol 0x2c/0x30, deaggr 0x34, rsrc 0x38, seq 0x3c, `ENDP_STATUS` 0x40, filter/router hash
  config 0x5c. Plus `LOCAL_PKT_PROC_CNTXT` 0x1e8 and `FLAVOR_0` 0x210.
- **There is no endpoint -> channel binding register to write.** mainline's `ipa_data-v4.7.c` pairs
  each AP endpoint with a fixed EE0 channel (7<->5, 2<->2, 16<->7) and never programs a binding;
  the pairing belongs to the IPA/EE. The earlier `0x8000 + 4*ep` guess and the `IPAGSI_HAVE_SETUP`
  switch built on it are gone, so nothing writes to an unverified IPA address.
- Hardware facts already established on this phone (unchanged): IPA base 0x1e40000, GSI EE0 at
  0x1e04000, IPA local memory window at 0x1e50000 (base 0, size 0x3000), `LOCAL_PKT_PROC_CNTXT`
  = 0xad0, IPA streams 0x520/0x522 run with translation off on SMMU CB8/CB9, so the addresses
  `pm_alloc()` returns are physical and go straight into rings and TREs; INIT_DRIVER accepted
  (modem ctrl_ep 14, default_ep 14); DPM port {embedded 4, iface 1, rx-ep 2, tx-ep 16}; WDA raw
  IP + QMAP v1 both directions, DL 16 datagrams / 0x3800; mux 1; QMAP header 4 bytes.
- `ipagsi.c` passes `gcc -fsyntax-only -Wall -Wextra` at both `IPAGSI_PHASE=1` and `=2` (Linux,
  which is as far as it can be checked here - `cell.c`/`wland.c` need macOS headers, so they can't
  be syntax-checked this way, and no phone build was attempted).

## Open - answer these from the phase 1 dump before phase 2

1. **Do the modem's AP channels release?** the dump's `ch N state` lines must show channels 2 and 7
   (and 5 with `ipacmd=1`) in `not-alloc` on EE0, and event rings 0/2/3 `not-alloc`. If they are
   still `started`, the IPA/modem owns them and nothing else here makes sense - stop and look at
   the modem's IPA QMI teardown instead.
2. **Endpoint 16 aggregation**: `AGGR_EN`, `BYTE_LIMIT` units (`AGGR_GRAN_SEL` clear means KB, but
   v4.7's units are not confirmed), `TIME_LIMIT` is a Qtime tick count whose encoding
   mainline works out with `ipa_qtime_val()` - not reproduced here. The dump logs whether the byte
   limit matches our 8192-byte RX buffers; only turn `IPAGSI_SET_AGGR` on once this is known.
3. **Endpoint 2's status register** (`ENDP_STATUS` 0x40): does the modem send status to ep 14, and
   does that matter for our TX? Sequence type 4 (`IPA_SEQ_2_PASS_SKIP_LAST_UC`) is what mainline
   uses; the dump shows what is set.
4. **Are the modem's tables enough?** `ipagsi_tables()` does nothing yet: spec step 5 (header /
   system / filter / route init by immediate command on channel 5) is unimplemented, so we depend
   on the modem's tables as they are. That needs an answer: if the AP really must own the tables,
   the modem's current local-memory tables (`ipaqmi.c`'s `ipa47_mem[]`: v4 filter 0x288/0x308,
   v6 0x388/0x408, v4 route 0x488/0x508, v6 0x588/0x608, modem hdr 0x688, proc ctx 0xad0) have to
   be copied to DDR first, then the commit/imm-TRE commands written on channel 5.
5. **Does `INIT_MODEM_DRIVER_CMPLT` (0x35) arrive after our AP endpoints are re-initialised**, and
   is `IPA_ENABLE` involved anywhere (mainline 6.7 enables it inside its own setup)?
6. **TX carries IPv4 only.** The WDS 0x20 request used PDP type 1 and no IPv6 endpoint exists, so
   `tx_pump()` drops non-AF_INET utun packets and counts them (`other-af`); RX hands both AFs to
   the utun. If IPv6 shows up, the modem side needs its own endpoint/format.
7. **Ring counts / moderation**: event rings are 64 elements with mainline's moderation setting
   (`EV_MODT` 32, `EV_MODC` 1), channel rings 256/512/256 TREs; TX uses a 32-buffer ring, RX 32
   buffers pre-posted with one TRE each. These are our choices, not verified against the modem's -
   watch the counters line for a stuck doorbell (`ch7 wp` vs `done`).

## Also still to look at (software side)

- `ipagsi_stop()` deliberately does not reset GSI or the IPA, hence one attempt per wland run.
  Proper teardown (channel STOP/RESET/DE_ALLOC, event ring RESET, `pm_free`) is not implemented.
- `qmap_deagg()` only walks frames; it does not handle a frame split across two RX buffers
  (aggregation closes on the packet limit, so it should not happen, but nothing reassembles it).
- The RX path assumes completions come back in TRE order (`ctx.rx_done % RX_BUFS`), which GPI does
  for a single channel - worth confirming from the first run's counters.
