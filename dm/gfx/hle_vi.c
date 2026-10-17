// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"

// The SDK reads [layer + 0x20] for the layer id (nn::vi::GetLayerIdWithValidation,
// which main calls directly rather than through the take-over table) and
// [display + 0x30] for its vtbl, so these host objects must be big enough to
// hold those offsets.  They used to be 4 bytes, which read past the end into
// whatever followed them.
static struct { uint32_t magic; uint32_t pad[15]; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

// nn::vi::Initialize fills vi::detail::ServiceReferenceHolder, the static block
// nn::vi::GetService reads: {system, application, manager, blocking} at
// +8 / +0x10 / +0x18 / +0x20.  The host cannot run its body -- it builds nn::sf
// proxies over IPC to visrv, a service this host does not implement -- but
// leaving the block zeroed makes GetService return null, and every SDK function
// that takes a vi object then dereferences it (Mario Kart 8 Deluxe: SIGSEGV at
// sdk+0x1d4818, GetLayerIdWithValidation reached from main).
//
// So the take-over still does not run the body, but it plants host objects in
// the block afterwards: GetService then hands out something the SDK's own
// wrappers can call through.
void dm_vi_install_references(void);
HLE_FORCE("_ZN2nn2vi10InitializeEv", "显示服务初始化；宿主填好 GetService 读的静态块", void, h_vi_Init, void) {
    dm_vi_install_references();
}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "开默认显示；宿主给静态对象", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "建图层；宿主给静态对象", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "原生窗口句柄，交给 NVN 窗口 builder；宿主给静态对象", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64)
// nn::vi::SetLayerScalingMode(Layer*, ScalingMode): MK8DX calls it directly.
// The SDK's version asks GetService for the system display service and invokes
// virtual slot 0x90 on it, which is a visrv IPC the host does not implement.
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "图层缩放模式；宿主无合成器，忽略", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; (void)mode; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "图层裁切；宿主出图不裁", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer; LOG("vi::SetLayerCrop %d,%d %dx%d（忽略）", x, y, w, h); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "销毁图层；宿主静态对象", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "关显示；宿主静态对象", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "显示服务收尾；宿主无 vi 服务", void, h_vi_Fini, void) {}

// Plant the host's vi objects in the SDK's ServiceReferenceHolder block.
//
// The block's address is not an exported symbol, so find it by reading it out of
// nn::vi::GetService itself: the function is `adrp x9, page; add x9, x9, #imm;
// ldr x0, [x9, #8]`.  Doing it that way keeps this correct if the SDK build
// changes, instead of hard-coding an offset for one title.
static void *vi_ref_block(void) {
    module_t *ms = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    if (!ms) return NULL;
    uint32_t *f = module_sym(ms, "_ZN2nn2vi10GetServiceEv");
    if (!f) return NULL;
    // adrp x9, #page
    if ((f[0] & 0x9F000000) != 0x90000000) return NULL;
    int64_t imm = (((int64_t)(f[0] >> 5) & 0x7ffff) << 2) | ((f[0] >> 29) & 3);
    if (imm & (1 << 20)) imm -= 1 << 21;
    uint64_t page = ((uint64_t)(uintptr_t)f & ~0xfffull) + ((uint64_t)imm << 12);
    // add x9, x9, #imm12
    if ((f[1] & 0xFFC00000) != 0x91000000) return NULL;
    uint64_t block = page + ((f[1] >> 10) & 0xfff);
    return (void *)(uintptr_t)block;
}

// The object GetService hands out is a C++ interface: GetService stores the
// pointer, then calls [obj][0] (a reference-count / "get" thunk).  A plain data
// struct is not enough -- the SDK would jump to whatever the first word holds.
//
// Build a minimal but well-formed object instead: a vtbl whose slot 0 returns
// the object unchanged, and enough trailing words for the SDK's own wrappers to
// read.  Nothing behind it needs to work, because every vi entry point the game
// calls is taken over above; this only has to survive the SDK's internal calls.
static void *vi_self_thunk(void *self) { return self; }

void dm_vi_install_references(void) {
    void *b = vi_ref_block();
    if (!b) { LOG("vi::Initialize：找不到 GetService 的静态块，SDK 的 vi 调用会拿到空指针"); return; }
    static void *vtbl[1];
    static struct { void **vtbl; uint32_t magic; uint32_t pad[15]; } obj;
    vtbl[0] = (void *)vi_self_thunk;
    obj.vtbl = vtbl; obj.magic = 0x50534944;
    void **slot = (void **)((uint8_t *)b + 8);
    slot[0] = &obj;   // system service
    slot[1] = &obj;   // application service
    slot[2] = &obj;   // manager service
    slot[3] = &obj;   // blocking service
    LOG("vi::Initialize：GetService 的静态块 %p 已填好（宿主对象 %p）", b, (void *)&obj);
}
