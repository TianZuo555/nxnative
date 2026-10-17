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
        static int cnt; if (g_mii_probe && cnt++ < 3) LOG("mii vtable 槽 %d（+0x%x）被调用", n, n * 8); return 0; }
#define MII_VT_SLOT10(n) MII_VT_SLOT(n##0) MII_VT_SLOT(n##1) MII_VT_SLOT(n##2) MII_VT_SLOT(n##3) MII_VT_SLOT(n##4) \
                         MII_VT_SLOT(n##5) MII_VT_SLOT(n##6) MII_VT_SLOT(n##7) MII_VT_SLOT(n##8) MII_VT_SLOT(n##9)
MII_VT_SLOT10() MII_VT_SLOT10(1) MII_VT_SLOT10(2) MII_VT_SLOT(30) MII_VT_SLOT(31)
#define MII_VT_SLOT_PTR(n) (void *)mii_vt_##n

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
static void mii_fill(void *e, int idx) {
    memset(e, 0, MII_CHARINFO);
    ((uint8_t *)e)[0x00] = 3;                              // CharInfo version
    *(uint64_t *)((uint8_t *)e + 0x08) = (uint64_t)idx + 1; // create id: never zero, the game keys on it
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
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "Mii 索引：找不到（-1）", Result, h_mii_GetIndex, void **self, int *index, const void *info) {
    (void)self; (void)info; if (index) *index = -1; return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database14IsFullDatabaseEv", "Mii 数据库未满（空）", unsigned char, h_mii_IsFull, void **self) {
    (void)self; return 0;
}
// AppendMii: the system would add the console's own Mii; there is none.
HLE_FORCE("_ZN2nn3mii9AppendMiiEPi", "追加 Mii：没有 Mii 可加", Result, h_mii_Append, int *count) {
    if (count) *count = 0;
    return RES_OK;
}
