
# UET reference device — register map

Everything the `uet_ref` driver and its provider can address on the device,
BAR by BAR.

The authoritative definitions are in `uet_dev_abi.h`, beside this file; this
document explains them. If the two disagree, the header is right and this
file is stale.

This document says what each register is and who owns it.
[`VFIO_MEMORY_MODEL.md`](VFIO_MEMORY_MODEL.md) says what an access to one
actually guarantees - where vfio-user is ordered more strictly than PCIe,
where it is weaker, and what this environment therefore cannot catch. Read
it before trusting any result about ordering.

It lives with the device model rather than with the driver because the
device *defines* this interface and the driver merely consumes it - and
because `kmod/` is what becomes an upstream kernel driver, where an
emulated device's BAR layout does not belong.

**Device ABI 1.0.** The driver checks it at probe: the major must match
exactly and the minor must be at least the driver's. A device older than the
driver is refused at load rather than left to misbehave later.

---

<!-- TOC -->

## Table of Contents

- [1. Identity and BAR summary](#1-identity-and-bar-summary)
- [2. BAR0 — status region](#2-bar0--status-region)
  - [2.1 `MAX_PAYLOAD` (0x04c) and `MAX_MSG_SIZE` (0x068)](#21-max_payload-0x04c-and-max_msg_size-0x068)
  - [2.2 `GID_TYPE` (0x06c)](#22-gid_type-0x06c)
  - [2.3 `CAPS` (0x008) bit layout](#23-caps-0x008-bit-layout)
  - [2.4 `IPADDR` (0x09c) — not an integer](#24-ipaddr-0x09c--not-an-integer)
  - [2.5 The 64-bit counter at 0x104/0x108](#25-the-64-bit-counter-at-0x1040x108)
- [3. BAR1 — MSI-X](#3-bar1--msi-x)
  - [3.1 Vector assignment](#31-vector-assignment)
- [4. BAR2 — control region](#4-bar2--control-region)
  - [4.1 L2 rings](#41-l2-rings)
  - [4.2 Admin queue](#42-admin-queue)
- [5. BAR3 — user doorbell pages](#5-bar3--user-doorbell-pages)
  - [5.1 Within one page](#51-within-one-page)
  - [5.2 Doorbell values](#52-doorbell-values)
  - [5.3 The ownership check](#53-the-ownership-check)
- [6. Ownership and threading](#6-ownership-and-threading)
  - [6.1 A limit worth knowing](#61-a-limit-worth-knowing)
- [7. Limits](#7-limits)
- [8. Reading them from the guest](#8-reading-them-from-the-guest)
  - [8.1 What this is for](#81-what-this-is-for)

<!-- /TOC -->


## 1. Identity and BAR summary

```
                    PCI 14e4:4242   (Broadcom)
                    PCI Express capability
                    MSI-X, 4 vectors

  ┌──────┬─────────┬──────────┬──────────────┬───────────────────────────┐
  │ BAR  │  Size   │  Access  │  Written by  │ Contents                  │
  ├──────┼─────────┼──────────┼──────────────┼───────────────────────────┤
  │  0   │  4 KiB  │ read     │      —       │ identity, config, counters│
  │  1   │  4 KiB  │ read/wr  │  PCI core    │ MSI-X table and PBA       │
  │  2   │  4 KiB  │ write    │  kernel only │ ring config, doorbells,   │
  │      │         │          │              │ admin queue               │
  │  3   │ 256 KiB │ write    │  userspace   │ 64 doorbell pages, one    │
  │      │         │          │              │ per ucontext              │
  │ 4,5  │    —    │    —     │      —       │ not implemented           │
  └──────┴─────────┴──────────┴──────────────┴───────────────────────────┘
```

**Why BAR2 and BAR3 are separate.** BAR2 carries the admin queue and the L2
rings, and is only ever written by the kernel. A doorbell has to be
reachable from an unprivileged process, and mapping BAR2 to reach one would
hand that process the admin queue as well. So user doorbells get a region
whose only content is doorbells.

```
   guest kernel                          guest userspace
   (uet_ref.ko)                          (libuet_ref provider)
        │                                        │
        │ ioremap                                │ mmap one page
        ▼                                        ▼
   ┌─────────┬─────────┐                  ┌──────────────────┐
   │  BAR0   │  BAR2   │                  │ BAR3 page N      │
   │ status  │ control │                  │ SQ / RQ / CQ dbs │
   └─────────┴─────────┘                  └──────────────────┘
        read     write                       write, one page
                                             per ucontext
```

---

## 2. BAR0 — status region

**4 KiB, read only.** A write is not an error — a driver may legitimately
probe — but it changes nothing and is counted at `0x114` so it shows up
rather than vanishing.

All registers are 32-bit little endian unless noted.

```
 offset
       ┌────────────────────────────────────────────┐
 0x000 │ MAGIC          0x55455444  "UETD"          │ identity
 0x004 │ ABI_VERSION    major<<16 | minor           │
 0x008 │ CAPS           bitmap, see below           │
 0x00c │ STATE          0 init / 1 ready / 2 error  │
       ├────────────────────────────────────────────┤
 0x040 │ PDS_MODE       0 sng / 1 pds               │ how the instance
 0x044 │ SEC_MODE       0 none 1 direct 2 cluster   │ was actually built
       │                3 server                    │ — from the env
 0x048 │ NIC_SHIM       0 rawsock / 1 xdp           │   vars, not from
 0x04c │ MAX_PAYLOAD    bytes, one packet           │   anything the
 0x050 │ PORT_PROTO     udp port 15:0, proto 23:16  │   guest chose
 0x054 │ MAX_TX_RETRIES                             │
 0x058 │ TX_TIMEOUT     milliseconds                │
 0x05c │ PKT_DROP_THRESH                            │
 0x060 │ SEC_SSI        shared secret in use        │
 0x064 │ IOV_LIMIT      segments the library allows │
 0x068 │ MAX_MSG_SIZE   bytes, one message not one  │
       │                packet — see below          │
 0x06c │ GID_TYPE       0 UET UDP / 1 UET IP /      │
       │                2 UET UFH — see below       │
       ├────────────────────────────────────────────┤
 0x080 │ IFNAME         16 bytes, nul padded ascii  │ interface identity
 0x090 │ MAC_LO         mac[0..3]                   │
 0x094 │ MAC_HI         mac[4..5] in bits 15:0      │
 0x098 │ IP_VER         4 or 6                      │
 0x09c │ IPADDR         16 bytes, address order     │
       ├────────────────────────────────────────────┤
 0x100 │ PROGRESS_ALIVE                             │ live counters —
 0x104 │ PROGRESS_ITERS_LO                          │ these change while
 0x108 │ PROGRESS_ITERS_HI                          │ the guest watches
 0x10c │ UPTIME_MS                                  │
 0x110 │ BAR0_READS                                 │
 0x114 │ BAR0_WRITES    writes ignored, counted     │
 0x118 │ DOORBELLS      user doorbell writes seen   │
 0x11c │ IRQS_FIRED     MSI-X triggers attempted    │
       ├────────────────────────────────────────────┤
 0x130 │ TX_CONS        device's TX consumer index  │ L2 rings —
 0x134 │ RX_CONS        device's RX consumer index  │ see §4.1
 0x138 │ TX_FRAMES      sent to the wire            │
 0x13c │ TX_ERRORS                                  │
 0x140 │ RX_FRAMES      delivered to the guest      │
 0x144 │ RX_DROPPED     no RX buffer was posted     │
 0x148 │ RX_UET_FRAMES  went to the uet instance    │
       ├────────────────────────────────────────────┤
 0x14c │ ADMIN_SQ_CONS  device finished to here     │ admin queue —
 0x150 │ ADMIN_CQ_PROD  device produced to here     │ see §4.2
 0x154 │ ADMIN_CMDS     commands completed          │
 0x158 │ ADMIN_ERRORS   status != OK, or no answer  │
 0x15c │ JOBS_LIVE                                  │
 0x160 │ JKEYS_LIVE                                 │
 0x164 │ MRS_LIVE                                   │
 0x168 │ QPS_LIVE                                   │
 0x16c │ CQS_LIVE                                   │
       ├────────────────────────────────────────────┤
 0x170 │ SQ_POSTED      work requests accepted      │ datapath —
 0x174 │ RQ_POSTED                                  │ see §5
 0x178 │ CQ_POSTED      completions written         │
 0x17c │ CQ_OVERRUNS    no room in the ring         │
 0x180 │ DB_REJECTED    doorbell, unowned handle    │
 0x184 │ QPS_CLOSING    destroyed, still draining   │
 0x188 │ SQ_ERRORS      send ring unreadable        │
 0x18c │ RQ_ERRORS      receive ring unreadable     │
 0x190 │ CQ_ERRORS      completion unwritable       │
 0x194 │ FLUSHED        abandoned on a retiring qp  │
       ├────────────────────────────────────────────┤
 0x198 │            unused, reads as zero           │
 0xfff └────────────────────────────────────────────┘
```

### 2.1 `MAX_PAYLOAD` (0x04c) and `MAX_MSG_SIZE` (0x068)

Two different quantities, and confusing them is easy.

`MAX_PAYLOAD` is what one **packet** carries, so it is what `max_mtu` and
`active_mtu` are derived from. `MAX_MSG_SIZE` is the largest **message** the
transport will accept, which it segments across as many packets as it needs;
it is what `ib_port_attr.max_msg_sz` wants. They differ by three orders of
magnitude.

`MAX_PAYLOAD` belongs to the instance and is revised once the library's
domain exists, so the register can never drift from the length the library
actually segments against. `MAX_MSG_SIZE` is fixed by the implementation and
is written once.

### 2.2 `GID_TYPE` (0x06c)

Which encapsulation the instance puts UET on the wire in. UET verbs
identifies the transport underneath the SES and PDS headers by GID type, and
an application selects the protocol by selecting a GID entry, so a driver
reporting a GID type needs this.

Read from the library, not assumed, for a reason the register makes easy to
miss: the receive path accepts more than the transmit path sends. This
implementation parses UET over UDP but has no UDP transmit path at all —
`uet_build_ipv4_hdr()` unconditionally writes `uet_ipproto` (253) — so the
answer today is `UET IP`, and `uet_tx_encap()` in `uet_api.c` is the one
place that changes when that stops being true.

The driver publishes the matching string as `gid_type` in its `uet`
attribute group; see the FIXME there and in `libibverbs/cmd_device.c`.

### 2.3 `CAPS` (0x008) bit layout

```
  31                            10  9   8   7   6   5   4   3   2   1   0
 ┌─────────────────────────────┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
 │           reserved          │RIJ│ RI│JOB│UNR│UUD│RDI│IMP│SEC│PRG│INS│
 └─────────────────────────────┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
```

| Bit | Name | Meaning |
|---|---|---|
| 0 | `INSTANCE` | a uet instance exists — the library came up |
| 1 | `PROGRESS_THREAD` | the progress thread is running |
| 2 | `SECURITY` | TSS enabled |
| 3 | `IMPAIRMENT` | impairment shim on (drops/delays transmits) |
| 4 | `FORCE_RUDI` | |
| 5 | `FORCE_UUD` | |
| 6 | `MR_UNRESTRICTED` | `{Device, PD}` regions supported |
| 7 | `MR_JOB_RESTRICTED` | `{Device, PD, JobID}` |
| 8 | `MR_RI_RESTRICTED` | `{Device, PD, QP}` |
| 9 | `MR_RI_JOB_RESTRICTED` | `{Device, PD, QP, JobID}` |

The four region bits exist so the driver can report what this device really
supports instead of hardcoding a guess. Device and driver are built and
loaded separately; a stale assumption here would have a provider offer a
restriction the device refuses.

### 2.4 `IPADDR` (0x09c) — not an integer

16 bytes in **address order**: byte 0 is the leading octet. A v4 address
uses the first four and leaves the rest zero. A driver can print these bytes
directly; treating them as a host-order integer gives the wrong answer.

```
  IPv4  10.99.0.1
  ┌────┬────┬────┬────┬────┬── ... ──┬────┐
  │ 10 │ 99 │  0 │  1 │ 00 │   ...   │ 00 │
  └────┴────┴────┴────┴────┴── ... ──┴────┘
    0    1    2    3    4              15
```

### 2.5 The 64-bit counter at 0x104/0x108

`PROGRESS_ITERS` is the only value split across two registers. It is written
low word first and is not atomic, so a reader that needs exactness must read
HI, LO, HI and retry if HI changed. Nothing does today; it is a liveness
indicator, and the low word alone is enough for that.

---

## 3. BAR1 — MSI-X

**4 KiB.** Standard layout, handled by the PCI core rather than by any
device logic of ours.

```
       ┌────────────────────────────────────────────┐
 0x000 │  MSI-X table    4 vectors × 16 bytes       │
       │                 = 64 bytes used            │
 0x800 │  PBA            pending bit array          │
 0xfff └────────────────────────────────────────────┘
```

### 3.1 Vector assignment

| Vector | Raised when | Consumer |
|---|---|---|
| 0 | L2 TX frames consumed | netdev transmit completion |
| 1 | L2 RX frames delivered | netdev receive |
| 2 | an admin command completed | admin queue |
| 3 | a completion was written to an **armed** completion queue | datapath |

Vector 3 is `UET_DEV_CQ_VECTOR_BASE`. A completion queue names its vector at
creation, and the driver reports `num_comp_vectors = 1`, so today every
queue names vector 3.

There were eight. Vector 0 raised on a write to a generic BAR2 doorbell that
existed to prove the doorbell-to-MSI-X path during bring-up, and 5-7 were
spare completion vectors nothing allocated. Both went when the datapath made
them redundant.

---

## 4. BAR2 — control region

**4 KiB, written by the kernel only.** Reads return zero — there is nothing
to read here, and a driver that wants state reads BAR0.

```
 offset
       ┌────────────────────────────────────────────┐
 0x000 │ TX_RING_BASE_LO                            │  §4.1
 0x004 │ TX_RING_BASE_HI                            │  L2 transmit ring
 0x008 │ TX_RING_ENTRIES                            │
 0x00c │ TX_RING_CTRL     bit 0 = ENABLE            │
 0x010 │ TX_PROD          ◄── doorbell              │
       ├────────────────────────────────────────────┤
 0x040 │ RX_RING_BASE_LO                            │  L2 receive ring
 0x044 │ RX_RING_BASE_HI                            │
 0x048 │ RX_RING_ENTRIES                            │
 0x04c │ RX_RING_CTRL     bit 0 = ENABLE            │
 0x050 │ RX_PROD          ◄── doorbell              │
       ├────────────────────────────────────────────┤
 0x080 │ ADMIN_SQ_BASE_LO                           │  §4.2
 0x084 │ ADMIN_SQ_BASE_HI                           │  command ring
 0x088 │ ADMIN_SQ_ENTRIES                           │
 0x08c │ ADMIN_SQ_CTRL    bit 0 = ENABLE            │
 0x090 │ ADMIN_SQ_PROD    ◄── doorbell              │
       ├────────────────────────────────────────────┤
 0x0a0 │ ADMIN_CQ_BASE_LO                           │  completion ring
 0x0a4 │ ADMIN_CQ_BASE_HI                           │
 0x0a8 │ ADMIN_CQ_ENTRIES                           │
 0x0ac │ ADMIN_CQ_CTRL    bit 0 = ENABLE            │
 0x0b0 │ ADMIN_CQ_CONS    ◄── doorbell              │
       ├────────────────────────────────────────────┤
 0x0b4 │             unused, writes ignored         │
 0xfff └────────────────────────────────────────────┘
```

**Writing a `_CTRL` register to 0 disables the ring and resets the device's
indices for it.** That is how a driver takes ring memory back, and it
matters to anyone driving the same ring twice in one process: without the
disable, the device's indices are still where the last user left them.

### 4.1 L2 rings

Two rings of `struct uet_dev_desc` carry ordinary Ethernet frames — ARP,
ICMP, ND, DHCP — between the guest's netdev and the wire. The device
demultiplexes its interface: UET traffic goes to the uet instance, and
everything else crosses these rings.

Rings must be **physically contiguous**, a power of two entries, and no more
than `UET_DEV_RING_MAX_ENTRIES` (1024).

```
  struct uet_dev_desc — 16 bytes, little endian

   0        8        12       16
   ┌────────┬────────┬────────┐
   │  addr  │  len   │ flags  │
   └────────┴────────┴────────┘
     u64      u32      u32

   addr   guest physical address of the frame buffer
   len    TX: frame length
          RX: buffer capacity in, received length out
   flags  bit 0  DONE   device finished the slot
          bit 1  ERR
```

#### 4.1.1 Index discipline

The same for both rings, and the whole of the ownership rule:

```
   guest owns ──► PROD          published by writing BAR2, that write
                                is the doorbell

   device owns ──► CONS         published in BAR0, then MSI-X

   a slot is the device's when   cons != prod
```

Indices are **free-running u32 counters, never wrapped**. The slot is
`index % entries`. Comparing them directly is therefore always safe; masking
before comparing is the classic way to get this wrong.

```
   TX                                     RX
   ─────────────────────────────          ─────────────────────────────
   guest fills addr + len                 guest posts empty buffers,
   writes TX_PROD  ────────────┐          len = capacity
                               │          writes RX_PROD  ───────────┐
   device reads the frame,     │                                     │
   writes it to the wire       │          device writes the frame    │
                               │          into addr, sets len to the │
   nothing is written back     │          real length, sets DONE     │
   to the descriptor —         │                                     │
   TX_CONS is the completion   │          then updates RX_CONS       │
                               ▼                                     ▼
                        MSI-X vector 0                      MSI-X vector 1
```

A frame larger than the posted buffer is **dropped and counted**
(`RX_DROPPED`), never truncated.

### 4.2 Admin queue

The slowpath: resource management from the driver. Jobs, job keys, address
tables, memory regions, completion queues, queue pairs, address handles.

**Two rings, not one.** The driver produces commands and consumes
responses; the device does the reverse. Neither side writes a ring the other
owns, which is both the shape a real completion queue has and the reason
ownership can be reasoned about at all.

#### 4.2.1 Registers

| Register | BAR | Offset | Written by | Meaning |
|---|---|---|---|---|
| `ADMIN_SQ_BASE_LO` / `_HI` | 2 | `0x080` / `0x084` | driver | command ring, bus address |
| `ADMIN_SQ_ENTRIES` | 2 | `0x088` | driver | depth: a power of two, 1 to 64 |
| `ADMIN_SQ_CTRL` | 2 | `0x08c` | driver | bit 0 `ENABLE`; writing 0 zeroes both command indices |
| `ADMIN_SQ_PROD` | 2 | `0x090` | driver | **doorbell**: commands produced up to here |
| `ADMIN_CQ_BASE_LO` / `_HI` | 2 | `0x0a0` / `0x0a4` | driver | completion ring, bus address |
| `ADMIN_CQ_ENTRIES` | 2 | `0x0a8` | driver | depth: a power of two, 1 to 64 |
| `ADMIN_CQ_CTRL` | 2 | `0x0ac` | driver | bit 0 `ENABLE`; writing 0 zeroes both completion indices |
| `ADMIN_CQ_CONS` | 2 | `0x0b0` | driver | **doorbell**: completions reclaimed up to here |
| `ADMIN_SQ_CONS` | 0 | `0x14c` | device | commands *finished* up to here, not merely read |
| `ADMIN_CQ_PROD` | 0 | `0x150` | device | completions written up to here |
| `ADMIN_CMDS` | 0 | `0x154` | device | commands that produced a completion, whatever its status |
| `ADMIN_ERRORS` | 0 | `0x158` | device | completions with status other than `OK`, plus commands lost without one |

Indices follow the same discipline as the L2 rings (§4.1): free-running
u32 counters, slot `index % entries`, never masked before comparing.

An `ENTRIES` value that is zero, not a power of two, or above
`UET_DEV_ADMIN_MAX_ENTRIES` is stored as zero, and a ring of depth zero
never runs. Nothing reports that: doorbells are accepted and every command
times out.

#### 4.2.2 Bring-up and teardown

```
  1. allocate both rings, physically contiguous and zeroed:
       commands     entries × 64 bytes
       completions  entries × 32 bytes
  2. write BASE_LO, BASE_HI and ENTRIES for each ring
  3. ADMIN_CQ_CTRL = ENABLE, then ADMIN_SQ_CTRL = ENABLE
```

The completion ring must start zeroed because `VALID` is how an entry's
arrival is detected; a slot has to read as empty before the device first
writes it.

The device drains only while both rings are enabled with a nonzero depth,
and it checks on every doorbell rather than at enable. A doorbell written
early is remembered and acted on at the *next* doorbell, not when the
second ring comes up. Enabling completions first is the driver's convention,
not something the device needs.

Teardown is the reverse: clear both `_CTRL` registers, then free the
memory. Clearing resets the device's indices to zero, so a driver that
re-enables must restart its own at zero as well.

#### 4.2.3 Formats

```
  struct uet_dev_admin_cmd — 64 bytes, little endian

   0      2      4      8            16
   ┌──────┬──────┬──────┬────────────┐
   │opcode│flags │ rsvd │   cookie   │
   └──────┴──────┴──────┴────────────┘
   16                               48
   ┌─────────────────────────────────┐
   │          param[0..3]            │   u64 each, opcode specific
   └─────────────────────────────────┘
   48          56       60       64
   ┌────────────┬────────┬────────┐
   │  buf_addr  │buf_len │ rsvd2  │
   └────────────┴────────┴────────┘

   opcode     UET_DEV_ADMIN_*
   flags      reserved: write 0. The device does not read it.
   cookie     the driver's; echoed back untouched. The device gives it no
              meaning, but a driver matches completions by it, so it must
              be unique among commands that could still be answered.
   param      small arguments, in place — see the command set
   buf_addr   out-of-line payload, or 0 — see below
   rsvd       write 0
```

```
  struct uet_dev_admin_cqe — 32 bytes, little endian

   0            8      10     12       16              32
   ┌────────────┬──────┬──────┬────────┬───────────────┐
   │   cookie   │opcode│flags │ status │  result[0..1] │
   └────────────┴──────┴──────┴────────┴───────────────┘

   cookie   copied from the command
   opcode   copied from the command, even one the device does not know
   flags    bit 0  VALID   the device has written this entry.
                           Set by the device, cleared by the driver
                           when it reclaims the slot.
   status   UET_DEV_ADMIN_ST_*
   result   opcode specific, and zero where the opcode defines none
```

Status values: `UET_DEV_ADMIN_ST_*` — `OK`, `EINVAL`, `ENOSPC`,
`EOPNOTSUPP`, `ETOOSMALL`, `EEXIST`, `ENOENT`, `EBUSY`.

#### 4.2.4 The out-of-line buffer

Anything that does not fit in four parameter words travels in a buffer the
command names. It must be one bus-address range inside memory the device
can reach; in practice, a coherent DMA allocation.

The direction belongs to the opcode: a query or `DEV_INFO` has the device
write it, a create or insert has the device read it. The device checks
`buf_len` against the structure it needs **before** touching the buffer:

- too short: `ETOOSMALL`, and `result[0]` holds the size it wanted
- the transfer fails: `EINVAL`
- otherwise it moves exactly the structure's size, however large `buf_len`
  is, and a command that moves a buffer and returns no handle reports the
  size in `result[0]`

The largest structure today is `QP_CREATE`'s at 104 bytes; the driver
allocates 256.

#### 4.2.5 One command, end to end

```
   driver                                        device
   ──────                                        ──────
 1 fill SQ[prod % entries]; any
   payload into the buffer
 2 wmb
 3 prod++; write ADMIN_SQ_PROD ── posted ──►  4 while SQ cons != prod
   the write returns at once:                     and the CQ has room:
   see "Doorbells are posted"                     read SQ[cons]
                                                  execute it
                                                  write CQ[cq_prod], VALID set
                                                  cq_prod++, cons++
                                               5 one MSI-X on vector 2,
                                                  if anything completed
 6 woken, or out of time:
   for each CQ[cq_cons] with VALID:
     read the entry after the flag
     keep it if the cookie is ours,
     drop it if not
     clear VALID; cq_cons++
 7 wmb; write ADMIN_CQ_CONS ───────────────►  8 those slots are free; drain
                                                  again, so commands held back
                                                  for want of room now run
```

The drain runs inside the handler for the doorbell write, on the vfio-user
thread, so steps 4 and 5 are finished before the device handles any later
message from the guest.

#### 4.2.6 Doorbells are posted

QEMU posts vfio-user writes to memory BARs: `hw/vfio/pci.c` marks every
BAR that is not I/O space for posted writes, so the write is queued on the
socket with no reply requested and the vCPU resumes at once. Reads are never
posted, and the server handles messages strictly in order, so a read of any
region cannot pass an earlier write. Config-space writes are not posted.
`-device vfio-user-pci,...,x-no-posted-writes=on` makes every write wait
for the server; `uet-pair-up` leaves it off.

For this queue:

- **When the write to `ADMIN_SQ_PROD` returns, the command has usually not
  run.** Looking at the completion ring straight after the doorbell normally
  finds nothing. A driver has to wait.
- **A BAR0 read is a flush.** Reading `ADMIN_SQ_CONS` after a doorbell
  returns the state after that doorbell's drain, because the drain runs in
  the write's handler and the read is handled after it.
- **Driver and device really do run at the same time**, so the order in
  which a completion entry becomes visible matters. See the known gap below.

That is the shape PCIe has — posted writes, and reads that push them — so a
driver written to it is correct on hardware. [`VFIO_MEMORY_MODEL.md`](VFIO_MEMORY_MODEL.md) §2 has the general case.

#### 4.2.7 Flow control

- **Completion ring full.** The device stops before reading the next
  command, leaving it queued; `ADMIN_SQ_CONS` stops short of the producer. A
  write to `ADMIN_CQ_CONS` restarts the drain. Nothing is lost and nothing
  is overwritten.
- **Command ring full.** The device does not check. The driver must keep
  `prod - ADMIN_SQ_CONS <= entries`; producing past that overwrites commands
  the device has not read.

The driver runs a depth of 8 with one command outstanding, so neither limit
is reached in normal operation.

#### 4.2.8 Interrupts

Vector 2, raised once per drain that wrote at least one completion — not
once per command. It is raised after the entries are in memory.

An interrupt is a hint to look, not an answer. It can cover several
completions, belong to a command the driver has given up on, or, if lost,
never arrive for an entry that is sitting in memory. A driver must look at
the ring before declaring a timeout.

#### 4.2.9 Failures

| What went wrong | Completion | `ADMIN_SQ_CONS` | Counted in |
|---|---|---|---|
| unknown opcode | `EOPNOTSUPP` | advances | `ADMIN_CMDS`, `ADMIN_ERRORS` |
| bad argument, missing or busy object | its status | advances | `ADMIN_CMDS`, `ADMIN_ERRORS` |
| buffer too short | `ETOOSMALL` | advances | `ADMIN_CMDS`, `ADMIN_ERRORS` |
| buffer transfer fails | `EINVAL` | advances | `ADMIN_CMDS`, `ADMIN_ERRORS` |
| the command cannot be read | **none** | advances | `ADMIN_ERRORS` |
| the completion cannot be written | **none**, and the command **did** run | advances | `ADMIN_ERRORS` |

The last two consume a command with no answer, so the driver sees only a
timeout. The last is the worse of them: a create that ran and lost its
completion leaves an object in the device the driver has no handle for.
Both need a ring outside memory the device can reach, so neither is
expected once bring-up has succeeded.

#### 4.2.10 What the device guarantees

- Commands run one at a time, in ring order, each finished before the next
  is read.
- `ADMIN_SQ_CONS` advances only once a command has run and its completion
  is written, or it has failed in one of the two ways above. So
  `ADMIN_SQ_CONS == prod` means the device holds no command and will not
  touch any payload buffer again.
- A completion is in memory before `ADMIN_CQ_PROD` or `ADMIN_SQ_CONS`
  reflects it, and before its interrupt.
- `cookie` and `opcode` come back as they went in.
- Commands are serialised against the datapath's sweep, so a destroy never
  races a request that is using the object.

**Known gap: `VALID` is not written last.** The model writes the whole
32-byte entry, flag included, in one copy — libvfio-user's direct DMA path
is a `memcpy` — and `flags` sits at byte 10, ahead of both `status` and
`result`. A driver reading while the copy is in progress can see
`VALID` and `status` before `result`. Whether that is observable depends on
how the host's `memcpy` splits 32 bytes. The fix is to write the entry with
`flags` zero, then a release barrier, then `flags` on its own.

#### 4.2.11 What the driver must do

`uet_ref_admin.c` does all of this; it is the reference for how.

1. **Match completions by cookie.** One command outstanding is the driver's
   intent, not something the ring enforces: a command that times out may
   still be answered later.
2. **Reclaim in ring order.** A valid entry that is not the one awaited is
   a late answer to an abandoned command. Drop it, and say so.
3. **Never clear a slot ahead of the consumer index.** A slot about to be
   used can hold a late completion not yet reclaimed; clearing it loses the
   device's place in the ring, and every later command then waits on a
   slot the device has already moved past.
4. **Clear `VALID`, barrier, then write `ADMIN_CQ_CONS`.** Otherwise the
   device can refill a slot the driver still sees as valid.
5. **Do not post while the device holds an earlier command.** Read
   `ADMIN_SQ_CONS` and compare it with the producer before reusing a command
   slot or the shared payload buffer. A late command would otherwise read a
   payload meant for the next one, or write its answer over it.
6. **On timeout, look once more before giving up.** A lost interrupt is not
   a lost completion.
7. **Expect a timed-out command to run anyway.** A destroy that timed out
   may still happen; a create that timed out may still leave an object
   behind.

#### 4.2.12 Command set

Opcodes are grouped by the object they act on, with room left in each group
so a later object never forces an earlier one to renumber. Handles are
one-based: zero means "none" everywhere.

| Opcode | Command | Parameters | Buffer | Results | Notes |
|---|---|---|---|---|---|
| `0x0000` | `NOP` | | | | proves the transport |
| `0x0001` | `DEV_INFO` | | out, `dev_info`, 32 B | r0 bytes written | limits and caps; the driver asks at probe rather than assuming |
| `0x0020` | `JOB_ALLOC` | p0 JobID; p1 address entries, 0 for the default; p2 bits 7:0 port, 15:8 source GID index; p3 flags | | r0 job | a JobID may be live only once |
| `0x0021` | `JOB_FREE` | p0 job | | | releases the job's keys with it |
| `0x0022` | `JOB_QUERY` | p0 job | out, `job_info`, 16 B | r0 bytes written | |
| `0x0030` | `JKEY_CREATE` | p0 job; p1 PD; p2 flags | | r0 key handle; r1 the job key | binds a job into a protection domain |
| `0x0031` | `JKEY_DESTROY` | p0 key handle | | | |
| `0x0040` | `ADDR_INSERT` | p0 job; p1 index | in, `addr`, 40 B | r0 bytes read | at a caller-chosen index, replacing what is there |
| `0x0041` | `ADDR_REMOVE` | p0 job; p1 index | | | the index stops resolving at once |
| `0x0042` | `ADDR_QUERY` | p0 job; p1 index | out, `addr`, 40 B | r0 bytes written | an unwritten index is `ENOENT`, not zeroes |
| `0x0050` | `MR_REG` | p0 page list, bus address; p1 bits 31:0 page size, 63:32 page-list level; p2 base VA; p3 length | in, `mr_reg`, 40 B | r0 region; r1 key | registers the guest's page list with the library |
| `0x0051` | `MR_DEREG` | p0 region | | | |
| `0x0053` | `MR_ATTACH` | p0 region; p1 queue pair | | | binds a region to a queue pair and enables it (RI classes only) |
| `0x0054` | `MR_DETACH` | p0 region | | | |
| `0x0060` | `CQ_CREATE` | | in, `cq_create`, 32 B | r0 completion queue | takes a ring descriptor |
| `0x0061` | `CQ_DESTROY` | p0 completion queue | | | |
| `0x0070` | `QP_CREATE` | | in, `qp_create`, 104 B | r0 queue pair | takes the QPN, the completion queues, both rings, and the pair's path MTU |
| `0x0071` | `QP_DESTROY` | p0 queue pair | | | cannot fail for a live handle; the device retires the object in the background |
| `0x0072` | `QP_MODIFY` | p0 queue pair; p1 state | | | state changes only: RESET, RTS and ERR. INIT and RTR are refused with `EOPNOTSUPP` — this device has no connection to negotiate |
| `0x0080` | `AH_CREATE` | p0 PD | in, `addr`, 40 B | r0 address handle | a peer named directly rather than by index into a job's table |
| `0x0081` | `AH_DESTROY` | p0 address handle | | | |

Those are an *admin command's* status. A data-path request reports a
different set in its own completion, `UET_DEV_WC_*` in `struct uet_dev_cqe`,
and the two are easy to confuse because several names look alike:

| Status | Meaning |
|---|---|
| `SUCCESS` | |
| `GENERAL_ERR` | nothing more specific applies |
| `BAD_KEY` | a segment named a key no region has |
| `BAD_ADDR` | the job is known, the peer named within it is not |
| `BAD_JOB` | the request's job key resolves to no job |
| `BAD_LOCAL_ADDR` | the region was found; the segment does not lie within it |
| `TOO_LONG` | |
| `FLUSHED` | the queue pair went to ERROR with this request outstanding |

`BAD_KEY` / `BAD_ADDR` / `BAD_JOB` / `BAD_LOCAL_ADDR` are deliberately four
codes rather than one: each costs a different debugging cycle to work out
from a single generic failure.

**Protection domains are deliberately absent from the command set.** The
driver owns the PD id space; the device learns a PD only when a region or
queue pair names one, and its job from there is to enforce the binding — not
to allocate anything.

---

## 5. BAR3 — user doorbell pages

**256 KiB: 64 pages of 4 KiB, one per ucontext.** This is the only part of
the device an unprivileged process ever maps.

```
        BAR3
         ┌────────────────────────┐
 0x00000 │  page 0   ucontext A   │ ── mmap ──► process A
 0x01000 │  page 1   ucontext B   │ ── mmap ──► process B
 0x02000 │  page 2   unallocated  │
         │           ...          │
 0x3f000 │  page 63               │
 0x40000 └────────────────────────┘  end
```

The driver allocates a page from an ida at `alloc_ucontext`, exposes it
through `rdma_user_mmap_entry_insert`, and reports the offset in
`struct uet_ref_ib_alloc_ucontext_resp`. ib_core checks on `mmap()` that the
offset belongs to the asking context, and tears the mapping down when the
context dies. The page is mapped `pgprot_noncached`.

### 5.1 Within one page

```
       ┌──────────────────────────────────────┐
 0x000 │  SQ_DB    64-bit write               │
 0x008 │  RQ_DB    64-bit write               │
 0x010 │  CQ_DB    64-bit write               │
       ├──────────────────────────────────────┤
 0x018 │            unused                    │
 0xfff └──────────────────────────────────────┘
```

Reads return zero. The page exists to be written.

### 5.2 Doorbell values

Each doorbell is a **single 64-bit write**, so the handle and the index can
never be observed apart.

```
  SQ_DB / RQ_DB
   63                          32 31                           0
  ┌──────────────────────────────┬──────────────────────────────┐
  │        queue pair handle     │       producer index         │
  └──────────────────────────────┴──────────────────────────────┘

  CQ_DB
   63   62                     32 31                           0
  ┌────┬─────────────────────────┬──────────────────────────────┐
  │ARM │  completion queue handle│       consumer index         │
  └────┴─────────────────────────┴──────────────────────────────┘
```

| Field | Direction | Range |
|---|---|---|
| handle | in | the device's own handle, returned by `QP_CREATE` / `CQ_CREATE` and passed to userspace in the create response |
| producer index | in | free-running u32; the slot is `index % entries` |
| consumer index | in | free-running u32; how far the process has read |
| `ARM` (bit 63) | in | request one interrupt on the next completion |

**Arming is one-shot.** The device raises one interrupt and disarms itself,
so a consumer that wants another must ask again. Anything else would mean an
interrupt per completion for a process that is already polling.

### 5.3 The ownership check

This is why the region exists.

```
   process A                                   device
   ─────────                                   ──────
   writes (qpB << 32 | prod)                   which page did this land on?
   into its own page 0        ────────────►      page 0  → ucontext A
                                                 qpB     → owned by B
                                                 ─────────────────────
                                                 refuse, count DB_REJECTED
```

The page a write lands on is the only evidence of who wrote it; the handle
in the value is what the writer claims to be ringing. The device compares
the two. A process that maps its own doorbell page and then rings another
context's queue pair is refused by the **device**, not by anything in the
kernel path — which is the property a kernel-mediated datapath could not
have modelled.

Rejections are counted in `DB_REJECTED` (BAR0 `0x180`).

---

## 6. Ownership and threading

Who writes what, which is the part that bites:

| State | Written by | Read by | Published where |
|---|---|---|---|
| L2 `TX_PROD` / `RX_PROD` | guest kernel | device | BAR2 |
| L2 `TX_CONS` / `RX_CONS` | device | guest kernel | BAR0 |
| `ADMIN_SQ_PROD` / `ADMIN_CQ_CONS` | guest kernel | device | BAR2 |
| `ADMIN_SQ_CONS` / `ADMIN_CQ_PROD` | device | guest kernel | BAR0 |
| datapath SQ/RQ producer | guest **userspace** | device | BAR3 |
| datapath CQ consumer | guest **userspace** | device | BAR3 |
| datapath CQ producer | device | guest userspace | the CQE `valid` field |

Inside the device model, a doorbell arrives on the **vfio-user thread**
while the ring is serviced by the **progress thread**. Every index shared
between them is `volatile` for that reason; a plain `uint32_t` let the
progress thread keep an index in a register for a whole sweep and never
observe the doorbell, which appeared from userspace as work that was posted
and then simply never happened.

### 6.1 A limit worth knowing

A provider must order its descriptor write before the doorbell that
announces it. On real hardware, omitting that barrier is a bug.

**This device cannot reproduce that bug.** A doorbell store is trapped, and
the VM exit drains the vCPU's earlier stores before QEMU even sees it, so a
provider missing its barrier passes here and fails on silicon. Mapped rings buy the ownership
check and the ring semantics — not the memory ordering.

This is one case of a general problem. `VFIO_MEMORY_MODEL.md` covers both
directions: MMIO is ordered more strictly here than on PCIe, device→host DMA
is *weaker*, and each hides a different class of bug.

---

## 7. Limits

| Object | Maximum | Constant |
|---|---|---|
| job ids | 256 | `UET_DEV_MAX_JOB_IDS` |
| job keys | 256 | `UET_DEV_MAX_JOB_KEYS` |
| address table entries | 4096 | `UET_DEV_MAX_ADDR_ENTRIES` |
| memory regions | 1024 | `UET_DEV_MAX_MRS` |
| queue pairs | 256 | `UET_DEV_MAX_QPS` |
| completion queues | 256 | `UET_DEV_MAX_CQS` |
| L2 ring entries | 1024 | `UET_DEV_RING_MAX_ENTRIES` |
| admin ring entries | 64 | `UET_DEV_ADMIN_MAX_ENTRIES` |
| doorbell pages | 64 | `UET_DEV_MAX_DB_PAGES` |
| MSI-X vectors | 4 | `UET_DEV_NUM_VECTORS` |
| segments per work request | 8 | `UET_DEV_MAX_SGE` |
| address handles | 256 | `UET_DEV_MAX_AHS` |
| protection domains (advisory) | 1024 | `UET_DEV_MAX_PDS_ADVISORY` |
| L2 frame buffer | 2048 | `UET_DEV_MAX_FRAME` |
| MTU | 1500 | `UET_DEV_MTU` |

Ring depths must be a power of two everywhere: the slot index is taken by
masking.

Two defaults, which are what a caller gets for asking for nothing rather
than limits on what it may ask for:

| Default | Value | Constant |
|---|---|---|
| address table entries per job | 16 | `UET_DEV_DEF_ADDR_ENTRIES` |
| library descriptor pool depth | 64 | `UET_DEV_DEF_QUEUE_DEPTH` |

A job that asks for no address table gets one anyway: relative addressing
names a peer by index into it, so a job without one cannot address
anything, and asking for none is not a request for zero. The pool depth is
a floor rather than a cap - a work request larger than the path MTU becomes
several packets and wants a descriptor for each, so the queue pair's own
depth is raised to this and never lowered to it.

## 8. Reading them from the guest

Every numeric register above is published by the driver through ib_core's
hardware statistics mechanism, one cat-able file per counter:

    $ cat /sys/class/infiniband/uet_ref0/hw_counters/qps_live
    3
    $ ls /sys/class/infiniband/uet_ref0/hw_counters/
    admin_cmds  cq_overruns  jobs_live  qps_live  tx_frames  uptime_ms  ...

Names follow the register names in lower case, except that the
configuration registers are prefixed `cfg_` so a value that describes how
the device was built is not mistaken for something that moves. The
`DEV_INFO` limits - `max_jobs`, `max_jkeys`, `max_addr_entries`,
`max_imm_size` - and `dev_caps` are there too; they are not registers, they
come from the admin response the driver already holds.

`progress_iters` is the one 64-bit value, assembled from the `_LO`/`_HI`
pair. `lifespan` in that directory belongs to ib_core, not to us: it is how
many milliseconds a reading is cached for.

The three values that are not numbers live on the PCI device instead,
because hw_counters carries `u64` and a MAC printed as a decimal integer is
no use to anyone:

    $ cat /sys/bus/pci/devices/0000:00:06.0/uet/mac
    3a:46:25:3c:a7:30
    $ cat /sys/bus/pci/devices/0000:00:06.0/uet/ip_addr
    10.99.0.1
    $ cat /sys/bus/pci/devices/0000:00:06.0/uet/ifname
    veth0

They are on the PCI device rather than beside the counters because
`ib_device_ops::device_group`, which would put them there, carries a
comment telling new drivers not to use it.

### 8.1 What this is for

Chiefly: telling whether something leaked. The live counts return to their
baseline when a process dies, whatever way it died, and that is now one
command instead of an inference:

    baseline   jobs=0 jkeys=0 mrs=0 qps=0 cqs=0
    during     jobs=1 jkeys=1 mrs=1 qps=1 cqs=1
    after kill jobs=0 jkeys=0 mrs=0 qps=0 cqs=0

Before this existed the only way to observe a count from outside was to
exhaust a table and watch creation start failing, which took three hundred
killed processes and twelve minutes to answer a question one number
answers.

