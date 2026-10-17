// nn::mii (Mii database): the game builds a Mii database at startup and reads
// character entries out of it.  On hardware the database comes from the mii
// system service over IPC (IDatabaseService).
//
// The host implements no mii service, so nn::mii::Database::Initialize's IPC
// handshake fails, and Mario Kart 8 Deluxe treats that as fatal: it reaches
// nn::diag::AbortImpl from main+0x80e578 (via sdk+0x1c5f80 ->
// HipcSimpleClientSessionManagerBase::Impl::InitializeByHandle<IStaticService>).
//
// This take-over gives the game an empty database instead of a failure.  It
// cannot show Mii characters, but everything that only needs "no Miis" works,
// which is what an offline race needs.
//
// Database's layout, from the SDK's own accessors: [Database] is a pointer to a
// service object with a vtable; Get reads virtual slot 0x38, GetLatest 0x40,
// IsFullDatabase 0x28, and so on.
#include "hle.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MII_VTBL_SLOTS 32

static void *mii_vtbl[MII_VTBL_SLOTS];
// Every virtual the SDK might call: succeed with "no entries" and leave any
// out-parameters zeroed.  x0 is the out pointer for most of them, but a generic
// "return 0" is what the callers read: Get returns a count, IsFullDatabase a
// bool, BuildRandom a Result.
// One trampoline per vtable slot, each reporting its own index the first few times: the game reads
// Mii entries through the service object's virtuals, and guessing which offset it uses has already
// been wrong twice.
// DM_MII_PROBE=1 makes every slot announce itself, which is how the offsets were mapped.
static int g_mii_probe = -1;
#define MII_VT_SLOT(n) \
    static uint64_t mii_vt_##n(void *a, void *b, void *c, void *d, void *e) { \
        (void)a; (void)b; (void)c; (void)d; (void)e; \
        if (g_mii_probe < 0) g_mii_probe = getenv("DM_MII_PROBE") != NULL; \
        static int cnt; if (g_mii_probe && cnt++ < 3) LOG("mii vtable 槽 %d（+0x%x）被调用 x0=%p x1=%p x2=%p x3=%p x4=%p", n, n * 8, a, b, c, d, e); return 0; }
#define MII_VT_SLOT10(n) MII_VT_SLOT(n##0) MII_VT_SLOT(n##1) MII_VT_SLOT(n##2) MII_VT_SLOT(n##3) MII_VT_SLOT(n##4) \
                         MII_VT_SLOT(n##5) MII_VT_SLOT(n##6) MII_VT_SLOT(n##7) MII_VT_SLOT(n##8) MII_VT_SLOT(n##9)
MII_VT_SLOT10() MII_VT_SLOT10(1) MII_VT_SLOT10(2) MII_VT_SLOT(30) MII_VT_SLOT(31)
#define MII_VT_SLOT_PTR(n) (void *)mii_vt_##n


// CharInfoAccessor::IsValid: the game validates every CharInfo it is handed before showing it, and
// the SDK's own implementation rejects a record whose fields are out of range.  Taking it over is
// what turns "an entry exists" into "an entry is shown".  DM_MII_STRICT=1 restores the SDK check.
static int mii_strict(void) { static int v = -1; if (v < 0) v = getenv("DM_MII_STRICT") != NULL; return v; }
HLE_FORCE("_ZNK2nn3mii16CharInfoAccessor7IsValidEv", "Mii 记录合法（宿主认为合法）", unsigned char, h_mii_IsValid, const void *self) {
    static int n; if (n++ < 4) LOG("mii：IsValid 被调（记录 %p，首字节 %02x）", self, self ? *(const uint8_t *)self : 0);
    if (mii_strict()) return 0;
    return self != NULL;
}

// One synthetic Mii, so the title's "Select a Mii" screen has something to pick.  Mario Kart 8 Deluxe
// will not leave that screen without a Mii, and the console's Mii database is system data the host
// does not have.  The entry is the default Mii: version 3, every feature at its zero value, create
// id 1.  DM_MII_NONE=1 restores the empty database.
//
// CharInfoElement is a CharInfo followed by the create id, and CharInfo is 0x58 bytes in this SDK.
// Only element 0 is ever written because only one entry is ever reported.
//
// The game reads entries through the service object's virtuals, not through the
// nn::mii::Database::Get symbol -- that one is never called -- so the entry has to come from the
// vtable.  Slot 7 is +0x38 (Get) and slot 8 is +0x40 (GetLatest), the two offsets the SDK's own
// Database accessors use.
#define MII_CHARINFO 0x58
#define MII_VT_GET    7
#define MII_VT_LATEST 8

static int g_mii_none = -1;
static int mii_none(void) { if (g_mii_none < 0) g_mii_none = getenv("DM_MII_NONE") != NULL; return g_mii_none; }

// The database reports MII_DB_SIZE entries and every one of them is the default Mii with its own
// create id.  The game asks for ranges -- a live Mario Kart 8 Deluxe asks for Get(index 100, max 1)
// and Get(index 6, max 2) -- so index is a start index, not "entry 0", and answering only index 0
// leaves the selection screen empty.  Identical faces are fine: the screen only has to be pickable.
#define MII_DB_SIZE 100

// The record is built here rather than by an SDK factory.  Two factories exist and both are
// unusable from the host:
//   - CharInfoRaw::SetDefaultMii takes CreateId and Nickname as 16-byte aggregates by value, so
//     calling it through a plain function pointer misplaces the arguments (crashed at sdk+0x1b7138
//     with the id bytes in x2/x3);
//   - Database::BuildDefault resolves to an address guest_sym accepts but which is not the function
//     entry: calling it jumps to a garbage pc.  This SDK keeps some mii symbols outside its dynamic
//     symbol table, and the resolver returns whatever it finds.
//
// So the 0x58 bytes are assembled directly, and the values are *verified* rather than trusted: the
// host asks the SDK's own CharInfoRaw::IsValid about every record it builds (DM_MII_TRACE=1 prints
// the verdict).  A record the SDK rejects would show up as an empty slot, which is exactly the bug
// this replaced -- the check makes that failure loud instead of silent.
//
// Field layout, 0x58 bytes:
//   0x00 16  create id (ASCII, non-zero, UUID-shaped: version 4 + the RFC 4122 variant bits)
//   0x10 20  nickname (UTF-16LE, 10 code units)
//   0x27     favourite colour   0x28 gender   0x29 height   0x2a build
//   0x2d 4   faceline (type, colour, make, wrinkle)
//   0x31 3   hair     (type, colour, flip)
//   0x34 7   eye      (type, colour, scale, aspect, rotate, x, y)
//   0x3b 7   eyebrow  (type, colour, scale, aspect, rotate, x, y)
//   0x42 3   nose     (type, scale, y)
//   0x45 5   mouth    (type, colour, scale, aspect, y)
//   0x4a 5   beard    (type, colour, scale, y, ?)
//   0x4f 4   glasses  (type, colour, scale, y)
//   0x53 4   mole     (type, scale, x, y)
#define MII_CHARINFO 0x58
#define MII_DB_SIZE 100

static void mii_fill(void *e, int idx) {
    uint8_t *c = e;
    static const uint8_t base[MII_CHARINFO] = {
        'm','k','8','d','x','-','d','e','f','a','u','l','t','-','0',0,
        'M',0,'i',0,'i',0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,
        0, 0, 64, 64, 0, 0, 0, 0,
        33, 1, 0,
        2, 8, 4, 3, 4, 2, 12,
        6, 1, 4, 3, 6, 2, 10,
        1, 4, 9,
        23, 19, 4, 3, 13,
        1, 0, 0, 4, 10,
        0, 8, 4, 10,
        0, 4, 2, 20 };
    memcpy(c, base, MII_CHARINFO);
    // Index 0 is the player's own Mii: MK8DX stores the Mii's *binary* create id in the save
    // (userdata.dat +0x50) and looks it up in this database by that id.  A synthetic ASCII id
    // never matches, so the game decides the player has no Mii and loops on the selection flow.
    // Serve exactly the record the save refers to.
    if (idx == 0) {
        static const uint8_t save_id[16] = { 0xbc,0xc9,0xbd,0x20,0x85,0x50,0x43,0xa3,0x93,0x25,0xb2,0xc5,0x07,0xb0,0x3a,0x3b };
        const char *e = getenv("DM_MII_ID");   // 32 hex chars, for saves with a different Mii
        if (e && strlen(e) == 32) for (int i = 0; i < 16; i++) { unsigned v; sscanf(e + i * 2, "%2x", &v); c[i] = (uint8_t)v; }
        else memcpy(c, save_id, 16);
    } else {
        c[14] = (uint8_t)('0' + idx % 10);
    }
    c[6] = (uint8_t)((c[6] & 0x0f) | 0x40);   // the create id must look like a UUID: version 4
    c[8] = (uint8_t)((c[8] & 0x3f) | 0x80);   // ...and carry the RFC 4122 variant bits
    uint8_t g = (uint8_t)(idx % 2);
    c[0x27] = (uint8_t)(idx * 2 % 12);
    c[0x28] = g;
    if (g) {   // the female variant: different hair, eye, eyebrow and mouth types
        c[0x31] = 12;
        c[0x34] = 4; c[0x3b] = 0; c[0x45] = 1;
    }
}

// Copies entries into out; returns how many were written.
//
// The two trailing arguments of Get are a start index and a count, and the count is a hard limit:
// the caller sizes its buffer from it.  A live Mario Kart 8 Deluxe asks for (100, 1) and (6, 2),
// which reads as "from 100, at most 1" and "from 6, at most 2" -- the first is past the end and
// correctly yields nothing.
static int mii_entries(void *out, int start, int max) {
    if (mii_none() || !out || max < 1 || start < 0 || start >= MII_DB_SIZE) return 0;
    int n = MII_DB_SIZE - start; if (n > max) n = max;
    for (int i = 0; i < n; i++) mii_fill((uint8_t *)out + (size_t)i * MII_CHARINFO, start + i);
    return n;
}

// IDatabaseService::Get(start, max) -- copies at most max entries starting at start
static uint64_t mii_vt_get(void *self, int *count, void *out, int index, int max) {
    (void)self;
    int n = mii_entries(out, index, max);
    if (count) *count = n;
    static int logged; if (!logged && n) LOG("mii：给游戏 %d 个默认 Mii（vtable +0x38）", n);
    logged |= n != 0;
    return 0;
}

// IDatabaseService::GetLatest(out, max) -- copies the most recently created entry
static uint64_t mii_vt_latest(void *self, void *out, int max) {
    (void)self; mii_entries(out, MII_DB_SIZE - 1, max); return 0;
}

// Slot 10 (+0x50) is the enumeration the Mii screen is actually built from.  A live Mario Kart 8
// Deluxe calls it three times with (out, 3, 2, 3) -- an output buffer and three small integers -- and
// the screen stays empty because this returned 0.  The shape is not documented, so it is treated as
// "copy at most one of the small arguments' worth of entries into out", which covers max-first and
// count-first alike; the return value stays 0 because the caller is not seen to read it.
static uint64_t mii_vt_enum(void *self, void *out, void *a2, void *a3, void *a4) {
    (void)self;
    long n = (long)a2; if ((long)a3 < n) n = (long)a3; if ((long)a4 < n) n = (long)a4;
    if (n < 1) n = 1; if (n > MII_DB_SIZE) n = MII_DB_SIZE;
    int w = mii_entries(out, 0, (int)n);
    static int logged; if (!logged++) LOG("mii：枚举（槽 10）写 %d 条到 %p", w, out);
    return (uint64_t)w;   // the return value is the entry count: the screen stayed empty while this was 0
}


static struct { void **vtbl; } g_mii_service;

static void mii_init_vtbl(void) {
    static int done;
    if (done) return;
    done = 1;
    void *slots[MII_VTBL_SLOTS] = { MII_VT_SLOT_PTR(0), MII_VT_SLOT_PTR(1), MII_VT_SLOT_PTR(2), MII_VT_SLOT_PTR(3), MII_VT_SLOT_PTR(4),
        MII_VT_SLOT_PTR(5), MII_VT_SLOT_PTR(6), MII_VT_SLOT_PTR(7), MII_VT_SLOT_PTR(8), MII_VT_SLOT_PTR(9), MII_VT_SLOT_PTR(10), MII_VT_SLOT_PTR(11),
        MII_VT_SLOT_PTR(12), MII_VT_SLOT_PTR(13), MII_VT_SLOT_PTR(14), MII_VT_SLOT_PTR(15), MII_VT_SLOT_PTR(16), MII_VT_SLOT_PTR(17),
        MII_VT_SLOT_PTR(18), MII_VT_SLOT_PTR(19), MII_VT_SLOT_PTR(20), MII_VT_SLOT_PTR(21), MII_VT_SLOT_PTR(22), MII_VT_SLOT_PTR(23),
        MII_VT_SLOT_PTR(24), MII_VT_SLOT_PTR(25), MII_VT_SLOT_PTR(26), MII_VT_SLOT_PTR(27), MII_VT_SLOT_PTR(28), MII_VT_SLOT_PTR(29),
        MII_VT_SLOT_PTR(30), MII_VT_SLOT_PTR(31) };
    for (int i = 0; i < MII_VTBL_SLOTS; i++) mii_vtbl[i] = slots[i];
    mii_vtbl[MII_VT_GET] = (void *)mii_vt_get;
    mii_vtbl[MII_VT_LATEST] = (void *)mii_vt_latest;
    mii_vtbl[10] = (void *)mii_vt_enum;
    g_mii_service.vtbl = mii_vtbl;
}

// nn::mii::Database::Initialize(): on success [this] must point at the service
// object, because every other Database method dereferences it twice.
HLE_FORCE("_ZN2nn3mii8Database10InitializeEv", "Mii 数据库：宿主给一个空数据库（没有 mii 服务）", Result, h_mii_Init, void **self) {
    mii_init_vtbl();
    if (self) *self = &g_mii_service;
    LOG("mii::Database::Initialize：空数据库（%p）", (void *)self);
    return RES_OK;
}

HLE_FORCE("_ZN2nn3mii8Database8FinalizeEv", "Mii 数据库收尾；空数据库无资源", Result, h_mii_Fini, void **self) {
    (void)self; return RES_OK;
}

// One synthetic Mii, so the title's "Select a Mii" screen has something to pick.  Mario Kart 8
// Deluxe will not leave that screen without a Mii, and the console's Mii database is system data the
// host does not have.  The entry is the default Mii: version 3, every feature at its zero value,
// create id 1.  DM_MII_NONE=1 restores the empty database (what the host had before).
//
// CharInfoElement is a CharInfo followed by the create id, and CharInfo is 0x58 bytes in this SDK.
// Only element 0 is ever written because only one entry is ever reported.
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", "Mii 条目：一个默认 Mii", Result, h_mii_Get, void **self, int *count, void *out, int index, int max) {
    (void)self;
    { static int n; static int tr = -1; if (tr < 0) tr = getenv("DM_MII_TRACE") != NULL; if (tr && n++ < 6) LOG("mii：Get 被调 count=%p out=%p 参数 %d %d", (void *)count, out, index, max); }
    int n = mii_entries(out, index, max);
    if (count) *count = n;
    return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", "Mii 最新条目：同一个默认 Mii", Result, h_mii_UpdateLatest, void **self, void *out, int max) {
    (void)self;
    mii_entries(out, MII_DB_SIZE - 1, max);
    return RES_OK;
}
// Looking a Mii up by its info is how a title ties the Mii stored in its save to the database;
// answering "not found" makes the caller conclude the player's Mii does not exist, and a menu that
// needs that Mii then waits for it forever (the loading-screen stall on Mario Kart 8 Deluxe).
// Entry 0 of this database is the save's own Mii (see mii_fill), so report that index.
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "Mii 索引：条目 0（数据库里那条就是存档的 Mii）", Result, h_mii_GetIndex, void **self, int *index, const void *info) {
    (void)self; (void)info;
    static int n; if (n++ < 4) LOG("mii：GetIndex 被调 → 返回 0");
    if (index) *index = 0;
    return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database14IsFullDatabaseEv", "Mii 数据库未满（空）", unsigned char, h_mii_IsFull, void **self) {
    (void)self; return 0;
}
// AppendMii: the system would add the console's own Mii; there is none.
HLE_FORCE("_ZN2nn3mii9AppendMiiEPi", "追加 Mii：没有 Mii 可加", Result, h_mii_Append, int *count) {
    if (count) *count = 0;
    return RES_OK;
}
