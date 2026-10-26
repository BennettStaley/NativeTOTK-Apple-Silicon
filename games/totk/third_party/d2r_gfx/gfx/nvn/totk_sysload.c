// TOTK: system load sampled for the bottom-right line (user, 2026-10-16 night). A background thread reads every 0.5 s:
// - per-core CPU load: host_processor_info(PROCESSOR_CPU_LOAD_INFO) tick deltas; performance / efficiency cores from the
//   device tree's "cluster-type" (E cores have the lowest logical ids on Apple silicon; that is the fallback);
// - the GPU's own utilisation: IOAccelerator PerformanceStatistics "Device Utilization %" (what Activity Monitor's GPU
//   history shows; our keep-alive fills the GPU's idle gaps, so it reads near 100 % while the game runs);
// - the game's GPU share: the union of our command buffers' GPU intervals (sysload_note_gpu) per wall time;
// - power (user, 2026-10-23): CPU and GPU energy from IOReport's "Energy Model" group (libIOReport, loaded with dlopen: a
//   private library, so a missing symbol only hides those two values), and the battery controller's own measurements
//   (AppleSmartBattery: whole-Mac load, charger input, battery current), averaged over SYSLOAD_POWER_SECONDS.
// It never touches the game threads; reading the device tree / IOKit happens on its own thread.
#include "totk_sysload.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if TARGET_OS_OSX
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#endif
#include <math.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sysload_info g_cur;
static int g_have;
static sysload_busy g_busy;   // under g_mu (GPU completion handlers)

int sysload_enabled(void) {
    static int on = -1;
#if TARGET_OS_IPHONE
    const int def = 0;   // no IOKit on iOS; the phone shows only the FPS counter
#else
    const int def = 1;
#endif
    if (on < 0) { const char *e = getenv("TOTK_LOAD_OVERLAY"); on = e ? strcmp(e, "0") != 0 : def; }
    return on;
}

double sysload_busy_add(sysload_busy *b, double s, double e) {
    if (!(e > s)) return 0;
    double from = s > b->last_end ? s : b->last_end, add = e > from ? e - from : 0;
    if (e > b->last_end) b->last_end = e;
    b->busy += add;
    return add;
}
void sysload_note_gpu(double s, double e) { pthread_mutex_lock(&g_mu); sysload_busy_add(&g_busy, s, e); pthread_mutex_unlock(&g_mu); }

void sysload_text(char *out, size_t n, const sysload_info *in) {
    size_t o = 0;
    #define PUT(...) do { int k = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (k > 0) o += (size_t)k; } while (0)
    /* the GPU first, then the CPU cores (user 2026-10-25) */
    if (in->gpu_sys >= 0) PUT("GPU %d%%", in->gpu_sys); else PUT("GPU --");
    if (in->gpu_game >= 0) PUT(" (%d%%)", in->gpu_game);
    PUT("  P"); for (int i = 0; i < in->np; i++) PUT(" %2d%%", in->p[i]);   // every value with % (user, 2026-10-16 night)
    PUT("  E"); for (int i = 0; i < in->ne; i++) PUT(" %2d%%", in->e[i]);
    if (in->thread_max >= 0) PUT("  T %d%%", in->thread_max);   // the busiest thread (user, 2026-10-16 night)
    #undef PUT
    if (n) out[n - 1 < o ? n - 1 : o] = 0;
}

int sysload_power_text(char *out, size_t n, const sysload_info *in) {
    size_t o = 0; int drain = 0;
    if (n) out[0] = 0;
    if (!in->have_power) return -1;
    #define PUT(...) do { int k = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (k > 0) o += (size_t)k; } while (0)
    #define SEP() do { if (o) PUT("  "); } while (0)
    if (in->cpu_w >= 0) { SEP(); PUT("CPU %.1f W", in->cpu_w); }
    if (in->gpu_w >= 0) { SEP(); PUT("GPU %.1f W", in->gpu_w); }
    if (in->sys_w >= 0) { SEP(); PUT("Total %.0f W", in->sys_w); }
    if (in->charger_w > 0) { SEP(); PUT("Charger %d W", in->charger_w); if (in->in_w >= 0) PUT(" (%.0f W in)", in->in_w); }
    else if (in->charger_w == 0) { SEP(); PUT("No charger"); }
    if (!isnan(in->bat_w)) {
        SEP();
        if (in->bat_w <= -0.5) {
            PUT("Battery %.0f W draining", in->bat_w); drain = 1;
            if (in->left_min >= 600) PUT(", over 10 h left");
            else if (in->left_min >= 60) PUT(", %d h %02d min left", in->left_min / 60, in->left_min % 60);
            else if (in->left_min >= 0) PUT(", %d min left", in->left_min);
        }
        else if (in->bat_w >= 0.5) PUT("Battery +%.0f W charging", in->bat_w);
        else PUT("Battery 0 W");
    }
    #undef SEP
    #undef PUT
    if (n) out[n - 1 < o ? n - 1 : o] = 0;
    return o ? drain : -1;
}

int sysload_get(sysload_info *out) {
    pthread_mutex_lock(&g_mu); int have = g_have; if (have) *out = g_cur; pthread_mutex_unlock(&g_mu);
    return have;
}

static double now_s(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;   // the clock of CACurrentMediaTime and GPUStartTime
}

/* 'P' or 'E' per logical CPU id. */
static void classify(int ncpu, char *kind) {
    int ne = 0; size_t sz = sizeof ne;
    if (sysctlbyname("hw.perflevel1.logicalcpu", &ne, &sz, NULL, 0) || ne < 0 || ne > ncpu) ne = 0;   // perflevel1 = efficiency
    for (int i = 0; i < ncpu; i++) kind[i] = i < ne ? 'E' : 'P';
#if TARGET_OS_OSX
    io_registry_entry_t cpus = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/cpus");
    io_iterator_t it = 0;
    if (cpus && IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it) == KERN_SUCCESS) {
        io_object_t c;
        while ((c = IOIteratorNext(it))) {
            CFTypeRef id = IORegistryEntryCreateCFProperty(c, CFSTR("logical-cpu-id"), kCFAllocatorDefault, 0);
            CFTypeRef ct = IORegistryEntryCreateCFProperty(c, CFSTR("cluster-type"), kCFAllocatorDefault, 0);
            int lid = -1;
            if (id && CFGetTypeID(id) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)id, kCFNumberIntType, &lid);
            if (lid >= 0 && lid < ncpu && ct && CFGetTypeID(ct) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)ct) > 0) {
                char k = (char)CFDataGetBytePtr((CFDataRef)ct)[0]; if (k == 'P' || k == 'E') kind[lid] = k;
            }
            if (id) CFRelease(id);
            if (ct) CFRelease(ct);
            IOObjectRelease(c);
        }
        IOObjectRelease(it);
    }
    if (cpus) IOObjectRelease(cpus);
#endif
}

#if TARGET_OS_OSX
static io_object_t g_accel;
static int gpu_util(void) {
    if (!g_accel) {
        io_iterator_t it;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return -1;
        io_object_t s;
        while ((s = IOIteratorNext(it))) {
            CFTypeRef ps = IORegistryEntryCreateCFProperty(s, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
            int ok = ps && CFGetTypeID(ps) == CFDictionaryGetTypeID() && CFDictionaryContainsKey((CFDictionaryRef)ps, CFSTR("Device Utilization %"));
            if (ps) CFRelease(ps);
            if (ok && !g_accel) g_accel = s; else IOObjectRelease(s);
        }
        IOObjectRelease(it);
        if (!g_accel) return -1;
    }
    int v = -1;
    CFTypeRef ps = IORegistryEntryCreateCFProperty(g_accel, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (ps && CFGetTypeID(ps) == CFDictionaryGetTypeID()) {
        CFTypeRef n = CFDictionaryGetValue((CFDictionaryRef)ps, CFSTR("Device Utilization %"));
        if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, &v);
    }
    if (ps) CFRelease(ps);
    return v < 0 ? -1 : v > 100 ? 100 : v;
}
#else
static int gpu_util(void) { return -1; }
#endif

/* Power sources (see the top). Each returns 0 when it has nothing. */
#if TARGET_OS_OSX
typedef void *ior_sub;
static struct {
    int state;   // 0 not tried, 1 ready, -1 unavailable
    CFMutableDictionaryRef (*copy_channels)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
    ior_sub (*create_sub)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
    CFDictionaryRef (*samples)(ior_sub, CFMutableDictionaryRef, CFTypeRef);
    CFDictionaryRef (*delta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
    int64_t (*int_value)(CFDictionaryRef, int32_t);
    CFStringRef (*name)(CFDictionaryRef), (*unit)(CFDictionaryRef);
    ior_sub sub; CFMutableDictionaryRef subbed; CFDictionaryRef last;
} g_ior;
/* CPU and GPU energy in joules since the previous call (the first call only takes the starting sample). */
static int soc_energy(double *cpu_j, double *gpu_j) {
    if (g_ior.state < 0) return 0;
    if (!g_ior.state) {
        g_ior.state = -1;
        void *h = dlopen("/usr/lib/libIOReport.dylib", RTLD_NOW | RTLD_LOCAL); if (!h) return 0;
        g_ior.copy_channels = dlsym(h, "IOReportCopyChannelsInGroup"); g_ior.create_sub = dlsym(h, "IOReportCreateSubscription");
        g_ior.samples = dlsym(h, "IOReportCreateSamples"); g_ior.delta = dlsym(h, "IOReportCreateSamplesDelta");
        g_ior.int_value = dlsym(h, "IOReportSimpleGetIntegerValue"); g_ior.name = dlsym(h, "IOReportChannelGetChannelName");
        g_ior.unit = dlsym(h, "IOReportChannelGetUnitLabel");
        if (!g_ior.copy_channels || !g_ior.create_sub || !g_ior.samples || !g_ior.delta || !g_ior.int_value || !g_ior.name || !g_ior.unit) return 0;
        CFMutableDictionaryRef ch = g_ior.copy_channels(CFSTR("Energy Model"), NULL, 0, 0, 0); if (!ch) return 0;
        g_ior.sub = g_ior.create_sub(NULL, ch, &g_ior.subbed, 0, NULL); CFRelease(ch);
        if (!g_ior.sub || !g_ior.subbed) return 0;
        g_ior.last = g_ior.samples(g_ior.sub, g_ior.subbed, NULL); if (!g_ior.last) return 0;
        g_ior.state = 1;
        return 0;
    }
    CFDictionaryRef cur = g_ior.samples(g_ior.sub, g_ior.subbed, NULL); if (!cur) return 0;
    CFDictionaryRef d = g_ior.delta(g_ior.last, cur, NULL);
    CFRelease(g_ior.last); g_ior.last = cur;
    if (!d) return 0;
    int got = 0; *cpu_j = *gpu_j = 0;
    CFArrayRef arr = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    for (CFIndex i = 0; arr && CFGetTypeID(arr) == CFArrayGetTypeID() && i < CFArrayGetCount(arr); i++) {
        CFDictionaryRef c = CFArrayGetValueAtIndex(arr, i); CFStringRef nm = g_ior.name(c); if (!nm) continue;
        int is_cpu = CFStringCompare(nm, CFSTR("CPU Energy"), 0) == kCFCompareEqualTo, is_gpu = CFStringCompare(nm, CFSTR("GPU Energy"), 0) == kCFCompareEqualTo;
        if (!is_cpu && !is_gpu) continue;
        char u[8] = "mJ"; CFStringRef us = g_ior.unit(c); if (us) CFStringGetCString(us, u, sizeof u, kCFStringEncodingUTF8);
        double scale = !strcmp(u, "nJ") ? 1e-9 : !strcmp(u, "uJ") ? 1e-6 : !strcmp(u, "J") ? 1 : 1e-3;
        double j = (double)g_ior.int_value(c, 0) * scale;
        if (is_cpu) *cpu_j = j; else *gpu_j = j;
        got |= is_cpu ? 1 : 2;
    }
    CFRelease(d);
    return got == 3;
}

static int dict_num(CFDictionaryRef d, CFStringRef key, int64_t *v) {
    CFTypeRef n = d ? CFDictionaryGetValue(d, key) : NULL;
    return n && CFGetTypeID(n) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)n, kCFNumberSInt64Type, v);
}
static int prop_num(io_object_t o, CFStringRef key, int64_t *v) {
    CFTypeRef n = IORegistryEntryCreateCFProperty(o, key, kCFAllocatorDefault, 0);
    int ok = n && CFGetTypeID(n) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)n, kCFNumberSInt64Type, v);
    if (n) CFRelease(n);
    return ok;
}
static CFDictionaryRef prop_dict(io_object_t o, CFStringRef key) {
    CFTypeRef d = IORegistryEntryCreateCFProperty(o, key, kCFAllocatorDefault, 0);
    if (d && CFGetTypeID(d) != CFDictionaryGetTypeID()) { CFRelease(d); d = NULL; }
    return d;
}
/* The battery controller's view: whole-Mac load and charger input (W, -1 = not known), battery power (W, + into it,
   NaN = no battery) and the charger's rating (W, 0 = none, -1 = not known). 0 on a Mac without a battery controller. */
static int battery_power(double *sys_w, double *in_w, double *bat_w, int *charger_w, double *left_wh) {
    static io_service_t batt; static int tried;
    if (!tried) { tried = 1; batt = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery")); }
    if (!batt) return 0;
    *sys_w = *in_w = -1; *bat_w = NAN; *charger_w = -1; *left_wh = -1;
    CFDictionaryRef t = prop_dict(batt, CFSTR("PowerTelemetryData")); int64_t v;
    if (dict_num(t, CFSTR("SystemLoad"), &v) && v >= 0) *sys_w = v / 1000.0;
    if (dict_num(t, CFSTR("SystemPowerIn"), &v) && v >= 0) *in_w = v / 1000.0;
    if (t) CFRelease(t);
    int64_t ma, mv; if (prop_num(batt, CFSTR("Amperage"), &ma) && prop_num(batt, CFSTR("Voltage"), &mv)) *bat_w = (double)ma * (double)mv / 1e6;   // signed mA x mV
    int64_t mah; if (prop_num(batt, CFSTR("AppleRawCurrentCapacity"), &mah) && mah > 0 && prop_num(batt, CFSTR("Voltage"), &mv) && mv > 0) *left_wh = (double)mah * (double)mv / 1e6;
    CFTypeRef ext = IORegistryEntryCreateCFProperty(batt, CFSTR("ExternalConnected"), kCFAllocatorDefault, 0);
    int plugged = ext && CFGetTypeID(ext) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)ext);
    if (ext) CFRelease(ext);
    if (!plugged) { *charger_w = 0; *in_w = 0; }
    else { CFDictionaryRef a = prop_dict(batt, CFSTR("AdapterDetails")); if (dict_num(a, CFSTR("Watts"), &v) && v > 0) *charger_w = (int)v; if (a) CFRelease(a); }
    return 1;
}
#else
static int soc_energy(double *cpu_j, double *gpu_j) { (void)cpu_j; (void)gpu_j; return 0; }
static int battery_power(double *sys_w, double *in_w, double *bat_w, int *charger_w, double *left_wh) { (void)sys_w; (void)in_w; (void)bat_w; (void)charger_w; (void)left_wh; return 0; }
#endif

/* Averages over the last SYSLOAD_POWER_SECONDS of 0.5 s samples; NaN marks a sample without that value. */
enum { PW_N = SYSLOAD_POWER_SECONDS * 2, PW_CPU = 0, PW_GPU, PW_SYS, PW_IN, PW_BAT, PW_K };
static double g_pw[PW_K][PW_N]; static unsigned g_pw_n;
enum { DR_N = SYSLOAD_DRAIN_SECONDS * 2 };
static double g_drain[DR_N]; static unsigned g_drain_n;   // battery power over the longer window, for the time left
static void power_sample(sysload_info *s, double dt) {
    double cj, gj, v[PW_K]; for (int k = 0; k < PW_K; k++) v[k] = NAN;
    if (soc_energy(&cj, &gj) && dt > 0) { v[PW_CPU] = cj / dt; v[PW_GPU] = gj / dt; }
    int charger = -1; double left_wh = -1;
    if (battery_power(&v[PW_SYS], &v[PW_IN], &v[PW_BAT], &charger, &left_wh)) { if (v[PW_SYS] < 0) v[PW_SYS] = NAN; if (v[PW_IN] < 0) v[PW_IN] = NAN; }
    else v[PW_SYS] = v[PW_IN] = v[PW_BAT] = NAN;
    unsigned slot = g_pw_n++ % PW_N;
    double avg[PW_K];
    for (int k = 0; k < PW_K; k++) {
        g_pw[k][slot] = v[k];
        double sum = 0; int c = 0; for (unsigned i = 0; i < PW_N && i < g_pw_n; i++) if (!isnan(g_pw[k][i])) { sum += g_pw[k][i]; c++; }
        avg[k] = c ? sum / c : NAN;
    }
    s->cpu_w = isnan(avg[PW_CPU]) ? -1 : avg[PW_CPU]; s->gpu_w = isnan(avg[PW_GPU]) ? -1 : avg[PW_GPU];
    s->sys_w = isnan(avg[PW_SYS]) ? -1 : avg[PW_SYS]; s->in_w = isnan(avg[PW_IN]) ? -1 : avg[PW_IN];
    s->bat_w = avg[PW_BAT]; s->charger_w = charger;
    g_drain[g_drain_n++ % DR_N] = v[PW_BAT];
    double dsum = 0; int dc = 0; for (unsigned i = 0; i < DR_N && i < g_drain_n; i++) if (!isnan(g_drain[i])) { dsum += g_drain[i]; dc++; }
    double drain_w = dc ? -dsum / dc : 0;   // positive while draining
    s->left_min = left_wh > 0 && drain_w >= 0.5 ? (int)(left_wh / drain_w * 60 + 0.5) : -1;
    s->have_power = s->cpu_w >= 0 || s->sys_w >= 0 || !isnan(s->bat_w);
}

/* The busiest thread of this process over the last interval: CPU time (user + system) per thread from thread_info, kept by
   thread id between samples; % of one core. Threads seen for the first time are skipped (no earlier time). */
enum { MAX_THREADS = 512 };
typedef struct { uint64_t id, us; } thread_time;
static thread_time g_tt[MAX_THREADS]; static int g_ntt;
static int busiest_thread(double wall_us, char name[32]) {
    thread_act_array_t th = NULL; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &th, &nth) != KERN_SUCCESS || !th) return -1;
    static thread_time cur[MAX_THREADS]; int ncur = 0; double best = -1; name[0] = 0;
    for (mach_msg_type_number_t i = 0; i < nth; i++) {
        thread_identifier_info_data_t idi; mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t c2 = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c1) == KERN_SUCCESS &&
            thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c2) == KERN_SUCCESS && ncur < MAX_THREADS) {
            uint64_t us = (uint64_t)bi.user_time.seconds * 1000000 + (uint64_t)bi.user_time.microseconds +
                          (uint64_t)bi.system_time.seconds * 1000000 + (uint64_t)bi.system_time.microseconds;
            cur[ncur++] = (thread_time){idi.thread_id, us};
            for (int k = 0; k < g_ntt; k++) if (g_tt[k].id == idi.thread_id) {
                double pct = us >= g_tt[k].us && wall_us > 0 ? (us - g_tt[k].us) / wall_us * 100 : 0;
                if (pct > best) { best = pct; pthread_t pt = pthread_from_mach_thread_np(th[i]); name[0] = 0; if (pt) pthread_getname_np(pt, name, 32); if (!name[0]) snprintf(name, 32, "thread %llu", (unsigned long long)idi.thread_id); }
                break;
            }
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof *th);
    memcpy(g_tt, cur, (size_t)ncur * sizeof *cur); g_ntt = ncur;
    return best < 0 ? -1 : best > 100 ? 100 : (int)(best + 0.5);
}

/* Ticks (user + system + nice, total) per CPU; returns the CPU count, 0 on failure. */
static int cpu_ticks(uint64_t busy[SYSLOAD_MAX_CORES], uint64_t total[SYSLOAD_MAX_CORES]) {
    static host_t host; if (!host) host = mach_host_self();
    natural_t n = 0; processor_info_array_t info = NULL; mach_msg_type_number_t cnt = 0;
    if (host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS || !info) return 0;
    int nc = (int)n < SYSLOAD_MAX_CORES ? (int)n : SYSLOAD_MAX_CORES;
    for (int i = 0; i < nc; i++) {
        const integer_t *t = info + i * CPU_STATE_MAX;
        busy[i] = (uint64_t)(uint32_t)t[CPU_STATE_USER] + (uint32_t)t[CPU_STATE_SYSTEM] + (uint32_t)t[CPU_STATE_NICE];
        total[i] = busy[i] + (uint32_t)t[CPU_STATE_IDLE];
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
    return nc;
}

static void *sampler(void *arg) {
    (void)arg;
    pthread_setname_np("totk-sysload");
    uint64_t b0[SYSLOAD_MAX_CORES], t0[SYSLOAD_MAX_CORES], b1[SYSLOAD_MAX_CORES], t1[SYSLOAD_MAX_CORES];
    int ncpu = cpu_ticks(b0, t0); char kind[SYSLOAD_MAX_CORES]; classify(ncpu, kind);
    char tname[32]; (void)busiest_thread(0, tname);   // the first thread times
    double w0 = now_s(), g0; pthread_mutex_lock(&g_mu); g0 = g_busy.busy; pthread_mutex_unlock(&g_mu);
    { double cj, gj; (void)soc_energy(&cj, &gj); }   // the starting energy sample
    for (unsigned k = 1;; k++) {
        usleep(500000);
        int nc = cpu_ticks(b1, t1);
        sysload_info s = {0}; s.gpu_sys = gpu_util();
        double w1 = now_s(), g1; pthread_mutex_lock(&g_mu); g1 = g_busy.busy; pthread_mutex_unlock(&g_mu);
        s.thread_max = busiest_thread((w1 - w0) * 1e6, s.thread_name);
        double gp = w1 > w0 ? (g1 - g0) / (w1 - w0) * 100 : -1;
        s.gpu_game = gp < 0 ? -1 : gp > 100 ? 100 : (int)(gp + 0.5);
        power_sample(&s, w1 - w0);
        for (int i = 0; i < nc && i < ncpu; i++) {
            uint64_t db = b1[i] - b0[i], dt = t1[i] - t0[i]; int v = dt ? (int)((db * 100 + dt / 2) / dt) : 0;
            if (kind[i] == 'P' && s.np < SYSLOAD_MAX_CORES) s.p[s.np++] = v; else if (s.ne < SYSLOAD_MAX_CORES) s.e[s.ne++] = v;
        }
        pthread_mutex_lock(&g_mu); g_cur = s; g_have = nc > 0; pthread_mutex_unlock(&g_mu);
        memcpy(b0, b1, sizeof b0); memcpy(t0, t1, sizeof t0); w0 = w1; g0 = g1;
        if (k % 60 == 0) { char t[128]; sysload_text(t, sizeof t, &s); fprintf(stderr, "[totk-gfx] [load] %s (T is the busiest thread: %s; parentheses show the game's own GPU work, excluding keep-alive)\n", t, s.thread_name);
                             if (sysload_power_text(t, sizeof t, &s) >= 0) fprintf(stderr, "[totk-gfx] [power] %s (%d s averages)\n", t, SYSLOAD_POWER_SECONDS); }   // every 30 s
    }
    return NULL;
}
static void start_once(void) { pthread_t th; if (!pthread_create(&th, NULL, sampler, NULL)) pthread_detach(th); }
void sysload_start(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, start_once); }
