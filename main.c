/*
 *Includes
*/

#include "config.h"

/*
 * Prototypes
 */

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char *prompt, void (*callback)(char*, int));

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
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &EDITOR.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &EDITOR.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = EDITOR.orig_termios;


    // disable:
        // echoing,
        // canonical mode,
        // handling of SIGINT AND SIGSTP
        // handling of ctrl-v's terminal functionality
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
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
    if(write(STDOUT_FILENO,
             "\x1b[6n",
             4) != 4)
        return -1;

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
        if(write(STDOUT_FILENO,
                 "\x1b[999C\x1b[999B", 12)
           != 12)
            return -1;
        return getCurorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/*
 * Syntax Highlighting
 */

int is_separator(int c){
    return isspace(c)
        || c == '\0'
        || strchr(",.()+-*/=~%<>[];",c) != NULL;
}

void editorUpdateSyntax(erow *row){
    row->highlight = realloc(row->highlight,row->rsize);
    memset(row->highlight,
           KILONE_HL_NORMAL,
           row->rsize);

    if(EDITOR.syntax == NULL) return;

    char **keywords = EDITOR.syntax->keywords;

    char *scs = EDITOR.syntax->singleline_comment_start;
    char *mcs = EDITOR.syntax->multiline_comment_start;
    char *mce = EDITOR.syntax->multiline_comment_end;

    int scs_len = scs?
            strlen(scs):
            0;
    int mcs_len = mcs?
            strlen(mcs):
            0;
    int mce_len = mce?
            strlen(mce):
            0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && EDITOR.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while(i < row->rsize){
        char c = row->render[i];
        unsigned char prev_hl = (i > 0)?
            row->highlight[i - 1]:
            KILONE_HL_NORMAL;

        // ignore the comment prefix setting if its empty or if were in a string
        if(scs_len
           && !in_string
           && !in_comment){
            if(!strncmp(&row->render[i], scs, scs_len)){
                memset(&row->highlight[i],
                       KILONE_HL_COMMENT,
                       row->rsize - i);
                break;
            }
        }

        if(mcs_len
           && mce_len
           &&!in_string){
            if(in_comment){
                row->highlight[i] = KILONE_HL_MLCOMMENT;
                if(!strncmp(&row->render[i], mce, mce_len)){
                    memset(&row->highlight[i],
                           KILONE_HL_MLCOMMENT,
                           mcs_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i],
                                mcs,
                                mcs_len)){
                memset(&row->highlight[i],
                       KILONE_HL_MLCOMMENT,
                       mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if(EDITOR.syntax->flags & KILONE_HL_HIGHLIGHT_STRINGS) {
            if(in_string) {
                row->highlight[i] = KILONE_HL_STRING;

                if(c == '\\' && i + 1 < row->rsize){
                    row->highlight[i] = KILONE_HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if(c == '"' || c == '\''){
                    in_string = c;
                    row->highlight[i] = KILONE_HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if(EDITOR.syntax->flags & KILONE_HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c)
                && (prev_sep
                    || prev_hl == KILONE_HL_NUMBER))
            || (c == '.'
                && prev_hl == KILONE_HL_NUMBER)){
                row->highlight[i] = KILONE_HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if(prev_sep){
            int j;
            for(j = 0; keywords[j]; j++){
                int klen = strlen(keywords[j]);
                // check if we want to highlight the character as a type
                // types in the keyword list are defined by
                // adding "|" as the last char
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if(!strncmp(&row->render[i],
                            keywords[j],
                            klen)
                   && is_separator(row->render[i + klen])){
                    memset(&row->highlight[i],
                           kw2?
                           KILONE_HL_KEYWORD2:
                           KILONE_HL_KEYWORD1,
                           klen);
                    i += klen;
                    break;
                }

                if(keywords[j] != NULL) {
                    prev_sep = 0;
                    continue;
                }
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if(changed
       && row->idx + 1 < EDITOR.numrows)
        editorUpdateSyntax(&EDITOR.row[row->idx + 1]);
}


void editorSelectSyntaxHighlight() {
    EDITOR.syntax = NULL;
    if(EDITOR.filename == NULL) return;

    char *ext = strrchr(EDITOR.filename, '.');
    for(unsigned int j = 0; j < KILONE_HLDB_ENTRIES; j++){
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while(s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext
                 &&ext
                 &&!strcmp(ext,s->filematch[i]))
                || (!is_ext
                    && strstr(EDITOR.filename,
                              s->filematch[i]))){
                    EDITOR.syntax = s;

                    int filerow;
                    for(filerow = 0; filerow < EDITOR.numrows; filerow++){
                        editorUpdateSyntax(&EDITOR.row[filerow]);
                    }

                    return;
                }
                i++;
        }
    }
}

/*
 * Row Operations
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

int editorRowRxToCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t')
            cur_rx += (KILONE_TAB_STOP - 1) - (cur_rx % KILONE_TAB_STOP);
        cur_rx++;

        if(cur_rx > rx) return cx;
    }
    return cx;
}

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
            while (idx % KILONE_TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else{
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}


void editorInsertRow(int at,char* s, size_t len){
    if(at < 0 || at > EDITOR.numrows)
        return;

    EDITOR.row = realloc(EDITOR.row,
                         sizeof(erow) * (EDITOR.numrows + 1));
    memmove(&EDITOR.row[at + 1],
            &EDITOR.row[at],
            sizeof(erow) * (EDITOR.numrows - at));
    for(int j = at + 1; j <= EDITOR.numrows; j++) EDITOR.row[j].idx++;

    EDITOR.row[at].idx = at;

    EDITOR.row[at].size = len;
    EDITOR.row[at].chars = malloc(len + 1);
    memcpy(EDITOR.row[at].chars,
           s,
           len);
    EDITOR.row[at].chars[len] = '\0';

    EDITOR.row[at].rsize = 0;
    EDITOR.row[at].render = NULL;
    EDITOR.row[at].highlight = NULL;
    EDITOR.row[at].hl_open_comment = 0;
    editorUpdateRow(&EDITOR.row[at]);

    EDITOR.numrows++;
    EDITOR.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void editorDelRow(int at){
    if(at < 0 || at >= EDITOR.numrows) return;
    editorFreeRow(&EDITOR.row[at]);
    memmove(&EDITOR.row[at],
            &EDITOR.row[at + 1],
            sizeof(erow) * (EDITOR.numrows - at - 1));
    for(int j = at; j < EDITOR.numrows; j++)
        EDITOR.row[j].idx++;
    EDITOR.numrows--;
    EDITOR.dirty++;
}

void editorRowInsertChar(erow *row,int at, int c){
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars,
                         row->size + 2);
    memmove(&row->chars[at + 1],
            &row->chars[at],
            row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    EDITOR.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars,
                         row->size + len + 1);
    memcpy(&row->chars[row->size],
           s,
           len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    EDITOR.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at],
            &row->chars[at + 1],
            row->size - at);
    row->size--;
    editorUpdateRow(row);
    EDITOR.dirty++;
}


/*
** Editor Operations
*/
void editorInsertChar(int c){
    if(EDITOR.cy == EDITOR.numrows){
        editorInsertRow(EDITOR.numrows,"", 0);
    }
    editorRowInsertChar(&EDITOR.row[EDITOR.cy],
                        EDITOR.cx,
                        c);
    EDITOR.cx++;
}

void editorInsertNewLine() {
    if(EDITOR.cx == 0){
        editorInsertRow(EDITOR.cy, "", 0);
    } else {
        erow *row = &EDITOR.row[EDITOR.cy];
        editorInsertRow(EDITOR.cy + 1,
                        &row->chars[EDITOR.cx],
                        row->size - EDITOR.cx);
        row = &EDITOR.row[EDITOR.cy];
        row->size = EDITOR.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    EDITOR.cy++;
    EDITOR.cx = 0;
}


void editorDelChar(){
    if(EDITOR.cy == EDITOR.numrows) return;
    if(EDITOR.cx == 0 && EDITOR.cy == 0) return;

    erow *row = &EDITOR.row[EDITOR.cy];
    if(EDITOR.cx > 0){
        editorRowDelChar(row, EDITOR.cx - 1);
        EDITOR.cx--;
    } else {
        EDITOR.cx = EDITOR.row[EDITOR.cy - 1].size;
        editorRowAppendString(&EDITOR.row[EDITOR.cy - 1],
                              row->chars,
                              row->size);
        editorDelRow(EDITOR.cy);
        EDITOR.cy--;
    }
}


/*
 * file i/o
*/

char* editorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for(j = 0; j < EDITOR.numrows; j++){
        totlen += EDITOR.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j = 0;j < EDITOR.numrows; j++){
        memcpy(p,
               EDITOR.row[j].chars,
               EDITOR.row[j].size);
        p += EDITOR.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char* filename) {
    free(EDITOR.filename);
    EDITOR.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    linelen = 0;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        if(linelen != -1){
            while(linelen > 0 &&
                (line[linelen-1] == '\n' ||
                line[linelen - 1] == '\r')) {
                linelen--;
            }
            editorInsertRow(EDITOR.numrows,line, linelen);
        }
    }
    free(line);
    fclose(fp);
    EDITOR.dirty = 0;
}

void editorSave(){
    if(EDITOR.filename == NULL){
        EDITOR.filename = editorPrompt("Save as: %s", NULL);
        if(EDITOR.filename == NULL) {
            editorSetStatusMessage("Save aborted!");
            return;
        }

        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(EDITOR.filename,
                  O_RDWR | O_CREAT,
                  0644);
    if(fd != 1){
        if(ftruncate(fd,len) != 1){
            if(write(fd,buf,len) == len){
                close(fd);
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                EDITOR.dirty = 0;
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Cant save! I/O error: %s:", strerror(errno));
}

/*
 * Find
 */

void editorFindCallback(char* query, int key){
    static int last_match = -1;
    static int direction = 1;

    // the currently highlighted characters in the search
    // so we can clear the highlight later
    static int saved_hl_line;
    static char *saved_hl = NULL;

    if(saved_hl){
        memcpy(EDITOR.row[saved_hl_line].highlight,
               saved_hl,
               EDITOR.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if(key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    }
    else if(key == CURSOR_RIGHT || key == CURSOR_DOWN) {
        direction = 1;
    }
    else if(key == CURSOR_LEFT || key == CURSOR_UP) {
        direction = -1;
            }
    else {
        last_match = -1;
        direction = 1;
    }


    if(last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for(i = 0; i < EDITOR.numrows; i++){
        current += direction;
        if(current == -1) current = EDITOR.numrows - 1;
        else if (current == EDITOR.numrows) current = 0;


        erow *row = &EDITOR.row[i];

        char *match = strstr(row->render, query);
        if(match){
            last_match = current;
            EDITOR.cy = current;
            EDITOR.cx = editorRowRxToCx(row, match - row->render);
            EDITOR.rowoff = EDITOR.numrows;

            // Lets also color the matching characters shall we?
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl,
                   row->highlight,
                   row->rsize);
            memset(&row->highlight[match - row->render],
                   KILONE_HL_MATCH,
                   strlen(query));
            break;
        }
    }

}

void editorFind() {
    int saved_cx = EDITOR.cx;
    int saved_cy = EDITOR.cy;
    int saved_coloff = EDITOR.coloff;
    int saved_rowoff = EDITOR.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
                               editorFindCallback);

    if(query){
        free(query);
    } else {
        EDITOR.cx = saved_cx;
        EDITOR.cy = saved_cy;
        EDITOR.coloff = saved_coloff;
        EDITOR.rowoff = saved_rowoff;

    }
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


void editorScroll(){
    EDITOR.rx = 0;
    if(EDITOR.cy < EDITOR.numrows){
        EDITOR.rx = editorRowCxToRx(&EDITOR.row[EDITOR.cy],
                                    EDITOR.cx);
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
                                          "Kilone editor -- mockery is flattery -- version %s",
                                          KILONE_VERSION);
                if(welcomelen > EDITOR.screencols)
                    welcomelen = EDITOR.screencols;
                
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

            char *c = &EDITOR.row[filerow].render[EDITOR.coloff];
            unsigned char *hl = &EDITOR.row[filerow].highlight[EDITOR.coloff];
            int cur_color = -1;
            int j;
            for(j = 0; j < len; j++){
                if(iscntrl(c[j])){
                    char sym = (c[j] < 26)?
                        '@' + c[j]: '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if(cur_color != -1){
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf),
                                            "\x1b[%dm",
                                            cur_color);
                        abAppend(ab, buf, clen);
                    }
                }
                if(hl[j] == KILONE_HL_NORMAL){
                    if(cur_color != -1){
                        abAppend(ab, "\x1b[39m", 5);
                        cur_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if(color != cur_color){
                        cur_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }


        // clear the line ahead of the cursor
        abAppend(ab, "\x1b[K", 4);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status),
                       "%.20s - %d lines %s",
                       EDITOR.filename ? EDITOR.filename : "[No Name]",
                       EDITOR.numrows,
                       EDITOR.dirty? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
                        "%s | %d/%d",
                        EDITOR.syntax?
                            EDITOR.syntax->filetype :
                            "no ft",
                        EDITOR.cy + 1,
                        EDITOR.numrows);
    if(len > EDITOR.screencols)
        len = EDITOR.screencols;
    abAppend(ab, status, len);
    while(len < EDITOR.screencols){
        if(EDITOR.screencols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(EDITOR.statusmsg);
    if(msglen > EDITOR.screencols)
        msglen = EDITOR.screencols;
    if(msglen && time(NULL) - EDITOR.statusmsg_time < 5)
        abAppend(ab,
                 EDITOR.statusmsg,
                 msglen);
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // hide the cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // put cursor in the top left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

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

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EDITOR.statusmsg, sizeof(EDITOR.statusmsg),
              fmt,
              ap);
    va_end(ap);
    EDITOR.statusmsg_time = time(NULL);
}

/*
 * Input
 */
keycode editorReadKey(){
    int nread;
    char c = '\0';
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
            die("read");
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

char *editorPrompt(char* prompt, void (*callback)(char*, int)){
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen != 0) buf[--buflen] = '\0';
        }
        if(c == '\x1b'){
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        }
        if(c == '\r') {
            if(buflen != 0){
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if(!iscntrl(c) && c < 128){
            if(buflen == bufsize - 1){
                bufsize *=2;
                buf = realloc(buf,bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);

    }
}

void editorMoveCursor(keycode key){
    erow *row = (EDITOR.cy >= EDITOR.numrows)?
        NULL :
        &EDITOR.row[EDITOR.cy];

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

    row = (EDITOR.cy >= EDITOR.numrows)?
        NULL :
        &EDITOR.row[EDITOR.cy];
    int rowlen = row ? row->size : 0;
    if(EDITOR.cx > rowlen){
        EDITOR.cx = rowlen;
    }
}

void editorProcessKeyPress(){
    keycode c = editorReadKey();
    static int quit_times = KILONE_QUIT_TIMES;

    switch(c){
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
            if(EDITOR.dirty
               && quit_times > 0){
                editorSetStatusMessage("WARNING!!! File has unsaved changes."
                                       "Press Ctrl-Q %d more times to quit.",
                                       quit_times);
                quit_times--;
                return;
            }
            // clear the screen
            write(STDOUT_FILENO, "\x1b[2J]",4);
            // put cursor in the top left corner
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;

        // HOME and END move to the beginning/end of the row
        case HOME_KEY:
            EDITOR.cx = 0;
            break;
        case END_KEY:
            EDITOR.cx = EDITOR.screencols - 1;
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY)
                editorMoveCursor(CURSOR_RIGHT);
            editorDelChar();
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
                if(EDITOR.cy > EDITOR.numrows)
                    EDITOR.cy = EDITOR.numrows;
            }

            int times = EDITOR.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP?
                                    CURSOR_UP :
                                    CURSOR_DOWN);
        };
        break;

        // move the cursor
        case CURSOR_LEFT:
        case CURSOR_RIGHT:
        case CURSOR_UP:
        case CURSOR_DOWN:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            /* TODO */
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILONE_QUIT_TIMES;
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
    EDITOR.dirty = 0;
    EDITOR.filename = NULL;
    EDITOR.statusmsg[0] = '\0';
    EDITOR.statusmsg_time = 0;
    EDITOR.syntax = NULL;

    if(getWindowSize(&EDITOR.screenrows, &EDITOR.screencols) == -1)
        die("getWindowSize");
    EDITOR.screenrows -= 2;
}

int main(int argc, char* argv[]){
    //init functions
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
