#ifndef KILONE_CONFIG_H_
#define KILONE_CONFIG_H_

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/*
 *Includes
*/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>

#include <locale.h>
#include <ncurses.h>

/*
** Defines
*/
#define CTRL_KEY(k) ((k) & 0x1f)

#define KILONE_VERSION "0.1.0"
#define KILONE_TAB_STOP 4
#define KILONE_QUIT_TIMES 3

enum editorKey {
    BACKSPACE = 127,
    CURSOR_LEFT = 1000,
    CURSOR_RIGHT ,
    CURSOR_UP,
    CURSOR_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    KILONE_QUIT,
    KILONE_SAVE,
};

enum editorHighlight {
    KILONE_HL_NORMAL = 1,
    KILONE_HL_COMMENT,
    KILONE_HL_MLCOMMENT,
    KILONE_HL_KEYWORD1,
    KILONE_HL_KEYWORD2,
    KILONE_HL_STRING,
    KILONE_HL_NUMBER,
    KILONE_HL_MATCH, // search highlighting
    KILONE_HL_STATUS, // status bar
};

#define KILONE_HL_HIGHLIGHT_NUMBERS (1<<0)
#define KILONE_HL_HIGHLIGHT_STRINGS (1<<1)

/*
 * Data
*/
typedef int err_no;
typedef int keycode;

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *highlight;
    int hl_open_comment;
} erow;

// Global Editor State
struct editorConfig {
    int cx, cy; // cursor position
    int rx; // rendered cursor position
    int rowoff, coloff; // offset of the file
    int screenrows, screencols; // screen size
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
} EDITOR;

/*
* filetypes
*/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//",
        "/*",
        "*/",
        KILONE_HL_HIGHLIGHT_NUMBERS |
        KILONE_HL_HIGHLIGHT_STRINGS
    },
};

#define KILONE_HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* Color definitions, use this to define your themes*/
void editorInitializeColorPairs(){
    start_color();
    init_pair(KILONE_HL_NORMAL, COLOR_WHITE, COLOR_BLACK);
    init_pair(KILONE_HL_COMMENT, COLOR_CYAN, COLOR_BLACK);
    init_pair(KILONE_HL_MLCOMMENT, COLOR_CYAN, COLOR_BLACK);
    init_pair(KILONE_HL_KEYWORD1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(KILONE_HL_KEYWORD2, COLOR_GREEN, COLOR_BLACK);
    init_pair(KILONE_HL_STRING, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(KILONE_HL_NUMBER, COLOR_RED, COLOR_BLACK);
    init_pair(KILONE_HL_MATCH, COLOR_BLUE, COLOR_BLACK);
    init_pair(KILONE_HL_STATUS, COLOR_BLACK, COLOR_WHITE);
}


#endif // KILONE_CONFIG_H_
