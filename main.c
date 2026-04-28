#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <wchar.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <ncurses.h>
#include <FLAC/metadata.h>

#define MAX_FIELD    512
#define MAX_PATH    1024
#define NUM_FIELDS     5
#define MAX_DIRS      32
#define CMD_BUF    16384

#define FIELD_TITLE    0
#define FIELD_ARTIST   1
#define FIELD_ALBUM    2
#define FIELD_DATE     3
#define FIELD_COVER    4

#define CLR_HEADER     1
#define CLR_LABEL      2
#define CLR_ACTIVE     3
#define CLR_VALUE      4
#define CLR_STATUS     5
#define CLR_ERROR      6
#define CLR_BORDER     7
#define CLR_HINT       8
#define CLR_WARN       9

#define THEME_HEADER_FG      COLOR_CYAN
#define THEME_HEADER_BG      (-1)

#define THEME_LABEL_FG       COLOR_YELLOW
#define THEME_LABEL_BG       (-1)

#define THEME_ACTIVE_FG      COLOR_WHITE
#define THEME_ACTIVE_BG      COLOR_BLACK

#define THEME_VALUE_FG       COLOR_WHITE
#define THEME_VALUE_BG       (-1)

#define THEME_STATUS_OK_FG   COLOR_BLACK
#define THEME_STATUS_OK_BG   COLOR_GREEN

#define THEME_STATUS_ERR_FG  COLOR_WHITE
#define THEME_STATUS_ERR_BG  COLOR_RED

#define THEME_WARN_FG        COLOR_WHITE
#define THEME_WARN_BG        COLOR_BLUE

#define THEME_BORDER_FG      COLOR_CYAN
#define THEME_BORDER_BG      (-1)

#define THEME_HINT_FG        COLOR_GREEN
#define THEME_HINT_BG        (-1)

#define SYM_ACTIVE_ARROW  ">"
#define SYM_UNSAVED       "* Unsaved"
#define SYM_SAVE_OK       "[OK]  Saved successfully!"
#define SYM_SAVE_ERR      "[!!] Save failed! Check permissions / file."

#define FZF_COLORS \
"--color='bg:#181818,bg+:#285577,fg:#f5f4f9,fg+:#ffffff," \
"hl:#77c686,hl+:#77c686,info:#fdf3b6,marker:#f43841," \
"pointer:#57afc0,prompt:#77c686,spinner:#57afc0," \
"border:#453d41,header:#57afc0'"

#define FZF_LAYOUT \
" --height=100%% --border=sharp --margin=10%%,15%% --layout=reverse"

#define MY_KEY_CTRL_LEFT         (KEY_MAX + 1)
#define MY_KEY_CTRL_RIGHT        (KEY_MAX + 2)
#define MY_KEY_CTRL_SHIFT_LEFT   (KEY_MAX + 3)
#define MY_KEY_CTRL_SHIFT_RIGHT  (KEY_MAX + 4)
#define MY_KEY_CTRL_BACKSPACE    (KEY_MAX + 5)

#define CLR_SELECT       10
#define THEME_SELECT_FG  COLOR_BLACK
#define THEME_SELECT_BG  COLOR_CYAN

typedef struct {
    char flac_path[MAX_PATH];
    char album_path[MAX_PATH];
    char display_path[MAX_PATH];
    char title [MAX_FIELD];
    char artist[MAX_FIELD];
    char album [MAX_FIELD];
    char date  [MAX_FIELD];
    char cover_path[MAX_PATH];
    char orig_title [MAX_FIELD];
    char orig_artist[MAX_FIELD];
    char orig_album [MAX_FIELD];
    char orig_date  [MAX_FIELD];
    char orig_cover_path[MAX_PATH];
    char **album_files;
    size_t album_file_count;
    size_t album_file_cap;
    int album_mode;
    int has_cover;
    unsigned sample_rate;
    unsigned channels;
    unsigned bits;
    uint64_t total_samples;
    int dirty;
} FlecState;

#define UNDO_MAX 128

typedef struct {
    char buf[MAX_FIELD];
    int cursor;
    int len;
} EditSnap;

typedef struct {
    char buf[MAX_FIELD];
    int cursor;
    int len;
    int sel_anchor;
    EditSnap undo[UNDO_MAX];
    int undo_top;
} EditBuf;

static volatile sig_atomic_t g_need_redraw = 0;

static void handle_sigcont(int sig)
{
    (void)sig;
    g_need_redraw = 1;
}

static int vc_field_match(const FLAC__StreamMetadata_VorbisComment_Entry *e,
const char *name)
{
    size_t nlen = strlen(name);
    if (e->length <= nlen) return 0;
    if (strncasecmp((const char *)e->entry, name, nlen) != 0) return 0;
    return e->entry[nlen] == '=';
}

static void vc_get_value(const FLAC__StreamMetadata_VorbisComment_Entry *e,
const char *name, char *dst, size_t dstsz)
{
    size_t nlen = strlen(name) + 1;
    size_t vlen = e->length - nlen;
    if (vlen >= dstsz) vlen = dstsz - 1;
    memcpy(dst, e->entry + nlen, vlen);
    dst[vlen] = '\0';
}

static int read_image_file(const char *path, uint8_t **data, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) { fclose(f); return 0; }
    *size = (uint32_t)st.st_size;
    *data = malloc(*size);
    if (!*data) { fclose(f); return 0; }
    if (fread(*data, 1, *size, f) != *size) {
        free(*data); *data = NULL; fclose(f); return 0;
    }
    fclose(f);
    return 1;
}

static const char *detect_mime(const uint8_t *data, uint32_t size)
{
    if (size >= 8 &&
    data[0]==0x89 && data[1]=='P' && data[2]=='N' && data[3]=='G')
    return "image/png";
    if (size >= 3 &&
    data[0]==0xFF && data[1]==0xD8 && data[2]==0xFF)
    return "image/jpeg";
    if (size >= 4 &&
    data[0]=='G' && data[1]=='I' && data[2]=='F')
    return "image/gif";
    if (size >= 2 &&
    data[0]=='B' && data[1]=='M')
    return "image/bmp";
    return "application/octet-stream";
}

static void png_dimensions(const uint8_t *d, uint32_t sz,
uint32_t *w, uint32_t *h)
{
    *w = *h = 0;
    if (sz < 24) return;
    *w = ((uint32_t)d[16]<<24)|((uint32_t)d[17]<<16)|
    ((uint32_t)d[18]<<8)|d[19];
    *h = ((uint32_t)d[20]<<24)|((uint32_t)d[21]<<16)|
    ((uint32_t)d[22]<<8)|d[23];
}

static void jpeg_dimensions(const uint8_t *d, uint32_t sz,
uint32_t *w, uint32_t *h)
{
    *w = *h = 0;
    uint32_t i = 2;
    while (i + 4 < sz) {
        if (d[i] != 0xFF) break;
        uint8_t marker = d[i+1];
        uint16_t seg_len = ((uint16_t)d[i+2]<<8)|d[i+3];
        if ((marker & 0xF0) == 0xC0 && marker != 0xC4 &&
        marker != 0xC8 && marker != 0xCC) {
            if (i + 8 < sz) {
                *h = ((uint32_t)d[i+5]<<8)|d[i+6];
                *w = ((uint32_t)d[i+7]<<8)|d[i+8];
            }
            return;
        }
        i += 2 + seg_len;
    }
}

static int fzf_finish(char *buf, char *out, size_t outsz)
{
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = '\0';
    if (len == 0) return 0;
    snprintf(out, outsz, "%s", buf);
    return 1;
}

static int fzf_available(void)
{
    FILE *f = popen("fzf --version >/dev/null 2>&1", "r");
    if (!f) return 0;
    return pclose(f) == 0;
}

static void expand_tilde(char *path, size_t sz);
static int flec_load(FlecState *st);

static int run_fzf_cover_tty(char *out, size_t outsz,
char (*dirs)[MAX_PATH], int ndirs)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') home = "";

    char tmpfile[] = "/tmp/flec_XXXXXX";
    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) return 0;
    close(tmpfd);

    char find_args[CMD_BUF / 2];
    find_args[0] = '\0';

    char prompt[128];
    char prompt_dir[64];

    if (ndirs == 0) {
        snprintf(find_args, sizeof(find_args), "\"%s\"", home);
        snprintf(prompt, sizeof(prompt), "Select cover image: ");
    } else {
        for (int i = 0; i < ndirs; i++) {
            char exp[MAX_PATH];
            strncpy(exp, dirs[i], MAX_PATH - 1);
            exp[MAX_PATH - 1] = '\0';
            expand_tilde(exp, MAX_PATH);
            size_t cur = strlen(find_args);
            snprintf(find_args + cur, sizeof(find_args) - cur,
            "\"%s\" ", exp);
        }
        if (ndirs == 1) {
            strncpy(prompt_dir, dirs[0], sizeof(prompt_dir) - 1);
            prompt_dir[sizeof(prompt_dir) - 1] = '\0';
            snprintf(prompt, sizeof(prompt), "Select cover image (%s): ", prompt_dir);
        } else {
            snprintf(prompt, sizeof(prompt), "Select cover image (%d dirs): ", ndirs);
        }
    }

    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd),
    "find %s -maxdepth 8 "
    "\\( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png'"
    "   -o -iname '*.bmp'  -o -iname '*.webp' \\)"
    " 2>/dev/null"
    " | sort -u"
    " | fzf " FZF_COLORS " --prompt='%s'"
    FZF_LAYOUT,
    find_args, prompt);

    fflush(stdout);
    ssize_t bytes_written = write(STDOUT_FILENO, "\033[2J\033[H", 7);
    (void)bytes_written;

    int ttyfd = open("/dev/tty", O_RDWR);

    pid_t pid = fork();
    if (pid == 0) {
        if (ttyfd >= 0) { dup2(ttyfd, STDIN_FILENO);  close(ttyfd); }
        int outfd = open(tmpfile, O_WRONLY | O_TRUNC);
        if (outfd >= 0) { dup2(outfd, STDOUT_FILENO); close(outfd); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    if (ttyfd >= 0) close(ttyfd);

    int got = 0;
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        curs_set(1);
        FILE *f = fopen(tmpfile, "r");
        if (f) {
            char buf[MAX_PATH] = "";
            got = (fgets(buf, sizeof(buf), f) != NULL);
            fclose(f);
            if (got)
            got = fzf_finish(buf, out, outsz);
        }
    }

    unlink(tmpfile);
    return got;
}

static int run_fzf_tty(char *out, size_t outsz,
char (*dirs)[MAX_PATH], int ndirs)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') home = "";

    char tmpfile[] = "/tmp/flec_XXXXXX";
    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) return 0;
    close(tmpfd);

    char find_args[CMD_BUF / 2];
    find_args[0] = '\0';

    char prompt[128];
    char prompt_dir[64];

    if (ndirs == 0) {
        snprintf(find_args, sizeof(find_args), "\"%s\" .", home);
        snprintf(prompt, sizeof(prompt), "Select FLAC file: ");
    } else {
        for (int i = 0; i < ndirs; i++) {
            char exp[MAX_PATH];
            strncpy(exp, dirs[i], MAX_PATH - 1);
            exp[MAX_PATH - 1] = '\0';
            expand_tilde(exp, MAX_PATH);
            size_t cur = strlen(find_args);
            snprintf(find_args + cur, sizeof(find_args) - cur,
            "\"%s\" ", exp);
        }
        if (ndirs == 1) {
            strncpy(prompt_dir, dirs[0], sizeof(prompt_dir) - 1);
            prompt_dir[sizeof(prompt_dir) - 1] = '\0';
            snprintf(prompt, sizeof(prompt), "Select FLAC (%s): ", prompt_dir);
        } else {
            snprintf(prompt, sizeof(prompt), "Select FLAC (%d dirs): ", ndirs);
        }
    }

    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd),
    "find %s -maxdepth 8 -iname '*.flac' 2>/dev/null"
    " | sort -u"
    " | fzf " FZF_COLORS " --prompt='%s'"
    FZF_LAYOUT,
    find_args, prompt);

    fflush(stdout);
    ssize_t bytes_written = write(STDOUT_FILENO, "\033[2J\033[H", 7);
    (void)bytes_written;

    int ttyfd = open("/dev/tty", O_RDWR);

    pid_t pid = fork();
    if (pid == 0) {
        if (ttyfd >= 0) { dup2(ttyfd, STDIN_FILENO); close(ttyfd); }
        int outfd = open(tmpfile, O_WRONLY | O_TRUNC);
        if (outfd >= 0) { dup2(outfd, STDOUT_FILENO); close(outfd); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    if (ttyfd >= 0) close(ttyfd);

    int got = 0;
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        curs_set(1);
        FILE *f = fopen(tmpfile, "r");
        if (f) {
            char buf[MAX_PATH] = "";
            got = (fgets(buf, sizeof(buf), f) != NULL);
            fclose(f);
            if (got)
            got = fzf_finish(buf, out, outsz);
        }
    }

    unlink(tmpfile);
    return got;
}


static int run_fzf_dir_tty(char *out, size_t outsz,
char (*dirs)[MAX_PATH], int ndirs)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') home = "";

    char tmpfile[] = "/tmp/flec_XXXXXX";
    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) return 0;
    close(tmpfd);

    char find_args[CMD_BUF / 2];
    find_args[0] = '\0';

    char prompt[128];
    char prompt_dir[64];

    if (ndirs == 0) {
        snprintf(find_args, sizeof(find_args), "\"%s\"", home);
        snprintf(prompt, sizeof(prompt), "Select album directory: ");
    } else {
        for (int i = 0; i < ndirs; i++) {
            char exp[MAX_PATH];
            strncpy(exp, dirs[i], MAX_PATH - 1);
            exp[MAX_PATH - 1] = '\0';
            expand_tilde(exp, MAX_PATH);
            size_t cur = strlen(find_args);
            snprintf(find_args + cur, sizeof(find_args) - cur,
                "\"%s\" ", exp);
        }
        if (ndirs == 1) {
            strncpy(prompt_dir, dirs[0], sizeof(prompt_dir) - 1);
            prompt_dir[sizeof(prompt_dir) - 1] = '\0';
            snprintf(prompt, sizeof(prompt), "Select album directory (%s): ", prompt_dir);
        } else {
            snprintf(prompt, sizeof(prompt), "Select album directory (%d dirs): ", ndirs);
        }
    }

    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd),
    "find %s -maxdepth 8 -type d 2>/dev/null"
    " | sort -u"
    " | fzf " FZF_COLORS " --prompt='%s'"
    FZF_LAYOUT,
    find_args, prompt);

    fflush(stdout);
    ssize_t bytes_written = write(STDOUT_FILENO, "\033[2J\033[H", 7);
    (void)bytes_written;

    int ttyfd = open("/dev/tty", O_RDWR);

    pid_t pid = fork();
    if (pid == 0) {
        if (ttyfd >= 0) { dup2(ttyfd, STDIN_FILENO); close(ttyfd); }
        int outfd = open(tmpfile, O_WRONLY | O_TRUNC);
        if (outfd >= 0) { dup2(outfd, STDOUT_FILENO); close(outfd); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    if (ttyfd >= 0) close(ttyfd);

    int got = 0;
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        curs_set(1);
        FILE *f = fopen(tmpfile, "r");
        if (f) {
            char buf[MAX_PATH] = "";
            got = (fgets(buf, sizeof(buf), f) != NULL);
            fclose(f);
            if (got)
                got = fzf_finish(buf, out, outsz);
        }
    }

    unlink(tmpfile);
    return got;
}

static void expand_tilde(char *path, size_t sz)
{
    if (path[0] != '~' || path[1] != '/') return;
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return;
    size_t hlen = strlen(home);
    size_t rlen = strlen(path + 1);
    if (hlen + rlen >= sz) return;
    memmove(path + hlen, path + 1, rlen + 1);
    memcpy(path, home, hlen);
}


typedef struct {
    char **items;
    size_t count;
    size_t cap;
} PathList;

static void path_list_free(PathList *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int path_list_push(PathList *list, const char *path)
{
    if (!list || !path || path[0] == '\0') return 0;
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 32;
        char **tmp = realloc(list->items, new_cap * sizeof(*tmp));
        if (!tmp) return 0;
        list->items = tmp;
        list->cap = new_cap;
    }
    list->items[list->count] = strdup(path);
    if (!list->items[list->count]) return 0;
    list->count++;
    return 1;
}

static int path_cmp(const void *a, const void *b)
{
    const char *pa = *(const char * const *)a;
    const char *pb = *(const char * const *)b;
    return strcmp(pa, pb);
}

static int has_flac_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    return ext && strcasecmp(ext, ".flac") == 0;
}

static int collect_flac_files_rec(const char *dir, int depth, int max_depth,
PathList *list)
{
    DIR *dp = opendir(dir);
    if (!dp) return 0;

    struct dirent *de;
    int ok = 1;

    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[MAX_PATH];
        int n = snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        if (n < 0 || n >= (int)sizeof(child)) continue;

        struct stat st;
        if (lstat(child, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {
            if (has_flac_ext(child) && !path_list_push(list, child)) {
                ok = 0;
                break;
            }
        } else if (S_ISDIR(st.st_mode) && depth < max_depth) {
            if (!collect_flac_files_rec(child, depth + 1, max_depth, list)) {
                ok = 0;
                break;
            }
        }
    }

    closedir(dp);
    return ok;
}

static int collect_album_files(const char *root, PathList *list)
{
    if (!root || !list) return 0;
    if (!collect_flac_files_rec(root, 0, 2, list)) return 0;
    if (list->count > 1) {
        qsort(list->items, list->count, sizeof(list->items[0]), path_cmp);
    }
    return list->count > 0;
}

static void flec_sync_snapshot(FlecState *st)
{
    if (!st) return;
    memcpy(st->orig_title,      st->title,      MAX_FIELD);
    memcpy(st->orig_artist,     st->artist,     MAX_FIELD);
    memcpy(st->orig_album,      st->album,      MAX_FIELD);
    memcpy(st->orig_date,       st->date,       MAX_FIELD);
    st->cover_path[0] = '\0';
    st->orig_cover_path[0] = '\0';
    st->dirty = 0;
}

static void flec_update_dirty(FlecState *st)
{
    if (!st) return;
    int dirty = 0;
    if (!st->album_mode)
        dirty |= strcmp(st->title, st->orig_title) != 0;
    dirty |= strcmp(st->artist, st->orig_artist) != 0;
    dirty |= strcmp(st->album,  st->orig_album)  != 0;
    dirty |= strcmp(st->date,   st->orig_date)   != 0;
    dirty |= strcmp(st->cover_path, st->orig_cover_path) != 0;
    st->dirty = dirty;
}


static void flec_free_album_files(FlecState *st)
{
    if (!st) return;
    for (size_t i = 0; i < st->album_file_count; i++) free(st->album_files[i]);
    free(st->album_files);
    st->album_files = NULL;
    st->album_file_count = 0;
    st->album_file_cap = 0;
}


static int flec_load_album_selection(FlecState *st, const char *root)
{
    if (!st || !root || root[0] == '\0') return 0;

    char root_copy[MAX_PATH];
    snprintf(root_copy, sizeof(root_copy), "%s", root);

    size_t rlen = strlen(root_copy);
    while (rlen > 1 && root_copy[rlen - 1] == '/')
        root_copy[--rlen] = '\0';

    PathList list = {0};
    if (!collect_album_files(root_copy, &list)) {
        path_list_free(&list);
        return 0;
    }
    if (list.count == 0) {
        path_list_free(&list);
        return 0;
    }

    FlecState probe;
    memset(&probe, 0, sizeof(probe));
    probe.album_mode = 1;
    strncpy(probe.flac_path, list.items[0], MAX_PATH - 1);
    probe.flac_path[MAX_PATH - 1] = '\0';

    if (!flec_load(&probe)) {
        path_list_free(&list);
        return 0;
    }

    flec_sync_snapshot(&probe);
    flec_free_album_files(st);
    *st = probe;
    strncpy(st->album_path, root_copy, MAX_PATH - 1);
    st->album_path[MAX_PATH - 1] = '\0';
    st->album_mode = 1;
    st->album_files = list.items;
    st->album_file_count = list.count;
    st->album_file_cap = list.cap;
    list.items = NULL;
    list.count = 0;
    list.cap = 0;
    return 1;
}

static const char *validate_image_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
    return "File not found";
    if (S_ISDIR(st.st_mode))
    return "That's a directory, not an image file";
    const char *ext = strrchr(path, '.');
    if (!ext)
    return "No extension, expected jpg / png / bmp / webp";
    ext++;
    if (strcasecmp(ext, "jpg")  != 0 &&
    strcasecmp(ext, "jpeg") != 0 &&
    strcasecmp(ext, "png")  != 0 &&
    strcasecmp(ext, "bmp")  != 0 &&
    strcasecmp(ext, "webp") != 0)
    return "Unsupported format, expected jpg / png / bmp / webp";
    return NULL;
}

static int flec_load(FlecState *st)
{
    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return 0;

    if (!FLAC__metadata_chain_read(chain, st->flac_path)) {
        FLAC__metadata_chain_delete(chain);
        return 0;
    }

    FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(it, chain);

    st->title[0] = st->artist[0] = st->album[0] = st->date[0] = '\0';
    st->has_cover = 0;

    do {
        FLAC__StreamMetadata *blk = FLAC__metadata_iterator_get_block(it);

        if (blk->type == FLAC__METADATA_TYPE_STREAMINFO) {
            st->sample_rate   = blk->data.stream_info.sample_rate;
            st->channels      = blk->data.stream_info.channels;
            st->bits          = blk->data.stream_info.bits_per_sample;
            st->total_samples = blk->data.stream_info.total_samples;
        }

        if (blk->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            for (uint32_t i = 0; i < blk->data.vorbis_comment.num_comments; i++) {
                FLAC__StreamMetadata_VorbisComment_Entry *e =
                &blk->data.vorbis_comment.comments[i];
                if (vc_field_match(e, "TITLE"))
                vc_get_value(e, "TITLE", st->title, MAX_FIELD);
                else if (vc_field_match(e, "ARTIST"))
                vc_get_value(e, "ARTIST", st->artist, MAX_FIELD);
                else if (vc_field_match(e, "ALBUM"))
                vc_get_value(e, "ALBUM", st->album, MAX_FIELD);
                else if (vc_field_match(e, "DATE"))
                vc_get_value(e, "DATE", st->date, MAX_FIELD);
            }
        }

        if (blk->type == FLAC__METADATA_TYPE_PICTURE)
        st->has_cover = 1;

    } while (FLAC__metadata_iterator_next(it));

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);
    return 1;
}


static int flec_save(FlecState *st, const char *path)
{
    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return 0;

    if (!FLAC__metadata_chain_read(chain, path)) {
        FLAC__metadata_chain_delete(chain);
        return 0;
    }

    FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(it, chain);

    FLAC__StreamMetadata *vc_block = NULL;

    do {
        FLAC__StreamMetadata *blk = FLAC__metadata_iterator_get_block(it);

        if (blk->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            vc_block = blk;
        }
        if (blk->type == FLAC__METADATA_TYPE_PICTURE &&
        st->cover_path[0] != '\0') {
            FLAC__metadata_iterator_delete_block(it, true);
        }
    } while (FLAC__metadata_iterator_next(it));

    if (!vc_block) {
        vc_block = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        FLAC__metadata_iterator_init(it, chain);
        FLAC__metadata_iterator_insert_block_after(it, vc_block);
    }

    struct { const char *name; const char *val; } tags[5];
    int tag_count = 0;
    if (!st->album_mode)
        tags[tag_count++] = (typeof(tags[0])){ "TITLE", st->title };
    tags[tag_count++] = (typeof(tags[0])){ "ARTIST", st->artist };
    tags[tag_count++] = (typeof(tags[0])){ "ALBUM",  st->album  };
    tags[tag_count++] = (typeof(tags[0])){ "DATE",   st->date   };

    for (int i = 0; i < tag_count; i++) {
        FLAC__StreamMetadata_VorbisComment_Entry entry;
        if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
        &entry, tags[i].name, tags[i].val)) {
            if (!FLAC__metadata_object_vorbiscomment_replace_comment(
            vc_block, entry, 0, false)) {
                FLAC__metadata_object_vorbiscomment_append_comment(
                vc_block, entry, false);
            }
        }
    }

    if (st->cover_path[0] != '\0') {
        uint8_t *img_data = NULL;
        uint32_t img_size = 0;

        if (read_image_file(st->cover_path, &img_data, &img_size)) {
            const char *mime = detect_mime(img_data, img_size);
            uint32_t w = 0, h = 0;

            if (strcmp(mime, "image/png") == 0)
            png_dimensions(img_data, img_size, &w, &h);
            else if (strcmp(mime, "image/jpeg") == 0)
            jpeg_dimensions(img_data, img_size, &w, &h);

            FLAC__StreamMetadata *pic =
            FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);

            FLAC__metadata_object_picture_set_mime_type(pic, (char*)mime, true);
            FLAC__metadata_object_picture_set_description(
            pic, (FLAC__byte*)"", true);
            FLAC__metadata_object_picture_set_data(
            pic, img_data, img_size, true);
            pic->data.picture.type   = FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER;
            pic->data.picture.width  = w;
            pic->data.picture.height = h;
            pic->data.picture.depth  = 24;
            pic->data.picture.colors = 0;

            free(img_data);

            FLAC__metadata_iterator_init(it, chain);
            while (FLAC__metadata_iterator_next(it));
            FLAC__metadata_iterator_insert_block_after(it, pic);

            st->has_cover = 1;
        }
    }

    FLAC__metadata_iterator_delete(it);

    FLAC__metadata_chain_sort_padding(chain);
    int ok = FLAC__metadata_chain_write(chain, true, false);

    FLAC__metadata_chain_delete(chain);
    return ok;
}

static int flec_save_album(FlecState *st)
{
    if (!st || !st->album_files || st->album_file_count == 0) return 0;
    for (size_t i = 0; i < st->album_file_count; i++) {
        if (!flec_save(st, st->album_files[i]))
            return 0;
    }
    flec_sync_snapshot(st);
    return 1;
}

static void make_display_path(const char *full, char *display, size_t dsz)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        size_t hlen = strlen(home);
        if (strncmp(full, home, hlen) == 0 &&
        (full[hlen] == '/' || full[hlen] == '\0')) {
            snprintf(display, dsz, "~%s", full + hlen);
            return;
        }
    }
    snprintf(display, dsz, "%s", full);
}

static void init_colors(void)
{
    start_color();
    use_default_colors();
    init_pair(CLR_HEADER, THEME_HEADER_FG,     THEME_HEADER_BG);
    init_pair(CLR_LABEL,  THEME_LABEL_FG,      THEME_LABEL_BG);
    init_pair(CLR_ACTIVE, THEME_ACTIVE_FG,     THEME_ACTIVE_BG);
    init_pair(CLR_VALUE,  THEME_VALUE_FG,      THEME_VALUE_BG);
    init_pair(CLR_STATUS, THEME_STATUS_OK_FG,  THEME_STATUS_OK_BG);
    init_pair(CLR_ERROR,  THEME_STATUS_ERR_FG, THEME_STATUS_ERR_BG);
    init_pair(CLR_BORDER, THEME_BORDER_FG,     THEME_BORDER_BG);
    init_pair(CLR_HINT,   THEME_HINT_FG,       THEME_HINT_BG);
    init_pair(CLR_WARN,   THEME_WARN_FG,       THEME_WARN_BG);
    init_pair(CLR_SELECT, THEME_SELECT_FG,      THEME_SELECT_BG);
}


static int utf8_seqlen(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_prev_cp(const char *buf, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80)
    pos--;
    return pos;
}


static int utf8_encode(wchar_t wc, char out[4])
{
    uint32_t cp = (uint32_t)wc;
    if (cp < 0x80) {
        out[0] = (char)cp; return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int utf8_width_range(const char *buf, int from, int to)
{
    int cols = 0, i = from;
    while (i < to) {
        int clen = utf8_seqlen((unsigned char)buf[i]);
        if (i + clen > to) break;
        char tmp[5] = {0};
        memcpy(tmp, buf + i, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        int w = wcwidth(wc);
        cols += (w < 1) ? 1 : w;
        i += clen;
    }
    return cols;
}

static int utf8_compute_scroll(const char *buf, int cursor_pos, int vwidth)
{
    int cursor_col = utf8_width_range(buf, 0, cursor_pos);
    if (cursor_col < vwidth) return 0;
    int target = cursor_col - (vwidth - 1);
    int i = 0, col = 0;
    while (i < cursor_pos) {
        int clen = utf8_seqlen((unsigned char)buf[i]);
        char tmp[5] = {0};
        memcpy(tmp, buf + i, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        int w = wcwidth(wc);
        if (w < 1) w = 1;
        if (col + w > target) break;
        col += w;
        i += clen;
    }
    return i;
}

static void draw_border(int rows, int cols)
{
    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(0,      0,      ACS_HLINE, cols);
    mvhline(rows-1, 0,      ACS_HLINE, cols);
    mvvline(1,      0,      ACS_VLINE, rows-2);
    mvvline(1,      cols-1, ACS_VLINE, rows-2);
    mvaddch(0,      0,      ACS_ULCORNER);
    mvaddch(0,      cols-1, ACS_URCORNER);
    mvaddch(rows-1, 0,      ACS_LLCORNER);
    mvaddch(rows-1, cols-1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CLR_BORDER));
}

static void draw_header(int cols, const char *path, int album_mode)
{
    const char *label = album_mode ? "Folder: " : "File: ";
    attron(COLOR_PAIR(CLR_VALUE));
    mvprintw(1, 2, "%s", label);
    attroff(COLOR_PAIR(CLR_VALUE));
    attron(A_BOLD);
    int label_len = (int)strlen(label);
    int avail = cols - (label_len + 4);
    if (avail < 0) avail = 0;
    if ((int)strlen(path) > avail) {
        int cut = (int)strlen(path) - avail + 3;
        if (cut < 0) cut = 0;
        mvprintw(1, 2 + label_len, "...%s", path + cut);
    } else {
        mvprintw(1, 2 + label_len, "%s", path);
    }
    attroff(A_BOLD);

    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(2, 1, ACS_HLINE, cols - 2);
    attroff(COLOR_PAIR(CLR_BORDER));
}

static void draw_stream_info(int row, int cols, const FlecState *st)
{
    attron(COLOR_PAIR(CLR_HINT));
    const char *msg = st->album_mode ? "[ Album Editor ]" : NULL;
    char buf[64];
    if (!msg) {
        double secs = st->sample_rate > 0
        ? (double)st->total_samples / st->sample_rate : 0;
        int m = (int)secs / 60, s = (int)secs % 60;
        snprintf(buf, sizeof(buf),
        "[ %u Hz  %uch  %ubit  %d:%02d ]",
        st->sample_rate, st->channels, st->bits, m, s);
        msg = buf;
    }
    int col = (cols - (int)strlen(msg)) / 2;
    if (col < 1) col = 1;
    mvprintw(row, col, "%s", msg);
    attroff(COLOR_PAIR(CLR_HINT));
}

static void draw_field(int row, int col, int width,
const char *label, const char *value,
int active, int editing,
int cursor_pos, int sel_anchor)
{
    int label_w = 10;

    attron(COLOR_PAIR(CLR_LABEL) | A_BOLD);
    mvprintw(row, col, "%-*s", label_w, label);
    attroff(COLOR_PAIR(CLR_LABEL) | A_BOLD);

    int vstart = col + label_w;
    int vwidth = width - label_w - 2;
    int vlen   = (int)strlen(value);

    int scroll = editing ? utf8_compute_scroll(value, cursor_pos, vwidth) : 0;

    if (editing) {
        attron(COLOR_PAIR(CLR_ACTIVE));
        mvhline(row, vstart, ' ', vwidth);
        attroff(COLOR_PAIR(CLR_ACTIVE));

        int i    = scroll;
        int dcol = 0;

        while (dcol < vwidth) {
            int is_cursor = (i == cursor_pos);
            int in_sel    = 0;

            if (sel_anchor != -1 && sel_anchor != cursor_pos) {
                int lo = cursor_pos < sel_anchor ? cursor_pos : sel_anchor;
                int hi = cursor_pos > sel_anchor ? cursor_pos : sel_anchor;
                in_sel = (i >= lo && i < hi);
            }

            char tmp[5] = {0};
            int  cw     = 1;

            if (i < vlen) {
                int clen = utf8_seqlen((unsigned char)value[i]);
                if (i + clen > vlen) clen = 1;
                memcpy(tmp, value + i, clen);
                wchar_t wc = 0;
                mbstowcs(&wc, tmp, 1);
                int w = wcwidth(wc);
                cw = (w < 1) ? 1 : w;
                if (dcol + cw > vwidth) break;
            } else {
                if (!is_cursor) break;
                tmp[0] = ' ';
            }

            int sx = vstart + dcol;

            if (is_cursor) {
                attron(COLOR_PAIR(CLR_ACTIVE) | A_UNDERLINE | A_BOLD);
                mvaddstr(row, sx, tmp[0] ? tmp : " ");
                attroff(COLOR_PAIR(CLR_ACTIVE) | A_UNDERLINE | A_BOLD);
            } else if (in_sel) {
                attron(COLOR_PAIR(CLR_SELECT));
                mvaddstr(row, sx, tmp);
                attroff(COLOR_PAIR(CLR_SELECT));
            } else {
                attron(COLOR_PAIR(CLR_ACTIVE));
                mvaddstr(row, sx, tmp);
                attroff(COLOR_PAIR(CLR_ACTIVE));
            }

            if (i >= vlen) break;
            dcol += cw;
            i    += utf8_seqlen((unsigned char)value[i]);
        }
    } else {
        if (active) {
            attron(A_REVERSE);
            mvhline(row, vstart, ' ', vwidth);
        } else {
            attron(COLOR_PAIR(CLR_VALUE));
            mvhline(row, vstart, ' ', vwidth);
        }

        int i = 0, dcol = 0;
        while (i < vlen && dcol < vwidth) {
            int clen = utf8_seqlen((unsigned char)value[i]);
            if (i + clen > vlen) break;
            char tmp[5] = {0};
            memcpy(tmp, value + i, clen);
            wchar_t wc = 0;
            mbstowcs(&wc, tmp, 1);
            int w = wcwidth(wc);
            int cw = (w < 1) ? 1 : w;
            if (dcol + cw > vwidth) break;
            mvaddstr(row, vstart + dcol, tmp);
            dcol += cw;
            i    += clen;
        }

        if (active)
        attroff(A_REVERSE);
        else
        attroff(COLOR_PAIR(CLR_VALUE));
    }

    if (active || editing) {
        attron(COLOR_PAIR(CLR_HINT) | A_BOLD);
        mvaddstr(row, col - 2, SYM_ACTIVE_ARROW);
        attroff(COLOR_PAIR(CLR_HINT) | A_BOLD);
    }
}

static void draw_status(int rows, int cols, const char *msg, int status_type)
{
    int pair = (status_type == 1) ? CLR_ERROR
    : (status_type == 2) ? CLR_WARN
    : CLR_STATUS;
    attron(COLOR_PAIR(pair) | A_BOLD);
    int avail = cols - 2;
    int len = (int)strlen(msg);
    int x = (cols - len) / 2;
    if (x < 1) x = 1;
    int print_len = avail - (x - 1);
    if (print_len < 1) print_len = 1;
    mvhline(rows - 2, 1, ' ', avail);
    mvprintw(rows - 2, x, "%.*s", print_len, msg);
    attroff(COLOR_PAIR(pair) | A_BOLD);
}

static void draw_warn(int rows, int cols, const char *msg)
{
    attron(COLOR_PAIR(CLR_WARN) | A_BOLD);
    int avail = cols - 2;
    int len = (int)strlen(msg);
    int x = (cols - len) / 2;
    if (x < 1) x = 1;
    int print_len = avail - (x - 1);
    if (print_len < 1) print_len = 1;
    mvhline(rows - 2, 1, ' ', avail);
    mvprintw(rows - 2, x, "%.*s", print_len, msg);
    attroff(COLOR_PAIR(CLR_WARN) | A_BOLD);
}

static void draw_cover_indicator(int row, int col, int cols, const FlecState *st)
{
    attron(COLOR_PAIR(CLR_LABEL) | A_BOLD);
    mvprintw(row, col, "%-10s", "Cover Art");
    attroff(COLOR_PAIR(CLR_LABEL) | A_BOLD);

    int vstart = col + 10;
    int avail = cols - vstart - 1;
    if (avail < 1) return;

    if (st->cover_path[0] != '\0') {
        int path_avail = avail - 5;
        attron(COLOR_PAIR(CLR_ACTIVE));
        if (path_avail <= 3) {
            mvprintw(row, vstart, "%.*s", avail, "New: ...");
        } else {
            int plen = (int)strlen(st->cover_path);
            if (plen > path_avail) {
                mvprintw(row, vstart, "New: ...%s",
                st->cover_path + plen - (path_avail - 3));
            } else {
                mvprintw(row, vstart, "New: %-*s", path_avail, st->cover_path);
            }
        }
        attroff(COLOR_PAIR(CLR_ACTIVE));
    } else {
        const char *label = st->has_cover ? "[Embedded cover present]"
        : "[No cover art]";
        attron(st->has_cover ? COLOR_PAIR(CLR_HINT) : COLOR_PAIR(CLR_VALUE));
        mvprintw(row, vstart, "%.*s", avail, label);
        attroff(st->has_cover ? COLOR_PAIR(CLR_HINT) : COLOR_PAIR(CLR_VALUE));
    }
}

static void ebuf_init(EditBuf *e, const char *src)
{
    strncpy(e->buf, src, MAX_FIELD - 1);
    e->buf[MAX_FIELD - 1] = '\0';
    e->len = strlen(e->buf);
    e->cursor = e->len;
    e->sel_anchor = -1;
    e->undo_top = 0;
}

static void ebuf_push_undo(EditBuf *e)
{
    if (e->undo_top < UNDO_MAX) {
        memcpy(e->undo[e->undo_top].buf, e->buf, MAX_FIELD);
        e->undo[e->undo_top].cursor = e->cursor;
        e->undo[e->undo_top].len = e->len;
        e->undo_top++;
    }
}

static void ebuf_undo(EditBuf *e)
{
    if (e->undo_top == 0) return;
    e->undo_top--;
    memcpy(e->buf, e->undo[e->undo_top].buf, MAX_FIELD);
    e->cursor = e->undo[e->undo_top].cursor;
    e->len = e->undo[e->undo_top].len;
    e->sel_anchor = -1;
}

static int  sel_active(const EditBuf *e) { return e->sel_anchor != -1; }
static int  sel_lo    (const EditBuf *e) { return e->cursor < e->sel_anchor ? e->cursor : e->sel_anchor; }
static int  sel_hi    (const EditBuf *e) { return e->cursor > e->sel_anchor ? e->cursor : e->sel_anchor; }
static void sel_clear (EditBuf *e)       { e->sel_anchor = -1; }

static void sel_start(EditBuf *e)
{
    if (!sel_active(e)) e->sel_anchor = e->cursor;
}

static void ebuf_delete_selection_raw(EditBuf *e)
{
    if (!sel_active(e)) return;
    int lo = sel_lo(e), hi = sel_hi(e);
    memmove(e->buf + lo, e->buf + hi, e->len - hi + 1);
    e->len -= (hi - lo);
    e->cursor = lo;
    sel_clear(e);
}

static void ebuf_delete_selection(EditBuf *e)
{
    if (!sel_active(e)) return;
    ebuf_push_undo(e);
    ebuf_delete_selection_raw(e);
}

static void ebuf_select_all(EditBuf *e)
{
    e->sel_anchor = 0;
    e->cursor = e->len;
}

static int word_skip_left(const EditBuf *e)
{
    int p = e->cursor;
    while (p > 0) {
        int prev = utf8_prev_cp(e->buf, p);
        int clen = p - prev;
        char tmp[5] = {0};
        memcpy(tmp, e->buf + prev, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        if (iswalnum((wint_t)wc)) break;
        p = prev;
    }
    while (p > 0) {
        int prev = utf8_prev_cp(e->buf, p);
        int clen = p - prev;
        char tmp[5] = {0};
        memcpy(tmp, e->buf + prev, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        if (!iswalnum((wint_t)wc)) break;
        p = prev;
    }
    return p;
}

static int word_skip_right(const EditBuf *e)
{
    int p = e->cursor;
    while (p < e->len) {
        int clen = utf8_seqlen((unsigned char)e->buf[p]);
        char tmp[5] = {0};
        memcpy(tmp, e->buf + p, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        if (!iswalnum((wint_t)wc)) break;
        p += clen;
    }
    while (p < e->len) {
        int clen = utf8_seqlen((unsigned char)e->buf[p]);
        char tmp[5] = {0};
        memcpy(tmp, e->buf + p, clen);
        wchar_t wc = 0;
        mbstowcs(&wc, tmp, 1);
        if (iswalnum((wint_t)wc)) break;
        p += clen;
    }
    return p;
}

static void ebuf_insert_wchar(EditBuf *e, wchar_t wc)
{
    char utf8[4];
    int n = utf8_encode(wc, utf8);
    ebuf_push_undo(e);
    ebuf_delete_selection_raw(e);
    if (e->len + n >= MAX_FIELD) return;
    memmove(e->buf + e->cursor + n, e->buf + e->cursor,
    e->len - e->cursor + 1);
    memcpy(e->buf + e->cursor, utf8, n);
    e->cursor += n;
    e->len    += n;
}

static void ebuf_delete_word_left(EditBuf *e)
{
    if (sel_active(e)) { ebuf_delete_selection(e); return; }
    int dest = word_skip_left(e);
    if (dest == e->cursor) return;
    ebuf_push_undo(e);
    int nbytes = e->cursor - dest;
    memmove(e->buf + dest, e->buf + e->cursor,
    e->len - e->cursor + 1);
    e->len    -= nbytes;
    e->cursor  = dest;
}

static void ebuf_delete(EditBuf *e)
{
    if (sel_active(e)) { ebuf_delete_selection(e); return; }
    if (e->cursor == 0) return;
    ebuf_push_undo(e);
    int new_cursor = utf8_prev_cp(e->buf, e->cursor);
    int nbytes = e->cursor - new_cursor;
    memmove(e->buf + new_cursor, e->buf + e->cursor,
    e->len - e->cursor + 1);
    e->cursor  = new_cursor;
    e->len    -= nbytes;
}

static void ebuf_delete_fwd(EditBuf *e)
{
    if (sel_active(e)) { ebuf_delete_selection(e); return; }
    if (e->cursor >= e->len) return;
    ebuf_push_undo(e);
    int nbytes = utf8_seqlen((unsigned char)e->buf[e->cursor]);
    if (e->cursor + nbytes > e->len) nbytes = e->len - e->cursor;
    memmove(e->buf + e->cursor, e->buf + e->cursor + nbytes,
    e->len - e->cursor - nbytes + 1);
    e->len -= nbytes;
}

static void ebuf_insert_str(EditBuf *e, const char *s, int len)
{
    if (len <= 0 && !sel_active(e)) return;
    ebuf_push_undo(e);
    ebuf_delete_selection_raw(e);
    for (int i = 0; i < len; i++) {
        if (e->len >= MAX_FIELD - 1) break;
        memmove(e->buf + e->cursor + 1, e->buf + e->cursor,
        e->len - e->cursor + 1);
        e->buf[e->cursor++] = s[i];
        e->len++;
    }
}

static void ebuf_paste_clipboard(EditBuf *e)
{
    FILE *p = popen(
    "xclip -o -selection clipboard 2>/dev/null ||"
    " xsel -bo 2>/dev/null ||"
    " wl-paste --no-newline 2>/dev/null",
    "r");
    if (!p) return;

    char clip[MAX_FIELD];
    int  clen = 0;
    int  c;
    while ((c = fgetc(p)) != EOF && clen < MAX_FIELD - 1) {
        if (c == '\n' || c == '\r') continue;
        clip[clen++] = (char)c;
    }
    clip[clen] = '\0';
    pclose(p);

    ebuf_insert_str(e, clip, clen);
}

static const char *field_labels[NUM_FIELDS] = {
    "Title", "Artist", "Album", "Date", "Cover Art"
};

#define ANSI_RESET     "\033[0m"
#define ANSI_BOLD      "\033[1m"
#define ANSI_UNDERLINE "\033[4m"

#define COL "%-26s"

static void print_help(void)
{
    printf(
    ANSI_BOLD "flec" ANSI_RESET ", "
    "FLAC metadata editor in C.\n"

    "\n"
    "Usage: flec [OPTIONS]\n"
    "\n"

    ANSI_UNDERLINE "Options:" ANSI_RESET "\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "\n"

    ANSI_UNDERLINE "Keybinds:" ANSI_RESET "\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "\n"

    ANSI_UNDERLINE "Dependencies:" ANSI_RESET "\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n"
    "  " COL "  %s\n",

    "-f, --flac  <dir>...",     "Directories to search for FLAC files.",
    "-c, --cover <dir>...",     "Directories to search for cover images.",
    "-o, --open  <path>",       "Open a FLAC file directly; in album mode this must be a directory.",
    "-a, --album",              "Edit album-wide tags for every FLAC in a directory tree.",
    "-nf, --no-fzf",            "Disable fzf; use manual path input instead.",
    "-h, --help",               "Show this message.",

    "j / k / arrows",           "Navigate fields.",
    "Enter / e / a",            "Edit selected field.",
    "Escape",                   "Cancel current edit.",
    "Ctrl+S",                   "Save changes to file.",
    "Ctrl+X",                   "Discard pending cover path.",
    "r",                        "Search another FLAC file to edit.",
    "q",                        "Quit (prompts if there are unsaved changes).",

    "libFLAC",                  "Used for FLAC stream metadata editing (read/write).",
    "ncurses",                  "Used for terminal UI rendering.",
    "fzf",                      "Nice file picker (optional)."
    );
}

static int prompt_path_ncurses(char *out, size_t outsz, int want_dir)
{
    EditBuf ebuf;
    ebuf_init(&ebuf, "");
    char status_msg[256] = "";
    int  status_err = 0;

    for (;;) {
        if (g_need_redraw) { g_need_redraw = 0; clearok(stdscr, TRUE); }

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        draw_border(rows, cols);

        attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        mvprintw(1, 2, "flec");
        attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);

        attron(COLOR_PAIR(CLR_BORDER));
        mvhline(2, 1, ACS_HLINE, cols - 2);
        attroff(COLOR_PAIR(CLR_BORDER));

        int fcol   = 4;
        int fwidth = cols - 6;
        int row    = rows / 2 - 1;

        attron(COLOR_PAIR(CLR_HINT));
        mvprintw(row - 2, fcol,
        want_dir ? "Enter directory path  (Escape to quit):"
        : "Enter FLAC file path  (Escape to quit):");
        attroff(COLOR_PAIR(CLR_HINT));

        draw_field(row, fcol, fwidth,
        want_dir ? "Directory" : "File path", ebuf.buf, 1, 1,
        ebuf.cursor, ebuf.sel_anchor);

        attron(COLOR_PAIR(CLR_HINT));
        mvprintw(row + 1, fcol,
        want_dir
        ? "Tip: ~/relative or /absolute paths are both accepted."
        : "Tip: ~/relative or /absolute paths are both accepted.");
        attroff(COLOR_PAIR(CLR_HINT));

        if (status_msg[0])
        draw_status(rows, cols, status_msg, status_err);

        curs_set(1);
        int vstart  = fcol + 10;
        int vwidth  = fwidth - 10 - 2;
        int scroll  = utf8_compute_scroll(ebuf.buf, ebuf.cursor, vwidth);
        int cur_col = utf8_width_range(ebuf.buf, scroll, ebuf.cursor);
        move(row, vstart + cur_col);

        refresh();

        wint_t wch;
        int chtype = get_wch(&wch);

        switch (wch) {

        case 19:
        case '\n':
        case KEY_ENTER:
            sel_clear(&ebuf);
            if (ebuf.len == 0) {
                snprintf(status_msg, sizeof(status_msg),
                "[!!] Path cannot be empty.");
                status_err = 1;
                break;
            }
            snprintf(out, outsz, "%s", ebuf.buf);
            expand_tilde(out, outsz);
            return 1;

        case 27:
            return 0;

        case 1:
            ebuf_select_all(&ebuf);
            break;

        case 22:
            ebuf_paste_clipboard(&ebuf);
            break;

        case 26:
            ebuf_undo(&ebuf);
            break;

        case KEY_BACKSPACE:
        case 127:
            ebuf_delete(&ebuf);
            break;

        case 8:
        case MY_KEY_CTRL_BACKSPACE:
            ebuf_delete_word_left(&ebuf);
            break;

        case KEY_DC:
            ebuf_delete_fwd(&ebuf);
            break;

        case 11:
            sel_clear(&ebuf);
            ebuf_push_undo(&ebuf);
            ebuf.buf[ebuf.cursor] = '\0';
            ebuf.len = ebuf.cursor;
            break;

        case 21:
            sel_clear(&ebuf);
            ebuf_push_undo(&ebuf);
            memmove(ebuf.buf, ebuf.buf + ebuf.cursor,
            ebuf.len - ebuf.cursor + 1);
            ebuf.len    -= ebuf.cursor;
            ebuf.cursor  = 0;
            break;

        case KEY_LEFT:
            if (sel_active(&ebuf)) {
                ebuf.cursor = sel_lo(&ebuf);
                sel_clear(&ebuf);
            } else if (ebuf.cursor > 0) {
                ebuf.cursor--;
            }
            break;

        case KEY_RIGHT:
            if (sel_active(&ebuf)) {
                ebuf.cursor = sel_hi(&ebuf);
                sel_clear(&ebuf);
            } else if (ebuf.cursor < ebuf.len) {
                ebuf.cursor++;
            }
            break;

        case KEY_SLEFT:
            if (ebuf.cursor > 0) {
                sel_start(&ebuf);
                ebuf.cursor--;
                if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
            }
            break;

        case KEY_SRIGHT:
            if (ebuf.cursor < ebuf.len) {
                sel_start(&ebuf);
                ebuf.cursor++;
                if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
            }
            break;

        case MY_KEY_CTRL_LEFT:
            ebuf.cursor = word_skip_left(&ebuf);
            sel_clear(&ebuf);
            break;

        case MY_KEY_CTRL_RIGHT:
            ebuf.cursor = word_skip_right(&ebuf);
            sel_clear(&ebuf);
            break;

            case MY_KEY_CTRL_SHIFT_LEFT: {
                int dest = word_skip_left(&ebuf);
                if (dest != ebuf.cursor) {
                    sel_start(&ebuf);
                    ebuf.cursor = dest;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;
            }

            case MY_KEY_CTRL_SHIFT_RIGHT: {
                int dest = word_skip_right(&ebuf);
                if (dest != ebuf.cursor) {
                    sel_start(&ebuf);
                    ebuf.cursor = dest;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;
            }

        case KEY_HOME:
            ebuf.cursor = 0;
            sel_clear(&ebuf);
            break;

        case KEY_END:
            ebuf.cursor = ebuf.len;
            sel_clear(&ebuf);
            break;

        case KEY_RESIZE:
            break;

        default:
            if (chtype == OK && (wchar_t)wch >= 32)
            ebuf_insert_wchar(&ebuf, (wchar_t)wch);
            break;
        }
    }
}


int main(int argc, char **argv)
{
    FlecState st;
    memset(&st, 0, sizeof(st));

    char flac_dirs[MAX_DIRS][MAX_PATH];
    int  flac_dirs_count = 0;
    char cover_dirs[MAX_DIRS][MAX_PATH];
    int  cover_dirs_count = 0;
    char open_path[MAX_PATH] = "";
    int  no_fzf_mode = 0;
    int  album_mode = 0;

    typedef enum { MODE_NONE, MODE_FLAC_DIRS, MODE_COVER_DIRS } ParseMode;
    ParseMode mode = MODE_NONE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 ||
        strcmp(argv[i], "--flac") == 0) {
            mode = MODE_FLAC_DIRS;

        } else if (strcmp(argv[i], "-c") == 0 ||
        strcmp(argv[i], "--cover") == 0) {
            mode = MODE_COVER_DIRS;

        } else if (strcmp(argv[i], "-a") == 0 ||
        strcmp(argv[i], "--album") == 0) {
            album_mode = 1;
            mode = MODE_NONE;

        } else if (strcmp(argv[i], "-h") == 0 ||
        strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;

        } else if (strcmp(argv[i], "-o") == 0 ||
        strcmp(argv[i], "--open") == 0) {
            i++;
            if (i >= argc || argv[i][0] == '-') {
                fprintf(stderr,
                "Error: %s requires exactly one path argument.\n"
                "  Example: flec -o \"~/Music/file.flac\"\n",
                argv[i - 1]);
                return 1;
            }
            strncpy(open_path, argv[i], MAX_PATH - 1);
            open_path[MAX_PATH - 1] = '\0';
            mode = MODE_NONE;

        } else if (strcmp(argv[i], "-nf") == 0 ||
        strcmp(argv[i], "--no-fzf") == 0) {
            no_fzf_mode = 1;
            mode = MODE_NONE;

        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr,
            "Usage: flec [OPTIONS]\n"
            "  -f,  --flac   <dir>...  directories to search for FLAC files\n"
            "  -c,  --cover  <dir>...  directories to search for cover images\n"
            "  -o,  --open   <path>    open this FLAC file directly\n"
            "  -a,  --album            edit an album directory instead of one file\n"
            "  -nf, --no-fzf           disable fzf; use manual path input\n"
            "  -h,  --help             show help message\n");
            return 1;

        } else {
            if (mode == MODE_FLAC_DIRS) {
                if (flac_dirs_count >= MAX_DIRS) {
                    fprintf(stderr, "Error: too many -f directories (max %d).\n",
                    MAX_DIRS);
                    return 1;
                }
                strncpy(flac_dirs[flac_dirs_count], argv[i], MAX_PATH - 1);
                flac_dirs[flac_dirs_count][MAX_PATH - 1] = '\0';
                flac_dirs_count++;

            } else if (mode == MODE_COVER_DIRS) {
                if (cover_dirs_count >= MAX_DIRS) {
                    fprintf(stderr, "Error: too many -c directories (max %d).\n",
                    MAX_DIRS);
                    return 1;
                }
                strncpy(cover_dirs[cover_dirs_count], argv[i], MAX_PATH - 1);
                cover_dirs[cover_dirs_count][MAX_PATH - 1] = '\0';
                cover_dirs_count++;

            } else {
                fprintf(stderr,
                "Unexpected argument: %s\n"
                "Use -f to specify FLAC directories, -o for a direct path,"
                " -a for album mode, or -nf for no-fzf mode.\n",
                argv[i]);
                return 1;
            }
        }
    }

    for (int i = 0; i < flac_dirs_count; i++) {
        char exp[MAX_PATH];
        strncpy(exp, flac_dirs[i], MAX_PATH - 1);
        exp[MAX_PATH - 1] = '\0';
        expand_tilde(exp, MAX_PATH);
        struct stat ds;
        if (stat(exp, &ds) != 0) {
            fprintf(stderr, "Error: -f directory not found: '%s'\n", flac_dirs[i]);
            return 1;
        }
        if (!S_ISDIR(ds.st_mode)) {
            fprintf(stderr, "Error: -f path is not a directory: '%s'\n", flac_dirs[i]);
            return 1;
        }
    }

    for (int i = 0; i < cover_dirs_count; i++) {
        char exp[MAX_PATH];
        strncpy(exp, cover_dirs[i], MAX_PATH - 1);
        exp[MAX_PATH - 1] = '\0';
        expand_tilde(exp, MAX_PATH);
        struct stat ds;
        if (stat(exp, &ds) != 0) {
            fprintf(stderr, "Error: -c directory not found: '%s'\n", cover_dirs[i]);
            return 1;
        }
        if (!S_ISDIR(ds.st_mode)) {
            fprintf(stderr, "Error: -c path is not a directory: '%s'\n", cover_dirs[i]);
            return 1;
        }
    }

    setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);

    define_key("\033[1;5D", MY_KEY_CTRL_LEFT);
    define_key("\033[1;5C", MY_KEY_CTRL_RIGHT);
    define_key("\033[1;6D", MY_KEY_CTRL_SHIFT_LEFT);
    define_key("\033[1;6C", MY_KEY_CTRL_SHIFT_RIGHT);
    define_key("\033[27;5;8~", MY_KEY_CTRL_BACKSPACE);
    define_key("\033[127;5u",  MY_KEY_CTRL_BACKSPACE);

    signal(SIGCONT, handle_sigcont);

    if (has_colors()) init_colors();

    int have_fzf = fzf_available();
    int use_fzf  = have_fzf && !no_fzf_mode;

    if (open_path[0] != '\0') {
        expand_tilde(open_path, MAX_PATH);
        struct stat fs;
        if (stat(open_path, &fs) != 0) {
            endwin();
            fprintf(stderr, "Error: -o path not found: '%s'\n", open_path);
            return 1;
        }
        if (album_mode) {
            if (!S_ISDIR(fs.st_mode)) {
                endwin();
                fprintf(stderr,
                "Error: -o path is not a directory in album mode: '%s'\n",
                open_path);
                return 1;
            }
            strncpy(st.album_path, open_path, MAX_PATH - 1);
            st.album_path[MAX_PATH - 1] = '\0';
        } else {
            if (!S_ISREG(fs.st_mode)) {
                endwin();
                fprintf(stderr, "Error: -o path is not a regular file: '%s'\n",
                open_path);
                return 1;
            }
            strncpy(st.flac_path, open_path, MAX_PATH - 1);
            st.flac_path[MAX_PATH - 1] = '\0';
        }

    } else if (no_fzf_mode) {
        if (album_mode) {
            if (!prompt_path_ncurses(st.album_path, MAX_PATH, 1)) {
                endwin();
                return 0;
            }
            struct stat fs;
            if (stat(st.album_path, &fs) != 0) {
                endwin();
                fprintf(stderr, "Error: path not found: '%s'\n", st.album_path);
                return 1;
            }
            if (!S_ISDIR(fs.st_mode)) {
                endwin();
                fprintf(stderr, "Error: path is not a directory: '%s'\n",
                st.album_path);
                return 1;
            }
        } else {
            if (!prompt_path_ncurses(st.flac_path, MAX_PATH, 0)) {
                endwin();
                return 0;
            }
            struct stat fs;
            if (stat(st.flac_path, &fs) != 0) {
                endwin();
                fprintf(stderr, "Error: path not found: '%s'\n", st.flac_path);
                return 1;
            }
            if (!S_ISREG(fs.st_mode)) {
                endwin();
                fprintf(stderr, "Error: path is not a regular file: '%s'\n",
                st.flac_path);
                return 1;
            }
        }

    } else {
        if (album_mode) {
            if (!run_fzf_dir_tty(st.album_path, MAX_PATH, flac_dirs, flac_dirs_count)) {
                endwin();
                return 0;
            }
        } else {
            if (!run_fzf_tty(st.flac_path, MAX_PATH, flac_dirs, flac_dirs_count)) {
                endwin();
                return 0;
            }
        }
    }

    if (album_mode) {
        if (!flec_load_album_selection(&st, st.album_path)) {
            endwin();
            fprintf(stderr,
            "Error: cannot read FLAC metadata from album directory '%s'.\n"
            "Make sure the directory exists and contains readable FLAC files.\n",
            st.album_path);
            return 1;
        }
        make_display_path(st.album_path, st.display_path, MAX_PATH);
    } else {
        make_display_path(st.flac_path, st.display_path, MAX_PATH);
        if (!flec_load(&st)) {
            endwin();
            fprintf(stderr,
            "Error: cannot read FLAC metadata from '%s'.\n"
            "Make sure the file exists and is a valid FLAC file.\n",
            st.flac_path);
            return 1;
        }
        flec_sync_snapshot(&st);
    }

    if (album_mode) {
        make_display_path(st.album_path, st.display_path, MAX_PATH);
    }

    int selected = 0;
    int editing  = 0;
    EditBuf ebuf;
    char status_msg[256] = "";
    int  status_err = 0;
    int  running = 1;
    char *fields_ptr[NUM_FIELDS] = {
        st.title, st.artist, st.album, st.date, st.cover_path
    };

    int visible_fields[NUM_FIELDS];
    int visible_count = album_mode ? 4 : 5;
    if (album_mode) {
        visible_fields[0] = FIELD_ARTIST;
        visible_fields[1] = FIELD_ALBUM;
        visible_fields[2] = FIELD_DATE;
        visible_fields[3] = FIELD_COVER;
    } else {
        visible_fields[0] = FIELD_TITLE;
        visible_fields[1] = FIELD_ARTIST;
        visible_fields[2] = FIELD_ALBUM;
        visible_fields[3] = FIELD_DATE;
        visible_fields[4] = FIELD_COVER;
    }

    while (running) {
        if (g_need_redraw) {
            g_need_redraw = 0;
            clearok(stdscr, TRUE);
        }

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        draw_border(rows, cols);
        draw_header(cols, st.album_mode ? st.album_path : st.display_path, st.album_mode);
        draw_stream_info(3, cols, &st);

        attron(COLOR_PAIR(CLR_BORDER));
        mvhline(4, 1, ACS_HLINE, cols - 2);
        attroff(COLOR_PAIR(CLR_BORDER));

        int field_rows[NUM_FIELDS];
        for (int i = 0; i < visible_count; i++) {
            field_rows[i] = 6 + i * 2;
        }
        int fcol   = 4;
        int fwidth = cols - 6;

        for (int i = 0; i < visible_count; i++) {
            int actual = visible_fields[i];
            int act  = (selected == i);
            int edit = (editing && selected == i);

            if (actual == FIELD_COVER) {
                if (edit) {
                    draw_field(field_rows[i], fcol, fwidth,
                    field_labels[actual], ebuf.buf, act, edit,
                    ebuf.cursor, ebuf.sel_anchor);
                } else {
                    if (act) {
                        attron(COLOR_PAIR(CLR_HINT) | A_BOLD);
                        mvaddstr(field_rows[i], fcol - 2, SYM_ACTIVE_ARROW);
                        attroff(COLOR_PAIR(CLR_HINT) | A_BOLD);
                    }
                    draw_cover_indicator(field_rows[i], fcol, cols, &st);
                    if (act) {
                        attron(COLOR_PAIR(CLR_HINT));
                        if (use_fzf) {
                            mvprintw(field_rows[i] + 1, fcol,
                            "Press Enter to browse with fzf"
                            "  (jpg / png / bmp / webp)");
                        } else {
                            mvprintw(field_rows[i] + 1, fcol,
                            "Press Enter to type a path"
                            "  (jpg / png / bmp / webp)");
                        }
                        attroff(COLOR_PAIR(CLR_HINT));
                    }
                }
            } else {
                const char *val = (edit) ? ebuf.buf : fields_ptr[actual];
                int cur = (edit) ? ebuf.cursor : 0;
                int anch = (edit) ? ebuf.sel_anchor : -1;
                draw_field(field_rows[i], fcol, fwidth,
                field_labels[actual], val, act, edit, cur, anch);
                if (actual == FIELD_DATE && act && !edit) {
                    attron(COLOR_PAIR(CLR_HINT));
                    mvprintw(field_rows[i] + 1, fcol,
                    "Format: YYYY  or  YYYY-MM  or  YYYY-MM-DD");
                    attroff(COLOR_PAIR(CLR_HINT));
                }
            }
        }

        if (st.dirty) {
            attron(COLOR_PAIR(CLR_LABEL) | A_BOLD);
            mvprintw(4, cols - (int)strlen(SYM_UNSAVED) - 3,
            " %s ", SYM_UNSAVED);
            attroff(COLOR_PAIR(CLR_LABEL) | A_BOLD);
        }

        if (status_msg[0])
            draw_status(rows, cols, status_msg, status_err);

        if (editing) {
            curs_set(1);
            int r = field_rows[selected];
            int vstart = fcol + 10;
            int vwidth = fwidth - 10 - 2;
            int scroll  = utf8_compute_scroll(ebuf.buf, ebuf.cursor, vwidth);
            int cur_col = utf8_width_range(ebuf.buf, scroll, ebuf.cursor);
            int cx = vstart + cur_col;
            move(r, cx);
        } else {
            curs_set(0);
        }

        refresh();

        wint_t wch;
        int chtype = get_wch(&wch);

        if (editing) {
            int actual = visible_fields[selected];
            switch (wch) {
            case 27:
                editing = 0;
                sel_clear(&ebuf);
                status_msg[0] = '\0';
                break;

            case 1:
                ebuf_select_all(&ebuf);
                break;

            case 22:
                ebuf_paste_clipboard(&ebuf);
                break;

            case 19:
            case '\n':
            case KEY_ENTER:
                sel_clear(&ebuf);
                if (strcmp(ebuf.buf, fields_ptr[actual]) == 0) {
                    editing = 0;
                    break;
                }
                if (actual == FIELD_COVER) {
                    expand_tilde(ebuf.buf, sizeof(ebuf.buf));
                    ebuf.len = strlen(ebuf.buf);

                    if (ebuf.buf[0] == '\0') {
                        st.cover_path[0] = '\0';
                        editing = 0;
                        flec_update_dirty(&st);
                        status_msg[0] = '\0';
                        status_err = 0;
                    } else {
                        const char *err = validate_image_path(ebuf.buf);
                        if (err) {
                            snprintf(status_msg, sizeof(status_msg),
                            "[!!] %s", err);
                            status_err = 1;
                        } else {
                            strncpy(st.cover_path, ebuf.buf, MAX_PATH - 1);
                            st.cover_path[MAX_PATH - 1] = '\0';
                            flec_update_dirty(&st);
                            editing = 0;
                            snprintf(status_msg, sizeof(status_msg),
                            "Cover path set successfully.");
                            status_err = 0;
                        }
                    }
                } else {
                    strncpy(fields_ptr[actual], ebuf.buf, MAX_FIELD - 1);
                    fields_ptr[actual][MAX_FIELD - 1] = '\0';
                    flec_update_dirty(&st);
                    editing = 0;
                    snprintf(status_msg, sizeof(status_msg),
                    "%s field set successfully.", field_labels[actual]);
                    status_err = 0;
                }
                if (wch == 19 && !status_err) {
                    if (!st.dirty) {
                        snprintf(status_msg, sizeof(status_msg),
                        "No changes to be saved.");
                        status_err = 2;
                    } else if (album_mode ? flec_save_album(&st)
                    : flec_save(&st, st.flac_path)) {
                        snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_OK);
                        status_err = 0;
                    } else {
                        snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_ERR);
                        status_err = 1;
                    }
                }
                break;

            case 26:
                ebuf_undo(&ebuf);
                break;

            case KEY_BACKSPACE:
            case 127:
                ebuf_delete(&ebuf);
                break;

            case 8:
            case MY_KEY_CTRL_BACKSPACE:
                ebuf_delete_word_left(&ebuf);
                break;

            case KEY_DC:
                ebuf_delete_fwd(&ebuf);
                break;

            case KEY_LEFT:
                if (sel_active(&ebuf)) {
                    ebuf.cursor = sel_lo(&ebuf);
                    sel_clear(&ebuf);
                } else if (ebuf.cursor > 0) {
                    ebuf.cursor--;
                }
                break;

            case KEY_RIGHT:
                if (sel_active(&ebuf)) {
                    ebuf.cursor = sel_hi(&ebuf);
                    sel_clear(&ebuf);
                } else if (ebuf.cursor < ebuf.len) {
                    ebuf.cursor++;
                }
                break;

            case KEY_SLEFT:
                if (ebuf.cursor > 0) {
                    sel_start(&ebuf);
                    ebuf.cursor--;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;

            case KEY_SRIGHT:
                if (ebuf.cursor < ebuf.len) {
                    sel_start(&ebuf);
                    ebuf.cursor++;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;

            case MY_KEY_CTRL_LEFT:
                ebuf.cursor = word_skip_left(&ebuf);
                sel_clear(&ebuf);
                break;

            case MY_KEY_CTRL_RIGHT:
                ebuf.cursor = word_skip_right(&ebuf);
                sel_clear(&ebuf);
                break;

            case MY_KEY_CTRL_SHIFT_LEFT: {
                int dest = word_skip_left(&ebuf);
                if (dest != ebuf.cursor) {
                    sel_start(&ebuf);
                    ebuf.cursor = dest;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;
            }

            case MY_KEY_CTRL_SHIFT_RIGHT: {
                int dest = word_skip_right(&ebuf);
                if (dest != ebuf.cursor) {
                    sel_start(&ebuf);
                    ebuf.cursor = dest;
                    if (ebuf.cursor == ebuf.sel_anchor) sel_clear(&ebuf);
                }
                break;
            }

            case KEY_HOME:
                ebuf.cursor = 0;
                sel_clear(&ebuf);
                break;

            case KEY_END:
                ebuf.cursor = ebuf.len;
                sel_clear(&ebuf);
                break;

            case KEY_RESIZE:
                break;

            default:
                if (chtype == OK && (wchar_t)wch >= 32)
                    ebuf_insert_wchar(&ebuf, (wchar_t)wch);
                break;
            }
            continue;
        }

        switch (wch) {
        case 'q':
        case 'Q':
            if (st.dirty) {
                draw_warn(rows, cols,
                "Unsaved changes! Press Q again to quit, any key to cancel.");
                refresh();
                { wint_t c2; get_wch(&c2);
                if (c2 == 'q' || c2 == 'Q') running = 0; }
            } else {
                running = 0;
            }
            break;

        case '\t':
        case KEY_DOWN:
        case 'j':
            selected = (selected + 1) % visible_count;
            status_msg[0] = '\0';
            break;

        case KEY_UP:
        case 'k':
            selected = (selected - 1 + visible_count) % visible_count;
            status_msg[0] = '\0';
            break;

        case '\n':
        case KEY_ENTER:
        case 'a':
        case 'A':
        case 'e':
        case 'E': {
            int actual = visible_fields[selected];
            if (actual == FIELD_COVER && use_fzf) {
                char img_path[MAX_PATH] = "";
                int picked = run_fzf_cover_tty(img_path, MAX_PATH,
                cover_dirs, cover_dirs_count);
                clearok(stdscr, TRUE);
                if (picked && img_path[0] != '\0') {
                    const char *err = validate_image_path(img_path);
                    if (err) {
                        snprintf(status_msg, sizeof(status_msg), "[!!] %s", err);
                        status_err = 1;
                    } else {
                        strncpy(st.cover_path, img_path, MAX_PATH - 1);
                        st.cover_path[MAX_PATH - 1] = '\0';
                        flec_update_dirty(&st);
                        snprintf(status_msg, sizeof(status_msg),
                        "Cover path set successfully.");
                        status_err = 0;
                    }
                }
            } else {
                ebuf_init(&ebuf, fields_ptr[actual]);
                editing = 1;
                status_msg[0] = '\0';
            }
            break;
        }

        case 19:
            if (!st.dirty) {
                snprintf(status_msg, sizeof(status_msg), "No changes to be saved.");
                status_err = 2;
                break;
            }
            snprintf(status_msg, sizeof(status_msg), "Saving...");
            status_err = 0;
            refresh();
            if (album_mode ? flec_save_album(&st) : flec_save(&st, st.flac_path)) {
                snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_OK);
                status_err = 0;
            } else {
                snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_ERR);
                status_err = 1;
            }
            break;

        case 24:
            if (visible_fields[selected] == FIELD_COVER) {
                strncpy(st.cover_path, st.orig_cover_path, MAX_PATH - 1);
                st.cover_path[MAX_PATH - 1] = '\0';
                flec_update_dirty(&st);
                status_msg[0] = '\0';
                status_err = 0;
            }
            break;

        case 'r':
        case 'R':
            if (st.dirty) {
                draw_warn(rows, cols,
                "Unsaved changes! Press R again to discard, any key to cancel.");
                refresh();
                { wint_t rc2; get_wch(&rc2);
                if (rc2 != 'r' && rc2 != 'R') break; }
            }
            {
                if (album_mode) {
                    char new_root[MAX_PATH] = "";
                    int picked = use_fzf
                    ? run_fzf_dir_tty(new_root, MAX_PATH,
                    flac_dirs, flac_dirs_count)
                    : prompt_path_ncurses(new_root, MAX_PATH, 1);
                    clearok(stdscr, TRUE);
                    if (picked && new_root[0] != '\0') {
                        struct stat ds;
                        if (stat(new_root, &ds) != 0) {
                            snprintf(status_msg, sizeof(status_msg),
                            "[!!] Path not found: %s", new_root);
                            status_err = 1;
                        } else if (!S_ISDIR(ds.st_mode)) {
                            snprintf(status_msg, sizeof(status_msg),
                            "[!!] Path is not a directory: %s", new_root);
                            status_err = 1;
                        } else if (flec_load_album_selection(&st, new_root)) {
                            make_display_path(st.album_path, st.display_path, MAX_PATH);
                            selected   = 0;
                            editing    = 0;
                            status_err = 0;
                            snprintf(status_msg, sizeof(status_msg),
                            "Now viewing album: %.200s", st.album_path);
                        } else {
                            snprintf(status_msg, sizeof(status_msg),
                            "[!!] Cannot read FLAC metadata from selected album.");
                            status_err = 1;
                        }
                    }
                } else {
                    char new_path[MAX_PATH] = "";
                    int picked = use_fzf
                    ? run_fzf_tty(new_path, MAX_PATH,
                    flac_dirs, flac_dirs_count)
                    : prompt_path_ncurses(new_path, MAX_PATH, 0);
                    clearok(stdscr, TRUE);
                    if (picked && new_path[0] != '\0') {
                        FlecState probe;
                        memset(&probe, 0, sizeof(probe));
                        snprintf(probe.flac_path, MAX_PATH, "%s", new_path);
                        if (flec_load(&probe)) {
                            flec_sync_snapshot(&probe);
                            st = probe;
                            selected   = 0;
                            editing    = 0;
                            status_err = 0;
                            snprintf(st.display_path, MAX_PATH, "%s", "");
                            make_display_path(st.flac_path, st.display_path, MAX_PATH);
                            snprintf(status_msg, sizeof(status_msg),
                            "Now viewing: %.200s", st.display_path);
                        } else {
                            snprintf(status_msg, sizeof(status_msg),
                            "[!!] Cannot read FLAC metadata from selected file.");
                            status_err = 1;
                        }
                    }
                }
            }
            break;

        case 26:
            def_prog_mode();
            endwin();
            raise(SIGTSTP);
            reset_prog_mode();
            clearok(stdscr, TRUE);
            break;

        case KEY_RESIZE:
            break;

        default:
            break;
        }
    }

    endwin();
    flec_free_album_files(&st);
    return 0;
}
