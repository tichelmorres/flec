#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ncurses.h>
#include <FLAC/metadata.h>

#define MAX_FIELD   512
#define MAX_PATH   1024
#define NUM_FIELDS    4

#define FIELD_TITLE   0
#define FIELD_ARTIST  1
#define FIELD_ALBUM   2
#define FIELD_COVER   3

#define CLR_HEADER    1
#define CLR_LABEL     2
#define CLR_ACTIVE    3
#define CLR_VALUE     4
#define CLR_STATUS    5
#define CLR_ERROR     6
#define CLR_BORDER    7
#define CLR_HINT      8
#define CLR_WARN      9

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
    "pointer:#57afc0,prompt:#9e95c7,spinner:#57afc0," \
    "border:#453d41,header:#57afc0'"

typedef struct {
    char  flac_path[MAX_PATH];
    char  display_path[MAX_PATH];
    char  title [MAX_FIELD];
    char  artist[MAX_FIELD];
    char  album [MAX_FIELD];
    char  cover_path[MAX_PATH];
    char  orig_title [MAX_FIELD];
    char  orig_artist[MAX_FIELD];
    char  orig_album [MAX_FIELD];
    char  orig_cover_path[MAX_PATH];
    int   has_cover;
    unsigned sample_rate;
    unsigned channels;
    unsigned bits;
    uint64_t total_samples;
    int   dirty;
} FlecState;

typedef struct {
    char  buf[MAX_FIELD];
    int   cursor;
    int   len;
} EditBuf;

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

static int pick_file_fzf(char *out, size_t outsz, int music_only)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') home = "";

    char cmd[2048];
    if (music_only) {
        snprintf(cmd, sizeof(cmd),
            "find \"%s/Music\" -maxdepth 8 -iname '*.flac' 2>/dev/null"
            " | sort -u"
            " | sed 's|^%s/|~/|'"
            " | fzf " FZF_COLORS " --prompt='Select FLAC (~/Music/): '"
            " --height=40%% --border",
            home, home);
    } else {
        snprintf(cmd, sizeof(cmd),
            "find \"%s\" . -maxdepth 6 -iname '*.flac' 2>/dev/null"
            " | sort -u"
            " | fzf " FZF_COLORS " --prompt='Select FLAC file: '"
            " --height=40%% --border",
            home);
    }

    FILE *p = popen(cmd, "r");
    if (!p) return 0;

    char buf[MAX_PATH] = "";
    int got = (fgets(buf, sizeof(buf), p) != NULL);
    pclose(p);

    if (!got) return 0;
    if (!fzf_finish(buf, out, outsz)) return 0;

    if (music_only && out[0] == '~' && out[1] == '/') {
        size_t hlen = strlen(home);
        size_t rlen = strlen(out + 1);
        if (hlen + rlen < outsz) {
            memmove(out + hlen, out + 1, rlen + 1);
            memcpy(out, home, hlen);
        }
    }

    return 1;
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

static const char *validate_image_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return "File not found";
    if (S_ISDIR(st.st_mode))
        return "That's a directory, not an image file";
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "No extension – expected jpg / png / gif / bmp / webp";
    ext++;
    if (strcasecmp(ext, "jpg")  != 0 &&
        strcasecmp(ext, "jpeg") != 0 &&
        strcasecmp(ext, "png")  != 0 &&
        strcasecmp(ext, "gif")  != 0 &&
        strcasecmp(ext, "bmp")  != 0 &&
        strcasecmp(ext, "webp") != 0)
        return "Unsupported format – expected jpg / png / gif / bmp / webp";
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

    st->title[0] = st->artist[0] = st->album[0] = '\0';
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
            }
        }

        if (blk->type == FLAC__METADATA_TYPE_PICTURE)
            st->has_cover = 1;

    } while (FLAC__metadata_iterator_next(it));

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);
    return 1;
}

static int flec_save(FlecState *st)
{
    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return 0;

    if (!FLAC__metadata_chain_read(chain, st->flac_path)) {
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

    struct { const char *name; const char *val; } tags[] = {
        { "TITLE",  st->title  },
        { "ARTIST", st->artist },
        { "ALBUM",  st->album  },
    };
    for (int i = 0; i < 3; i++) {
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

    if (ok) {
        st->dirty = 0;
        st->cover_path[0] = '\0';
    }
    return ok;
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

static void draw_header(int cols, const char *path)
{
    attron(COLOR_PAIR(CLR_VALUE));
    mvprintw(1, 2, "File: ");
    attroff(COLOR_PAIR(CLR_VALUE));
    attron(A_BOLD);
    int avail = cols - 10;
    if ((int)strlen(path) > avail) {
        mvprintw(1, 8, "...%s", path + strlen(path) - avail + 3);
    } else {
        mvprintw(1, 8, "%s", path);
    }
    attroff(A_BOLD);

    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(2, 1, ACS_HLINE, cols - 2);
    attroff(COLOR_PAIR(CLR_BORDER));
}

static void draw_stream_info(int row, int cols, const FlecState *st)
{
    attron(COLOR_PAIR(CLR_HINT));
    double secs = st->sample_rate > 0
        ? (double)st->total_samples / st->sample_rate : 0;
    int m = (int)secs / 60, s = (int)secs % 60;
    char buf[64];
    snprintf(buf, sizeof(buf),
             "[ %u Hz  %uch  %ubit  %d:%02d ]",
             st->sample_rate, st->channels, st->bits, m, s);
    int col = (cols - (int)strlen(buf)) / 2;
    if (col < 1) col = 1;
    mvprintw(row, col, "%s", buf);
    attroff(COLOR_PAIR(CLR_HINT));
}

static void draw_field(int row, int col, int width,
                       const char *label, const char *value,
                       int active, int editing,
                       int cursor_pos)
{
    int label_w = 10;

    attron(COLOR_PAIR(CLR_LABEL) | A_BOLD);
    mvprintw(row, col, "%-*s", label_w, label);
    attroff(COLOR_PAIR(CLR_LABEL) | A_BOLD);

    int vstart = col + label_w;
    int vwidth = width - label_w - 2;

    if (editing) {
        attron(COLOR_PAIR(CLR_ACTIVE));
    } else if (active) {
        attron(A_REVERSE);
    } else {
        attron(COLOR_PAIR(CLR_VALUE));
    }

    int vlen = strlen(value);
    int scroll = 0;
    if (editing && cursor_pos > vwidth - 1)
        scroll = cursor_pos - (vwidth - 1);

    mvhline(row, vstart, ' ', vwidth);

    int visible = vlen - scroll;
    if (visible > vwidth) visible = vwidth;
    if (visible > 0)
        mvaddnstr(row, vstart, value + scroll, visible);

    if (editing) {
        int cx = vstart + (cursor_pos - scroll);
        if (cx >= vstart && cx < vstart + vwidth) {
            char ch = (cursor_pos < vlen) ? value[cursor_pos] : ' ';
            mvaddch(row, cx, ch | A_UNDERLINE | A_BOLD);
        }
        attroff(COLOR_PAIR(CLR_ACTIVE));
    } else if (active) {
        attroff(A_REVERSE);
    } else {
        attroff(COLOR_PAIR(CLR_VALUE));
    }

    if (active || editing) {
        attron(COLOR_PAIR(CLR_HINT) | A_BOLD);
        mvaddstr(row, col - 2, SYM_ACTIVE_ARROW);
        attroff(COLOR_PAIR(CLR_HINT) | A_BOLD);
    }
}

static void draw_status(int rows, int cols, const char *msg, int is_error)
{
    attron(COLOR_PAIR(is_error ? CLR_ERROR : CLR_STATUS) | A_BOLD);
    int len = strlen(msg);
    int x = (cols - len) / 2;
    if (x < 1) x = 1;
    mvhline(rows - 2, 1, ' ', cols - 2);
    mvprintw(rows - 2, x, "%s", msg);
    attroff(COLOR_PAIR(is_error ? CLR_ERROR : CLR_STATUS) | A_BOLD);
}

static void draw_warn(int rows, int cols, const char *msg)
{
    attron(COLOR_PAIR(CLR_WARN) | A_BOLD);
    int len = strlen(msg);
    int x = (cols - len) / 2;
    if (x < 1) x = 1;
    mvhline(rows - 2, 1, ' ', cols - 2);
    mvprintw(rows - 2, x, "%s", msg);
    attroff(COLOR_PAIR(CLR_WARN) | A_BOLD);
}

static void draw_cover_indicator(int row, int col, const FlecState *st)
{
    attron(COLOR_PAIR(CLR_LABEL) | A_BOLD);
    mvprintw(row, col, "%-10s", "Cover Art");
    attroff(COLOR_PAIR(CLR_LABEL) | A_BOLD);

    int vstart = col + 10;
    if (st->cover_path[0] != '\0') {
        attron(COLOR_PAIR(CLR_ACTIVE));
        mvprintw(row, vstart, "New: %-30s", st->cover_path);
        attroff(COLOR_PAIR(CLR_ACTIVE));
    } else {
        attron(st->has_cover ? COLOR_PAIR(CLR_HINT) : COLOR_PAIR(CLR_VALUE));
        mvprintw(row, vstart, "%s",
                 st->has_cover ? "[Embedded cover present]"
                               : "[No cover art]");
        attroff(st->has_cover ? COLOR_PAIR(CLR_HINT) : COLOR_PAIR(CLR_VALUE));
    }
}

static void ebuf_init(EditBuf *e, const char *src)
{
    strncpy(e->buf, src, MAX_FIELD - 1);
    e->buf[MAX_FIELD - 1] = '\0';
    e->len    = strlen(e->buf);
    e->cursor = e->len;
}

static void ebuf_insert(EditBuf *e, char c)
{
    if (e->len >= MAX_FIELD - 1) return;
    memmove(e->buf + e->cursor + 1, e->buf + e->cursor,
            e->len - e->cursor + 1);
    e->buf[e->cursor++] = c;
    e->len++;
}

static void ebuf_delete(EditBuf *e)
{
    if (e->cursor == 0) return;
    memmove(e->buf + e->cursor - 1, e->buf + e->cursor,
            e->len - e->cursor + 1);
    e->cursor--;
    e->len--;
}

static void ebuf_delete_fwd(EditBuf *e)
{
    if (e->cursor >= e->len) return;
    memmove(e->buf + e->cursor, e->buf + e->cursor + 1,
            e->len - e->cursor);
    e->len--;
}

static const char *field_labels[NUM_FIELDS] = {
    "Title", "Artist", "Album", "Cover Art"
};

int main(int argc, char **argv)
{
    FlecState st;
    memset(&st, 0, sizeof(st));

    int music_only  = 0;
    const char *file_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--music") == 0) {
            music_only = 1;
        } else if (argv[i][0] != '-') {
            file_arg = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr,
                    "Usage: flec [-m|--music] [file.flac]\n"
                    "  -m, --music   restrict fzf search to ~/Music\n");
            return 1;
        }
    }

    if (file_arg) {
        snprintf(st.flac_path, MAX_PATH, "%s", file_arg);
    } else {
        if (!pick_file_fzf(st.flac_path, MAX_PATH, music_only)) {
            fprintf(stderr,
                    "Usage: flec [-m|--music] [file.flac]\n"
                    "  -m, --music   restrict fzf search to ~/Music\n"
                    "  (fzf must be installed for interactive picking)\n");
            return 1;
        }
    }

    make_display_path(st.flac_path, st.display_path, MAX_PATH);

    if (!flec_load(&st)) {
        fprintf(stderr,
                "Error: cannot read FLAC metadata from '%s'.\n"
                "Make sure the file exists and is a valid FLAC file.\n",
                st.flac_path);
        return 1;
    }
    memcpy(st.orig_title,      st.title,      MAX_FIELD);
    memcpy(st.orig_artist,     st.artist,     MAX_FIELD);
    memcpy(st.orig_album,      st.album,      MAX_FIELD);
    memcpy(st.orig_cover_path, st.cover_path, MAX_PATH);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);

    if (has_colors()) init_colors();

    int selected = 0;
    int editing  = 0;
    EditBuf ebuf;
    char status_msg[256] = "";
    int  status_err = 0;
    int  running = 1;
    char *fields_ptr[NUM_FIELDS] = {
        st.title, st.artist, st.album, st.cover_path
    };

    while (running) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        draw_border(rows, cols);
        draw_header(cols, st.display_path);
        draw_stream_info(3, cols, &st);

        attron(COLOR_PAIR(CLR_BORDER));
        mvhline(4, 1, ACS_HLINE, cols - 2);
        attroff(COLOR_PAIR(CLR_BORDER));

        int field_rows[NUM_FIELDS] = {6, 8, 10, 12};
        int fcol   = 4;
        int fwidth = cols - 6;

        for (int i = 0; i < NUM_FIELDS - 1; i++) {
            int act  = (selected == i);
            int edit = (editing && selected == i);
            const char *val = (edit) ? ebuf.buf : fields_ptr[i];
            int cur = (edit) ? ebuf.cursor : 0;
            draw_field(field_rows[i], fcol, fwidth,
                       field_labels[i], val, act, edit, cur);
        }

        {
            int i    = FIELD_COVER;
            int act  = (selected == i);
            int edit = (editing && selected == i);
            if (edit) {
                draw_field(field_rows[i], fcol, fwidth,
                           field_labels[i], ebuf.buf, act, edit, ebuf.cursor);
            } else {
                if (act) {
                    attron(COLOR_PAIR(CLR_HINT) | A_BOLD);
                    mvaddstr(field_rows[i], fcol - 2, SYM_ACTIVE_ARROW);
                    attroff(COLOR_PAIR(CLR_HINT) | A_BOLD);
                }
                draw_cover_indicator(field_rows[i], fcol, &st);
                if (act) {
                    attron(COLOR_PAIR(CLR_HINT));
                    mvprintw(field_rows[i] + 1, fcol,
                        "Supported image extensions:"
                        "  (jpg / png / gif / bmp / webp)");
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
            int scroll = (ebuf.cursor > vwidth - 1)
                         ? ebuf.cursor - (vwidth - 1) : 0;
            int cx = vstart + (ebuf.cursor - scroll);
            move(r, cx);
        } else {
            curs_set(0);
        }

        refresh();

        int ch = getch();

        if (editing) {
            switch (ch) {
            case 27:
                editing = 0;
                status_msg[0] = '\0';
                break;

            case '\n':
            case KEY_ENTER:
                if (selected == FIELD_COVER) {
                    expand_tilde(ebuf.buf, sizeof(ebuf.buf));
                    ebuf.len = strlen(ebuf.buf);

                    if (ebuf.buf[0] == '\0') {
                        st.cover_path[0] = '\0';
                        editing = 0;
                        status_msg[0] = '\0';
                    } else {
                        const char *err = validate_image_path(ebuf.buf);
                        if (err) {
                            snprintf(status_msg, sizeof(status_msg),
                                     "[!!] %s", err);
                            status_err = 1;
                        } else {
                            strncpy(st.cover_path, ebuf.buf, MAX_PATH - 1);
                            st.cover_path[MAX_PATH - 1] = '\0';
                            st.dirty = (strcmp(st.title,      st.orig_title)      != 0 ||
                                        strcmp(st.artist,     st.orig_artist)     != 0 ||
                                        strcmp(st.album,      st.orig_album)      != 0 ||
                                        strcmp(st.cover_path, st.orig_cover_path) != 0);
                            editing  = 0;
                            snprintf(status_msg, sizeof(status_msg),
                                     "Cover path set successfully.");
                            status_err = 0;
                        }
                    }
                } else {
                    strncpy(fields_ptr[selected], ebuf.buf, MAX_FIELD - 1);
                    fields_ptr[selected][MAX_FIELD - 1] = '\0';
                    st.dirty = (strcmp(st.title,      st.orig_title)      != 0 ||
                                strcmp(st.artist,     st.orig_artist)     != 0 ||
                                strcmp(st.album,      st.orig_album)      != 0 ||
                                strcmp(st.cover_path, st.orig_cover_path) != 0);
                    editing  = 0;
                    snprintf(status_msg, sizeof(status_msg),
                             "%s field set successfully.", field_labels[selected]);
                    status_err = 0;
                }
                break;

            case KEY_BACKSPACE:
            case 127:
            case '\b':
                ebuf_delete(&ebuf);
                break;

            case KEY_DC:
                ebuf_delete_fwd(&ebuf);
                break;

            case KEY_LEFT:
                if (ebuf.cursor > 0) ebuf.cursor--;
                break;

            case KEY_RIGHT:
                if (ebuf.cursor < ebuf.len) ebuf.cursor++;
                break;

            case KEY_HOME:
            case 1:
                ebuf.cursor = 0;
                break;

            case KEY_END:
            case 5:
                ebuf.cursor = ebuf.len;
                break;

            case 11:
                ebuf.buf[ebuf.cursor] = '\0';
                ebuf.len = ebuf.cursor;
                break;

            case 21:
                memmove(ebuf.buf, ebuf.buf + ebuf.cursor,
                        ebuf.len - ebuf.cursor + 1);
                ebuf.len    -= ebuf.cursor;
                ebuf.cursor  = 0;
                break;

            default:
                if (ch >= 32 && ch < 256)
                    ebuf_insert(&ebuf, (char)ch);
                break;
            }
            continue;
        }

        switch (ch) {
        case 'q':
        case 'Q':
            if (st.dirty) {
                draw_warn(rows, cols,
                    "Unsaved changes! Press Q again to quit, any key to cancel.");
                refresh();
                int c2 = getch();
                if (c2 == 'q' || c2 == 'Q') running = 0;
            } else {
                running = 0;
            }
            break;

        case '\t':
        case KEY_DOWN:
        case 'j':
            selected = (selected + 1) % NUM_FIELDS;
            status_msg[0] = '\0';
            break;

        case KEY_UP:
        case 'k':
            selected = (selected - 1 + NUM_FIELDS) % NUM_FIELDS;
            status_msg[0] = '\0';
            break;

        case '\n':
        case KEY_ENTER:
        case 'e':
        case 'E':
            ebuf_init(&ebuf, fields_ptr[selected]);
            editing = 1;
            status_msg[0] = '\0';
            break;

        case 19:
            snprintf(status_msg, sizeof(status_msg), "Saving...");
            status_err = 0;
            refresh();
            if (flec_save(&st)) {
                snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_OK);
                status_err = 0;
            } else {
                snprintf(status_msg, sizeof(status_msg), "%s", SYM_SAVE_ERR);
                status_err = 1;
            }
            break;

        case 24:
            if (selected == FIELD_COVER) {
                strncpy(st.cover_path, st.orig_cover_path, MAX_PATH - 1);
                st.cover_path[MAX_PATH - 1] = '\0';
                st.dirty = (strcmp(st.title,      st.orig_title)      != 0 ||
                            strcmp(st.artist,     st.orig_artist)     != 0 ||
                            strcmp(st.album,      st.orig_album)      != 0 ||
                            strcmp(st.cover_path, st.orig_cover_path) != 0);
                status_msg[0] = '\0';
            }
            break;

        case KEY_RESIZE:
            break;

        default:
            break;
        }
    }

    endwin();
    return 0;
}
