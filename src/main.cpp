#include "main.h"

#define _BSD_SOURCE

#include <stdio.h>
#include <termios.h>   // for tcgetattr()
#include <errno.h>     // for ENOTTY
#include <string.h>
#include <stdlib.h>    // for exit()
#include <ctype.h>     // for isspace(), isprint(), isdigit()
#include <sys/ioctl.h> // for ioctl(), TIOCGWINSZ
#include <time.h>
#include <unistd.h>    // for STDIN_FILENO, isatty(), read(), close(), write(), ftruncate()
#include <stdarg.h>
#include <fcntl.h>

#define Assert(expression) if(!(expression)) { __builtin_trap(); }
#define TERMINAL_VERSION "0.0.1"
#define TAB_WIDTH 8

global struct termios GlobalOriginalSettings;
global b32 GlobalRunning = true;

#define CTRL_KEY(key) ((key) & 0x1f)

enum Key_Type {
    KeyType_Backspace = 127,
    KeyType_Up = 128,
    KeyType_Down,
    KeyType_Left,
    KeyType_Right,
    KeyType_PageUp,
    KeyType_PageDown,
    KeyType_Home,
    KeyType_End,
    KeyType_Del,
};

union v2u {
    size_t E[2];
    struct {
        size_t x, y;
    };
};

struct XBuffer {
    char* Data;
    size_t Used;
    size_t Size;
};

struct Line_Data {
    size_t Size;
    char* Data;
    
    // To keep track of the TAB width, we use 2 versions of a Line/Row
    size_t RenderSize;
    char* RenderData;
};

struct Term_Editor {
    char* Filename;
    char StatusMessage[80];
    time_t StatusMessageTime;
    b32 Dirty;
    
    b32 RawModeEnabled; // Is terminal raw mode enabled?
    size_t RowCount, ColumnCount;
    v2u CursorPos;
    size_t RenderCursorX; // We use this because TABs fault
    
    v2u Offset; // For scrolling
    size_t LineCount;
    Line_Data* Lines;
    XBuffer Buffer;
};

inline void 
RestoreTerminalSettings() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &GlobalOriginalSettings);
}

inline void
ClearTerminal() {
    // Clear the screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // Set the cursor position
    write(STDOUT_FILENO, "\x1b[H", 3);
}

static void
AppendToBuffer(XBuffer* buffer, char* data, int length) {
    if(!buffer->Data || (buffer->Used+length >= buffer->Size)) {
        buffer->Size = buffer->Used+length  + (length*2);
        buffer->Data = (char*)realloc(buffer->Data, buffer->Size);
    }
    
    Assert(buffer->Data);
    memcpy(buffer->Data + buffer->Used, data, length);
    buffer->Used += length;
}

inline void
FreeBuffer(XBuffer* buffer) {
    free(buffer->Data);
    *buffer = {};
}

inline void
ZeroBuffer(XBuffer* buffer) {
    size_t size = buffer->Used;
    char* byte = buffer->Data;
    while(size--) {
        *byte++ = 0;
    }
    buffer->Used = 0;
}

static void
UpdateScreen(Term_Editor* editor) {
    XBuffer* buffer = &editor->Buffer;
    
    { // Scroll
        Line_Data* line = editor->Lines + editor->CursorPos.y;
        
        editor->RenderCursorX = 0;
        { // Row CxToRx
            for(size_t colIndex = 0; colIndex < editor->CursorPos.x; colIndex++) {
                if(line->Data[colIndex] == '\t') {
                    editor->RenderCursorX += (TAB_WIDTH - 1) - (editor->RenderCursorX % TAB_WIDTH);
                }
                editor->RenderCursorX++;
            }
        }
        
        // Up
        if(editor->CursorPos.y < editor->Offset.y) {
            editor->Offset.y = editor->CursorPos.y;
        }
        // Down
        if(editor->CursorPos.y >= (editor->Offset.y + editor->RowCount)) {
            editor->Offset.y = editor->CursorPos.y - editor->RowCount + 2;
        }
        // Left
        if(editor->RenderCursorX < editor->Offset.x) {
            editor->Offset.x = editor->RenderCursorX;
        }
        // Right
        if(editor->CursorPos.x >= editor->Offset.x + editor->ColumnCount) {
            editor->Offset.x = editor->RenderCursorX - editor->ColumnCount + 1;
        }
    }
    
    AppendToBuffer(buffer, "\x1b[?25l", 6); // Hide the cursor
    //AppendToBuffer(buffer, "\x1b[2J", 4); // Clear the screen
    AppendToBuffer(buffer, "\x1b[H", 3); // Set the cursor at position 0,0
    
    { // Draw characters / Intro message / empty line
        for(size_t y = 0; y < editor->RowCount; y++) {
            size_t offsetY = y + editor->Offset.y;
            
            if(offsetY < editor->LineCount) {
                int len = editor->Lines[offsetY].RenderSize - editor->Offset.x;
                if(len < 0) len = 0; 
                if((size_t)len > editor->ColumnCount) len = editor->ColumnCount;
                AppendToBuffer(buffer, editor->Lines[offsetY].RenderData + editor->Offset.x, len);
            } else if(editor->LineCount == 0 && y == (editor->RowCount / 3)) { // Intro message
                char msg[64] = {};
                int len = snprintf(msg, sizeof(msg), "Terminal Editor - Version: %s", TERMINAL_VERSION);
                if((size_t)len > editor->ColumnCount) len = editor->ColumnCount;
                
                int padding = (editor->ColumnCount - len) / 2;
                if(padding--) {
                    AppendToBuffer(buffer, "~", 1);
                }
                while(padding--) AppendToBuffer(buffer, " ", 1);
                
                // Add the msg
                AppendToBuffer(buffer, msg, len);
            } else { // Draw the empty line indicator
                AppendToBuffer(buffer, "~", 1);
            }
            
            AppendToBuffer(buffer, "\x1b[K", 3); // Erase from the cursor position to the end of the line
            AppendToBuffer(buffer, "\r\n", 2);
        }
    }
    
    { // Draw StatusBar
        AppendToBuffer(buffer, "\x1b[7m", 4); // Invert colors
        
        char leftStatus[80] = {}, rightStatus[80] = {};
        size_t leftLen = snprintf(leftStatus, sizeof(leftStatus), " %.20s - %lu Lines %s", 
                                  editor->Filename ? editor->Filename : "[No Name]", editor->RowCount, 
                                  editor->Dirty ? "(modified)" : "");
        size_t rightLen = snprintf(rightStatus, sizeof(rightStatus), "%lu/%lu ", editor->CursorPos.y + 1, editor->RowCount);
        if(leftLen > editor->ColumnCount) leftLen = editor->ColumnCount; 
        AppendToBuffer(buffer, leftStatus, leftLen);
        
        while(leftLen < editor->ColumnCount) {
            if((editor->ColumnCount - leftLen) == rightLen) {
                AppendToBuffer(buffer, rightStatus, rightLen);
                break;
            } else {
                AppendToBuffer(buffer, " ", 1);
                leftLen++;
            }
        }
        AppendToBuffer(buffer, "\x1b[m", 3); // Go back to normal text formatting
        AppendToBuffer(buffer, "\r\n", 2); // New line
    }
    
    { // Draw MessageBar
        AppendToBuffer(buffer, "\x1b[K", 3);
        size_t msgLen = strlen(editor->StatusMessage);
        if(msgLen > editor->ColumnCount) msgLen = editor->ColumnCount;
        if(msgLen && time(0) - editor->StatusMessageTime < 5) AppendToBuffer(buffer, editor->StatusMessage, msgLen); 
    }
    
    // [debug info]
    {
        //AppendToBuffer(buffer, "\x1b[2;1H", 6); // Set the cursor position at 0,0
        char info[89] = {};
        snprintf(info, sizeof(info), "\x1b[1;%luHMem Used: %lu - Cap: %lu | CX: %lu - CY: %lu, OffX: %lu - OffY: %lu", 
                 editor->ColumnCount - sizeof(info), 
                 buffer->Used, buffer->Size,
                 editor->CursorPos.x, 
                 editor->CursorPos.y, 
                 editor->Offset.x,
                 editor->Offset.y);
        AppendToBuffer(buffer, info, sizeof(info));
    }
    
    // Draw cursor
    {
        char cursor[32] = {};
        snprintf(cursor, sizeof(cursor), "\x1b[%lu;%luH", 
                 (editor->CursorPos.y - editor->Offset.y) + 1, 
                 (editor->RenderCursorX - editor->Offset.x) + 1);
        AppendToBuffer(buffer, cursor, sizeof(cursor));
    }
    
    AppendToBuffer(buffer, "\x1b[?25h", 6); // Show the cursor again
    
    write(STDOUT_FILENO, buffer->Data, buffer->Used);
    ZeroBuffer(buffer);
    //FreeBuffer(buffer);
}

static void
SetStatusMessage(Term_Editor* editor, char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(editor->StatusMessage, sizeof(editor->StatusMessage), fmt, ap);
    va_end(ap);
    editor->StatusMessageTime = time(0);
}

static char*
PromptMessage(Term_Editor* editor, char* message) {
    size_t bufferSize = 128;
    char* buffer = (char*)malloc(bufferSize);
    buffer[0] = 0;
    
    size_t len = 0;
    for(;;) {
        SetStatusMessage(editor, message, buffer);
        UpdateScreen(editor);
        
        u8 character = 0;
        if(read(STDIN_FILENO, &character, 1) == -1) break;
        
        if(character == KeyType_Del || character == CTRL_KEY('h') || character == KeyType_Backspace) {
            if(len != 0) buffer[--len] = 0;
        } else if(character == '\x1b') {
            SetStatusMessage(editor, "");
            free(buffer);
            break;
        } else if(character == '\r') {
            if(len != 0) {
                SetStatusMessage(editor, "");
                return buffer;
            }
        } else if(!iscntrl(character) && character < 128) {
            if(len == bufferSize - 1) {
                bufferSize *= 2;
                buffer = (char*)realloc(buffer, bufferSize);
            }
            buffer[len++] = character;
            buffer[len] = 0;
        }
    }
    
    return 0;
}

static void
SaveFile(Term_Editor* editor) {
    if(!editor->Filename) {
        editor->Filename = PromptMessage(editor, "Save as: %s");
        if(!editor->Filename) {
            SetStatusMessage(editor, "Save aborted");
            return;
        }
    }
    
    int totalLen = 0;
    char* data;
    {
        for(size_t lineIndex = 0; lineIndex < editor->LineCount; lineIndex++) {
            totalLen += editor->Lines[lineIndex].Size + 1;
        }
        
        data = (char*)malloc(totalLen);
        char* ptr = data;
        for(size_t lineIndex = 0; lineIndex < editor->LineCount; lineIndex++) {
            memcpy(ptr, editor->Lines[lineIndex].Data, editor->Lines[lineIndex].Size);
            ptr += editor->Lines[lineIndex].Size;
            *ptr++ = '\n';
        }
    }
    
    int fileHandle = open(editor->Filename, O_RDWR | O_CREAT, 0644);
    Assert(fileHandle != -1); // TODO
    
    int result = ftruncate(fileHandle, totalLen);
    Assert(result != -1); // TODO
    
    ssize_t written = write(fileHandle, data, totalLen);
    Assert(written == totalLen); // TODO
    //SetStatusMessage(editor, "Can't save! I/O error: %s", strerror(errno));
    
    close(fileHandle);
    free(data);
    
    editor->Dirty = false;
    SetStatusMessage(editor, "%d bytes written to disk", totalLen);
}

static b32
EnableRawMode(Term_Editor* editor) {
    if(editor->RawModeEnabled) return true; // Already enabled.
    if(!isatty(STDIN_FILENO)) {
        errno = ENOTTY;
        return false;
    }
    
    if(tcgetattr(STDIN_FILENO, &GlobalOriginalSettings) == -1) return false;
    
    struct termios terminalSettings = GlobalOriginalSettings; // Modify the original mode
    // Input modes: no break, no CR to NL, no parity check, no strip char,
    // no start/stop output control.
    terminalSettings.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    // Output modes - disable post processing
    terminalSettings.c_oflag &= ~(OPOST);
    // Control modes - set 8 bit chars
    terminalSettings.c_cflag |= (CS8);
    // Local modes - echoing off, canonical off, no extended functions,
    // no signal chars (^Z,^C)
    terminalSettings.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // control chars - set return condition: min number of bytes and timer.
    terminalSettings.c_cc[VMIN] = 0; // Return each byte, or zero for timeout.
    terminalSettings.c_cc[VTIME] = 1; // 100 ms timeout (unit is tens of second).
    
    // Put terminal in raw mode after flushing
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminalSettings) < 0) return false;
    editor->RawModeEnabled = true;
    
    return true;
}

static void
UpdateRenderLine(Line_Data* line) {
    int tabCount = 0;
    for(size_t colIndex = 0; colIndex < line->Size; colIndex++) {
        if(line->Data[colIndex] == '\t') tabCount++;
    }
    
    free(line->RenderData);
    line->RenderData = (char*)malloc(line->Size + (tabCount*(TAB_WIDTH-1)) + 1); // line->Size already counts 1 for each tab
    
    int index = 0;
    for(size_t colIndex = 0; colIndex < line->Size; colIndex++) {
        if(line->Data[colIndex] == '\t') {
            line->RenderData[index++] = ' ';
            while(index % TAB_WIDTH != 0) line->RenderData[index++] = ' ';
        } else {
            line->RenderData[index++] = line->Data[colIndex];
        }
    }
    line->RenderData[index] = 0; // Null terminator
    line->RenderSize = index;
}

static void
InsertCharacterInLine(Line_Data* line, size_t at, u8 character) {
    if(at > line->Size) at = line->Size;
    line->Data = (char*)realloc(line->Data, line->Size + 2);
    memmove(line->Data + at+1, line->Data + at, line->Size - at+1);
    line->Size++;
    line->Data[at] = character;
    
    UpdateRenderLine(line);
}

static void
InsertLine(Term_Editor* editor, size_t at, char* data, size_t length) {
    if(at > editor->LineCount) return;
    
    editor->Lines = (Line_Data*)realloc(editor->Lines, (editor->LineCount+1)*sizeof(Line_Data));
    memmove(editor->Lines + at+1, editor->Lines + at, (editor->LineCount - at) * sizeof(Line_Data));
    
    editor->Lines[at].Size = length;
    editor->Lines[at].Data = (char*)malloc(length+1);
    memcpy(editor->Lines[at].Data, data, length);
    editor->Lines[at].Data[length] = 0;
    
    editor->Lines[at].RenderSize = 0;
    editor->Lines[at].RenderData = 0;
    
    UpdateRenderLine(editor->Lines + at);
    
    editor->LineCount++;
    editor->Dirty = true;
}

static void
InsertCharacter(Term_Editor* editor, u8 character) {
    if(editor->CursorPos.y == editor->LineCount) {
        InsertLine(editor, editor->LineCount, "", 0);
    }
    InsertCharacterInLine(editor->Lines + editor->CursorPos.y, editor->CursorPos.x, character);
    editor->Dirty = true;
    editor->CursorPos.x++;
}

static void
DeleteCharacterInLine(Line_Data* line, size_t at) {
    if(at >= line->Size) return;
    memmove(line->Data + at, line->Data + at+1, line->Size - at);
    line->Size--;
    UpdateRenderLine(line);
}

static void
DeleteLine(Term_Editor* editor, size_t at) {
    if(at >= editor->RowCount) return;
    Line_Data* line = editor->Lines + at;
    
    {
        free(line->RenderData);
        free(line->Data);
    }
    
    memmove(line, line + 1, (editor->RowCount - at-1) * sizeof(Line_Data));
    editor->RowCount--;
    editor->Dirty = true;
}

static void
DeleteCharacter(Term_Editor* editor) {
    if(editor->CursorPos.y == editor->RowCount) return;
    if(editor->CursorPos.x == 0 && editor->CursorPos.y == 0) return; 
    
    Line_Data* line = editor->Lines + editor->CursorPos.y;
    if(editor->CursorPos.x > 0) {
        DeleteCharacterInLine(line, editor->CursorPos.x - 1);
        editor->CursorPos.x--;
        editor->Dirty = true;
    } else {
        editor->CursorPos.x = editor->Lines[editor->CursorPos.y - 1].Size;
        {
            Line_Data* lineAbove = editor->Lines + editor->CursorPos.y - 1;
            lineAbove->Data = (char*)realloc(lineAbove->Data, lineAbove->Size + line->Size + 1);
            memcpy(lineAbove->Data + lineAbove->Size, line->Data, line->Size);
            lineAbove->Size += line->Size;
            lineAbove->Data[lineAbove->Size] = 0;
            UpdateRenderLine(lineAbove);
            editor->Dirty = true;
        }
        DeleteLine(editor, editor->CursorPos.y);
        editor->CursorPos.y--;
    }
}

static void
ProcessKeyInput(Term_Editor* editor, u8 character) {
#define KEY_ENTER 0xd
#define KEY_ESC 0x1b // '\x1b'
#define QUIT_TIMES 1
    Line_Data* line = editor->Lines + editor->CursorPos.y;
    persist int quitTimes = QUIT_TIMES;
    
    switch(character) {
        case CTRL_KEY('q'): {
            if(editor->Dirty && quitTimes > 0) {
                SetStatusMessage(editor, "WARNING!! File has unsaved changes. Press Ctrl-Q %d more time to quit.", quitTimes);
                quitTimes--;
                return;
            }
            GlobalRunning = false;
        } break;
        case KeyType_Up: {
            if(editor->CursorPos.y > 0) editor->CursorPos.y--;
        } break;
        case KeyType_Down: {
            if(editor->CursorPos.y < editor->LineCount-1) editor->CursorPos.y++; 
        } break;
        case KeyType_Left: {
            if(editor->CursorPos.x > 0) {
                editor->CursorPos.x--;
            } else if(editor->CursorPos.y > 0) {
                editor->CursorPos.y--;
                editor->CursorPos.x = editor->Lines[editor->CursorPos.y].Size;
            }
        } break;
        case KeyType_Right: {
            if(editor->CursorPos.x < line->Size) {
                editor->CursorPos.x++;
            } else if(editor->CursorPos.y < editor->LineCount-1) {
                editor->CursorPos.y++;
                editor->CursorPos.x = 0;
            }
        } break;
        case KeyType_PageDown:
        case KeyType_PageUp: {
            if(character == KeyType_PageUp) {
                editor->CursorPos.y = editor->Offset.y;
            } else if(character == KeyType_PageDown) {
                editor->CursorPos.y = editor->Offset.y + editor->RowCount-1;
                if(editor->CursorPos.y > editor->LineCount-1) editor->CursorPos.y = editor->LineCount-1;
            }
            
            int times = editor->RowCount;
            while(times--) {
                ProcessKeyInput(editor, character == KeyType_PageUp ? KeyType_Up : KeyType_Down);
            }
        } break;
        case KEY_ENTER: { // Enter
            { // editorInsertNewLine()
                if(editor->CursorPos.x == 0) {
                    InsertLine(editor, editor->CursorPos.y, "", 0);
                } else {
                    Line_Data* line = editor->Lines + editor->CursorPos.y;
                    InsertLine(editor, editor->CursorPos.y + 1, line->Data + editor->CursorPos.x, line->Size - editor->CursorPos.x);
                    line = editor->Lines + editor->CursorPos.y;
                    line->Size = editor->CursorPos.x;
                    line->Data[line->Size] = 0;
                    UpdateRenderLine(line);
                }
                editor->CursorPos.y++;
                editor->CursorPos.x = 0;
            }
        } break;
        case KEY_ESC: break;
        case CTRL_KEY('h'): break;
        case CTRL_KEY('l'): break;
        case CTRL_KEY('s'): SaveFile(editor); break;
        case KeyType_Del:
        case KeyType_Backspace: { 
            if(character == KeyType_Del) editor->CursorPos.x++; // Move cursor to the right
            
            DeleteCharacter(editor);
        } break;
        case KeyType_Home: {
            editor->CursorPos.x = 0;
        } break;
        case KeyType_End: {
            editor->CursorPos.x = line->Size;
        } break;
        default: InsertCharacter(editor, character); break;
    }
    
    // Update the current line in case the cursor position Y changed
    line = editor->Lines + editor->CursorPos.y;
    // Snap to the end of the line
    if(editor->CursorPos.x > line->Size) {
        editor->CursorPos.x = line->Size;
    }
    
    quitTimes = QUIT_TIMES;
}

static b32
GetCursorPosition(size_t* rows, size_t* columns) {
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return false;
    
    char buffer[32];
    size_t index = 0;
    while(index < sizeof(buffer) - 1) {
        if(read(STDIN_FILENO, &buffer[index], 1) != 1) break;
        if(buffer[index++] == 'R') break;
    }
    buffer[index] = 0;
    
    if(buffer[0] != '\x1b' || buffer[1] != '[') return false;
    if(sscanf(&buffer[2], "%lu;%lu", rows, columns) != 2) return false;
    
    return true;
}

static b32
GetWindowSize(size_t* rows, size_t* columns) {
    struct winsize windowSize;
    
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &windowSize) == -1 || windowSize.ws_col == 0) {
        // Try get the terminal size by setting the cursor at the bottom right and get the position.
        // Using Cursor Forward and Cursor Down
        write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12);
        return GetCursorPosition(rows, columns);
    }
    
    *rows = windowSize.ws_row;
    *columns = windowSize.ws_col;
    
    return true;
}

static b32
LoadFile(Term_Editor* editor, char* filename) {
    FILE* fileHandle = fopen(filename, "r");
    if(!fileHandle) {
        return false;
    }
    
    // Save the file name
    if(editor->Filename) {
        free(editor->Filename);
    }
    editor->Filename = strdup(filename);
    
    // Read the File
    char* line = 0;
    size_t lineCap = 0;
    ssize_t lineLen;
    while((lineLen = getline(&line, &lineCap, fileHandle)) != -1) {
        while(lineLen > 0 && (line[lineLen -1] == '\n' || line[lineLen - 1] == '\r')) {
            lineLen--;
        }
        
        InsertLine(editor, editor->LineCount, line, lineLen);
    }
    free(line);
    fclose(fileHandle);
    editor->Dirty = false;
    
    return true;
}

int main(int argCount, char** args) {
    Term_Editor editor = {};
    
    if(!EnableRawMode(&editor)) {
        fprintf(stderr,"ERROR: Failed setting the terminal to Raw Mode\n");
        return -1;
    };
    
    if(!GetWindowSize(&editor.RowCount, &editor.ColumnCount)) {
        return -1;
    }
    editor.RowCount -= 2; // leave room for the status bar
    
    if(argCount >= 2) {
        LoadFile(&editor, args[1]);
    }
    
    SetStatusMessage(&editor, "HELP: Ctrl-Q to quit | Ctrl-S to Save");
    
    // Main loop
    while(GlobalRunning) {
        UpdateScreen(&editor);
        
        u8 character = 0;
        if(read(STDIN_FILENO, &character, 1) == -1) break;
        if(character == '\x1b') {
            char sequence[3];
            
            if(read(STDIN_FILENO, &sequence[0], 1) != 1) goto end;
            if(read(STDIN_FILENO, &sequence[1], 1) != 1) goto end;
            
            if(sequence[0] == '[') {
                if(sequence[1] >= '0' && sequence[1] <= '9') {
                    if(read(STDIN_FILENO, &sequence[2], 1) != 1) goto end;
                    if(sequence[2] == '~') {
                        switch(sequence[1]) {
                            case '1': character = KeyType_Home; break;
                            case '3': character = KeyType_Del; break;
                            case '4': character = KeyType_End; break;
                            case '5': character = KeyType_PageUp; break;
                            case '6': character = KeyType_PageDown; break;
                            case '7': character = KeyType_Home; break;
                            case '8': character = KeyType_End; break;
                        }
                    }
                    
                } else {
                    switch(sequence[1]) {
                        case 'A': character = KeyType_Up; break;
                        case 'B': character = KeyType_Down; break;
                        case 'C': character = KeyType_Right; break;
                        case 'D': character = KeyType_Left; break;
                        case 'H': character = KeyType_Home; break;
                        case 'F': character = KeyType_End; break;
                    }
                }
            } else if(sequence[0] == 'O') {
                switch(sequence[1]) {
                    case 'H': character = KeyType_Home; break;
                    case 'F': character = KeyType_End; break;
                }
            }
        }
        end:
        
        if(character) ProcessKeyInput(&editor, character);
    }
    
    ClearTerminal();
    RestoreTerminalSettings();
    
    return 0;
}