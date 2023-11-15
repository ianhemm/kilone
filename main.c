/*
 *Includes
*/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

/*
** Defines
*/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILONE_VERSION "0.0.1"

/*
 * Data
*/

typedef int err_no;

// Global Editor State
struct editorConfig {
    int cx, cy; // cursor position
    int screenrows, screencols; // screen size
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
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

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

void editorDrawRows(struct abuf *ab){
    int y;
    for(y = 0; y < EDITOR.screenrows; y++){
        if(y == EDITOR.screenrows / 3) {
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
            abAppend(ab,"~",1);
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
    struct abuf ab = ABUF_INIT;

    // hide the cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // put cursor in the top left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Move the cursor to its current position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", EDITOR.cy + 1, EDITOR.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // show the cursor again after draw
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

}

/*
 * Input
 */
char editorReadKey(){
    int nread;
    char c = '\0';
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
    }
    return c;
}

void editorMoveCursor(char key){
    switch(key){
        case 'a':
            EDITOR.cx--;
            break;
        case 'd':
            EDITOR.cx++;
            break;
        case 's':
            EDITOR.cy++;
            break;
        case 'w':
            EDITOR.cy--;
            break;
    }
}

void editorProcessKeyPress(){
    char c = editorReadKey();

    switch(c){
        case CTRL_KEY('q'):
            // clear the screen
            write(STDOUT_FILENO, "\x1b[2J]",4);
            // put cursor in the top left corner
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case 'a':
        case 'd':
        case 's':
        case 'w':
            editorMoveCursor(c);
    }
}

/*
 * Init
 */
void initEditor() {
    EDITOR.cx = 10;
    EDITOR.cy = 10;

    if(getWindowSize(&EDITOR.screenrows, &EDITOR.screencols) == -1) die("getWindowSize");
}

int main(){
    //init functions
    enableRawMode();
    initEditor();

    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
