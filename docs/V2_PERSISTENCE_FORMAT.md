# V2 persistence format — Stage 6B / 10C.1A

## 1. Purpose, status and authority

**DESIGN ONLY; BYTE-FORMAT SPECIFICATION INDEPENDENTLY ACCEPTED FOR
DOCUMENTATION PUBLICATION (2026-10-27).**
The user reports that independent technical review passed with no blocking format
contradiction. The reviewer reproduced all four layouts, 23 golden artifact
outputs and cross-file bindings, and confirmed V1 representation compatibility
and P0–P8/R0–R5 preservation. This accepts the specification, not a working codec.
No V2 implementation, runtime fixture or dynamic V2 validation exists.
Original Phase 10C is not complete. The accepted semantic authority is
[AOF_COMPACTION_DESIGN.md](AOF_COMPACTION_DESIGN.md), especially sections 5–9,
11 and 13. Its architecture was accepted on feature
`5a090e87c3ae4aaf38d16007b4484e6a08a15b9e`, merged into accepted main
`6233f42e35dc5243314f1425cbc0e7d3c5cfe969`.

Entry guards verified repository path/origin, GitHub identity `sinthetaaa`, clean
worktree, fresh fetch, and HEAD/local main/origin/main/remote main equality to
that SHA. `fasttrack/aof-v2-format-spec` was absent locally/remotely and created
directly from that main. No source, test, build, CI or accepted architecture edits.

User-supplied accepted main CI: Networking **40456025900**, ASan/UBSan/fuzz
**40456025929**, Docker Linux **40456025919**. Accepted V1 evidence: Debug 79/79,
Release 79/79, sanitizer 80/80, 50 parser seeds, 10,000 fuzz executions, 15 container
cases, 10 private Compose checks, Redis parity 54 cases/40 routes, 10 operational
cases, 24 benchmark profiles/48 runs. These are historical V1 results, not V2 tests;
this milestone does not rerun or newly verify those CI execution logs.

Source authority inspected: [AofLog.hpp](../include/redisdb/persistence/AofLog.hpp),
[AofLog.cpp](../src/persistence/AofLog.cpp),
[Snapshot.hpp](../include/redisdb/persistence/Snapshot.hpp),
[Snapshot.cpp](../src/persistence/Snapshot.cpp),
[Database.hpp](../include/redisdb/storage/Database.hpp),
[Database.cpp](../src/storage/Database.cpp),
[MemoryAccounting.hpp](../include/redisdb/storage/MemoryAccounting.hpp),
[MemoryAccounting.cpp](../src/storage/MemoryAccounting.cpp), and
[Value.hpp](../include/redisdb/storage/Value.hpp).
Context: [AOF](AOF.md), [snapshots](SNAPSHOTS.md), [architecture](ARCHITECTURE.md),
[decisions](DECISIONS.md), [project state](PROJECT_STATE.md), [roadmap](ROADMAP.md),
[README](../README.md), [handoff](CONTINUATION_HANDOFF.md). Historical next-action
statements in these documents do not supersede the present authorized scope.

## 2. Common notation, compatibility and bounds

Offsets are zero-based bytes. Ranges `[a,b)` include a, exclude b. `u8/u32/u64`
are unsigned exact-width integers; `i64` is signed two's-complement. All multibyte
integers are big-endian. Byte arrays and ASCII magics have no terminator. There
is no alignment padding, native object serialization, BOM or newline. A field
marked reserved is all zero. Every offset below is normative.

All four V2 files use version **2** as u32. Header lengths are exact constants;
there are **no variable header portions or optional extensions** in version 2.
Unknown versions, flags, tags, accounting models, provenance kinds, larger header
lengths and unexplained bytes reject. Future extensions require a new explicitly
reviewed version; readers must not skip unknown fields. `END!` is hex `454e4421`.

CRC means CRC32C Castagnoli: reflected polynomial `0x82f63b78`, initial register
`0xffffffff`, final XOR `0xffffffff`, no augmentation. Empty CRC is zero.
`extend_crc32c(previous_finalized_crc, bytes)` equals CRC of the concatenation.
Serialize CRCs as BE u32. `CRC("123456789") = e3069283`. CRC over an entire file
includes every header checksum, frame checksum, footer and trailer. A prefix CRC
includes exact serialized bytes through its specified boundary, including record
trailers. Stored header CRC and CRC of the full header are distinct quantities.
CRC detects accidental corruption; it is neither authentication nor tamper
resistance, proof of semantic equivalence, or proof of completed fsync.

Portable supported base/tail size and offsets: **0..INT64_MAX** bytes, further
limited by `off_t::max`, `size_t::max` for each allocation, and host resources.
Reject negative stat sizes. No whole-file buffer or allocation by aggregate count.
Generation/sequence fields use full u64 range; sizes/counts/charge use checked u64
arithmetic with the additional bounds below. Fixed selector/marker files are exactly 112 bytes. Aggregate counts must fit
u64, feasible file bounds, size_t and host container limits; charge Q <=UINT64_MAX
and <=SIZE_MAX. Logical charge is not file length and need not fit INT64_MAX.
Max effects or KEY1 payload = **67,108,864** (64 MiB), max blob/raw LRU key =
**8,388,608**, max per-payload effects/collection/LRU count = **1,048,576**.
A TXN1 frame is at most **67,108,888** bytes; a base frame at most **67,108,880**.
Base totals are not capped at 64 MiB or one million keys.

Unchanged V1 magics: `RDBAOF01`, `RDBACM01`, `RDBSEL01`, `RDBSNP01`; V1
versions, headers, record codecs and recovery remain unchanged. V2 file magics
below differ at the first eight bytes from every V1 magic. A V1 reader must reject
a V2 container before reading any reused internal TXN1/KEY1 framing. No heuristic
upgrade/downgrade or implicit import; explicit V2 mode has its own directory.

## 3. File family, names and identities

| Artifact | Exact basename | Role |
|---|---|---|
| Selector | `CURRENT` | Sole authoritative generation selection |
| Base | `base-<g>.snap` | Immutable complete physical image |
| Journal | `tail-<g>.aof` | Immutable header, append-only TXN1 suffix |
| Watermark | `tail-<g>.commit` | Mandatory atomically replaced durable boundary |
| Lock | `LOCK` | Stable lifetime root lock, no binary payload specified |

`<g>` is canonical ASCII unsigned decimal, range **1..18446744073709551615**,
no leading zeros, signs, spaces, separators or alternate digits. Zero is invalid
in a selected V2 identity, including first initialization. Parse with checked
`n <= (UINT64_MAX-digit)/10` before multiplication. Derive names locally from a
validated GenerationId; metadata contains no paths. All final generation basenames fit 32 ASCII bytes
(the longest is `tail-` + 20 digits + `.commit`, 32). Check derived output round
trips to the same ID; no slash, dot components, NUL or user-selected basename.

Reserve temporary basenames `<final>.tmp.<token>` for each of the four final
artifacts, where token is exactly 32 lowercase hexadecimal digits from 128 random
bits. Temporary basenames are at most 69 ASCII bytes and never identify recovery state. Exclusive creation must
retry token collisions with a new token, never open/overwrite an existing temp;
future I/O uses the exact owned descriptor and recorded inode. If **any** of a
candidate generation's three final names is occupied by any directory entry,
skip that generation without opening/following it. No overwrite or deletion of
occupied generation artifacts. Unrelated/malformed names are not selectors.
CURRENT and its temp are the only selector publication targets; watermark
replacement only replaces the validated selected generation's marker. V1 uses
`generation-<g>.snap` and an externally configured AOF/marker; distinct V2 root,
magics and basename family prevent accidental reinterpretation. Ordinary V2
startup never initializes a missing CURRENT/LOCK or scans for a substitute.

StoreId is exactly 16 opaque bytes in stored order, no UUID endian conversion.
Explicit init/import draws 128 bits from the OS cryptographic random facility,
rejects all-zero and fails on entropy failure; no clock/PID/hash fallback. No UUID
version bits are imposed. Copy the same ID into every artifact in that family;
V1 import creates a new unrelated ID. ID identifies a family, not authentication.
No scan of old stores is required to prove global uniqueness. All-zero is reserved
for absent provenance only. Hex rendering is 32 lowercase digits, diagnostic only.

First generation is **1**; fresh base sequence is **0**. Each later selected
generation is strictly greater than its predecessor. Reserve the first unused
greater candidate, skipping occupied triples. Interrupted attempts can leave gaps;
an uninstalled candidate with no surviving final names may be retried because
there is no durable allocation ledger. Never reuse an occupied name, even partial.
UINT64_MAX is valid but cannot be advanced. Publication reservation fails if no
greater unoccupied triple exists. Sequence starts at 1 for a mutation
of a fresh store; compaction consumes no sequence and never resets it. A base at
S=UINT64_MAX is readable but cannot accept another mutation. Entered publication
rejects S=UINT64_MAX at P1 before selection (conservative no-new-writer rule);
reading an existing such generation remains valid. Overflow never wraps.

Immutable: store, generation, base sequence, base bytes/identity, tail header.
Mutable: appended tail records, written sequence W/length L, synchronized boundary
D/O/R/P and the marker file inode/bytes. CURRENT changes only on publication;
its selected store is invariant within a root. CURRENT does not contain W/L/D/O.
The marker is authoritative for a durable lower bound, **not** generation selection
or a replay cutoff. Selected artifacts must agree with CURRENT and derived names.
A recovery decision needs no retained previous generation or provenance source.

## 4. Base header: 184 bytes

Magic ASCII `RDBBAS02`, hex `5244424241533032`.

| Offset | Width | Type | Field and required validation |
|---|---|---|---|
| 0 | 8 | bytes | Magic exactly as above |
| 8 | 4 | u32 | Version = 2 |
| 12 | 4 | u32 | Flags: bit 0 = complete LRU present; all other bits zero |
| 16 | 4 | u32 | Header length = 184 |
| 20 | 4 | u32 | Accounting model = 1 |
| 24 | 16 | bytes | Nonzero StoreId |
| 40 | 8 | u64 | Generation g >= 1 |
| 48 | 8 | u64 | Base logical sequence S |
| 56 | 8 | u64 | Physical KEY1 count K |
| 64 | 8 | u64 | Aggregate physical logical charge Q |
| 72 | 8 | u64 | LRU1 count J; 0 without flag, K with flag |
| 80 | 4 | u32 | Source kind: 0 fresh, 1 V1 import, 2 V2 compaction |
| 84 | 4 | u32 | Reserved = 0 |
| 88 | 16 | bytes | Source StoreId (kind 2 only) |
| 104 | 8 | u64 | Source V2 generation (kind 2 only) |
| 112 | 8 | u64 | Source observed final logical sequence |
| 120 | 8 | u64 | Source observed end offset |
| 128 | 4 | u32 | Source observed last-record CRC |
| 132 | 4 | u32 | Source observed full-prefix CRC |
| 136 | 8 | u64 | Source durable logical sequence |
| 144 | 8 | u64 | Source durable end offset |
| 152 | 4 | u32 | Source durable last-record CRC |
| 156 | 4 | u32 | Source durable full-prefix CRC |
| 160 | 8 | u64 | Source V1 selected snapshot generation; 0 for AOF-only/selector-zero |
| 168 | 8 | u64 | Reserved = 0 |
| 176 | 4 | u32 | Header CRC = CRC(bytes [0,176)) |
| 180 | 4 | bytes | `END!` header trailer |

Width sum: 8+4+4+4+4+16+8+8+8+8+8+4+4+16+8+8+8+4+4+8+8+4+4+8+8+4+4
= **184**. Fields are contiguous; section 15 checks each start/end independently.
Body starts at 184; minimum complete base is 204 bytes (empty body plus DONE).

### 4.1 Source provenance (descriptive, never a recovery reference)

Kind 0 requires all bytes [88,176) zero, S=0, K=Q=J=0, flags=0, g=1.
This is explicit fresh initialization, not a representation for an emptied store
at later generations. Kind 1 requires g=1, source ID/g at [88,112) zero,
observed sequence=S, durable sequence <=S, durable offset <=observed offset,
20 <= both offsets <=INT64_MAX. V1 empty observed/durable tuples respectively
require sequence=0, offset=20, R=0, P=CRC(the validated V1 20-byte header).
Positive V1 tuple sequence requires offset>20. Equal observed/durable sequence
requires equality of both entire tuples; a lower durable sequence requires a
strictly smaller durable offset. Import computes the durable P while scanning
the V1 source (V1 marker has no P).
Positive source snapshot generation is recorded only when supplied and selected;
its full checkpoint association must have been validated by the read-only importer.
The source snapshot is not needed to recover the target. Source suffix past the
V1 watermark may be complete and valid; observed and durable tuples stay distinct.
Destination publication makes the captured state durable, without asserting source
suffix synchronization.

Kind 2 requires source ID == target ID, 1 <= source generation < g, source V1
snapshot generation=0, observed sequence=durable sequence=S, and both complete
source tuples equal the synchronized P0 checkpoint. Source offsets are in
[96,INT64_MAX]; O=96 requires R=0, and S=0 requires O=96/R=0.
If source header-only, O=96 and R=0. P refers to that source's
exact prefix, not the target header; it cannot be checked against discarded bytes
and may be any u32. No retired source is opened just to validate provenance.
For source tuples with positive sequence, R may be zero by CRC coincidence;
never use nonzero CRC as a presence test. Sequence alone cannot distinguish a
V2 header-only source with S>0, so no V1-style sequence-zero rule applies to kind 2.

Other provenance fields/reserved words must be zero as specified. Kind 1/2 do
not serialize a source path or claim the new tail contains old history. Capture/
import must validate source tuples and fidelity before P6; selected recovery can
check structural provenance consistency only. These extra observed/durable fields
are a **new format choice** to preserve the accepted import distinction.

## 5. Base frames, values, ordering and accounting

The base reuses the V1 **KEY1/LRU1/DONE frame bytes and payload representations**
exactly. Its distinct 184-byte V2 header replaces the V1 80-byte header. Reuse
does not mean a V1 snapshot file or checkpoint can be installed as a V2 base.

For every base frame at relative start F with payload length n:

| Relative offset | Width | Type | Meaning/validation |
|---|---|---|---|
| 0 | 4 | bytes | Expected tag: `KEY1`, then `LRU1`, finally `DONE` |
| 4 | 4 | u32 | n <= tag-specific limit and remaining file minus 16 |
| 8 | n | bytes | Exact payload below |
| 8+n | 4 | u32 | CRC(frame bytes [0,8+n)) |
| 12+n | 4 | bytes | `END!` |

Total n+16, no padding. Exactly K KEY1, then J LRU1, then one DONE, then EOF.
No interleaving, extra KEY/LRU/DONE, unknown frame or trailing byte is permitted.
KEY1 uses exactly `encode_effects` of one replacement, has_lru=false, zero order:
`u32(1) || blob(key) || tag || timed || [i64 deadline] || value || u8(0) || u32(0)`.
No tombstone. n <=64 MiB. `blob(x)=u32(byte_length)||x`, length <=8 MiB; empty
keys/strings/blobs allowed. See section 6 for the complete inherited value codec.
KEY1 keys strictly increase by unsigned-byte lexicographic comparison (shorter
prefix first); no duplicate key. This is the ordering of accepted std::string
comparisons on supported platforms, independent of locale and text encoding.

Encoders sort hash fields/set members. For byte-for-byte V1 decoder compatibility,
decoders accept unique hash/set items in any order; do **not** reject an otherwise
valid inherited KEY1/TXN1 solely for noncanonical inner order. This deliberate
choice preserves the accepted decoder behavior, not just its writer output.
Outer KEY1/effect-key order remains mandatory. List order and duplicates survive.
No integer type tag exists: integer command results remain unchanged ASCII decimal
StringValue bytes; arbitrary integer-looking strings are neither normalized nor
converted. Do not add an integer compression encoding.

LRU1 payload is the raw binary key, **without** length prefix (n is its length),
n <=8 MiB, including an empty key. J is u64 aggregate, not a u32 codec count.
Order is oldest-to-newest and must be an exact permutation of all K physical
keys, even under noeviction recovery. Reject duplicates, absent keys and omissions.
When the flag is absent, J=0; LRU-mode restoration uses binary sorted cold order.
With flag present and K=0, J=0 is a valid empty permutation (except kind 0).

DONE payload is exactly BE u32 CRC of **all bytes preceding DONE**, including the
complete base header and KEY1/LRU1 frames. DONE itself is the same 20-byte V1
frame: tag/length=4/prefix CRC/frame CRC/END!. BaseIdentity contains CRC of the
**entire base including DONE**, plus its exact byte length; neither is merely the
DONE payload or header CRC. This avoids a self-checksum cycle: emit header/body,
compute prefix, emit DONE, then compute full base CRC and length externally.

Capture every physically resident entry, including already-due TTL entries;
never expire/touch/evict during capture. Recompute model-1 charge Q before cleanup:
entry = 64+key bytes+value charge, plus 48+key bytes if deadline present;
string = value bytes; list = sum(48+element bytes); hash = sum(80+field bytes+
value bytes); set = sum(56+member bytes). Checked u64 accumulation and checked
native size_t conversion; restore no allocator capacities/pointers/iterators.
Rebuild expiry index and LRU iterators. Q must equal recomputed physical total;
noeviction does not subtract due entries until R5 cleanup. Budget admission later
uses live state without eviction, as accepted, rather than rejecting physical
expired charge before cleanup.

Preallocation bounds: for file size B >=204, let T=B-204. Each KEY1 consumes at
least 35 bytes (19-byte minimum payload); each LRU1 at least 16. Require
K <=T/35, then J <=(T-35*K)/16, with checked subtraction/multiplication;
Q>=64*K checked. These are necessary, not sufficient semantic checks. Stream
actual counts/charge; never reserve K/J from metadata alone. A complete base may
exceed 64 MiB: nine 8 MiB string values are nine separately bounded KEY1 frames;
one LRU1 per key avoids serializing the full permutation in one effects payload.
An oversized single entry fails candidate construction before selection; no
splitting of atomic ordinary mutations and no silently omitted state.

## 6. Tail journal: immutable 96-byte header and inherited records

Magic ASCII `RDBTAL02`, hex `52444254414c3032`.

| Offset | Width | Type | Field and validation |
|---|---|---|---|
| 0 | 8 | bytes | Exact magic |
| 8 | 4 | u32 | Version = 2 |
| 12 | 4 | u32 | Flags = 0 |
| 16 | 4 | u32 | Header length H = 96 |
| 20 | 4 | u32 | Accounting model = 1 |
| 24 | 16 | bytes | StoreId == selected base/CURRENT |
| 40 | 8 | u64 | g == selected generation |
| 48 | 8 | u64 | S == base/CURRENT sequence |
| 56 | 8 | u64 | Complete base byte length B >=204 |
| 64 | 4 | u32 | Complete base CRC including DONE |
| 68 | 4 | u32 | Base stored header CRC (at base offset 176) |
| 72 | 16 | bytes | Reserved = zero |
| 88 | 4 | u32 | Tail stored header CRC = CRC(bytes [0,88)) |
| 92 | 4 | bytes | `END!` |

Width sum = 8+4+4+4+4+16+8+8+8+4+4+16+4+4 = **96**.
TailHeaderIdentity is store/g/S + H + stored header CRC + CRC(all 96 header bytes).
No records are required; header-only tail has exact length 96. Tail has no global
footer/trailer because it is append-only; every record has its own terminal END!.
EOF is valid only immediately after the header or a complete validated record.
No separate tail UUID is needed because this immutable header binds complete base
identity. CRC collisions remain outside corruption-detection guarantees.

### 6.1 TXN1 record (unchanged V1 bytes)

| Relative offset | Width | Type | Meaning/validation |
|---|---|---|---|
| 0 | 4 | bytes | `TXN1` = hex `54584e31` |
| 4 | 8 | u64 | Sequence: first S+1, then previous+1 |
| 12 | 4 | u32 | Effects payload n <=67,108,864 |
| 16 | n | bytes | Complete inherited effects payload |
| 16+n | 4 | u32 | R = CRC(record bytes [0,16+n)) |
| 20+n | 4 | bytes | `END!` |

Total n+24; require sufficient remaining bytes before allocation. Empty effects
emit no record; count-zero TXN1 payload rejects. At sequence UINT64_MAX reject
another append/next record without overflowing expected_sequence. A sequence gap,
repeat, reset or wrapped zero rejects. Record offsets count from **tail byte 0**.
For record start A require n+24 <=INT64_MAX-A before computing end.

Complete inherited payload grammar (exact byte concatenation, no padding):

1. u32 changed-effect count N, 1..1,048,576.
2. N effects in strictly increasing unique binary key order: blob(key), u8 tag.
   Tag 0 is a tombstone with no further fields. Tags 1..4 are replacements:
   u8 timed (0 or 1), optional i64 absolute Unix-microsecond deadline if timed=1,
   then the value below. Other tag/timed values reject.
3. Tag 1: blob(string). Tag 2: u32 count followed by that many blob elements;
   order/duplicates preserved. Tag 3: u32 count then blob(field), blob(value)
   pairs, unique fields. Tag 4: u32 count then blob(member), unique members.
   Collection counts 1..1,048,576; zero collections reject. Hash/set writer sorts;
   reader accepts unique arbitrary order exactly as V1.
4. u8 has_lru (0 or 1), u32 order count M <=1,048,576, then M blob keys.
   has_lru=0 requires M=0; order keys must be unique. has_lru=1 permits M=0
   (e.g. deletion of final key). No unconsumed payload bytes.

Each count must also fit the remaining bytes divided by 4 before iteration, as
in accepted Reader::count; hash pairs need at least 8 bytes each and replacement/
flag suffixes need their own bounds checks. Parse field-by-field with subtraction
checks; tighter necessary bounds are welcome but must not reject valid V1 bytes.
Absolute TTL uses signed i64 bit pattern; no special sentinel or relative time.
No deadline value alone is invalid on disk, but native representation must fit:
for clock ticks/us scale q, require `INT64_MIN/q <= micros <= INT64_MAX/q`
(C++ division toward zero, as accepted deadline_from_micros), before multiplying.
On nanosecond clocks q=1000, on microsecond clocks q=1. Capture floors sub-us
fractions toward minus infinity and validates representability, never extends TTL.
Do not normalize integer-looking strings, deadlines or binary keys.

Payload structural validation inherits V1 decode_effects, including LRU uniqueness.
When replaying into configured LRU, full order must match resulting physical
keyspace and rebuild recency. Under noeviction retain accepted V1 replay semantics:
validate payload order uniqueness but do not demand a permutation for each record.
Base permutation is always validated regardless of policy. This distinction is
intentional; changing noeviction replay semantics needs separate review.

### 6.2 Prefix CRC and full EOF

Initialize running prefix P0=CRC(exact full 96-byte tail header), last-record R=0,
sequence W=S, length L=96. After each valid record extend prefix with **all n+24
record bytes**, set R to its stored frame CRC, W to its sequence and L to end.
At the durable O require tuple D/O/R/P equality; continue decoding/replaying to
EOF. W and L may exceed D and O. Complete valid suffix beyond the marker replays
with uncertain-client-outcome semantics; it is not discarded or automatically
claimed durable until subsequent synchronization. Any partial, malformed, corrupt
or extra suffix byte fails the **entire selected recovery**, even beyond O.
No truncation, marker synthesis, repair or fallback. A durable boundary missing
from a shorter otherwise valid tail also fails.

## 7. Commit watermark: exactly 112 bytes

Magic ASCII `RDBCOM02`, hex `524442434f4d3032`.

| Offset | Width | Type | Field and validation |
|---|---|---|---|
| 0 | 8 | bytes | Exact magic |
| 8 | 4 | u32 | Version = 2 |
| 12 | 4 | u32 | Flags = 0 |
| 16 | 4 | u32 | Total marker length = 112 |
| 20 | 4 | u32 | Reserved = 0 |
| 24 | 16 | bytes | StoreId == selected unit |
| 40 | 8 | u64 | g == selected unit |
| 48 | 4 | u32 | Tail header length = 96 |
| 52 | 4 | u32 | Tail stored header CRC (tail offset 88) |
| 56 | 4 | u32 | CRC of complete 96-byte tail header |
| 60 | 4 | u32 | Reserved = 0 |
| 64 | 8 | u64 | Durable sequence D >= S |
| 72 | 8 | u64 | Durable offset O in [96, tail size], <=INT64_MAX |
| 80 | 4 | u32 | Last durable record CRC R |
| 84 | 4 | u32 | Complete durable tail-prefix CRC P |
| 88 | 16 | bytes | Reserved = zero |
| 104 | 4 | u32 | Marker CRC = CRC(bytes [0,104)) |
| 108 | 4 | bytes | `END!`; EOF immediately after byte 111 |

Width sum = 8+4+4+4+4+16+8+4+4+4+4+8+8+4+4+16+4+4 = **112**.
Initial marker: **D=S, O=96, R=0, P=CRC(full tail header)**, mandatory even when
S>0 after compaction. Require all four exactly if D=S; conversely O=96 requires
D=S and that same tuple. For D>S require O>96 and match a completely validated
record boundary at sequence D, CRC R, accumulated P. R and P can validly be zero.
No CRC is treated as an absent-value marker. Tail header identity prevents cross-
generation marker reuse without referring to mutable fields from CURRENT.

Written state W/L/P_written/R_written changes on successful complete appends.
Synchronized state D/O/P/R changes only after tail fsync, marker temp write/fsync,
marker rename, root directory fsync succeed. During failure, visible marker may
be newer than the last in-memory confirmed durable tuple; stop and restart.
Weaker-policy acknowledgments can have W>D and L>O. Loss/truncation before O is
fatal; valid records after O remain eligible for replay. Newly selected generation
always starts its own O=96 and P=its new header CRC, never an old source offset/CRC.

## 8. CURRENT selector: exactly 112 bytes

Magic ASCII `RDBCUR02`, hex `5244424355523032`.

| Offset | Width | Type | Field and validation |
|---|---|---|---|
| 0 | 8 | bytes | Exact magic |
| 8 | 4 | u32 | Version = 2 |
| 12 | 4 | u32 | Flags = 0 |
| 16 | 4 | u32 | Total selector length = 112 |
| 20 | 4 | u32 | Accounting model = 1 |
| 24 | 16 | bytes | Nonzero StoreId |
| 40 | 8 | u64 | Selected generation g >=1 |
| 48 | 8 | u64 | Base sequence S |
| 56 | 8 | u64 | Complete base byte length B >=204, <=INT64_MAX |
| 64 | 4 | u32 | Complete base CRC including DONE |
| 68 | 4 | u32 | Base stored header CRC |
| 72 | 4 | u32 | Tail header length H = 96 |
| 76 | 4 | u32 | Tail stored header CRC |
| 80 | 4 | u32 | CRC of complete tail header |
| 84 | 20 | bytes | Reserved = zero |
| 104 | 4 | u32 | Selector CRC = CRC(bytes [0,104)) |
| 108 | 4 | bytes | `END!`; EOF immediately after byte 111 |

Width sum = 8+4+4+4+4+16+8+8+8+4+4+4+4+4+20+4+4 = **112**.
CURRENT freezes immutable references only. Tail length and marker D/O/R/P must
advance without republishing selection. No marker checksum/inode is embedded in
CURRENT. Base header/full-file checksums and length must match both CURRENT and
tail. Tail header checksums/length must match CURRENT and marker; every store/g
must agree, tail S==base S==CURRENT S, accounting=1 throughout. Base flags/LRU
counts are validated within the base; CURRENT has no second policy interpretation.

Reject invalid magic/version/flags/length/reserved/accounting/trailer/checksum,
short or overlong files, trailing bytes, g=0, zero ID, missing selected file,
wrong derived generation, mixed store, wrong base size/CRC/header CRC, wrong tail
binding or marker identity. “Unknown selected generation” means the exact derived
unit is missing/invalid, never permission to search. Missing CURRENT always fails
ordinary startup; only explicit init/import into an empty destination can create it.

**A checksum does not satisfy R4.** Every serving restart must retain pinned
CURRENT bytes/descriptor identity, validate R0–R3 read-only, then precheck CURRENT,
LOCK and selected file names/held identities, fsync the held root directory,
and postcheck those same identities/bytes before enabling R5 baseline/cleanup,
readiness or acknowledgment. Any error fails closed. No format-level durable bit
or header flag can establish that P7 ran before an earlier process died.

## 9. Cross-file validation and trust boundaries

Validation order: bounded selector structure → derived selected basenames → base
header/frames/DONE/full CRC/size/physical charge/permutation → tail immutable
header/base binding → marker structure/header binding → full tail records and
exact durable boundary → R4 filesystem barrier → R5 synchronization/cleanup.
Candidate validation at P2/P3 uses the same pure/streaming validators with supplied
candidate immutable metadata; it does not need a published CURRENT or active writer.
No circularity: base has only descriptive old provenance; tail binds completed
base; marker binds completed tail header; CURRENT binds base and tail header.
Nothing immutable hashes the mutable marker or complete growing tail.

Later filesystem validation requires existing real trusted root, root-relative
opens, nofollow/nonblocking/CLOEXEC discipline, regular single-link selected
CURRENT/base/tail/marker and LOCK, pinned descriptors, exact named dev/ino/size
checks where required, continuous root lock and tail lock in accepted order.
Stable LOCK is never replaced/created by ordinary recovery. Contents of unselected
files are irrelevant to selected recovery; final-name occupancy is checked only
when reserving new generations. Symlink/FIFO/device/hardlink/missing/changed
selected artifacts reject. These are future I/O checks, not properties a byte
span decoder can establish. Trusted-directory/cooperating-writer assumptions
remain; CRC/IDs cannot prevent deliberate whole-family rollback or recomputation.
EOF must be established against the pinned input extent; filesystem rechecks
remain necessary before R4. Codec validation must not mutate a live database.

## 10. Invalid-input and overflow matrix

| Input/failure | Required rejection before action | Validation layer |
|---|---|---|
| Header shorter than 184/96/112, malformed magic/trailer | Bounded read fails; no out-of-bounds access | Structural |
| Wrong version, length, accounting model, unknown flag/kind, nonzero reserved | Reject, no unknown-field skipping | Structural |
| Extra CURRENT/marker bytes, absent base DONE, extra base bytes | Exact size/EOF and tag order reject | Structural/streaming |
| Negative stat size or file >INT64_MAX/off_t max | Reject before conversion/pread | I/O bounds |
| n above payload/blob cap or n beyond remaining bytes | Compare by subtraction before allocating n | Structural |
| A+n+overhead overflow, O before H/after EOF | Check max-A, remaining-overhead, native offset range first | Structural |
| Crafted huge K/J/Q with tiny base | Minimum-size/count bounds; no reserve/loop by unchecked count | Structural |
| Effect/item count above 1M or byte-derived feasible count | Bound before allocation/iteration; hash pair minimum 8 | Structural |
| Allocation/host container max_size exceeded | Fail resource validation; no partial state publication | Resource |
| Zero store ID, g=0, wrong name spelling/range | Validated strong IDs and local name derivation | Structural/identity |
| Mixed-store/g, S mismatch, conflicting B/base CRC/header CRC | Exact cross-file comparison rejects | Cross-file |
| Wrong tail header checksum/full-header checksum | Recompute exact defined ranges, compare all references | Cross-file |
| Wrong header/frame/DONE/full-base CRC | Reject; no alternate CRC interpretation | Streaming |
| Unknown frame, out-of-order KEY/LRU, missing/extra frame | Fixed grammar and exact counts reject | Streaming |
| Duplicate/unsorted outer key, tombstone/multiple effects/LRU in KEY1 | Exactly one replacement and strict key order | Semantic |
| Duplicate hash field/set member or empty collection | Reject; unique unsorted inner order is permitted | Semantic |
| Invalid tag/timed/has_lru, unexpected payload suffix | Exact inherited grammar rejects | Structural/semantic |
| Signed TTL decode or scale overflow | Bit-pattern decode, checked native clock multiplication | Conversion |
| TTL already past or negative Unix time | Preserve physical entry; this alone is not corruption | Semantic |
| LRU count/flag mismatch, duplicates, foreign/missing base key | Full base permutation validation under both policies | Semantic |
| Q mismatch/charge overflow | Recompute checked charge before expiry; reject mismatch | Semantic |
| Invalid sequence start/gap/repeat/reset/exhaustion | Check prior <UINT64_MAX before prior+1 | Streaming |
| D<S, D beyond EOF sequence, O in record body, wrong R/P | Match exact validated boundary tuple | Streaming |
| Invalid D=S initial tuple, absent marker | Reject; never synthesize initial marker | Cross-file |
| Valid complete suffix after O | Validate and replay; not an error or replay cutoff | Streaming |
| Partial/corrupt suffix after O, missing committed suffix | Reject whole unit, never truncate/ignore | Streaming |
| Unknown checksum coverage or alternate endian attempt | Only defined version-2 coverage accepted | Structural |
| Invalid provenance constraints/old tuple copied as new boundary | Check kind-specific rules; no old source recovery dependency | Semantic |
| u64→size_t/off_t/ssize_t narrowing on macOS/Linux | Prove range before cast; chunk I/O <=SSIZE_MAX | Conversion/I/O |
| Missing/replaced/nonregular/symlink/hardlinked selected file | Descriptor/name/link-count checks; fail no fallback | Filesystem |
| R4 pre/post identity mismatch or unsupported/failed dir fsync | No R5 writes/readiness/ack, preserve files | Filesystem durability |

Before allocation establish remaining extent, format caps and native max_size;
use bounded per-frame buffers and incremental decoded objects. Do not multiply
counts, add offsets, compute n+overhead or subtract without prior guards. Aggregate
counts may exceed 1M but must fit feasible file bounds and actual streamed frames.
Use checked additions for charge, counters and lengths; check `b <=max-a` before
addition and `a==0 || b<=max/a` before multiplication. No allocator failure is
interpreted as empty state. Resource rejection is distinct from format corruption.
A private restored database can be discarded on failure; selected bytes stay intact.
No decoder recursively descends untrusted nesting (the grammar has fixed depth).
Streaming semantic validation can use O(K) key/permutation/index memory; bounded
frames do not promise O(1) total recovery memory or admission of arbitrary huge
stores. No counts imply a trustworthy allocation budget.

## 11. Proposed C++20 model/API (documentation examples only)

These signatures describe the later 10C.1 model; they create no header/source or
working ABI. Namespace `redisdb::persistence::v2` is separate from V1. No implicit
conversion to/from V1 `AofCheckpoint`, no default offset=20, no default initialized
validated identity. `Result<T>` is a proposed project-owned tagged value/error
(std::variant based in C++20, not C++23 std::expected); exact diagnostics are deferred.

```cpp
struct StoreId final { std::array<std::byte, 16> bytes; }; // factory rejects zero
struct GenerationId final { std::uint64_t value; };       // checked >=1
struct LogicalSequence final { std::uint64_t value; };    // zero allowed
struct TailOffset final { std::uint64_t value; };         // checked >=96
struct Crc32c final { std::uint32_t value; };
struct BaseIdentity final {
    StoreId store; GenerationId generation; LogicalSequence sequence;
    std::uint64_t byte_length;
    Crc32c header_crc; Crc32c file_crc;
};
struct TailHeaderIdentity final {
    BaseIdentity base; // includes store/g/S and complete base binding
    std::uint32_t header_length; // exactly 96
    Crc32c header_crc; Crc32c full_header_crc;
};
struct GenerationCheckpoint final {
    TailHeaderIdentity tail;
    LogicalSequence sequence;
    TailOffset offset;
    Crc32c record_crc; Crc32c prefix_crc;
};
class DurableTailBoundary final {
    GenerationCheckpoint checkpoint_; // construct only after marker/tail match
    // read-only accessors; private constructor, validation factory friend
};
class ValidatedSelector final {
    TailHeaderIdentity tail_;
    std::uint32_t accounting_model_; // 1
    std::array<std::byte, 112> exact_bytes_; // owned immutable value
    // private construction after local decoding and cross-file validation
};
class PreparedGeneration final { // opaque future 10D.1 ownership object
    // move-only; root-lock lease, candidate FDs, exact owned temp names/inodes,
    // validated immutable metadata and initial boundary; prepared writer disabled
    // private construction by publication coordinator; destructor noexcept
};
using ByteView = std::span<const std::byte>; // borrowed only for call duration
using OwnedBytes = std::vector<std::byte>;

Result<StoreId> make_store_id(std::array<std::byte, 16>);
Result<GenerationId> parse_generation(std::string_view canonical_decimal);
Result<GenerationId> next_generation(GenerationId); // checked arithmetic only
Result<LogicalSequence> next_sequence(LogicalSequence);
Result<std::string> base_basename(GenerationId); // also tail/marker variants
Result<std::array<std::byte, 184>> encode_base_header(const BaseHeaderFields&);
Result<BaseHeaderFields> decode_base_header(ByteView);
Result<std::array<std::byte, 96>> encode_tail_header(const BaseIdentity&);
Result<TailHeaderIdentity> decode_tail_header(ByteView);
Result<std::array<std::byte, 112>> encode_selector(const TailHeaderIdentity&);
Result<SelectorFields> decode_selector(ByteView); // locally valid, not selected
Result<std::array<std::byte, 112>> encode_marker(const GenerationCheckpoint&);
Result<MarkerFields> decode_marker(ByteView); // not yet a durable boundary
Result<OwnedBytes> encode_key_frame(const storage::EntryEffect&);
Result<storage::EntryEffect> decode_key_frame(ByteView);
Result<OwnedBytes> encode_txn(LogicalSequence, const storage::CommittedEffects&);
Result<DecodedTransaction> decode_txn(ByteView);
Result<ValidatedSelector> validate_links(
    const SelectorFields&, const ValidatedBase&, const TailHeaderIdentity&);
Result<DurableTailBoundary> validate_boundary(
    const MarkerFields&, const ValidatedTailScan&);
```

Fields shown as structs are **unvalidated values** until checked factory/decoder
success; implementation should use private representations/accessors for IDs to
prevent unchecked construction. Invalid caller-assembled structs must fail encoding.
Supporting types: BaseHeaderFields owns the exact scalar header/provenance fields;
SelectorFields/MarkerFields own decoded scalars and exact fixed bytes;
DecodedTransaction owns sequence and CommittedEffects; ValidatedBase owns verified
BaseIdentity/count/charge/LRU metadata; ValidatedTailScan owns tail identity, final
checkpoint and matched marker boundary, not a full tail buffer. Neither “Validated”
result implies fsync/lock ownership or authorization to serve. DurableTailBoundary
means a valid encoded durable claim matched to bytes; actual completed writer
synchronization is an I/O coordinator responsibility, not proven by a CRC.

Pure encoders/decoders perform no I/O, random generation, database mutation, clock
sampling, locking or marker publication. Fixed decoders accept exact spans of the
stated length, with no retained views. Streaming base/tail validators take bounded
caller-fed byte spans via an incremental state object, own buffered partial-frame
bytes/metadata, report NeedMore/Complete/Error, and require an explicit end-of-input
signal before issuing ValidatedBase/ValidatedTailScan. A final frame is not proof
of filesystem EOF. Decoded entries own key/value bytes; no string_view escapes.
Cross-file validators compare already decoded/stream-validated immutable values;
validate_links also checks accounting/S/store/g/B/checksums. validate_boundary
requires the actual recorded boundary and full-EOF scan, never a naked tuple.

GenerationCheckpoint is an observed generation-qualified tuple, which may be
written but unsynchronized. DurableTailBoundary is separately validated. Neither
is interchangeable with V1 AofCheckpoint. Old V1 provenance is an explicitly tagged
LegacySourceCheckpoint value; it cannot enter an encode_marker call. Immutable
metadata is copied by value or held by shared const ownership where needed, never
a mutable alias to a byte buffer. Future FD owners use move-only RAII; borrowed
FDs/ByteViews never close or outlive owners. PreparedGeneration retains a stable
root-lock lease across publication/activation, and cannot instantiate/call V1
AofLog recovery against a live database. Its exact methods are deliberately deferred.

Error categories: Truncated, InvalidMagic, UnsupportedVersion, UnsupportedFlags,
InvalidLength, InvalidReserved, InvalidIdentity, InvalidChecksum, InvalidTrailer,
InvalidFrame, NoncanonicalOrder, Duplicate, InvalidDeadline, InvalidAccounting,
SequenceExhausted, GenerationExhausted, IntegerOverflow, ResourceLimit,
CrossFileMismatch, InvalidDurableBoundary. Future I/O layer adds IoFailure,
LockUnavailable, UnsafeFileIdentity, SelectionChanged, DurabilityFailure;
preserve syscall category without embedding arbitrary paths/data in client replies.
No partial successful decoded value on error. bad_alloc is a resource failure;
no automatic repair. Strong-type construction and operator equality are explicit;
no general arithmetic/implicit integer or legacy checkpoint conversion.

| Slice | Authorized future responsibility after its separate gate |
|---|---|
| 10C.1 | Pure versioned models/codecs, bounded streaming validation, golden/error tests; no FS operations |
| 10D.1 | Stable locks/identities, owned temps, fsync/rename publication primitive, R4 helper/model, prepared ownership; independently tested before use |
| 10C.2 | Explicit init/read-only V1 import/V2 recovery/serving using accepted D.1; R0–R5, no pre-R4 writes |
| 10D.2 | Live P0 barrier/capture and P8 no-throw writer handoff; journal mutation/activation |

No function in the proposed pure API authorizes publication, selection, serving,
FD acquisition or append. No premature implementation of PreparedGeneration,
activation, filesystem adapters or locking interfaces is part of 10C.1A.

## 12. Publication and recovery compatibility proof

| Step | Byte-format support and preserved ordering |
|---|---|
| P0 | Synchronize source; generation-qualified C records old sequence/offset/R/P; capture physical entries/recency |
| P1 | Choose unused greater g; allocate checked metadata/temp/writer ownership; no old name overwrite |
| P2 | Emit 184-byte base header and bounded frames/DONE; file fsync, readback full CRC/EOF and frozen-state fidelity |
| P3 | Completed BaseIdentity supplies tail header; fsync it; initial marker uses S/96/0/CRC(header), fsync and validate; writer disabled |
| P4 | Install base, tail, marker final names, verify identities, root fsync; all references exist before selector |
| P5 | Encode CURRENT from completed immutable metadata, validate and file-sync temporary |
| P6 | Rename CURRENT is visibility/selection point only; new tail still unchanged |
| P7 | Root directory fsync makes selection durable; failure uncertain/fatal, no rollback |
| P8 | Prepared no-throw ownership swap; root lock continuous; first possible new record S+1 |
| R0 | Existing stable LOCK, trusted root, exclusive continuous root ownership |
| R1 | Pin exact 112 selector bytes and descriptor identity; derive exactly three names |
| R2 | Read-only complete selected base validation/physical restore, selected tail ownership without writes |
| R3 | Read-only tail/header/marker/full EOF scan, exact D/O/R/P, replay full valid suffix privately |
| R4 | Precheck exact selector/name identities, root fsync, postcheck; mandatory before **any** R5 write/readiness |
| R5 | Only then synchronize baseline, transactional due-expiry cleanup/live admission, flush under every policy and serve |

No checksum calculation depends on CURRENT or mutable marker bytes from inside
the base/tail header. Candidates can therefore be constructed and readback-validated
before selection. Current written state is separate from durable state. Import
preserves source physical TTL/LRU/charge read-only into a new StoreId/root; no
source path/old offset becomes a target reference. V1 source tuples are audit
provenance only, original V1 snapshot/AOF readers stay intact. A valid selected
unit stands alone after separately authorized future reclamation. Unselected
corrupt files are neither selected, repaired, deleted nor needed for recovery.

The format supports the accepted P6-before-P7 death → observe NEW → R4 → durable
new write → second power-loss model: immutable binding validates NEW; R4 makes
that selection durable before new journal mutation. CRC validation cannot replace
that ordering. No detected architectural incompatibility requires changing the
accepted document. A later discovered contradiction must stop work for architecture
review; this specification may not silently revise the accepted contract.

## 13. Independent-review disposition and deferred concerns

Concrete byte-level choices introduced by this specification and now independently
accepted for documentation publication: generation 1
start; opaque nonzero random 128-bit IDs without UUID bits; decimal names/temp
suffix grammar; fixed 184/96/112/112 layouts and magics; dual stored/full header
CRCs; extended observed/durable provenance; retained permissive inherited inner
hash/set decoding. Independent review found no blocking format contradiction.
The following review questions are retained with acceptance recorded; they remain
implementation verification obligations, not unresolved format alternatives:

1. Do the exact header tables, identities and redundant header CRCs provide a
   sufficiently simple corruption/mixing check without creating ambiguity?
2. Does fixed provenance preserve source physical state and observed-versus-durable
   distinction adequately, including derived V1 marker prefix CRC, without implying
   access to retired history? Source equivalence still needs P2 readback comparison.
3. Confirm deliberate reuse of V1 hash/set decoder permissiveness and noeviction
   journal LRU semantics, and stricter complete base permutation validation.
4. Confirm portable INT64_MAX file/offset ceiling, native deadline range rejection,
   aggregate streaming counts and inherited single-record/startup cleanup limits.
5. Confirm proposed strong-type validation stages cannot be mistaken for R4/fsync
   completion or used to activate a prepared writer prematurely.

Deferred engineering: exact syscall seams/FD identity objects, entropy API binding,
error text, IO chunk sizes, allocator budgets, publication fault hooks, supported
filesystem fsync semantics and resource/pause/free-space measurements. Locking,
publication, import, serving, live writer handoff and reclamation remain separate
milestones. Existing full-LRU transaction and large startup cleanup limits remain;
no unreviewed batching or limit extension. CRC collisions/random-ID collision,
resource exhaustion, synchronous pause, clock rollback and filesystem assumptions
remain risks; no authentication, anti-rollback or physical-power proof is claimed.

Required order: **10C.1 → 10D.1 → 10C.2 → 10D.2 → 10E → 10F → 10G.1 →
10G.2 → formal Phase 10 closure**, each independently accepted and separately
authorized. 10G.2 needs its own deletion review, stopped backup/pins/inventory,
identity-checked apply and physical reduction evidence. Retain-all is intermediate.
Stage 6C has not begun. Original Phase 16 remains excluded, never completed.

## 14. Precise next acceptance gate

Independent format review has passed as reported by the user. Guarded publication
is authorized: validate exactly these two documentation files, make one normal
feature commit, push normally and verify all three Linux workflows at that exact
feature SHA. Do not put the future commit SHA or anticipated CI results here;
the publication report supplies actual identities, run IDs and execution results.
STOP after feature CI. No main merge, PR or implementation is authorized.

A separate guarded main merge must recheck accepted main, exact reviewed/tested
feature head and two-document scope; verify normal non-fast-forward merge parents
(accepted main first, feature second) and merge-tree equality to the tested feature;
then require successful exact post-merge main CI. After accepted merge, separately
authorize **10C.1B codec/model implementation**. Original Phase 10C is not formally
complete; 10D.1, 10C.2 and later slices remain pending.

Future 10C.1B implementation tests must include positive and negative cases for:

- V1 import provenance kind 1, including observed versus durable source boundaries.
- Strings, lists, hashes and sets, including their inherited codec constraints.
- Absolute TTL deadlines and native clock-conversion edge cases.
- Generation and logical-sequence exhaustion.
- Full LRU reconstruction under both noeviction and allkeys-lru memory policies,
  preserving their specified differences in journal replay validation.

These are future test obligations; no binary fixtures or codec tests are created
or claimed executed by this documentation publication.

## 15. Canonical binary vectors and reproduction

The following Python 3 recipe is **documentation only**, writes no files and
requires no third-party module. Execute the fenced block with Python on stdin or
extract it in memory; do not write generated bytes as persistence fixtures. It
constructs exact bytes (including zeros and trailers) and prints length, CRC32C
and SHA-256 for every artifact. Assertions cross-check reflected bitwise CRC
against an independent forward-polynomial implementation (`0x1edc6f41`, per-byte
bit reflection). The reflected algorithm matches accepted AofLog.cpp CRC convention.
Fixed vector IDs are illustrative deterministic inputs, not production entropy.

Cases: E = fresh g=1/S=0 empty store; E.current references its first complete
base; E.marker is D=S. T appends one binary-string transaction to E and advances
the marker. C = g=3/S=1 compaction of synchronized E+T, preserving sequence and
permitting a generation gap; C.base contains binary key/value bytes. U is a second
transaction on g=1 that adds key ff and records full LRU. L = g=4/S=2 compaction
of synchronized E+T+U, with two keys and exact LRU `[ff,006b]` oldest-to-newest.
C and L are alternative compactions of E's family, not sequential publications.
The LRU order `[ff,006b]` is reachable when an intervening read/touch makes key
006b most recently used before the second mutation; for example, after T, a read
of 006b followed by an atomic second mutation that creates ff and touches 006b
last produces this order. U encodes the resulting valid physical/LRU state, not
an assertion that two isolated SET commands alone produce this ordering.
All entries in this recipe are string/no-TTL; section 6 specifies the inherited
collection/deadline grammar. Other-type/TTL negative and positive codec tests
remain required future implementation work.

```python
# V2_VECTOR_RECIPE_BEGIN
import hashlib
import struct

def be(n, width):
    return n.to_bytes(width, 'big')

def crc(data):
    r = 0xffffffff
    for x in data:
        r ^= x
        for _ in range(8):
            r = (r >> 1) ^ (0x82f63b78 if r & 1 else 0)
    return r ^ 0xffffffff

def reverse(x, bits):
    return int(f'{x:0{bits}b}'[::-1], 2)

def crc_forward(data):
    r = 0xffffffff
    for x in data:
        r ^= reverse(x, 8) << 24
        for _ in range(8):
            top = r & 0x80000000
            r = ((r << 1) & 0xffffffff) ^ (0x1edc6f41 if top else 0)
    return reverse(r, 32) ^ 0xffffffff

def sealed(prefix):
    return prefix + be(crc(prefix), 4) + b'END!'

def frame(tag, payload):
    return sealed(tag + be(len(payload), 4) + payload)

def blob(x):
    return be(len(x), 4) + x

def effect(k, v):
    return blob(k) + b'\x01\x00' + blob(v) # string, no TTL

def effects(entries, order=None):
    return (be(len(entries), 4) + b''.join(effect(k, v) for k, v in entries)
            + bytes([order is not None]) + be(len(order or []), 4)
            + b''.join(blob(k) for k in (order or [])))

def txn(seq, payload):
    return sealed(b'TXN1' + be(seq, 8) + be(len(payload), 4) + payload)

SID = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
OTHER = bytes.fromhex('101112131415161718191a1b1c1d1e1f')

def base(g, seq, entries, order=None, source=None):
    # source=(source_g, observed_sequence, observed_offset, R, P), kind 2
    h = (b'RDBBAS02' + be(2,4) + be(int(order is not None),4)
         + be(184,4) + be(1,4) + SID + be(g,8) + be(seq,8)
         + be(len(entries),8)
         + be(sum(64+len(k)+len(v) for k,v in entries),8)
         + be(len(order or []),8))
    assert len(h) == 80
    if source is None:
        h += bytes(96) # kind/reserved/all provenance
    else:
        sg, ss, so, sr, sp = source
        t = be(ss,8)+be(so,8)+be(sr,4)+be(sp,4)
        h += be(2,4)+bytes(4)+SID+be(sg,8)+t+t+bytes(16)
    assert len(h) == 176
    h = sealed(h)
    b = h + b''.join(frame(b'KEY1', effects([(k,v)])) for k,v in entries)
    b += b''.join(frame(b'LRU1', k) for k in (order or []))
    return b + frame(b'DONE', be(crc(b),4))

def tail(g, seq, b):
    h = (b'RDBTAL02'+be(2,4)+bytes(4)+be(96,4)+be(1,4)+SID
         +be(g,8)+be(seq,8)+be(len(b),8)+be(crc(b),4)+b[176:180]
         +bytes(16))
    assert len(h) == 88
    return sealed(h)

def selector(g, seq, b, h):
    p = (b'RDBCUR02'+be(2,4)+bytes(4)+be(112,4)+be(1,4)+SID
         +be(g,8)+be(seq,8)+be(len(b),8)+be(crc(b),4)+b[176:180]
         +be(96,4)+h[88:92]+be(crc(h),4)+bytes(20))
    assert len(p) == 104
    return sealed(p)

def marker(g, h, seq, full_tail, last_crc=0):
    p = (b'RDBCOM02'+be(2,4)+bytes(4)+be(112,4)+bytes(4)+SID
         +be(g,8)+be(96,4)+h[88:92]+be(crc(h),4)+bytes(4)
         +be(seq,8)+be(len(full_tail),8)+be(last_crc,4)
         +be(crc(full_tail),4)+bytes(16))
    assert len(p) == 104
    return sealed(p)

assert crc(b'123456789') == crc_forward(b'123456789') == 0xe3069283
Ebase = base(1, 0, [])
Eh = tail(1, 0, Ebase)
Ecur = selector(1, 0, Ebase, Eh)
Emark = marker(1, Eh, 0, Eh)
entries = [(b'\x00k', b'\xff\x00v')]
Tpayload = effects(entries)
T = txn(1, Tpayload)
Ttail = Eh + T
Tr = int.from_bytes(T[-8:-4], 'big')
Tmark = marker(1, Eh, 1, Ttail, Tr)
Cbase = base(3, 1, entries, source=(1,1,len(Ttail),Tr,crc(Ttail)))
Ch = tail(3, 1, Cbase)
Ccur = selector(3, 1, Cbase, Ch)
Cmark = marker(3, Ch, 1, Ch)
Lentries = entries + [(b'\xff', b'7')]
Lorder = [b'\xff', b'\x00k']
U = txn(2, effects([(b'\xff', b'7')], Lorder))
Utail = Ttail + U
Ur = int.from_bytes(U[-8:-4], 'big')
Lbase = base(4, 2, Lentries, Lorder,
             source=(1,2,len(Utail),Ur,crc(Utail)))
Lh = tail(4, 2, Lbase)
Lcur = selector(4, 2, Lbase, Lh)
Lmark = marker(4, Lh, 2, Lh)

artifacts = {
    'E.base': Ebase, 'E.tail': Eh, 'E.current': Ecur, 'E.marker': Emark,
    'T.payload': Tpayload, 'T.record': T, 'T.tail': Ttail, 'T.marker': Tmark,
    'C.base': Cbase, 'C.tail': Ch, 'C.current': Ccur, 'C.marker': Cmark,
    'U.record': U, 'U.tail': Utail,
    'L.base': Lbase, 'L.tail': Lh, 'L.current': Lcur, 'L.marker': Lmark,
}
for name, data in artifacts.items():
    assert crc(data) == crc_forward(data)
    print(name, len(data), f'{crc(data):08x}', hashlib.sha256(data).hexdigest())
for name, h, b, m in [('E',Eh,Ebase,Emark), ('C',Ch,Cbase,Cmark),
                      ('L',Lh,Lbase,Lmark)]:
    print(name+'.fields', 'base_hcrc='+b[176:180].hex(),
          'tail_hcrc='+h[88:92].hex(), 'D='+str(int.from_bytes(m[64:72],'big')),
          'O='+str(int.from_bytes(m[72:80],'big')), 'R='+m[80:84].hex(),
          'P='+m[84:88].hex(), 'marker_crc='+m[104:108].hex())
print('T.fields', 'payload='+Tpayload.hex(), 'R='+f'{Tr:08x}',
      'P='+f'{crc(Ttail):08x}', 'marker_crc='+Tmark[104:108].hex())
# Structurally valid hostile pairings: own CRCs remain correct.
def edited_fixed(data, offset, replacement):
    p = bytearray(data[:104])
    p[offset:offset+len(replacement)] = replacement
    return sealed(bytes(p))
wrong_store = edited_fixed(Emark,24,OTHER)
wrong_g = edited_fixed(Emark,40,be(2,8))
wrong_base_crc = edited_fixed(Ecur,64,be(crc(Ebase)^1,4))
assert len(wrong_store) == len(wrong_g) == len(wrong_base_crc) == 112
for name, data in [('bad.store',wrong_store), ('bad.generation',wrong_g),
                   ('bad.base_crc',wrong_base_crc),
                   ('bad.short_marker',Emark[:-1]), ('bad.short_tail',Ttail[:-1])]:
    assert crc(data) == crc_forward(data)
    print(name, len(data), f'{crc(data):08x}', hashlib.sha256(data).hexdigest())
# V2_VECTOR_RECIPE_END
```

Expected output (CRC columns are eight lowercase hex digits; SHA-256 is over
exact artifact bytes). These hashes identify documentation vectors, not source
history authentication or runtime fixture evidence:

```text
E.base 204 4ebad660 96afa112c2062512907265abbed197c4dabaddac8ba89e0a6e17ff1c496d5042
E.tail 96 dbfbc819 64e043bc2235f613d49e3d14f99cf912079114a429b259e8954f3399c54ff43d
E.current 112 ddd391ec c5fb8b6d4d2ba156aa27be159449aec79f55832307f22303540624d51cad9790
E.marker 112 3c3960b6 f6ad04331b9773f43a562905073b1e006e0cfeedf0627072cba796cef541cea5
T.payload 24 822afcc6 111f9055b55b9620483779e6e2a5166f2a927688c15eeef7ec664ebcbeb67dd9
T.record 48 b5c6c2b9 cb5a8c7fee3c92d24897fa33705b13919ab41f77d00037d600e6226535df330f
T.tail 144 04430f74 b79aa265925b04356704ffec42a577d6f8310f43a4ebe675ab0756dfe4dadcb4
T.marker 112 7a05287b d86c35a6b4caaa7aad417f1789304eccfe235d2ff93b8bcc0bfda5cd0b9756aa
C.base 244 155d9af1 e6c542a4dfa5a1c86d934a2d9b57b64d979ba65b4d9e23d2c4d45fa509adcf47
C.tail 96 28b640b9 702cfaef7fbbfe7a67e1092b58cb81ce7757bc467dcb0ad63d5254b3931a037f
C.current 112 8854d877 234f1dfcbd8123d73e911c34aedafbd7eecc6519428e2130f75b97007180c402
C.marker 112 6ba61e7e 065a9c8d6a068053f0c8f6308ba85161c2e9da928e01e03e89903fda9a711cf1
U.record 56 f18ecbb6 40c5ce735e8062d732ce0a3cf6b4d07da726cadb14d9d642218a4ccff680b507
U.tail 200 a33c606b bdf4ad5754a2046fce402bbb9d54c000f555ab3d713f6514dea84ba434aa9bbf
L.base 316 6ddee862 91cdc99151f461df21d944d01b777026f834c202fdfcb5dc23f2db0882eb2b41
L.tail 96 dc793392 02e06dc3ee1bcd92f500330094815f974c7652d3cd224963c8477994ab011f22
L.current 112 15bd7be8 abff86db10901cd20668409427d189f34f54afbc6bb69161d10f9e88bcf7376a
L.marker 112 978b12dd 345ddc430bde9c8c3a7f8a99701e2a694d01a2c3e7c9c4e1536c126325ffeaf2
E.fields base_hcrc=bc252022 tail_hcrc=cc38e707 D=0 O=96 R=00000000 P=dbfbc819 marker_crc=bba4806d
C.fields base_hcrc=213707da tail_hcrc=ad63e9bb D=1 O=96 R=00000000 P=28b640b9 marker_crc=c81f9e4b
L.fields base_hcrc=a646354b tail_hcrc=c1721669 D=2 O=96 R=00000000 P=dc793392 marker_crc=bf336709
T.fields payload=0000000100000002006b010000000003ff00760000000000 R=ad924c74 P=04430f74 marker_crc=c816349e
bad.store 112 a2bcc730 34b1092a3b9c0dc1fd3a0a6b132c802d3df52f87489dba0f3795b5953f4cbdc3
bad.generation 112 88d8389f a23a6c128d5ec839c5c2db06e1cc50fbe601fd208f3dcd4ae0147b0fde595dbb
bad.base_crc 112 87bd4134 8d4bb3b26eb682445758763585f65a9ca310db29f5c212e6bc5b9a6a73d73ad5
bad.short_marker 111 b446721e 3a1049b267739b74e99a920b69c923bcd78a257ed6e45261cedcd1322954bf27
bad.short_tail 143 0203b6a6 11a683395f6d448f7e526774fb29712056dfa9f4d1b76f998aced717ca92c34a
```

Invalid pairing expectations: `bad.store` with E.current/E.base/E.tail rejects
mixed StoreId; `bad.generation` rejects g mismatch and cannot redirect file lookup;
`bad.base_crc` with actual E.base/E.tail rejects complete base CRC binding even
though CURRENT's own checksum is valid. The CRC of the declared base must equal
both the actual complete file and tail binding. `bad.short_marker` is exactly 111
bytes, missing the final `!`, and rejects exact length/trailer. `bad.short_tail`
with T.marker is missing its final `!`, shorter than durable O and an incomplete
record; with E.marker it still rejects corrupt suffix beyond the durable boundary.
No invalid case permits repair or fallback. Equivalent positive controls use
unmodified E and T. Flipping checksums without resealing would test checksum
rejection earlier, rather than the intended cross-file semantic rejection.

## 16. Performed documentation audit and limits

The self-audit checks the complete draft against the accepted header/frame codec,
physical-state/accounting behavior and P/R protocol. Field tables and independent
width/offset sums, exact checksum slices, generated golden output (two independent
CRC algorithms), empty and nonzero-S initial boundaries, binary/LRU ordering,
provenance and cross-file identity consistency are checked without persistence I/O.
Tracked diff whitespace and new-file whitespace, Markdown links/fences/tables,
exact authorized scope and unchanged accepted architecture/ROADMAP (all original
169 identifiers) are checked separately. The additive handoff preserves its entire
prior text. Performed results: four contiguous offset tables and independent Python struct
width calculations passed (184/96/112/112); all 23 artifact byte sequences
reproduced the documented CRC32C/SHA-256 output with two CRC algorithms; three
complete generation families and two transactions passed separate in-memory
identity/frame/EOF/boundary calculations; 36 local Markdown links/anchors in the
two authorized documents resolved; fences and table column counts passed.
Historical external URLs in the preserved handoff were not re-fetched.
The initial audit helper accidentally included the transaction subtable when
checking the tail-header table; bounding it to the header table corrected that
audit-only assertion, with no format/layout change.
No proposed parser/fault tests in this document are counted as executed tests.
The original design checkpoint performed no C++ build, Docker/CI execution,
codec work, persistence fixture/data access, archive, commit, push, PR or merge.
Independent specification acceptance is now reported by the user. This publication
only commits/pushes documentation and verifies existing Linux CI at the resulting
feature head; actual outcomes belong in the publication report. No V2 runtime
validation is claimed. Future implementation must satisfy section 14's tests.
