// nn::sm (service manager): every nn::sf service client resolves its handle
// through nn::sm::GetServiceHandle, which talks to the "sm:" named port over IPC.
//
// The host implements no IPC transport, so that call fails for every service.
// Titles that reach their services through the SDK's own nn::* wrappers never
// notice -- the host takes those wrappers over -- but a title whose sdk builds
// hipc clients directly (Mario Kart 8 Deluxe: nn::sf::hipc::ConnectToHipcService
// -> nn::sm::GetServiceHandle) treats the failure as fatal and aborts.
//
// This take-over hands out a host-side handle for any service name.  The handle
// is never used for real IPC -- the services the game actually reads data from
// are taken over at the nn::* level -- it only has to be non-zero and stable so
// the client's initialisation completes.
#include "hle.h"

// One handle per name, so a client that reconnects gets the same value back.
#define SM_MAX 64
static struct { char name[64]; int handle; } g_sm[SM_MAX];
static int g_nsm;

static uint32_t sm_handle(const char *name) {
    for (int i = 0; i < g_nsm; i++) if (!strcmp(g_sm[i].name, name)) return (uint32_t)g_sm[i].handle;
    if (g_nsm == SM_MAX) return 0;
    // A handle the SDK will treat as a valid session: non-zero, and distinct per
    // name so a mix-up is visible in a log rather than silent.
    int h = 0x1000 + g_nsm;
    strlcpy(g_sm[g_nsm].name, name, sizeof g_sm[0].name);
    g_sm[g_nsm].handle = h;
    g_nsm++;
    LOG("sm::GetServiceHandle(%s) -> 0x%x（宿主句柄，无真实 IPC）", name, h);
    return (uint32_t)h;
}

// nn::sm::GetServiceHandle(Handle* out, const char* name, size_t name_len)
HLE_FORCE("_ZN2nn2sm16GetServiceHandleEPNS_3svc6HandleEPKcm", "服务句柄：宿主对任何服务名给一个假句柄", Result, h_sm_GetHandle, uint32_t *out, const char *name, size_t len) {
    char buf[64] = {0};
    if (name) { size_t n = len && len < sizeof buf - 1 ? len : sizeof buf - 1; memcpy(buf, name, n); }
    uint32_t h = sm_handle(buf);
    if (out) *out = h;
    return h ? RES_OK : RES_FAIL;
}
