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

#define MII_VTBL_SLOTS 32

static void *mii_vtbl[MII_VTBL_SLOTS];
// Every virtual the SDK might call: succeed with "no entries" and leave any
// out-parameters zeroed.  x0 is the out pointer for most of them, but a generic
// "return 0" is what the callers read: Get returns a count, IsFullDatabase a
// bool, BuildRandom a Result.
static uint64_t mii_stub(void) { return 0; }

static struct { void **vtbl; } g_mii_service;

static void mii_init_vtbl(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < MII_VTBL_SLOTS; i++) mii_vtbl[i] = (void *)mii_stub;
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

// Count of entries in the database: none.
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", "Mii 条目：0 个", Result, h_mii_Get, void **self, int *count, void *out, int index, int max) {
    (void)self; (void)out; (void)index; (void)max;
    if (count) *count = 0;
    return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", "Mii 最新条目：0 个", Result, h_mii_UpdateLatest, void **self, void *out, int max) {
    (void)self; (void)out; (void)max; return RES_OK;
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
