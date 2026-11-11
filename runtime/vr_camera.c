#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "fs.h"
#include "vk.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define DEG 0.017453292519943295f
#define UNITS_PER_M 1.0f   // game units in one metre of head movement, before TOTK_VR_MOVE
#define DEFAULT_MOVE 1.0f
#define DEFAULT_EYE_UP 0.1f
#define MAX_MOVE_M 0.5f
#define DEFAULT_VIEWS 1u

static struct {
    unsigned seen;
    int level, log, has_fake;
    float fov;        // TOTK_VR_FOV: vertical degrees of the main view; 0: the headset's, or the game's without one
    float p00, p11;   // the main view's projection as last widened; 0: not widened
    uint32_t views;   // bit n: view index n follows the head
    int first;        // TOTK_VR_FIRST: 1: the eye is at Link's head and his head is hidden
    int toggled;      // F9 pressed an odd number of times; not saved
    float eye_up;     // TOTK_VR_EYE_UP: metres from Link's head bone (his neck's top) up to the eye
    float move, fake_yaw, fake_pitch, fake_pos[3], fake_sway;   // fake_sway: degrees of a 1 Hz yaw swing
} K = {.seen = ~0u};

static struct {
    uint64_t caller[16], calls[16];
    unsigned view[16];
    uint64_t total;
} L;

static void settings(void) {
    unsigned v = cfg_version();
    if (v == K.seen) return;
    const char *l = cfg_get("TOTK_VR_LEVEL"), *s = cfg_get("TOTK_VR_MOVE"), *o = cfg_get("TOTK_VR_VIEWS");
    const char *y = getenv("OOT_VR_FAKE_YAW"), *p = getenv("OOT_VR_FAKE_PITCH");
    K.level = !(l && !strcmp(l, "0"));
    K.move = s && *s ? (float)atof(s) : DEFAULT_MOVE;
    K.fake_yaw = y ? (float)atof(y) * DEG : 0.0f;
    K.fake_pitch = p ? (float)atof(p) * DEG : 0.0f;
    const char *fs = getenv("OOT_VR_FAKE_SWAY");
    K.fake_sway = fs ? (float)atof(fs) * DEG : 0.0f;
    const char *fp = getenv("OOT_VR_FAKE_POS");
    memset(K.fake_pos, 0, sizeof K.fake_pos);
    if (fp) sscanf(fp, "%f,%f,%f", &K.fake_pos[0], &K.fake_pos[1], &K.fake_pos[2]);
    K.has_fake = y || p || fp || fs;
    const char *fi = cfg_get("TOTK_VR_FIRST");
    K.first = (fi && !strcmp(fi, "1")) ^ K.toggled;
    const char *eu = cfg_get("TOTK_VR_EYE_UP");
    K.eye_up = eu && *eu ? (float)atof(eu) : DEFAULT_EYE_UP;
    K.log = getenv("OOT_VR_CAMLOG") != NULL;
    K.views = o && *o ? (uint32_t)strtoul(o, NULL, 0) : DEFAULT_VIEWS;
    const char *fv = cfg_get("TOTK_VR_FOV");
    K.fov = fv && *fv ? (float)atof(fv) * DEG : 0.0f;
    K.seen = v;
}

static float norm3(float v[3]) {
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) v[0] /= n, v[1] /= n, v[2] /= n;
    return n;
}

// m: 3 rows of 4, world to camera, the camera looks along -z. fh: the head's forward in the play space (-z ahead, +y up,
// no roll); ph: the head's place in it, metres.
void vr_camera_view(float m[12], const float fh[3], const float ph[3], float move, int level, const float *eye) {
    float R[3], U[3], B[3], t[3] = {m[3], m[7], m[11]}, pos[3];
    for (int k = 0; k < 3; k++) R[k] = m[k], U[k] = m[4 + k], B[k] = m[8 + k];
    for (int k = 0; k < 3; k++) pos[k] = eye ? eye[k] : -(R[k] * t[0] + U[k] * t[1] + B[k] * t[2]);
    float flat[3] = {B[0], 0, B[2]};
    if (level && norm3(flat) > 1e-3f) {
        memcpy(B, flat, sizeof B);
        U[0] = 0, U[1] = 1, U[2] = 0;
        R[0] = B[2], R[1] = 0, R[2] = -B[0];
    }
    float d[3] = {ph[0], ph[1], ph[2]};
    const float dn = norm3(d), dm = fminf(dn, MAX_MOVE_M);
    float hr[3] = {-fh[2], 0, fh[0]};
    if (norm3(hr) <= 1e-6f) hr[0] = 1, hr[1] = 0, hr[2] = 0;
    const float hu[3] = {hr[1] * fh[2] - hr[2] * fh[1], hr[2] * fh[0] - hr[0] * fh[2], hr[0] * fh[1] - hr[1] * fh[0]};
    const float hb[3] = {-fh[0], -fh[1], -fh[2]};
    const float *hv[3] = {hr, hu, hb};
    const float s = UNITS_PER_M * move;
    for (int k = 0; k < 3; k++) pos[k] += (d[0] * R[k] + d[1] * U[k] + d[2] * B[k]) * dm * s;
    for (int i = 0; i < 3; i++) {
        float row[3];
        for (int k = 0; k < 3; k++) row[k] = hv[i][0] * R[k] + hv[i][1] * U[k] + hv[i][2] * B[k];
        for (int k = 0; k < 3; k++) m[i * 4 + k] = row[k];
        m[i * 4 + 3] = -(row[0] * pos[0] + row[1] * pos[1] + row[2] * pos[2]);
    }
}

static void log_view(uint64_t caller, unsigned view, const float *m) {
    int i = 0;
    while (i < 16 && L.caller[i] && !(L.caller[i] == caller && L.view[i] == view)) i++;
    if (i < 16) L.caller[i] = caller, L.view[i] = view, L.calls[i]++;
    if (++L.total % 700) return;
    for (i = 0; i < 16 && L.caller[i]; i++)
        rt_log("vr camera: set view %u from main+%#llx, %llu calls", L.view[i], (unsigned long long)(L.caller[i] - 0x7000000000ull),
               (unsigned long long)L.calls[i]);
    rt_log("vr camera: last view x %.3f %.3f %.3f back %.3f %.3f %.3f t %.2f %.2f %.2f", m[0], m[1], m[2], m[8], m[9], m[10], m[3],
           m[7], m[11]);
}

static uint64_t presents;
static unsigned ev_callers, ev_seen;   // this present's main view callers, by index; every index seen
static uint64_t cutscene_at;           // the present a cutscene camera was last seen at
#define CUTSCENE_HOLD 45               // presents first person stays off after the last cutscene camera
#define CUTSCENE_FRAMES 6              // presents in a row the listener view must be away from Link
#define CUTSCENE_SIDE 0.6f             // metres beside Link's feet
#define CUTSCENE_UP 0.3f               // metres from its usual height above them
#define LISTENER_CALLER 0x700033dd88ull   // this main view sits right above Link's feet in play, and at the camera in a cutscene

static int in_cutscene(void) { return cutscene_at && presents <= cutscene_at + CUTSCENE_HOLD; }

static float feet_hist[4][3];   // Link's feet at the last 4 presents: that view follows them a frame or two late
static struct {
    float fwd[3];   // the head's forward as the main view last used it
    uint64_t at;    // the present it was used at
} look;
static uint64_t controller;   // the camera controller the game updated last
static uint64_t target;       // the actor the camera follows

// Link's feet as the camera follows them: controller +0x10 owner, owner +0x420, +0x3e8 the camera's tracking data, +0x348
static const float *link_feet(void) {
    const uint64_t owner = *(const uint64_t *)(uintptr_t)(controller + 0x10);
    if (!owner) return NULL;
    const uint64_t a = *(const uint64_t *)(uintptr_t)(owner + 0x420);
    if (!a) return NULL;
    const uint64_t b = *(const uint64_t *)(uintptr_t)(a + 0x3e8);
    return b ? (const float *)(uintptr_t)(b + 0x348) : NULL;
}

#define HEAD_BONE 103
#define HEAD_SCALE 0.001f

// Link's model: target +0x8, +0x70 its parts, +0xb0 the body; the body's skeleton at +0x398: resource at +0, world matrices
// (4 columns of 4 floats per bone, from a local origin) at +0x20
static uint64_t link_model(void) {
    if (!target) return 0;
    const uint64_t x = *(const uint64_t *)(uintptr_t)(target + 0x8);
    const uint64_t parts = x ? *(const uint64_t *)(uintptr_t)(x + 0x70) : 0;
    return parts ? *(const uint64_t *)(uintptr_t)(parts + 0xb0) : 0;
}

static struct {
    uint64_t skeleton;
    int n;
    uint16_t bone[128];   // Head and every bone below it
    float head[3];        // the head bone in the world, last frame
    int has_head;
} H;

// the skeleton resource: bone count at +0x38, bones at +0x10 (0x58 each, parent index at +0x22)
static void head_bones(uint64_t skeleton) {
    if (H.skeleton == skeleton) return;
    H.skeleton = skeleton, H.n = 0;
    const unsigned count = *(const uint16_t *)(uintptr_t)(skeleton + 0x38);
    const uint64_t bones = *(const uint64_t *)(uintptr_t)(skeleton + 0x10);
    if (count <= HEAD_BONE || count > 1024 || *(const uint16_t *)(uintptr_t)(bones + HEAD_BONE * 0x58 + 0x22) != HEAD_BONE - 1) {
        rt_log("vr camera: Link's skeleton has %u bones and no head bone where expected", count);
        return;
    }
    for (unsigned b = 0; b < count && H.n < 128; b++) {
        unsigned k = b, depth = 0;
        while (k != HEAD_BONE && k < count && depth++ < 64) k = *(const uint16_t *)(uintptr_t)(bones + k * 0x58 + 0x22);
        if (k == HEAD_BONE) H.bone[H.n++] = (uint16_t)b;
    }
    rt_log("vr camera: Link's skeleton %u bones, %d in the head", count, H.n);
}

static void host_skeleton(Cpu *c) {
    c->x[30] = c->x[16];
    const uint64_t model = link_model();
    const uint64_t skeleton = model ? *(const uint64_t *)(uintptr_t)(model + 0x398) : 0;
    if (!skeleton || c->x[0] != skeleton) return;
    float *w = (float *)(uintptr_t)*(const uint64_t *)(uintptr_t)(skeleton + 0x20);
    const float *feet = (const float *)(uintptr_t)(target + 0x31c), *h = w + 16 * HEAD_BONE;
    for (int k = 0; k < 3; k++) H.head[k] = feet[k] + h[12 + k] - w[12 + k];
    H.has_head = 1;
    static int test_hide = -1;   // LOCAL TEST (Ocarina): OOT_VR_TEST_HIDE=1 hides the head in the game's own camera
    if (test_hide < 0) test_hide = getenv("OOT_VR_TEST_HIDE") != NULL;
    if ((!K.first || in_cutscene()) && !test_hide) return;
    head_bones(*(const uint64_t *)(uintptr_t)skeleton);
    const float t[3] = {h[12], h[13], h[14]};
    for (int i = 0; i < H.n; i++) {
        float *m = w + 16 * H.bone[i];
        for (int k = 0; k < 12; k++) m[k] = 0;
        m[0] = m[5] = m[10] = HEAD_SCALE;
        m[12] = t[0], m[13] = t[1], m[14] = t[2];
    }
}

// m: the controller's view direction from Link's eye; eye: his head bone (else his feet), eye_up higher
static int first_person(float m[12], float eye[3], float eye_up) {
    if (!controller) return 0;
    const float *e = (const float *)(uintptr_t)(controller + 0xd8), *t = (const float *)(uintptr_t)(controller + 0x100);
    const float *feet = H.has_head ? H.head : link_feet();
    if (!feet) return 0;
    float B[3] = {e[0] - t[0], e[1] - t[1], e[2] - t[2]};
    if (!(norm3(B) > 1e-4f)) return 0;
    float R[3] = {B[2], 0, -B[0]};
    if (!(norm3(R) > 1e-4f)) R[0] = 1, R[1] = 0, R[2] = 0;
    const float U[3] = {B[1] * R[2] - B[2] * R[1], B[2] * R[0] - B[0] * R[2], B[0] * R[1] - B[1] * R[0]};
    const float *rows[3] = {R, U, B};
    for (int k = 0; k < 3; k++) eye[k] = feet[k];
    eye[1] += eye_up;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) m[i * 4 + k] = rows[i][k];
        m[i * 4 + 3] = -(rows[i][0] * eye[0] + rows[i][1] * eye[1] + rows[i][2] * eye[2]);
    }
    return 1;
}

static _Alignas(16) float turned[32][2][12];   // per view index: the copies x2 and x4 point at
static _Alignas(16) uint8_t proj_copy[0x100];    // the main view's projection object with a wider matrix

int vr_camera_projection(float *p00, float *p11) {
    if (!(K.p00 > 0)) return 0;
    *p00 = K.p00, *p11 = K.p11;
    return 1;
}

#define MAIN_PROJECTION_VTABLE 0x7004bf8c70ull

// x3: the projection object. Its 4x4 matrix is at +0xc and again at +0x4c, rows of 4 floats.
static void widen(Cpu *c) {
    const uint8_t *src = (const uint8_t *)(uintptr_t)c->x[3];
    if (*(const uint64_t *)src != MAIN_PROJECTION_VTABLE) return;
    float game00, game11, tx, ty;
    memcpy(&game00, src + 0xc, 4), memcpy(&game11, src + 0xc + 20, 4);
    if (!(game00 > 0) || !(game11 > 0)) return;
    const int headset = vr_headset_tan(&tx, &ty);
    if (K.fov > 0) {
        const float ratio = headset ? tx / ty : game11 / game00;
        ty = tanf(K.fov * 0.5f), tx = ty * ratio;
    } else if (!headset) {
        K.p00 = K.p11 = 0;
        return;
    }
    memcpy(proj_copy, src, sizeof proj_copy);
    const float p00 = 1.0f / tx, p11 = 1.0f / ty;
    for (int o = 0xc; o <= 0x4c; o += 0x40) memcpy(proj_copy + o, &p00, 4), memcpy(proj_copy + o + 20, &p11, 4);
    K.p00 = p00, K.p11 = p11;
    c->x[3] = (uint64_t)(uintptr_t)proj_copy;
}

static void set_view(Cpu *c) {
    settings();
    const unsigned view = (unsigned)c->x[1];
    float *m2 = (float *)(uintptr_t)c->x[2], *m4 = (float *)(uintptr_t)c->x[4];
    if (K.log) {
        log_view(c->x[16], view, m2);
        if (L.total % 3000 == 1)
            rt_log("vr camera: view %u x2 %#llx x4 %#llx, x4 %s", view, (unsigned long long)c->x[2], (unsigned long long)c->x[4],
                   m2 == m4 ? "is x2" : memcmp(m2, m4, 48) ? "differs" : "holds the same matrix");
    }
    if (view == 0 && c->x[16] == LISTENER_CALLER && target) {
        const float *m = (const float *)(uintptr_t)c->x[2], *feet = (const float *)(uintptr_t)(target + 0x31c);
        float e[3];
        for (int j = 0; j < 3; j++) e[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
        float side = 1e30f, up = 0;
        for (int i = 0; i < 4; i++) {
            const float *f = i ? feet_hist[i] : feet, dx = e[0] - f[0], dz = e[2] - f[2], d = sqrtf(dx * dx + dz * dz);
            if (d < side) side = d, up = e[1] - f[1];
        }
        static float usual_up = -1;
        static int odd;
        const int away = side > CUTSCENE_SIDE || (usual_up >= 0 && fabsf(up - usual_up) > CUTSCENE_UP);
        odd = away ? odd + 1 : 0;
        if (!away && !in_cutscene()) usual_up = usual_up < 0 ? up : usual_up + (up - usual_up) * 0.05f;
        if (K.log && presents % 15 == 0)
            rt_log("vr camera: present %llu listener %.2f m beside the feet, %+.2f up (usual %+.2f)", (unsigned long long)presents, side, up, usual_up);
        if (odd >= CUTSCENE_FRAMES) {
            if (!in_cutscene()) rt_log("vr camera: cutscene camera, first person paused");
            cutscene_at = presents;
        }
    }
    if (view == 0) {
        static uint64_t callers[8];
        int k = 0;
        while (k < 8 && callers[k] && callers[k] != c->x[16]) k++;
        if (k < 8) callers[k] = c->x[16], ev_callers |= 1u << k;
        if (K.log && k < 8 && !(ev_seen >> k & 1)) ev_seen |= 1u << k, rt_log("vr event: main view caller %d is main+%#llx", k, (unsigned long long)(c->x[16] - 0x7000000000ull));
        if (K.log && controller && presents % 30 == 0) {
            const float *m = (const float *)(uintptr_t)c->x[2], *e = (const float *)(uintptr_t)(controller + 0xd8);
            float pos[3];
            for (int j = 0; j < 3; j++) pos[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
            const float dx = pos[0] - e[0], dy = pos[1] - e[1], dz = pos[2] - e[2];
            rt_log("vr event: present %llu caller %d eye %.2f %.2f %.2f, %.2f m from the controller's", (unsigned long long)presents, k, pos[0], pos[1], pos[2],
                   sqrtf(dx * dx + dy * dy + dz * dz));
        }
    }
    if (view >= 32 || !(K.views >> view & 1)) return;
    static const char *only = (const char *)1;   // LOCAL TEST (Ocarina): OOT_VR_ONLY_CALLER=<n> turns only main view caller n
    if (only == (const char *)1) only = getenv("OOT_VR_ONLY_CALLER");
    if (only && view == 0) {
        static uint64_t seen[8];
        int k = 0;
        while (k < 8 && seen[k] && seen[k] != c->x[16]) k++;
        if (k < 8 && !seen[k]) seen[k] = c->x[16];
        if (k != atoi(only)) return;
    }
    float fh[3], ph[3] = {0, 0, 0};
    if (K.has_fake) {
        const float yaw = K.fake_yaw + K.fake_sway * sinf((float)(presents % 30) * (6.2831853f / 30.0f));
        fh[0] = -sinf(yaw) * cosf(K.fake_pitch), fh[1] = sinf(K.fake_pitch), fh[2] = -cosf(yaw) * cosf(K.fake_pitch);
        memcpy(ph, K.fake_pos, sizeof ph);
    } else if (!vr_head_pose(fh, ph)) {
        return;
    }
    if (view == 0) {
        widen(c);
        memcpy(look.fwd, fh, sizeof look.fwd);
        look.at = presents;
    }
    float base[12], eye[3];
    const int first = K.first && !in_cutscene() && first_person(base, eye, K.eye_up);
    if (K.log && view == 0 && presents % 60 == 0 && controller) {
        const float *e = (const float *)(uintptr_t)(controller + 0xd8), *t = (const float *)(uintptr_t)(controller + 0x100);
        rt_log("vr camera: view 0 from main+%#llx, controller eye %.2f %.2f %.2f at %.2f %.2f %.2f%s", (unsigned long long)(c->x[16] - 0x7000000000ull),
               e[0], e[1], e[2], t[0], t[1], t[2], first ? ", first person" : "");
        if (target) {
            const float *f = (const float *)(uintptr_t)(target + 0x31c);
            rt_log("vr camera: present %llu feet %.3f %.3f %.3f", (unsigned long long)presents, f[0], f[1], f[2]);
        }
    }
    float *t2 = turned[view][0], *t4 = turned[view][1];
    memcpy(t2, first ? base : m2, 48);
    vr_camera_view(t2, fh, ph, K.move, K.level, first ? eye : NULL);
    c->x[2] = (uint64_t)(uintptr_t)t2;
    if (m4 == m2) {
        c->x[4] = c->x[2];
    } else {
        memcpy(t4, first ? base : m4, 48);
        vr_camera_view(t4, fh, ph, K.move, K.level, first ? eye : NULL);
        c->x[4] = (uint64_t)(uintptr_t)t4;
    }
}

void vr_camera_turn_stick(int32_t *x, int32_t *y) {
    if (!look.at || presents > look.at + 2) return;
    float a = look.fwd[0], b = -look.fwd[2];
    const float n = sqrtf(a * a + b * b);
    if (n < 1e-3f) return;
    a /= n, b /= n;
    const float sx = (float)*x, sy = (float)*y;
    const float tx = sx * b + sy * a, ty = sy * b - sx * a;
    *x = (int32_t)fmaxf(-32767.0f, fminf(32767.0f, tx));
    *y = (int32_t)fmaxf(-32767.0f, fminf(32767.0f, ty));
}

void vr_camera_toggle_first(void) {
    settings();
    K.toggled ^= 1, K.first ^= 1;
    rt_log("vr camera: first person %s", K.first ? "on" : "off");
}

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
// LOCAL TEST (Ocarina): OOT_VR_DR=<present>:<offset>: at that present, a hardware write breakpoint on the 4 bytes at controller+offset
uint64_t rt_lifted_function(uintptr_t rip);
static uint64_t dr_seen[64];
static volatile LONG dr_n;
static LONG CALLBACK dr_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 15)) return EXCEPTION_CONTINUE_SEARCH;
    const int which = __builtin_ctzll(ep->ContextRecord->Dr6 & 15);
    ep->ContextRecord->Dr6 = 0;
    const uint64_t f = rt_lifted_function((uintptr_t)ep->ContextRecord->Rip) * 4 + which;
    for (LONG i = 0; i < dr_n; i++)
        if (dr_seen[i] == f) return EXCEPTION_CONTINUE_EXECUTION;
    if (dr_n < 64) dr_seen[dr_n] = f, InterlockedIncrement(&dr_n), rt_log("vr dr: DR%d written by f_%llx (rip %p)", which, (unsigned long long)(f / 4), (void *)ep->ContextRecord->Rip);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static uint64_t dr_addr[4];
static int dr_rw;   // 1: break on reads too
static DWORD WINAPI dr_arm(void *p) {
    const uint64_t addr = (uint64_t)(uintptr_t)p;
    if (addr) dr_addr[0] = addr;
    AddVectoredExceptionHandler(1, dr_veh);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT ctx = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
        if (GetThreadContext(t, &ctx)) {
            ctx.Dr0 = dr_addr[0], ctx.Dr1 = dr_addr[1], ctx.Dr2 = dr_addr[2], ctx.Dr3 = dr_addr[3], ctx.Dr7 = 0;
            for (int k = 0; k < 4; k++)
                if (dr_addr[k]) ctx.Dr7 |= (1ull << (2 * k)) | ((dr_rw ? 3ull : 1ull) << (16 + 4 * k)) | (3ull << (18 + 4 * k));
            armed += SetThreadContext(t, &ctx) != 0;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    rt_log("vr dr: armed %d threads on %#llx %#llx %#llx %#llx", armed, (unsigned long long)dr_addr[0], (unsigned long long)dr_addr[1],
           (unsigned long long)dr_addr[2], (unsigned long long)dr_addr[3]);
    return 0;
}
// LOCAL TEST (Ocarina): OOT_VR_BFS=<present>: pointer paths from the target actor to strings starting "Head"
static int str_at(uint64_t q, char *out) {
    if (q < 0x100000000ull || q > 0x7100000000ull || !plat_mem_readable(q, 16)) return 0;
    const char *t = (const char *)(uintptr_t)q;
    int n = 0;
    while (n < 15 && t[n] >= 32 && t[n] < 127) n++;
    if (n < 3 || (n < 15 && t[n])) return 0;
    memcpy(out, t, n), out[n] = 0;
    return 1;
}

static void bfs_step(void) {
    static long at = -2;
    if (at == -2) { const char *e = getenv("OOT_VR_BFS"); at = e ? atol(e) : -1; }
    if (at < 0 || (long)presents != at || !target) return;
    enum { MAXN = 300000 };
    uint64_t *node = malloc(MAXN * 8), *par = malloc(MAXN * 8);
    int *off = malloc(MAXN * sizeof *off), *dep = malloc(MAXN * sizeof *dep);
    int n = 1, hits = 0;
    node[0] = target, par[0] = ~0ull, off[0] = 0, dep[0] = 0;
    for (int i = 0; i < n && hits < 80; i++) {
        if (dep[i] >= 4) continue;
        for (int o = 0; o < 0x400 && n < MAXN; o += 8) {
            if (!plat_mem_readable(node[i] + o, 8)) break;
            const uint64_t q = *(const uint64_t *)(uintptr_t)(node[i] + o);
            char name[16];
            if (str_at(q, name) && !strncmp(name, "Head", 4)) {
                char path[256];
                int len = snprintf(path, sizeof path, "+%#x", o);
                for (int k = i; k > 0 && len < 200; k = (int)par[k]) len += snprintf(path + len, sizeof path - len, " <- +%#x", off[k]);
                rt_log("vr bfs: \"%s\" at node %#llx (depth %d) path %s", name, (unsigned long long)node[i], dep[i], path);
                hits++;
                continue;
            }
            if (q < 0x100000000ull || q > 0x800000000ull || (q & 7)) continue;
            int seen = 0;
            for (int k = n - 1; k >= 0 && k > n - 2000; k--) if (node[k] == q) { seen = 1; break; }
            if (seen) continue;
            node[n] = q, par[n] = (uint64_t)i, off[n] = o, dep[n] = dep[i] + 1, n++;
        }
    }
    rt_log("vr bfs: %d nodes, %d hits", n, hits);
    const uint64_t x = *(const uint64_t *)(uintptr_t)(target + 0x8);
    rt_log("vr unit: target+0x8 = %#llx vt %#llx", (unsigned long long)x, (unsigned long long)*(const uint64_t *)(uintptr_t)x);
    const uint64_t arr = x + 0x70;
    for (int u = 0; u < 0x30; u++) {
        const uint64_t base = *(const uint64_t *)(uintptr_t)(arr);
        const uint64_t obj = plat_mem_readable(base + u * 8, 8) ? *(const uint64_t *)(uintptr_t)(base + u * 8) : 0;
        if (!obj || !plat_mem_readable(obj, 0x400)) continue;
        char line[1024];
        int len = snprintf(line, sizeof line, "vr unit: [%#x] %#llx vt %#llx:", u * 8, (unsigned long long)obj, (unsigned long long)*(const uint64_t *)(uintptr_t)obj);
        for (int o = 0; o < 0x400 && len < 900; o += 8) {
            char nm[16];
            if (str_at(*(const uint64_t *)(uintptr_t)(obj + o), nm)) len += snprintf(line + len, sizeof line - len, " +%#x=%s", o, nm);
            const uint64_t q = *(const uint64_t *)(uintptr_t)(obj + o);
            if (q > 0x100000000ull && q < 0x800000000ull && !(q & 7) && plat_mem_readable(q, 16) && str_at(*(const uint64_t *)(uintptr_t)q, nm))
                len += snprintf(line + len, sizeof line - len, " +%#x->%s", o, nm);
        }
        rt_log("%s", line);
    }
    free(node), free(par), free(off), free(dep);
}

// LOCAL TEST (Ocarina): OOT_VR_MTX=<present>: arrays of 3x4 bone matrices whose bone 0 is at Link's feet and bone 103 above them
static int qs_ok(const float *e) {
    const float q = e[4] * e[4] + e[5] * e[5] + e[6] * e[6] + e[7] * e[7];
    return fabsf(q - 1) < 0.01f && fabsf(e[8] - 1) < 0.02f && fabsf(e[9] - 1) < 0.02f && fabsf(e[10] - 1) < 0.02f &&
           fabsf(e[0]) < 5 && fabsf(e[1]) < 5 && fabsf(e[2]) < 5;
}

// LOCAL TEST (Ocarina): OOT_VR_QS=<present>: arrays of 309 Havok qsTransforms (translation, rotation, scale; 0x30 each)
static void qs_scan(void) {
    static long at = -2;
    if (at == -2) { const char *e = getenv("OOT_VR_QS"); at = e ? atol(e) : -1; }
    if (at < 0 || (long)presents != at) return;
    MEMORY_BASIC_INFORMATION mi;
    int hits = 0;
    for (uint64_t a = 0x100000000ull; a < 0x800000000ull && hits < 60 && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_READONLY)) || (mi.Protect & PAGE_GUARD)) continue;
        const float *f = (const float *)mi.BaseAddress;
        const size_t n = mi.RegionSize / 4;
        for (size_t i = 0; i + 12 * 310 <= n && hits < 60; i += 4) {
            if (!qs_ok(f + i) || !qs_ok(f + i + 12 * 103) || !qs_ok(f + i + 12 * 308)) continue;
            if (i >= 12 && qs_ok(f + i - 12)) continue;
            int good = 0;
            for (int b = 0; b < 309; b++) good += qs_ok(f + i + 12 * b);
            if (good < 300 || qs_ok(f + i + 12 * 309)) continue;
            const float *h = f + i + 12 * 103;
            rt_log("vr qs: array at %#llx good %d, bone0 t %.3f %.3f %.3f, head t %.3f %.3f %.3f", (unsigned long long)(uintptr_t)(f + i), good, f[i], f[i + 1],
                   f[i + 2], h[0], h[1], h[2]);
            hits++;
            i += 12 * 308;
        }
    }
    rt_log("vr qs: %d hits", hits);
    if (target && link_model()) {
        const uint64_t world = *(const uint64_t *)(uintptr_t)(link_model() + 0x3c8);
        int found = 0;
        for (uint64_t r = 0x100000000ull; r < 0x800000000ull && found < 20 && VirtualQuery((void *)(uintptr_t)r, &mi, sizeof mi); r = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_READONLY)) || (mi.Protect & PAGE_GUARD)) continue;
            const uint64_t *q = (const uint64_t *)mi.BaseAddress;
            for (size_t i = 4; i < mi.RegionSize / 8 && found < 20; i++) {
                if (q[i] != world) continue;
                const uint64_t p1 = (uint64_t)(uintptr_t)(q + i) - 0x20, res = q[i - 4];
                unsigned cnt = 0, par = 0;
                if (res > 0x100000000ull && plat_mem_readable(res, 0x40)) {
                    cnt = *(const uint16_t *)(uintptr_t)(res + 0x38);
                    const uint64_t bones = *(const uint64_t *)(uintptr_t)(res + 0x10);
                    if (plat_mem_readable(bones + 103 * 0x58, 0x58)) par = *(const uint16_t *)(uintptr_t)(bones + 103 * 0x58 + 0x2a) | (unsigned)*(const uint16_t *)(uintptr_t)(bones + 103 * 0x58 + 0x22) << 16;
                }
                rt_log("vr skel: pointer at %#llx (model %#llx), param %#llx res %#llx count %u parent+0x2a %u parent+0x22 %u", (unsigned long long)(uintptr_t)(q + i),
                       (unsigned long long)link_model(), (unsigned long long)p1, (unsigned long long)res, cnt, par & 0xffff, par >> 16);
                found++;
                if (cnt == 309) {
                    const uint64_t m = link_model();
                    for (int o = 0; o < 0x1000; o += 8) {
                        const uint64_t v = *(const uint64_t *)(uintptr_t)(m + o);
                        if (v >= p1 - 0x400 && v <= p1 + 0x400) rt_log("vr skel:   model+%#x -> %#llx (param %+lld)", o, (unsigned long long)v, (long long)(v - p1));
                    }
                }
            }
        }
    }
    if (!target) return;
    const uint64_t x = *(const uint64_t *)(uintptr_t)(target + 0x8), arr = *(const uint64_t *)(uintptr_t)(x + 0x70);
    const uint64_t model = *(const uint64_t *)(uintptr_t)(arr + 0xb0);
    rt_log("vr model: %#llx vt %#llx", (unsigned long long)model, (unsigned long long)*(const uint64_t *)(uintptr_t)model);
    const uint64_t bm = *(const uint64_t *)(uintptr_t)(model + 0x3c8);
    const int ids[] = {0, 1, 2, 3, 4, 11, 101, 102, 103, 104, 105, 200, 308, 309};
    for (int k = 0; k < (int)(sizeof ids / sizeof *ids); k++) {
        const float *e = (const float *)(uintptr_t)(bm + 0x40 * ids[k]);
        rt_log("vr bones: [%d] t %.3f %.3f %.3f %.3f  x %.3f %.3f %.3f", ids[k], e[12], e[13], e[14], e[15], e[0], e[1], e[2]);
    }
    for (int o = 0; o < 0x600; o += 8) {
        const uint64_t q = *(const uint64_t *)(uintptr_t)(model + o);
        if (q < 0x100000000ull || q > 0x800000000ull || (q & 15) || !plat_mem_readable(q, 0x40 * 110)) continue;
        for (int st = 0x30; st <= 0x40; st += 0x10) {
            const float *e0 = (const float *)(uintptr_t)q, *e = (const float *)(uintptr_t)(q + st * 103);
            rt_log("vr model: +%#x -> %#llx stride %#x: [0] %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f  [103] %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                   o, (unsigned long long)q, st, e0[0], e0[1], e0[2], e0[3], e0[4], e0[5], e0[6], e0[7], e0[8], e0[9], e0[10], e0[11],
                   e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11]);
        }
    }
}

static uint64_t mtx_found;
static void mtx_scan(void) {
    static long at = -2;
    if (mtx_found && presents % 20 == 0 && presents < (uint64_t)at + 400) {
        const float *h = (const float *)(uintptr_t)(mtx_found + 0x30 * 103), *a = (const float *)(uintptr_t)(mtx_found + 0x30 * 50);
        rt_log("vr mtx: present %llu head %.4f %.4f %.4f row0 %.4f %.4f %.4f bone50 %.4f %.4f %.4f", (unsigned long long)presents, h[3], h[7], h[11],
               h[0], h[1], h[2], a[3], a[7], a[11]);
    }
    if (at == -2) { const char *e = getenv("OOT_VR_MTX"); at = e ? atol(e) : -1; }
    if (at < 0 || (long)presents != at || !target) return;
    const float *feet = (const float *)(uintptr_t)(target + 0x31c);
    MEMORY_BASIC_INFORMATION mi;
    int hits = 0;
    for (uint64_t a = 0x100000000ull; a < 0x800000000ull && hits < 40 && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_READONLY)) || (mi.Protect & PAGE_GUARD)) continue;
        const float *f = (const float *)mi.BaseAddress;
        const size_t n = mi.RegionSize / 4;
        for (size_t i = 0; i + 12 * 104 <= n && hits < 40; i += 4) {
            for (int space = 0; space < 2; space++) {
                const float ox = space ? 0 : feet[0], oy = space ? 0 : feet[1], oz = space ? 0 : feet[2];
                if (fabsf(f[i + 3] - ox) > 0.01f || fabsf(f[i + 7] - oy) > 0.01f || fabsf(f[i + 11] - oz) > 0.01f) continue;
                if (space && fabsf(f[i] - 1) > 0.01f && fabsf(f[i + 5] - 1) > 0.01f) continue;
                const float *h = f + i + 12 * 103, *nk = f + i + 12 * 102, *sp = f + i + 12 * 4;
                const float hx = h[3] - ox, hy = h[7] - oy, hz = h[11] - oz;
                if (!(hy > 0.5f && hy < 1.3f && fabsf(hx) < 0.4f && fabsf(hz) < 0.4f)) continue;
                const float ny = nk[7] - oy, sy = sp[7] - oy;
                if (!(ny > 0.3f && ny < hy && hy - ny < 0.3f && sy > 0.2f && sy < ny)) continue;
                if (!(fabsf(h[0]) <= 1.01f && fabsf(h[5]) <= 1.01f && fabsf(h[10]) <= 1.01f)) continue;
                rt_log("vr mtx: %s array at %#llx, head %.3f %.3f %.3f", space ? "model" : "world", (unsigned long long)(uintptr_t)(f + i), h[3], h[7], h[11]);
                mtx_found = (uint64_t)(uintptr_t)(f + i);
                hits++;
            }
        }
    }
    rt_log("vr mtx: %d hits (feet %.3f %.3f %.3f)", hits, feet[0], feet[1], feet[2]);
}

static void owner_scan(void) {
    if (!controller || !getenv("OOT_VR_OWNER") || presents % 120) return;
    if (target) {
        const float *p = (const float *)(uintptr_t)(target + 0x31c);
        rt_log("vr target: %#llx vt %#llx +0x2c %d pos %.3f %.3f %.3f rot %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f",
               (unsigned long long)target, (unsigned long long)*(const uint64_t *)(uintptr_t)target, *(const int *)(uintptr_t)(target + 0x2c),
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
    }
    const float *t = (const float *)(uintptr_t)(controller + 0x100);
    const uint64_t owner = *(const uint64_t *)(uintptr_t)(controller + 0x10);
    rt_log("vr owner: controller %#llx owner %#llx at %.2f %.2f %.2f", (unsigned long long)controller, (unsigned long long)owner, t[0], t[1], t[2]);
    if (owner < 0x1000000 || !plat_mem_readable(owner, 0x2000)) return;
    for (int o = 0; o + 12 <= 0x2000; o += 4) {
        const float *f = (const float *)(uintptr_t)(owner + o);
        const float dx = f[0] - t[0], dy = f[1] - t[1], dz = f[2] - t[2];
        if (dx * dx + dy * dy + dz * dz < 9.0f) rt_log("vr owner:   float +%#x %.3f %.3f %.3f", o, f[0], f[1], f[2]);
    }
    for (int o = 0; o + 24 <= 0x2000; o += 8) {
        const double *d = (const double *)(uintptr_t)(owner + o);
        const double dx = d[0] - t[0], dy = d[1] - t[1], dz = d[2] - t[2];
        if (dx * dx + dy * dy + dz * dz < 9.0) rt_log("vr owner:   double +%#x %.3f %.3f %.3f", o, d[0], d[1], d[2]);
    }
}

// LOCAL TEST (Ocarina): OOT_VR_SCAN=<p1>:<p2>: float triples near the look-at point at present p1 that moved with it by p2
static uint64_t *scan_c;
static float *scan_v;
static float scan_t[3];
static size_t scan_n;
static void scan_step(void) {
    static long p1 = -1, p2 = -1;
    if (presents == 1) {
        const char *e = getenv("OOT_VR_SCAN");
        if (e) p1 = strtol(e, (char **)&e, 0), p2 = strtol(e + 1, NULL, 0);
    }
    if (!controller || ((long)presents != p1 && (long)presents != p2)) return;
    const float *t = (const float *)(uintptr_t)(controller + 0x100);
    if ((long)presents == p1) {
        scan_c = malloc(sizeof *scan_c * 4000000), scan_v = malloc(sizeof *scan_v * 3 * 4000000);
        memcpy(scan_t, t, 12);
        MEMORY_BASIC_INFORMATION mi;
        for (uint64_t a = 0x100000000ull; a < 0x800000000ull && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD)) continue;
            const float *f = (const float *)mi.BaseAddress;
            const size_t n = mi.RegionSize / 4;
            for (size_t i = 0; i + 3 <= n && scan_n < 4000000; i++) {
                const float dx = f[i] - t[0], dy = f[i + 1] - t[1], dz = f[i + 2] - t[2];
                if (dx * dx + dy * dy + dz * dz < 9.0f) memcpy(scan_v + 3 * scan_n, f + i, 12), scan_c[scan_n++] = (uint64_t)(uintptr_t)(f + i);
            }
        }
        rt_log("vr scan: %zu candidates near %.2f %.2f %.2f", scan_n, t[0], t[1], t[2]);
        return;
    }
    const float d[3] = {t[0] - scan_t[0], t[1] - scan_t[1], t[2] - scan_t[2]};
    rt_log("vr scan: look-at moved %.2f %.2f %.2f", d[0], d[1], d[2]);
    int shown = 0;
    for (size_t i = 0; i < scan_n && shown < 200; i++) {
        if (!plat_mem_readable(scan_c[i], 12)) continue;
        const float *f = (const float *)(uintptr_t)scan_c[i];
        const float ex = f[0] - (t[0]), ey = f[1] - (t[1]), ez = f[2] - (t[2]);
        const float *v = scan_v + 3 * i;
        const float mx = f[0] - v[0] - d[0], my = f[1] - v[1] - d[1], mz = f[2] - v[2] - d[2];
        if (ex * ex + ey * ey + ez * ez < 9.0f && mx * mx + my * my + mz * mz < 0.25f) {
            rt_log("vr scan:   %#llx %.3f %.3f %.3f", (unsigned long long)scan_c[i], f[0], f[1], f[2]);
            shown++;
            int k = 0, dup = 0;
            while (k < 4 && dr_addr[k]) dup |= (dr_addr[k] >> 24) == (scan_c[i] >> 24), k++;
            (void)dup;
        }
    }
    {
        const uint64_t ow = *(const uint64_t *)(uintptr_t)(controller + 0x10);
        const uint64_t l1 = *(const uint64_t *)(uintptr_t)(ow + 0x420), obj = *(const uint64_t *)(uintptr_t)(l1 + 0x3e8);
        rt_log("vr link: owner %#llx vt %#llx, l1 %#llx vt %#llx, obj %#llx vt %#llx", (unsigned long long)ow,
               (unsigned long long)*(const uint64_t *)(uintptr_t)ow, (unsigned long long)l1, (unsigned long long)*(const uint64_t *)(uintptr_t)l1,
               (unsigned long long)obj, (unsigned long long)*(const uint64_t *)(uintptr_t)obj);
        for (int o = 0x300; o < 0x420; o += 16) {
            const float *f = (const float *)(uintptr_t)(obj + o);
            rt_log("vr link:   +%#x %9.4f %9.4f %9.4f %9.4f", o, f[0], f[1], f[2], f[3]);
        }
    }
    const uint64_t owner = *(const uint64_t *)(uintptr_t)(controller + 0x10);
    int found = 0;
    for (int o1 = 0; o1 < 0x1000 && found < 60; o1 += 8) {
        const uint64_t q1 = *(const uint64_t *)(uintptr_t)(owner + o1);
        if (q1 < 0x100000000ull || q1 > 0x800000000ull || (q1 & 7) || !plat_mem_readable(q1, 0x800)) continue;
        for (int o2 = -8; o2 < 0x800 && found < 60; o2 += 8) {
            uint64_t q2 = q1;
            if (o2 >= 0) {
                q2 = *(const uint64_t *)(uintptr_t)(q1 + o2);
                if (q2 < 0x100000000ull || q2 > 0x800000000ull || (q2 & 3) || !plat_mem_readable(q2, 0x400)) continue;
            }
            for (int o3 = 0; o3 + 24 <= 0x400; o3 += 4) {
                const float *f = (const float *)(uintptr_t)(q2 + o3);
                const float fx = f[0] - t[0], fy = f[1] - t[1] + 0.8f, fz = f[2] - t[2];
                if (fx * fx + fy * fy + fz * fz < 0.04f) {
                    rt_log("vr path: owner+%#x%s+%#x float %.3f %.3f %.3f", o1, o2 >= 0 ? " ->" : "", o3, f[0], f[1], f[2]);
                    if (o2 >= 0) rt_log("vr path:   via +%#x", o2);
                    found++;
                }
                if (o3 % 8) continue;
                const double *dd = (const double *)(uintptr_t)(q2 + o3);
                const double gx = dd[0] - t[0], gy = dd[1] - t[1] + 0.8, gz = dd[2] - t[2];
                if (gx * gx + gy * gy + gz * gz < 0.04) {
                    rt_log("vr path: owner+%#x ->+%#x +%#x double %.3f %.3f %.3f", o1, o2, o3, dd[0], dd[1], dd[2]);
                    found++;
                }
            }
        }
    }
}

static void dr_step(void) {
    static const char *e;
    static long at = -1, off;
    if (presents == 1 && (e = getenv("OOT_VR_DR"))) at = strtol(e, (char **)&e, 0), off = strtol(e + 1, NULL, 0);
    if (at >= 0 && (long)presents == at && controller) {
        const float *feet = link_feet();
        uint64_t addr = off == 0xfee7 ? (feet ? (uint64_t)(uintptr_t)feet : 0) : controller + off;
        if (off == 0xbeef && link_model()) {
            dr_rw = 1;
            addr = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(link_model() + 0x398) + 0x20) + 0x40 * 164 + 0x30;
        }
        if (off == 0xbead && target) {
            const uint64_t x = *(const uint64_t *)(uintptr_t)(target + 0x8), arr = *(const uint64_t *)(uintptr_t)(x + 0x70);
            const uint64_t model = *(const uint64_t *)(uintptr_t)(arr + 0xb0);
            addr = *(const uint64_t *)(uintptr_t)(model + 0x3c8) + 0x40 * 103 + 0x30;
        }
        if (addr) CloseHandle(CreateThread(NULL, 0, dr_arm, (void *)(uintptr_t)addr, 0, NULL));
    }
}
#else
static void dr_step(void) {}
#endif

void vr_camera_live(void) {
    presents++;
    static int video;
    if ((romfs_videos_open() > 0) != video) video ^= 1, rt_log("vr camera: present %llu, video %s", (unsigned long long)presents, video ? "started" : "stopped");
    if (cutscene_at && presents == cutscene_at + CUTSCENE_HOLD + 1) rt_log("vr camera: cutscene camera gone, first person back");
    if (target) {
        memmove(feet_hist[1], feet_hist[0], sizeof feet_hist - sizeof feet_hist[0]);
        memcpy(feet_hist[0], (const float *)(uintptr_t)(target + 0x31c), sizeof feet_hist[0]);
    }
    if (K.log) {
        static unsigned last_callers;
        static uint64_t last_target, last_controller;
        if (ev_callers != last_callers || target != last_target || controller != last_controller)
            rt_log("vr event: present %llu main view callers %#x target %#llx controller %#llx", (unsigned long long)presents, ev_callers,
                   (unsigned long long)target, (unsigned long long)controller);
        last_callers = ev_callers, last_target = target, last_controller = controller, ev_callers = 0;
    }
    dr_step();
    owner_scan();
    bfs_step();
    mtx_scan();
    qs_scan();
    scan_step();
    static int set = -2;
    if (set == -2) {
        set = rt_patch_set("vrcam");
        if (set < 0 && vr_stereo_wanted()) rt_log("vr: this build has no camera patch; the head does not move the camera");
    }
    settings();
    const int on = vr_mode() == VR_WORLD || (K.has_fake && vr_mode() != VR_OFF);
    if (set >= 0 && rt_patch_enabled(set) != on) rt_patch_switch(set, on);
}

#ifdef RT_PATCH_PAGE

static void host_set_view(Cpu *c) {
    c->x[30] = c->x[16];
    set_view(c);
}

static uint64_t demo_at;   // the present a cutscene camera track last ran at

static void host_demo(Cpu *c) {
    c->x[30] = c->x[16];
    static uint64_t calls;
    if (K.log && (calls++ % 200 == 0 || presents > demo_at + 30)) rt_log("vr camera: cutscene camera at present %llu (%llu calls)", (unsigned long long)presents, (unsigned long long)calls);
    demo_at = presents;
}

static void host_target(Cpu *c) {
    c->x[30] = c->x[16];
    target = c->x[0];
}

static void host_controller(Cpu *c) {
    c->x[30] = c->x[16];
    controller = c->x[0];
}

__attribute__((constructor)) static void vr_camera_register(void) {
    rt_register_host(RT_PATCH_PAGE + 0xfb0, host_set_view);
    rt_register_host(RT_PATCH_PAGE + 0xfa0, host_controller);
    rt_register_host(RT_PATCH_PAGE + 0xf90, host_target);
    rt_register_host(RT_PATCH_PAGE + 0xf80, host_skeleton);
    rt_register_host(RT_PATCH_PAGE + 0xf70, host_demo);
}

#endif
