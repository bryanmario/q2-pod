/* Startup Screen (docs/boot.md#startup-screen): Display > Appearance's row opens a page of three
 * choices. Default is the firmware's own splash (or the card's boot logo). Image and Animation list
 * the card's pictures or GIFs; the one picked is checked from its own header, then, once the user
 * agrees (playback pauses), made once on a thread by stock ffmpeg at the lowest priority into
 * IMAGE_DST or ANIM_DST: filling the screen, cut to it, turned as the framebuffer is, so the splash
 * has nothing left to scale or turn (tools/build.py LOGO_HOOK). Only one of the two is ever kept: a
 * new one removes the other, and Default removes both. Then the page offers a restart. */
#include "navigation.h"

#define ANIM_DST "/mnt/data/startup.gif"  /* tools/build.py ANIM */
#define IMAGE_DST "/mnt/data/startup.jpg" /* tools/build.py STARTUP_IMAGE */
#define SU_PAL "/tmp/startup-pal.png"
#define SU_PROGRESS "/tmp/startup-progress"
#define SU_MAX (5 * 1024 * 1024 / 2)  /* 2.5 MB, the picked file */
#define SU_MADE_MAX (2 * 1024 * 1024) /* the made file, read back to be checked */
#define ANIM_W 480 /* "480p" across; the screen's 375 x 320 needs no fitting at all */
#define ANIM_H 360
#define IMAGE_W 720
#define IMAGE_H 540
#define ANIM_CS 500 /* 5 seconds, in the GIF's hundredths (tools/build.py ANIM_MAX_S) */
#define ANIM_SECS "5"
/* frames >= 100 / ANIM_FPS = 3 hundredths (a GIF can't store 3.33; 30 fps exports use 3) */
#define ANIM_FPS 30
/* fill the 375 x 320 screen, the middle kept, then turned as the framebuffer is */
#define SU_FILL "scale=375:320:force_original_aspect_ratio=increase,crop=375:320,transpose=clock"
/* left free on /mnt/data (17.7 MB, docs/hardware.md) once it is saved: settings and the library */
#define SU_SPARE_MB 4
#define SU_FILES 256 /* the most the list shows */
#define SU_DEPTH 2   /* the card's root and two levels of folders under it */

#ifdef PEQ_HOST
#define su_nice(n) ((void)(n))
#else
/* demo has no nice import; Linux o32's, as chapters.c's calls */
int su_nice(int n);
__asm__(".set push\n.set noreorder\n"
        ".globl su_nice\nsu_nice:\nli $v0,4034\nsyscall\njr $ra\nnop\n"
        ".set pop\n");
#endif

enum { SU_DEFAULT, SU_IMAGE, SU_ANIM };
extern int ringnav_reboot(void); /* playback.c: the queue saved, then stock's reboot */

static struct {
    void *page, *row, *status;
    char **files, src[600]; /* src: the job's own copy of the picked path */
    int n, kind, want, made_kind;
    unsigned timer, frames;
    unsigned long thread;
    volatile int busy, step, frame;
    const char *volatile err;
} su __attribute__((section(".scratch")));

/* The GIF's length in hundredths of a second and its frames (*frames), its blocks walked (GIF89a);
 * -1 if they don't hold together, -2 if a frame is shorter than ANIM_FPS allows. A delay under 2 is
 * shown as 10, as ffmpeg's gif demuxer (and browsers) show it. */
static int gif_length(const unsigned char *b, unsigned n, unsigned *frames) {
    unsigned i = 13, total = 0, delay = 0, size;
    *frames = 0;
    if (b[10] & 0x80) i += 3u << ((b[10] & 7) + 1); /* the global colour table */
    while (i < n) {
        if (b[i] == 0x3b) return (int)total; /* the trailer */
        if (b[i] == 0x21 && i + 2 < n) {
            if (b[i + 1] == 0xf9 && i + 6 < n) delay = b[i + 4] | b[i + 5] << 8;
            i += 2;
        } else if (b[i] == 0x2c && i + 10 < n) { /* a frame */
            unsigned cs = delay < 2 ? 10 : delay;
            if (cs < 100 / ANIM_FPS) return -2;
            if (total < ANIM_CS) ++*frames; /* the ones ffmpeg's -t keeps */
            total += cs, delay = 0;
            i += b[i + 9] & 0x80 ? 10 + (3u << ((b[i + 9] & 7) + 1)) : 10;
            i++; /* the LZW code size */
        } else
            return -1;
        while (i < n && (size = b[i])) i += size + 1; /* the sub-blocks, to their end */
        i++;
    }
    return -1;
}

/* What is wrong with path for kind, or 0; an animation's frames in su.frames. */
static const char *su_check(const char *path, int kind) {
    int w = 0, h = 0;
    long size = file_size(path);
    if (size < 0) return "Could not read the file";
    if (size > SU_MAX) return "The file must not exceed 2.5 MB.";
    if (kind == SU_IMAGE) {
        if (!picture_size(path, &w, &h)) return "The image must be a JPEG or PNG.";
        return w > IMAGE_W || h > IMAGE_H ? "The image must not exceed 720x540 pixels." : 0;
    }
    unsigned n = 0;
    unsigned char *b = read_all(path, SU_MAX, &n);
    const char *err = 0;
    int length = 0;
    if (!b) return "Could not read the file";
    if (n < 14 || memcmp(b, "GIF8", 4) || (length = gif_length(b, n, &su.frames)) == -1)
        err = "The animation must be a GIF.";
    else if ((b[6] | b[7] << 8) > ANIM_W || (b[8] | b[9] << 8) > ANIM_H) /* the GIF's own size */
        err = "The animation must not exceed 480x360 pixels.";
    else if (length == -2)
        err = "The animation must not exceed 30 frames per second.";
    else if (length > ANIM_CS)
        err = "The animation must not exceed 5 seconds.";
    free(b);
    return err;
}

/* Stock ffmpeg on src at the lowest priority, waited for, killed first if memory runs out; 1 when
 * it succeeded. 0: an animation's colours (SU_PAL), 1: its frames in them, 2: the image. Each
 * writes its progress to SU_PROGRESS (su_poll reads the frame count). */
static int su_ffmpeg(const char *src, int pass) {
    int status, pid = fork();
    if (!pid) {
        child_oom_first();
        su_nice(19); /* the player always first */
        if (pass == 0)
            execl("/usr/bin/ffmpeg", "/usr/bin/ffmpeg", "-nostdin", "-v", "quiet", "-y", "-i", src,
                  "-t", ANIM_SECS, "-vf", SU_FILL ",palettegen", SU_PAL, (char *)0);
        else if (pass == 1)
            execl("/usr/bin/ffmpeg", "/usr/bin/ffmpeg", "-nostdin", "-v", "quiet", "-y",
                  "-progress", SU_PROGRESS, "-i", src, "-i", SU_PAL, "-t", ANIM_SECS, "-lavfi",
                  SU_FILL "[a];[a][1:v]paletteuse", "-f", "gif", ANIM_DST ".tmp", (char *)0);
        else
            execl("/usr/bin/ffmpeg", "/usr/bin/ffmpeg", "-nostdin", "-v", "quiet", "-y", "-i", src,
                  "-vf", SU_FILL, "-frames:v", "1", "-pix_fmt", "yuvj444p", "-q:v", "2", "-f",
                  "mjpeg", IMAGE_DST ".tmp", (char *)0);
        child_exit(127);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && !status;
}

/* The made file (dst's .tmp) read back: a GIF, or the JPEG stock display_logo draws. */
static int su_made_ok(int kind) {
    unsigned n = 0;
    unsigned char *b =
        read_all(kind == SU_ANIM ? ANIM_DST ".tmp" : IMAGE_DST ".tmp", SU_MADE_MAX, &n);
    int ok = b && (kind == SU_ANIM ? n > 6 && !memcmp(b, "GIF8", 4)
                                   : jpeg_size(b, n) == (320u << 16 | 375));
    free(b); /* (/mnt/data is mounted sync: ffmpeg's file is on flash when it ends) */
    return ok;
}

/* The thread: su.files[su.want] made into its kind's file; the other kind's goes once it is. */
static void *su_job(void *unused) {
    const char *src = su.src, *err = 0;
    int kind = su.kind, ok;
    if (!card_room("/mnt/data", (SU_MADE_MAX >> 20) + SU_SPARE_MB, 0))
        err = "No room to save the startup screen";
    else {
        if (kind == SU_ANIM) {
            su.step = 1;
            ok = su_ffmpeg(src, 0);
            su.step = 2;
            ok = ok && su_ffmpeg(src, 1);
        } else
            ok = su_ffmpeg(src, 2);
        ok = ok && su_made_ok(kind);
        if (ok && !rename(kind == SU_ANIM ? ANIM_DST ".tmp" : IMAGE_DST ".tmp",
                          kind == SU_ANIM ? ANIM_DST : IMAGE_DST))
            unlink(kind == SU_ANIM ? IMAGE_DST : ANIM_DST); /* only one is ever kept */
        else
            err = "Could not make the startup screen";
    }
    unlink(ANIM_DST ".tmp"), unlink(IMAGE_DST ".tmp"), unlink(SU_PAL), unlink(SU_PROGRESS);
    su.err = err;
    su.busy = 0;
    return 0;
}

static int su_current(void) {
    return !access(ANIM_DST, 0) ? SU_ANIM : !access(IMAGE_DST, 0) ? SU_IMAGE : SU_DEFAULT;
}
static const char *const su_names[] = { "Default", "Image", "Animation" };

static void su_text(void) {
    char text[48];
    if (!su.row) return;
    tk_snprintf(text, sizeof text, "Startup Screen: %s", su_names[su_current()]);
    row_label(su.row, text);
}

static void su_drop_files(void) {
    for (int i = 0; i < su.n; ++i) free(su.files[i]);
    free(su.files);
    su.files = 0;
    su.n = 0;
}

/* A .gif (kind SU_ANIM) or a .jpg, .jpeg or .png that is not hidden. */
static int su_file(const char *name, int kind) {
    const char *dot = strrchr(name, '.');
    return kind == SU_ANIM ? name[0] != '.' && dot && !strcasecmp(dot + 1, "gif")
                           : photo_file(name);
}
/* dir's files of kind, and its folders' to depth more levels, into su.files. */
static void su_scan(const char *dir, int depth) {
    void *d = opendir(dir);
    char path[600];
    for (struct dirent *e; d && su.n < SU_FILES && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        tk_snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if ((e->d_type == 8 || !e->d_type) && su_file(e->d_name, su.kind)) {
            if ((su.files[su.n] = strdup(path))) ++su.n;
        } else if ((e->d_type == 4 || !e->d_type) && depth)
            su_scan(path, depth - 1);
    }
    if (d) closedir(d);
}

static int su_choice(void *ctx, event_t *event);
static int su_pick(void *ctx, event_t *event);
static int su_poll(const void *unused);

/* The three choices, the one in use saying so. */
static void su_main(void) {
    static const char *const details[] = { "The built-in screen", "A JPEG or PNG from the card",
                                           "A GIF from the card, up to 5 s" };
    int now = su_current();
    su_drop_files();
    su.status = 0;
    void *view = page_list(su.page, su.page, 0, "Startup Screen", 3, 64);
    for (int i = 0; i < 3; ++i) {
        char detail[64];
        tk_snprintf(detail, sizeof detail, "%s%s", i == now ? "In use: " : "", details[i]);
        page_row_detail(view, i, su_names[i], detail, su_choice);
    }
    su_text();
}

static int su_by_path(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The card's files of su.kind, by path, each its name over its folder. */
static void su_list(void) {
    su_drop_files();
    su.files = calloc(SU_FILES, sizeof *su.files);
    if (su.files && card_mounted()) su_scan("/mnt/mmc", SU_DEPTH);
    qsort(su.files, (unsigned)su.n, sizeof *su.files, su_by_path);
    void *view = page_list_or(
        su.page, su.page, su.kind == SU_ANIM ? "Choose a GIF" : "Choose an Image",
        su.kind == SU_ANIM ? "No GIFs on the card" : "No pictures on the card", su.n, 64);
    for (int i = 0; i < su.n; ++i) {
        const char *path = su.files[i] + sizeof "/mnt/mmc" - 1, *slash = strrchr(path, '/');
        char folder[300];
        tk_snprintf(folder, sizeof folder, "%.*s", slash > path ? (int)(slash - path) : 1, path);
        page_row_detail(view, i, slash + 1, folder, su_pick);
    }
}

/* Clicks only schedule (a click never destroys its own row); these run from the timer. */
static int su_choose(const void *unused) {
    su.timer = 0;
    if (su.want == SU_DEFAULT) {
        unlink(ANIM_DST), unlink(IMAGE_DST);
        toast("Startup screen set to Default");
        su_main();
    } else {
        su.kind = su.want;
        su_list();
    }
    return 0;
}
static int su_choice(void *ctx, event_t *event) {
    su.want = (int)(long)ctx;
    rearm(&su.timer, su_choose, 1);
    return 0;
}

static int su_start(const void *unused) {
    struct {
        int kind, ms;
        char text[0x400];
    } prompt = { 1, 0, "Music pauses while it's made." }; /* one line: the dialog's label */
    su.timer = 0;
    const char *err = su_check(su.files[su.want], su.kind);
    if (err) {
        toast(err);
        return 0;
    }
    if (navigator_to_with_context("dialog/confirminfo_dialog", &prompt) != 10 || !su.page) return 0;
    media_set_playing(0);
    tk_snprintf(su.src, sizeof su.src, "%s", su.files[su.want]);
    su.busy = 1, su.step = 0, su.err = 0;
    unlink(SU_PROGRESS);
    if (pthread_create(&su.thread, 0, su_job, 0)) {
        su.busy = 0;
        toast("Could not make the startup screen");
        return 0;
    }
    pthread_detach(su.thread);
    void *view = page_list(su.page, su.page, 0, "Startup Screen", 1, 48);
    su.status = page_row_detail(view, 0, "Getting ready", 0, su_pick);
    su.status = widget_get_child(su.status, 0);
    rearm(&su.timer, su_poll, 300);
    return 0;
}
static int su_pick(void *ctx, event_t *event) {
    if (su.busy || !su.files) return 0;
    su.want = (int)(long)ctx;
    rearm(&su.timer, su_start, 1);
    return 0;
}

/* The frames ffmpeg's -progress says it has written so far. */
static unsigned su_frames_done(void) {
    unsigned n = 0, frame = 0;
    char *b = read_all(SU_PROGRESS, 64 * 1024, &n), *at;
    for (at = b; at && (at = strstr(at, "frame=")); at += 6) frame = (unsigned)atoi(at + 6);
    free(b);
    return frame;
}

/* The job's progress while it runs, then the restart offer. */
static int su_poll(const void *unused) {
    char text[48];
    if (su.busy) {
        if (su.kind != SU_ANIM)
            tk_snprintf(text, sizeof text, "Making the image");
        else if (su.step < 2)
            tk_snprintf(text, sizeof text, "Step 1 of 2: colours");
        else {
            unsigned done = su_frames_done(), all = su.frames ? su.frames : 1;
            tk_snprintf(text, sizeof text, "Step 2 of 2: %u%%",
                        done >= all ? 99 : done * 100 / all);
        }
        if (su.status) widget_set_text_utf8(su.status, text);
        return 8; /* RET_REPEAT */
    }
    su.timer = 0;
    if (!su.page) return 0;
    if (su.err) {
        toast(su.err);
        su_main();
        return 0;
    }
    su_main();
    struct {
        int kind, ms;
        char text[0x400];
    } prompt = { 1, 0, "Startup screen ready. Restart now?" };
    if (navigator_to_with_context("dialog/confirminfo_dialog", &prompt) == 10) ringnav_reboot();
    return 0;
}

static int su_closed(void *ctx, event_t *event) {
    stop_timer(&su.timer); /* a running job finishes on its own; su_main reads the result */
    su.page = su.status = 0;
    su_drop_files();
    su_text();
    return 0;
}
/* Return goes back from a file list to the choices, else closes; not while the job runs. */
static int su_keyup(void *ctx, event_t *event) {
    if (I(event, EVENT_KEY) != KEY_RETURN) return 0;
    if (!su.busy && su.files)
        su_main();
    else if (!su.busy)
        navigator_back();
    return STOP;
}

static int su_open(void *ctx, event_t *event) {
    if (su.page || su.busy || !(su.page = page_open("startup_page", su_closed, su_keyup))) return 0;
    su_main();
    return 0;
}
static int su_shown(void *ctx, event_t *event) {
    su_text();
    return 0;
}
static int su_row_gone(void *ctx, event_t *event) {
    if (ctx == su.row) su.row = 0;
    return 0;
}
/* Display > Appearance's Startup Screen row. */
void startup_row(void *win, void *view) {
    if (!view) return;
    su.row = list_row(view, "system_display", su_open, 0);
    widget_on(su.row, EVT_DESTROY, su_row_gone, su.row);
    su_text();
    widget_on(win, EVT_WINDOW_FOREGROUND, su_shown, 0);
}
