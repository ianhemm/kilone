/*
 *Includes
*/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>

/*
** Defines
*/
#define CTRL_KEY(k) ((k) & 0x1f)

#define KILONE_VERSION "0.0.1"
#define KILONE_TAB_STOP 4

enum editorKey {
    CURSOR_LEFT = 1000,
    CURSOR_RIGHT ,
    CURSOR_UP,
    CURSOR_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};
/*
 * Data
*/

typedef int err_no;
typedef int keycode;

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

// Global Editor State
struct editorConfig {
    int cx, cy; // cursor position
    int rx; // rendered cursor position
    int rowoff, coloff; // offset of the file
    int screenrows, screencols; // screen size
    int numrows;
    erow *row;
    struct termios orig_termios; // terminal config we enter the program from
} EDITOR;



/*
 * Terminal
*/
void die(const char *s){
    // clear the screen
    write(STDOUT_FILENO, "\x1b[2J]",4);
    // put cursor in the top left corner
    write(STDOUT_FILENO, "\x1b[H", 3);
    // put the cursor back jim
    write(STDOUT_FILENO, "\x1b[?25h", 6);

    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &EDITOR.orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &EDITOR.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = EDITOR.orig_termios;


    // disable:
        // echoing,
        // canonical mode,
        // handling of SIGINT AND SIGSTP
        // handling of ctrl-v's terminal functionality
    raw.c_lflag &= ~(
        ECHO |
        ICANON |
        ISIG |
        IEXTEN);
    // disable:
        // software control flow keybinds
        // automatic translation of \r to \n
    raw.c_iflag &= ~(IXON | ICRNL);
    // fuck it, no more processing newlines into carriage returns
    raw.c_oflag &= ~(OPOST);
    // some other traditional feature disabling
    raw.c_cflag |= (CS8);
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);

    // set read() timeout
    // would make it shorter but for some reason keys keep dropping at 1
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 5;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

err_no getCurorPosition(int *rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    // request the goods
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    printf("\r\n");
    // read the goods
    while(1 < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[1] == 'R') break;
        i++;
    }
    // null termination or things get wierd
    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[')
        return -1; // damnit we didnt get an escape sequence YOU HAD ONE JOB
    if(sscanf(&buf[2],"%d;%d", rows, cols) != 2)
        return -1; // something parsed wierd or we straight up got wrong values

    return 0;
}

err_no getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ,&ws) == -1 || ws.ws_col == 0){
        // no ioctl? uh... lets just fuckin... push the cursor to the bottom right, max distance
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCurorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/*
 * Row Operations
*/
void editorUpdateRow(erow *row){
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t') tabs++;
    }


    free(row->render);
    row->render = malloc(row->size + tabs*(KILONE_TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while (idx % KILONE_TAB_STOP != 0) row->render[idx++] = ' ';
        } else{
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}


void editorAppendRow(char* s, size_t len){
    EDITOR.row = realloc(EDITOR.row,
                         sizeof(erow) * (EDITOR.numrows + 1));

    int at = EDITOR.numrows;
    EDITOR.row[at].size = len;
    EDITOR.row[at].chars = malloc(len + 1);
    memcpy(EDITOR.row[at].chars, s, len);
    EDITOR.row[at].chars[len] = '\0';

    EDITOR.row[at].rsize = 0;
    EDITOR.row[at].render = NULL;
    editorUpdateRow(&EDITOR.row[at]);

    EDITOR.numrows++;
}

/*
 * file i/o
*/
void editorOpen(char* filename) {
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    linelen = getline(&line, &linecap,fp);
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        if(linelen != -1){
            while(linelen > 0 &&
                (line[linelen-1] == '\n' ||
                line[linelen - 1] == '\r')) {
                linelen--;
            }
            editorAppendRow(line, linelen);
        }
    }
    free(line);
    fclose(fp);
}

/*
 * Append Buffer
*/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len){
    char* new = realloc(ab->b,
                        ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len],
           s,
           len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

/*
 * Output
*/
int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++){
        if(row->chars[j] == '\t'){
            rx += (KILONE_TAB_STOP - 1) - (rx % KILONE_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorScroll(){
    EDITOR.rx = 0;
    if(EDITOR.cy < EDITOR.numrows){
        EDITOR.rx = editorRowCxToRx(&EDITOR.row[EDITOR.cy], EDITOR.cx);
    }


    if(EDITOR.cy < EDITOR.rowoff) {
        EDITOR.rowoff = EDITOR.cy;
    }
    if(EDITOR.cy >= EDITOR.rowoff + EDITOR.screenrows) {
        EDITOR.rowoff = EDITOR.cy - EDITOR.screenrows + 1;
    }

    if(EDITOR.rx < EDITOR.coloff){
        EDITOR.coloff = EDITOR.rx;
    }
    if(EDITOR.rx >= EDITOR.coloff + EDITOR.screencols){
        EDITOR.coloff = EDITOR.rx - EDITOR.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab){
    int y;
    for(y = 0; y < EDITOR.screenrows; y++){
        int filerow = y + EDITOR.rowoff;
        if(filerow >= EDITOR.numrows){
            // Print MOTD if file is empty
            if(EDITOR.numrows == 0 && y == EDITOR.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Kilone editor -- mockery is flattery -- version %s", KILONE_VERSION);
                if(welcomelen > EDITOR.screencols) welcomelen = EDITOR.screencols;
                
                int padding = (EDITOR.screencols - welcomelen) / 2;
                if(padding){
                    abAppend(ab, "~", 1);
                    padding --;
                }
                while(padding--) abAppend(ab, " ", 1);
                
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = EDITOR.row[filerow].rsize - EDITOR.coloff;
            if(len < 0) len = 0;
            if(len > EDITOR.screencols) len = EDITOR.screencols;

            abAppend(ab,
                     &EDITOR.row[filerow].render[EDITOR.coloff],
                     len);
        }


        // clear the line ahead of the cursor
        abAppend(ab, "\x1b[K", 4);
        // prevent the last line from drawing a carriage return
        // otherwise the last line wont be drawn
        if(y < EDITOR.screenrows - 1){
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // hide the cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // put cursor in the top left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Move the cursor to its current position
    char buf[32];
    snprintf(buf, sizeof(buf),
             "\x1b[%d;%dH",
             (EDITOR.cy - EDITOR.rowoff) + 1,
             (EDITOR.rx - EDITOR.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // show the cursor again after draw
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

}

/*
 * Input
 */
keycode editorReadKey(){
    int nread;
    char c = '\0';
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
    }

    if(c == '\x1b'){
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if(seq[0] == '['){
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            if (seq[0] == 'O') {
                switch (seq[1]) {
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
            switch(seq[1]){
                case 'A': return CURSOR_UP;
                case 'B': return CURSOR_DOWN;
                case 'C': return CURSOR_RIGHT;
                case 'D': return CURSOR_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    return c;
}

void editorMoveCursor(keycode key){
    erow *row = (EDITOR.cy >= EDITOR.numrows) ? NULL : &EDITOR.row[EDITOR.cy];

    switch(key){
        case CURSOR_LEFT:
            if(EDITOR.cx != 0){
                EDITOR.cx--;
            } else if (EDITOR.cy > 0){
                EDITOR.cy--;
                EDITOR.cx = EDITOR.row[EDITOR.cy].size;
            }
            break;
        case CURSOR_RIGHT:
            if(row && EDITOR.cx < row->size){
                EDITOR.cx++;
            } else if (row && EDITOR.cx == row->size){
                EDITOR.cy++;
                EDITOR.cx = 0;
            }
            break;
        case CURSOR_DOWN:
            if(EDITOR.cy != EDITOR.numrows - 1){
                EDITOR.cy++;
            }
            break;
        case CURSOR_UP:
            if(EDITOR.cy != 0){
                EDITOR.cy--;
            }
            break;
    }

    row = (EDITOR.cy >= EDITOR.numrows) ? NULL : &EDITOR.row[EDITOR.cy];
    int rowlen = row ? row->size : 0;
    if(EDITOR.cx > rowlen){
        EDITOR.cx = rowlen;
    }
}

void editorProcessKeyPress(){
    keycode c = editorReadKey();

    switch(c){
        case CTRL_KEY('q'):
            // clear the screen
            write(STDOUT_FILENO, "\x1b[2J]",4);
            // put cursor in the top left corner
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        // HOME and END move to the beginning/end of the row
        case HOME_KEY:
            EDITOR.cx = 0;
            break;
        case END_KEY:
            EDITOR.cx = EDITOR.screencols - 1;
            break;

        // move up or down 1 page
        case PAGE_UP:
        case PAGE_DOWN:
        {
            if(c == PAGE_UP){
                EDITOR.cy = EDITOR.rowoff;
            }
            if(c == PAGE_DOWN){
                EDITOR.cy = EDITOR.rowoff + EDITOR.screenrows - 1;
                if(EDITOR.cy > EDITOR.numrows) EDITOR.cy = EDITOR.numrows;
            }

            int times = EDITOR.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? CURSOR_UP : CURSOR_DOWN);
        };
        break;

        // move the cursor
        case CURSOR_LEFT:
        case CURSOR_RIGHT:
        case CURSOR_UP:
        case CURSOR_DOWN:
            editorMoveCursor(c);
    }
}

/*
 * Init
 */
void initEditor() {
    EDITOR.cx = 0;
    EDITOR.cy = 0;
    EDITOR.rowoff = 0;
    EDITOR.coloff = 0;
    EDITOR.numrows = 0;
    EDITOR.row = NULL;

    if(getWindowSize(&EDITOR.screenrows, &EDITOR.screencols) == -1) die("getWindowSize");
}

int main(int argc, char* argv[]){
    //init functions
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }

    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
