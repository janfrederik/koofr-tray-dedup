// Drop-in libappindicator3.so.1 for the Koofr desktop client.
//
// Koofr re-sets an identical tray icon every second, each time via a fresh
// mkstemp() file it deletes a second later. The GNOME AppIndicator extension
// reads that path asynchronously, hits deleted/half-written files and can end
// up in a CPU loop inside gnome-shell. This shim forwards Koofr's six
// app_indicator_* calls to the real Ayatana library, but only passes an icon
// on when its bytes change, and then as a stable copy Koofr cannot delete.
//
// Koofr dlopen()s libappindicator3.so.1 before falling back to Ayatana, so
// putting this library first on LD_LIBRARY_PATH is enough; the Koofr binary
// is not modified.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Opaque on purpose: no build dependency on GTK or appindicator headers.
typedef void AppIndicator;

#define REAL_LIB "libayatana-appindicator3.so.1"
#define MAX_ICON_BYTES (1 << 20)

static void *real_lib;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static AppIndicator *(*real_new)(const char *, const char *, int);
static void (*real_set_icon_full)(AppIndicator *, const char *, const char *);
static void (*real_set_attention_icon_full)(AppIndicator *, const char *, const char *);
static void (*real_set_status)(AppIndicator *, int);
static void (*real_set_menu)(AppIndicator *, void *);
static void (*real_set_label)(AppIndicator *, const char *, const char *);

static void *must_sym(const char *name) {
    void *p = dlsym(real_lib, name);
    if (!p) {
        fprintf(stderr, "koofr-tray-dedup: %s: %s\n", name, dlerror());
        abort();
    }
    return p;
}

static void load_real(void) {
    real_lib = dlopen(REAL_LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!real_lib) {
        fprintf(stderr, "koofr-tray-dedup: %s\n", dlerror());
        abort();
    }
    real_new = must_sym("app_indicator_new");
    real_set_icon_full = must_sym("app_indicator_set_icon_full");
    real_set_attention_icon_full = must_sym("app_indicator_set_attention_icon_full");
    real_set_status = must_sym("app_indicator_set_status");
    real_set_menu = must_sym("app_indicator_set_menu");
    real_set_label = must_sym("app_indicator_set_label");
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
#define ENSURE_LOADED() pthread_once(&once, load_real)

// Last icon forwarded per slot (normal / attention).
struct slot {
    uint64_t hash;
    int valid;
    char *name;  // last non-file icon name, for plain dedup
};
static struct slot icon_slot, attention_slot;

static uint64_t fnv1a(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = malloc(MAX_ICON_BYTES);
    size_t n = buf ? fread(buf, 1, MAX_ICON_BYTES, f) : 0;
    int too_big = buf && n == MAX_ICON_BYTES && fgetc(f) != EOF;
    fclose(f);
    if (!buf || n == 0 || too_big) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

// Writes the icon to $XDG_RUNTIME_DIR/koofr-tray-dedup/<hash>.<ext> via
// rename(), so readers never see a partial file. Content-addressed names give
// each distinct icon its own path (the tray host may cache by path) and keep
// the file count bounded by the number of distinct icons Koofr uses.
static char *stable_copy(const unsigned char *buf, size_t len, uint64_t hash) {
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !*base) base = "/tmp";
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/koofr-tray-dedup", base);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return NULL;

    // Koofr's temp files have no extension; the tray host sniffs content, so
    // add one purely for readability when it is SVG.
    const char *ext = (len > 4 && memmem(buf, len < 512 ? len : 512, "<svg", 4)) ? ".svg" : "";

    char *path = NULL;
    if (asprintf(&path, "%s/%016llx%s", dir, (unsigned long long)hash, ext) < 0) return NULL;
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == len) return path;

    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) goto fail;
    int ok = fwrite(buf, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        goto fail;
    }
    return path;
fail:
    free(path);
    return NULL;
}

typedef void (*set_fn)(AppIndicator *, const char *, const char *);

static void dedup_set(struct slot *s, set_fn real, AppIndicator *ind, const char *name, const char *desc) {
    pthread_mutex_lock(&lock);
    size_t len = 0;
    unsigned char *buf = (name && name[0] == '/') ? read_file(name, &len) : NULL;

    if (!buf) {
        // Themed icon name, or an unreadable path: dedup on the string only.
        if (s->name && name && strcmp(s->name, name) == 0) goto out;
        free(s->name);
        s->name = name ? strdup(name) : NULL;
        s->valid = 0;
        pthread_mutex_unlock(&lock);
        real(ind, name, desc);
        return;
    }

    uint64_t h = fnv1a(buf, len);
    if (s->valid && s->hash == h) goto out;
    char *stable = stable_copy(buf, len, h);
    free(buf);
    buf = NULL;
    s->hash = h;
    s->valid = 1;
    free(s->name);
    s->name = NULL;
    pthread_mutex_unlock(&lock);
    real(ind, stable ? stable : name, desc);
    free(stable);
    return;
out:
    free(buf);
    pthread_mutex_unlock(&lock);
}

AppIndicator *app_indicator_new(const char *id, const char *icon_name, int category) {
    ENSURE_LOADED();
    // The initial icon is also a temp file Koofr deletes later.
    size_t len = 0;
    unsigned char *buf = (icon_name && icon_name[0] == '/') ? read_file(icon_name, &len) : NULL;
    char *stable = NULL;
    if (buf) {
        uint64_t h = fnv1a(buf, len);
        stable = stable_copy(buf, len, h);
        pthread_mutex_lock(&lock);
        icon_slot.hash = h;
        icon_slot.valid = 1;
        pthread_mutex_unlock(&lock);
        free(buf);
    }
    AppIndicator *ind = real_new(id, stable ? stable : icon_name, category);
    free(stable);
    return ind;
}

void app_indicator_set_icon_full(AppIndicator *ind, const char *name, const char *desc) {
    ENSURE_LOADED();
    dedup_set(&icon_slot, real_set_icon_full, ind, name, desc);
}

void app_indicator_set_attention_icon_full(AppIndicator *ind, const char *name, const char *desc) {
    ENSURE_LOADED();
    dedup_set(&attention_slot, real_set_attention_icon_full, ind, name, desc);
}

void app_indicator_set_status(AppIndicator *ind, int status) {
    ENSURE_LOADED();
    real_set_status(ind, status);
}

void app_indicator_set_menu(AppIndicator *ind, void *menu) {
    ENSURE_LOADED();
    real_set_menu(ind, menu);
}

void app_indicator_set_label(AppIndicator *ind, const char *label, const char *guide) {
    ENSURE_LOADED();
    real_set_label(ind, label, guide);
}
