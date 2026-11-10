/* The PS5's own on-screen keyboard (the IME dialog), for Crawl's text
   prompts: cio.cc, message.cc and windowmanager-sdl.cc call
   crawl_ps5_text_input().

   A title isn't given libSceImeDialog, and once the module is loaded
   sceKernelDlsym finds none of its functions (0x80020003). What works is to
   call them as ordinary imports, linked through the SDK's libSceImeDialog.so
   stub, after loading the module: the console binds an import at its first
   call, from the modules loaded by then. Until then the import's address
   reads 0, so they are called directly, never through a pointer. This is
   griches/PS5_DevilutionX's method for its mouse library; NetHack's PS5
   title uses it too. Tested on a PS5 (2026-11-09). */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* As ps5-payload-dev's SDL port declares them. The text is UTF-16. */
typedef struct {
    int userId;
    int type;                   /* 0: default, every language */
    uint64_t supportedLanguages;
    int enterLabel;             /* 0: default */
    int inputMethod;            /* 0: default */
    void *filter;
    uint32_t option;
    uint32_t maxTextLength;
    uint16_t *inputTextBuffer;
    float posx, posy;
    int halign, valign;         /* 1, 1: centred */
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} ime_dialog_param;

typedef struct {
    int outcome;                /* 0 OK, 1 cancelled, 2 aborted */
    int8_t reserved[12];
} ime_dialog_result;

enum { IME_STATUS_NONE, IME_STATUS_RUNNING, IME_STATUS_FINISHED };

int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                             uint32_t flags, void *option, int *result);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetForegroundUser(int32_t *user);

int sceImeDialogInit(const ime_dialog_param *param, void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ime_dialog_result *result);
int sceImeDialogTerm(void);

static int ime_load(void)
{
    static int state; /* 0 not tried, 1 loaded, -1 unavailable */
    const char *word;
    char path[128];
    int handle;

    if (state)
        return state > 0;
    state = -1;
    word = sceKernelGetFsSandboxRandomWord();
    snprintf(path, sizeof path, "/%s/common/lib/libSceImeDialog.sprx",
             word ? word : "system");
    handle = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        printf("crawl: cannot load the on-screen keyboard from %s (0x%08x)\n",
               path, (unsigned) handle);
        return 0;
    }
    state = 1;
    return 1;
}

/* UTF-8 to UTF-16 (the Basic Multilingual Plane; anything else becomes ?). */
static void to_utf16(const char *in, uint16_t *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *) in;
    size_t n = 0;

    while (*s && n + 1 < cap) {
        uint32_t c = *s++;
        int extra = 0;

        if (c >= 0xf0)
            c = '?', extra = 3;
        else if (c >= 0xe0)
            c &= 0x0f, extra = 2;
        else if (c >= 0xc0)
            c &= 0x1f, extra = 1;
        else if (c >= 0x80)
            c = '?';
        for (; extra && (*s & 0xc0) == 0x80; extra--, s++)
            if (c != '?')
                c = (c << 6) | (*s & 0x3f);
        out[n++] = (uint16_t) c;
    }
    out[n] = 0;
}

/* UTF-16 to UTF-8, never splitting a character at the end of out. */
static void to_utf8(const uint16_t *in, char *out, size_t cap)
{
    size_t n = 0;

    for (; *in; in++) {
        uint32_t c = *in;
        char b[3];
        size_t len;

        if (c >= 0xd800 && c < 0xe000)
            c = '?'; /* outside the Basic Multilingual Plane */
        if (c < 0x80)
            b[0] = (char) c, len = 1;
        else if (c < 0x800)
            b[0] = (char) (0xc0 | c >> 6), b[1] = (char) (0x80 | (c & 0x3f)),
            len = 2;
        else
            b[0] = (char) (0xe0 | c >> 12),
            b[1] = (char) (0x80 | ((c >> 6) & 0x3f)),
            b[2] = (char) (0x80 | (c & 0x3f)), len = 3;
        if (n + len >= cap)
            break;
        memcpy(out + n, b, len);
        n += len;
    }
    out[n] = '\0';
}

/* Ask for a line of text with the system keyboard, prompt as its title and
   fill as the starting text. redraw keeps the game drawn under it. Returns 1
   with the text in answer, 0 if the player cancelled, and -1 if the keyboard
   can't open (the caller then reads keys as before). */
int crawl_ps5_text_input(const char *prompt, const char *fill, char *answer,
                         int size, int width, int height,
                         void (*redraw)(void))
{
    static uint16_t title[128], text[256];
    ime_dialog_param param;
    ime_dialog_result result;
    int32_t user = -1;
    int status, rc;

    if (size < 2 || !ime_load())
        return -1;
    to_utf16(prompt ? prompt : "", title, sizeof title / sizeof *title);
    memset(text, 0, sizeof text);
    to_utf16(fill ? fill : "", text, sizeof text / sizeof *text);

    (void) sceUserServiceInitialize(NULL);
    sceUserServiceGetForegroundUser(&user);
    memset(&param, 0, sizeof param);
    param.userId = user;
    /* Leave room for the longest UTF-8 encoding of each character. */
    param.maxTextLength = (uint32_t) ((size - 1) / 3 > 0 ? (size - 1) / 3 : 1);
    if (param.maxTextLength > sizeof text / sizeof *text - 1)
        param.maxTextLength = sizeof text / sizeof *text - 1;
    param.inputTextBuffer = text;
    param.posx = width / 2.0f;
    param.posy = height / 2.0f;
    param.halign = param.valign = 1;
    param.title = title;

    rc = sceImeDialogInit(&param, NULL);
    if (rc < 0) {
        /* Basic Latin is the type tested in NetHack's title. */
        printf("crawl: on-screen keyboard type 0 refused (0x%08x)\n",
               (unsigned) rc);
        param.type = 1;
        rc = sceImeDialogInit(&param, NULL);
    }
    if (rc < 0) {
        printf("crawl: on-screen keyboard unavailable (0x%08x)\n",
               (unsigned) rc);
        return -1;
    }
    /* The system draws the keyboard over the title; keep the game drawn
       underneath until it closes. */
    while ((status = sceImeDialogGetStatus()) == IME_STATUS_RUNNING) {
        if (redraw)
            redraw();
        sceKernelUsleep(16000);
    }
    memset(&result, 0, sizeof result);
    rc = status == IME_STATUS_FINISHED ? sceImeDialogGetResult(&result) : -1;
    sceImeDialogTerm();
    printf("crawl: on-screen keyboard closed (status %d, result 0x%08x, "
           "outcome %d)\n", status, (unsigned) rc, result.outcome);
    if (rc < 0 || result.outcome != 0)
        return 0;
    to_utf8(text, answer, (size_t) size);
    return 1;
}
