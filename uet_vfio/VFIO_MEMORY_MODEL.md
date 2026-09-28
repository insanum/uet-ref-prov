# What vfio-user is not: memory access and ordering

This device is a stand-in for a PCIe NIC, and it is a faithful one in most
respects. Memory access is where it is not, and the differences run in both
directions: some things are *stronger* here than on a bus and some are
weaker. Both kinds mislead.

Read this before trusting a result about ordering, before removing a
barrier because "it clearly is not needed", and before forming any
intuition about which operations are expensive. §4 counts the payload
copies, which is the other thing this design gets asked about and the
answer is not obvious from either end.

`REGISTERS.md` §6 has the ownership table - who writes what
and where it is published. This document is about what the accesses
themselves guarantee.

## 1. The two directions are unrelated mechanisms

Only one of them resembles a bus.

| | Guest → device (MMIO) | Device → guest (DMA) |
|---|---|---|
| mechanism | `VFIO_USER_REGION_READ`/`WRITE` messages on a UNIX socket | `memcpy` into the client's shared mapping of guest RAM |
| here | all four BARs, `vfu_setup_region(..., NULL, 0, -1, 0)` — **trapped**, none mmap'd | `vfu_sgl_write(..., VFU_SGL_DIRECT_ACCESS)` through a memfd |
| path | guest access → KVM exit → QEMU → socket → device model; a read waits for the reply, a write does not | a copy, in the device model's own address space |
| cost | microseconds | as fast as memory |
| ordering | total: one socket, handled in order | the host CPU's memory model, and nothing else |

QEMU is the vfio-user *client*; the device model (`uet_dev_vfio.c`)
is the *server*. Guest RAM reaches the server because QEMU is started with
`-object memory-backend-memfd,share=on`, so the two processes share the same
pages.

## 2. MMIO is PCIe-shaped here, and ordered more strictly

On a real bus a posted write retires at the CPU and arrives at the device
later. That is why a driver needs a read-back to know a write landed, and
why a write barrier alone does not mean the doorbell has arrived.

vfio-user has the same shape. QEMU posts writes to memory BARs:
`hw/vfio/pci.c` marks every BAR that is not I/O space for posted writes, so
a write goes onto the socket with `VFIO_USER_NO_REPLY` and the vCPU resumes
without waiting for the device model. A read waits for its reply, and
because the server handles messages strictly in order, **a read of any
region cannot pass an earlier write** - the same rule as a PCIe read
pushing posted writes ahead of it. Config-space writes are not posted.
`-device vfio-user-pci,...,x-no-posted-writes=on` makes every write wait
for the server; the QEMU command line in `README.md` does not set it.

It is easy to assume a trapped write is complete when it returns, and the
difference is observable: the admin
driver checks its completion ring straight after ringing the doorbell, and
under fault injection a completion whose interrupt was suppressed was not
there yet - it was found only by the look the driver takes on timeout.

So:

- **A missing flush read fails here as it would on silicon.** A driver that
  assumes a doorbell has taken effect when the write returns is wrong on
  both; a BAR read after it is a flush on both.
- **Device and driver really do run concurrently**, which is what makes §3
  matter: the guest can be reading an entry while the device is writing it.
- **What is stronger than PCIe is the ordering, not the posting.** One
  socket carries everything in order, so there is no relaxed ordering and
  no reordering between BARs or between reads and writes.
- **Write combining does not exist.** A write-combining doorbell page
  batches stores on hardware; here every store is its own exit. A design
  that depends on WC batching behaves differently, and a benchmark of it is
  meaningless.
- **The barrier a provider needs before a doorbell cannot be shown to be
  necessary.** The trapped store is a VM exit, and the exit drains the
  vCPU's earlier stores before QEMU sees the doorbell, posted or not. Only
  the compiler can still reorder them. `REGISTERS.md` §6.1 records
  this; it is the single most likely bug to
  escape this environment.

## 3. DMA is weaker here than PCIe

This is the more dangerous direction, because the failure is silent and
architecture-dependent.

PCIe gives producer/consumer ordering for nothing: within a TLP stream with
relaxed ordering off, a consumer cannot observe a valid flag before the
payload it refers to. That is what makes the usual "fill the entry, then set
the flag" pattern safe on hardware.

A `memcpy` from a userspace thread gives no such guarantee. You get the host
CPU's memory model. On x86 TSO that is strong enough by accident, so **an
ordering bug in the device model is invisible on this host and appears on
ARM.**

This device avoids the question rather than answering it, and says so at
`uet_dev_cq_post()`:

```
 * The valid word is written with the entry rather than after it, because
 * the whole 64 bytes reach guest memory in one dma_write. There is no
 * window in which driver could see a half-built entry. On a real bus this
 * would need the payload to land before the valid flag.
```

That comment is wrong, and not only about a real bus. One `dma_write` is
one `memcpy`, and a `memcpy` of 64 bytes is several stores; with writes
posted (§2), the guest is free to be polling the entry while they land. The
datapath entry gets away with it on x86 only because `valid` is its last
word, the copy stores its tail last, and TSO makes the stores visible in
order - an accident of layout and of `memcpy`, and not one ARM honours.

The admin completion does not get away with it even on x86: its `VALID`
flag is at byte 10 of 32, ahead of `status` and `result`, so a driver
reading mid-copy can see the flag before the result words it vouches for.
`REGISTERS.md` §4.2.10 records it as a known gap.

The consequence for a driver is real either way: a consumer written against
this device may assume a completion entry appears atomically, and that
assumption does not survive contact with a NIC. The model keeps the driver
honest only if it writes the payload, barrier, then the flag - as separate
operations, even though one copy would do.

## 4. Where the payload actually gets copied

Three times on transmit, twice on receive - and **none of them at the
vfio-user boundary**, which is the part that surprises people.

### The process boundary is free

`uet_dev_xlate()` calls `uet_dev_dma_map()`, which returns a *pointer*:

```c
p = ((uint8_t *)r->vaddr + (addr - r->iova));
```

QEMU shares guest RAM through a memfd, so that pointer is the guest
application's own buffer seen from the device model's address space. The
library installs the translation with `uet_set_dma_xlate()` and reaches
guest pages through `uet_dma_to_host()` during any page-list walk.

So payload never crosses the socket. The socket carries doorbells, BAR
accesses and interrupts; data is moved by ordinary loads and stores against
shared memory, which is where a real DMA engine would put it too. Adding a
process boundary cost zero copies.

### Transmit: three

The provider does not copy: `post_send` writes an SGE - address, length,
key - and rings the doorbell. The device model reads the *work request* out
of the guest ring (`uet_dev_ring_read()`, 256 bytes, not payload), resolves
each SGE to a `uet_mr_seg`, and passes segments to `uet_sendseg()`. Nothing
has touched the data yet.

Then, per packet:

| # | Where | From → to |
|---|---|---|
| 1 | `gather_seg_to_flat()` → `uet_mr_gather()`, in `uet_tx_msg()` | guest application pages → a freshly `calloc`'d flat buffer |
| 2 | `memcpy(payload, pkt, pkt_len)` in `uet_pds_tx_pkt()`; `uet_rudi_build_frame()` for RUDI | flat buffer → the frame, alongside the eth/IP/PDS/SES headers |
| 3 | `sendto()` in `nic_rawsock_tx_pkt()`, or `memcpy()` into the umem in `nic_xdp_tx_pkt()` | frame → kernel skb, or → the XDP frame |

### Receive: two

| # | Where | From → to |
|---|---|---|
| 1 | `recvfrom()` in `nic_rawsock_rx_pkt()`, or `memcpy()` out of the umem in `nic_xdp_rx_pkt()` | wire → the shim's packet buffer |
| 2 | `scatter_flat_to_seg()` → `uet_mr_scatter()`, in `uet_rx_req_pkt()` | packet buffer → **straight into the guest application's pages** |

Parsing costs nothing: `pp->payload` points into the shim's buffer.

### The asymmetry is not inherent

Receive scatters from the packet buffer into guest memory in one hop.
Transmit gathers into a scratch buffer *and then* copies that into the
frame, because the frame is assembled contiguously rather than as an
iovec. `uet_pds_tx_pkt()` says so itself, immediately above the second
copy:

```c
	/* copy in the SES header and payload */
	/* TODO: support for gather iov send */
```

The non-segment path already proves it is avoidable - `pkt_buf =
buf_desc.buf + buf_desc.buf_off`, no gather at all. Building the frame as an iovec (headers plus
the segment list) and handing it to `sendmsg()` would remove copies 1 and 2
on the raw-socket backend, taking transmit from three to one. XDP would
still need one, because a umem frame must be contiguous.

### Two caveats on the counts

These are **per packet**, not per message: a multi-packet message pays them
for every packet. And with TSS enabled the frame buffer is allocated at
`2 × max_pkt_size` with reserved head space, so encryption may add a pass -
that path has not been traced, and the counts above are for security
disabled.

### Why it matters here

Copies are the reason this device model is not a performance model, and
they compound with §9: a doorbell costs a VM exit and a socket message that hardware does not
pay, and the payload is copied three times where a NIC's DMA engine would
read it once from host memory. Neither number resembles silicon, and they
err in opposite directions.

They also matter for correctness of a different kind. Copy 2 exists partly
because a reliable transport must retain the frame for retransmission - the
PDC packet owns `pkt_buf` until the peer acknowledges it. A zero-copy
transmit path has to answer what the retransmit buffer is, and "the
application's buffer, which it may have reused by then" is not an answer.

## 5. Interrupts

`vfu_irq_trigger()` writes an eventfd the client registered, and the client
injects MSI-X. It is a different path from the DMA entirely.

It works here because the `dma_write` is synchronous and completes before
the trigger, so the data is visible first. On PCIe the same ordering holds,
but because MSI-X is a posted write ordered behind the data TLPs. **Same
outcome, different reason** - and if the device model ever makes DMA
asynchronous, the guarantee disappears and needs an explicit barrier before
the trigger. Nothing would flag that.

## 6. Trapped or mmap'd, and what each costs

`vfu_setup_region()` takes an optional fd and mmap area list. Passing them
lets the client map the BAR straight into the guest.

| | Trapped (this device) | mmap'd |
|---|---|---|
| guest access | a message the server handles | a store to shared memory |
| the server | sees every access | sees nothing; must poll |
| cost | a message per access; a round trip per read | a store |

This device traps everything, including the userspace doorbell page in
BAR3. `uet_dev_vfio.c` gives the reason for each region it comments -
*"trapped rather than mmap'd, so every guest access arrives here as a
message"* - so the device observes every doorbell rather than the guest
touching shared memory it cannot see.

The trade is deliberate and it is not free. A real NIC's userspace doorbell
is a write-combining store with no syscall; here it is a KVM exit and a
socket message. If the datapath ever needs to be fast rather than
correct, this is the first thing to revisit - at the cost of the device no
longer observing doorbells directly.

## 7. Guest memory can be unmapped underneath you

The client may unmap a DMA region at any time, so a pointer into guest
memory must not outlive the call that resolved it. The model copies rather
than retains:

```
 * libvfio-user resolves a guest address to a scatter/gather
 * list, then copies through it. The copy form means no mapping outlives the
 * call, so the device model never holds a reference into guest memory.
```

It also keeps its own region table, so translation is a lookup plus an
offset rather than a `vfu_sgl_get` that would have to be released again.
Holding a reference across a call is a use-after-free of another process's
memory, which presents as corruption with no obvious author.

## 8. Two threads, not one

BAR accesses arrive on the **vfio-user socket thread**; rings are serviced
by the **progress thread**. Anything both touch needs locking *and* needs to
be re-read rather than cached - see `REGISTERS.md` §6, and the `volatile` on
every shared ring index. Caching one in a register let the progress thread
miss a doorbell for an entire sweep, which appeared from userspace as work
posted and then simply never done.

## 9. The performance ratio is inverted

On a NIC, ringing a doorbell is cheap and moving data is expensive. Here,
**ringing a doorbell is expensive and moving data is cheap** - though not
free: §4 counts three copies on transmit and two on receive, none of them
at the process boundary.

Any intuition about cost built in this environment is backwards. Concretely:
do not poll a BAR register in a loop, keep hot state in guest RAM where the
rings already are, and do not benchmark anything and believe the number.

## 10. What this environment catches, and what it cannot

**Catches:** protocol logic, ABI mismatches, ring and phase-bit handling,
object lifetime and teardown, the whole verbs surface, and every class of
bug that is about *what* is exchanged rather than *when* it lands. That is
most of a driver.

**Cannot catch, silently:**

- a missing write-posting flush
- write-combining assumptions
- relaxed ordering, and IOMMU behaviour generally
- device→host store ordering, on any host weaker than the one you tested on
- anything timing-dependent, in either direction

None of these produce a failure here. They produce a driver that passes
everything and then does not work.
