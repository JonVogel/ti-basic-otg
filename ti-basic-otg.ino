/*
 * TI Extended BASIC Simulator v0.2
 * For ESP32-S3-USB-OTG
 *
 * Refactored architecture:
 *   - Execution Manager (EM): handles program flow
 *   - Token Parser (TP): state machine interprets tokens
 *   - Expression Parser: evaluates arithmetic/logical expressions
 *   - Variable Table: manages numeric and string variables
 *
 * Display: 28 columns x 24 rows (8x8 pixel characters)
 * Storage: Programs tokenized in RAM, saved as text to LittleFS
 *
 * Board settings (Arduino IDE):
 *   Board: "ESP32S3 Dev Module"
 *   Partition: Custom (8MB with SPIFFS)
 *   USB CDC On Boot: "Enabled"
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <LittleFS.h>
#include <SD.h>
#include "ti_font.h"
#include "ble_keyboard.h"
#include "exec_manager.h"
#include "file_io.h"
#include "sprites.h"
#include "line_editor.h"

// LCD pins
#define LCD_DC    4
#define LCD_CS    5
#define LCD_SCLK  6
#define LCD_MOSI  7
#define LCD_RST   8
#define LCD_BL    9

// Buttons
#define BTN_OK    0
#define BTN_UP   10
#define BTN_DOWN 11
#define BTN_MENU 14

// LEDs
#define LED_GREEN  15
#define LED_YELLOW 16

// Display geometry
#define COLS       28
#define ROWS       24
#define CHAR_W     8
#define CHAR_H     8
#define SCREEN_W   240
#define SCREEN_H   240

// Display offsets for centering/border
#define DISPLAY_X_OFFSET 8
#define DISPLAY_Y_OFFSET 16

// Status bar at the bottom
#define STATUS_Y   (DISPLAY_Y_OFFSET + ROWS * CHAR_H)

// ---------------------------------------------------------------------------
// Keyword table — maps text to tokens (used by tokenizer)
// ---------------------------------------------------------------------------

struct KeywordEntry
{
  const char* text;
  Token token;
};

static const KeywordEntry keywords[] =
{
  // RUN, NEW, DIR are handled as pre-tokenize string commands
  // (so they're not listed here)
  {"LIST",       TOK_LIST},
  {"OLD",        TOK_OLD},
  {"SAVE",       TOK_SAVE},
  {"BYE",        TOK_BYE},
  {"NUMBER",     TOK_NUM},
  {"NUM",        TOK_NUM},
  {"PRINT",      TOK_PRINT},
  {"USING",      TOK_USING},
  {"DISPLAY",    TOK_DISPLAY},
  {"ACCEPT",     TOK_ACCEPT},
  {"GOTO",       TOK_GOTO},
  {"GO TO",      TOK_GOTO},
  {"GOSUB",      TOK_GOSUB},
  {"RETURN",     TOK_RETURN},
  {"IF",         TOK_IF},
  {"THEN",       TOK_THEN},
  {"ELSE",       TOK_ELSE},
  {"FOR",        TOK_FOR},
  {"TO",         TOK_TO},
  {"STEP",       TOK_STEP},
  {"NEXT",       TOK_NEXT},
  {"LET",        TOK_LET},
  {"INPUT",      TOK_INPUT},
  {"LINPUT",     TOK_LINPUT},
  {"DIM",        TOK_DIM},
  {"REM",        TOK_REM},
  {"END",        TOK_END},
  {"STOP",       TOK_STOP},
  {"DATA",       TOK_DATA},
  {"READ",       TOK_READ},
  {"RESTORE",    TOK_RESTORE},
  {"RANDOMIZE",  TOK_RANDOMIZE},
  {"DEF",        TOK_DEF},
  {"ON",         TOK_ON},
  {"OPTION",     TOK_OPTION},
  {"BASE",       TOK_BASE},
  {"BREAK",      TOK_BREAK},
  {"UNBREAK",    TOK_UNBREAK},
  {"ERROR",      TOK_ERROR},
  {"WARNING",    TOK_WARNING},
  {"CONTINUE",   TOK_CONTINUE},
  {"CON",        TOK_CONTINUE},
  {"RESEQUENCE", TOK_RES},
  {"RES",        TOK_RES},
  {"SIZE",       TOK_SIZE},
  {"MERGE",      TOK_MERGE},
  {"CALL",       TOK_CALL},
  {"SUB",        TOK_SUB},
  {"SUBEND",     TOK_SUBEND},
  {"SUBEXIT",    TOK_SUBEXIT},
  {"OPEN",       TOK_OPEN},
  {"CLOSE",      TOK_CLOSE},
  {"OUTPUT",     TOK_OUTPUT},
  {"UPDATE",     TOK_UPDATE},
  {"APPEND",     TOK_APPEND},
  {"SEQUENTIAL", TOK_SEQUENTIAL},
  {"RELATIVE",   TOK_RELATIVE},
  {"INTERNAL",   TOK_INTERNAL},
  {"FIXED",      TOK_FIXED},
  {"PERMANENT",  TOK_PERMANENT},
  {"VARIABLE",   TOK_VARIABLE_KW},
  {"REC",        TOK_REC},
  {"DELETE",     TOK_DELETE},
  {"IMAGE",      TOK_IMAGE},
  {"TRACE",      TOK_TRACE},
  {"UNTRACE",    TOK_UNTRACE},
  {"AND",        TOK_AND},
  {"OR",         TOK_OR},
  {"XOR",        TOK_XOR},
  {"NOT",        TOK_NOT},
  {NULL, TOK_EOL}
};

// ---------------------------------------------------------------------------
// Tokenizer (converts text to tokens)
// ---------------------------------------------------------------------------

static int skipSpaces(const char* src, int pos)
{
  while (src[pos] == ' ')
  {
    pos++;
  }
  return pos;
}

static int matchKeyword(const char* src, int pos)
{
  int bestMatch = -1;
  int bestLen = 0;

  for (int i = 0; keywords[i].text != NULL; i++)
  {
    const char* kw = keywords[i].text;
    int klen = strlen(kw);
    bool match = true;

    for (int j = 0; j < klen; j++)
    {
      if (toupper(src[pos + j]) != kw[j])
      {
        match = false;
        break;
      }
    }

    if (match && klen > bestLen)
    {
      char next = src[pos + klen];
      if (next == '\0' || !isalnum(next))
      {
        bestMatch = i;
        bestLen = klen;
      }
    }
  }

  return bestMatch;
}

static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen)
{
  int pos = 0;
  int out = 0;

  while (src[pos] != '\0')
  {
    pos = skipSpaces(src, pos);
    if (src[pos] == '\0')
    {
      break;
    }

    // REM — rest of line is literal text
    if (out > 0 && tokens[out - 1] == TOK_REM)
    {
      int remLen = strlen(&src[pos]);
      if (out + 2 + remLen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)remLen;
      memcpy(&tokens[out], &src[pos], remLen);
      out += remLen;
      break;
    }

    // Quoted string
    if (src[pos] == '"')
    {
      pos++;
      int start = pos;
      while (src[pos] != '\0' && src[pos] != '"')
      {
        pos++;
      }
      int slen = pos - start;
      if (src[pos] == '"')
      {
        pos++;
      }
      if (out + 2 + slen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      continue;
    }

    // Number literal — stored as TOK_UNQUOTED_STR + length + ASCII digits
    if (isdigit(src[pos]) || (src[pos] == '.' && isdigit(src[pos + 1])))
    {
      int start = pos;
      while (isdigit(src[pos]) || src[pos] == '.' || src[pos] == 'E' ||
             src[pos] == 'e' ||
             ((src[pos] == '+' || src[pos] == '-') &&
              (src[pos - 1] == 'E' || src[pos - 1] == 'e')))
      {
        pos++;
      }
      int slen = pos - start;
      if (slen > 255 || out + 2 + slen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_UNQUOTED_STR;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      continue;
    }

    // Operators and punctuation
    bool foundOp = true;
    switch (src[pos])
    {
      case '+':  tokens[out++] = TOK_PLUS;       pos++; break;
      case '-':  tokens[out++] = TOK_MINUS;      pos++; break;
      case '*':  tokens[out++] = TOK_MULTIPLY;   pos++; break;
      case '/':  tokens[out++] = TOK_DIVIDE;     pos++; break;
      case '^':  tokens[out++] = TOK_POWER;      pos++; break;
      case '&':  tokens[out++] = TOK_CONCAT;     pos++; break;
      case '(':  tokens[out++] = TOK_LPAREN;     pos++; break;
      case ')':  tokens[out++] = TOK_RPAREN;     pos++; break;
      case ',':  tokens[out++] = TOK_COMMA;      pos++; break;
      case ';':  tokens[out++] = TOK_SEMICOLON;  pos++; break;
      case ':':  tokens[out++] = TOK_COLON;      pos++; break;
      case '=':  tokens[out++] = TOK_EQUAL;      pos++; break;
      case '<':
        // TI encodes compound comparisons as two separate tokens:
        // <=  →  TOK_LESS + TOK_EQUAL
        // <>  →  TOK_LESS + TOK_GREATER
        // >=  →  TOK_GREATER + TOK_EQUAL
        tokens[out++] = TOK_LESS;
        if (src[pos + 1] == '=')      { tokens[out++] = TOK_EQUAL;   pos += 2; }
        else if (src[pos + 1] == '>') { tokens[out++] = TOK_GREATER; pos += 2; }
        else                          {                              pos++;    }
        break;
      case '>':
        tokens[out++] = TOK_GREATER;
        if (src[pos + 1] == '=') { tokens[out++] = TOK_EQUAL; pos += 2; }
        else                     {                            pos++;    }
        break;
      default:
        foundOp = false;
        break;
    }
    if (foundOp)
    {
      continue;
    }

    // Keyword match
    int kwIdx = matchKeyword(src, pos);
    if (kwIdx >= 0)
    {
      if (out >= maxLen)
      {
        return -1;
      }
      tokens[out++] = keywords[kwIdx].token;
      pos += strlen(keywords[kwIdx].text);
      continue;
    }

    // Variable name
    if (isalpha(src[pos]))
    {
      // Variable name — stored as raw ASCII bytes (TI format)
      int start = pos;
      while (isalnum(src[pos]) || src[pos] == '_')
      {
        pos++;
      }
      if (src[pos] == '$')
      {
        pos++;
      }
      int vlen = pos - start;
      if (out + vlen >= maxLen)
      {
        return -1;
      }
      for (int i = 0; i < vlen; i++)
      {
        tokens[out++] = toupper(src[start + i]);
      }
      continue;
    }

    pos++;
  }

  if (out >= maxLen)
  {
    return -1;
  }
  tokens[out++] = TOK_EOL;
  return out;
}

// ---------------------------------------------------------------------------
// Detokenizer (converts tokens back to text for LIST/SAVE)
// ---------------------------------------------------------------------------

static void appendStr(char* buf, int& out, int bufSize, const char* str)
{
  int slen = strlen(str);
  int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
  memcpy(&buf[out], str, copyLen);
  out += copyLen;
}

static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize)
{
  int pos = 0;
  int out = 0;

  while (pos < length && tokens[pos] != TOK_EOL)
  {
    uint8_t tok = tokens[pos++];

    // Raw ASCII identifier — variable name
    if (isIdentStart(tok))
    {
      if (out < bufSize - 1) buf[out++] = tok;
      while (pos < length && isIdentCont(tokens[pos]))
      {
        if (out < bufSize - 1) buf[out++] = tokens[pos];
        pos++;
      }
      continue;
    }

    // String literal
    if (tok == TOK_QUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      if (out + slen + 2 >= bufSize) break;
      buf[out++] = '"';
      memcpy(&buf[out], &tokens[pos], slen);
      out += slen;
      pos += slen;
      buf[out++] = '"';
      continue;
    }

    // Number / unquoted string
    if (tok == TOK_UNQUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
      memcpy(&buf[out], &tokens[pos], copyLen);
      out += copyLen;
      pos += slen;
      continue;
    }

    // Operators
    switch (tok)
    {
      case TOK_PLUS:       appendStr(buf, out, bufSize, "+"); continue;
      case TOK_MINUS:      appendStr(buf, out, bufSize, "-"); continue;
      case TOK_MULTIPLY:   appendStr(buf, out, bufSize, "*"); continue;
      case TOK_DIVIDE:     appendStr(buf, out, bufSize, "/"); continue;
      case TOK_POWER:      appendStr(buf, out, bufSize, "^"); continue;
      case TOK_EQUAL:      appendStr(buf, out, bufSize, "="); continue;
      case TOK_LESS:       appendStr(buf, out, bufSize, "<"); continue;
      case TOK_GREATER:    appendStr(buf, out, bufSize, ">"); continue;
      // Compound comparisons (<=, <>, >=) are stored as two-token
      // sequences and reconstructed in the keyword loop below; we
      // don't need single-token cases for them here.
      case TOK_CONCAT:     appendStr(buf, out, bufSize, "&"); continue;
      case TOK_AND:        appendStr(buf, out, bufSize, " AND "); continue;
      case TOK_OR:         appendStr(buf, out, bufSize, " OR "); continue;
      case TOK_NOT:        appendStr(buf, out, bufSize, " NOT "); continue;
      case TOK_LPAREN:     appendStr(buf, out, bufSize, "("); continue;
      case TOK_RPAREN:     appendStr(buf, out, bufSize, ")"); continue;
      case TOK_COMMA:      appendStr(buf, out, bufSize, ","); continue;
      case TOK_SEMICOLON:  appendStr(buf, out, bufSize, ";"); continue;
      case TOK_COLON:      appendStr(buf, out, bufSize, ":"); continue;
      default: break;
    }

    // Keywords — look up in keyword table
    for (int i = 0; keywords[i].text != NULL; i++)
    {
      if (keywords[i].token == tok)
      {
        int klen = strlen(keywords[i].text);
        if (out + klen + 2 >= bufSize) break;
        if (out > 0 && buf[out - 1] != ' ') buf[out++] = ' ';
        memcpy(&buf[out], keywords[i].text, klen);
        out += klen;
        buf[out++] = ' ';
        break;
      }
    }
  }

  buf[out] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Display driver
// ---------------------------------------------------------------------------

SPIClass lcd_spi(FSPI);
Adafruit_ST7789 tft(&lcd_spi, LCD_CS, LCD_DC, LCD_RST);

static int cursorCol = 0;
static int cursorRow = 0;

// TI Extended BASIC colors (black text on cyan background)
static uint16_t fgColor = 0x0000;  // black
static uint16_t bgColor = 0x07FF;  // cyan

// Display framebuffer
static char screenBuf[ROWS][COLS];
static char prevScreenBuf[ROWS][COLS];

// TI color palette (indices 1-16) → RGB565
static const uint16_t tiPalette[17] =
{
  0x0000,   // 0 unused
  0x0000,   // 1 transparent (resolves to screen color in drawCell)
  0x0000,   // 2 black
  0x0585,   // 3 medium green
  0x2D8B,   // 4 light green
  0x0012,   // 5 dark blue
  0x0417,   // 6 light blue
  0x8000,   // 7 dark red
  0x0EBF,   // 8 cyan
  0xE000,   // 9 medium red
  0xF2A3,   // 10 light red
  0xD5C0,   // 11 dark yellow
  0xE600,   // 12 light yellow
  0x0280,   // 13 dark green
  0xB816,   // 14 magenta
  0xC618,   // 15 gray
  0xFFFF,   // 16 white
};

// Per-character color palette indices (1-16; 1=transparent=use screen color)
static uint8_t charFgIdx[256];
static uint8_t charBgIdx[256];
static uint8_t screenColorIdx = 8;   // cyan by default

static void initDisplay()
{
  // Keep backlight off until display is ready to prevent showing garbage
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);

  lcd_spi.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  tft.init(240, 240, SPI_MODE0);
  tft.setRotation(2);
  tft.fillScreen(bgColor);
  tft.setTextSize(1);
  tft.setTextColor(fgColor, bgColor);

  // Now turn backlight on
  digitalWrite(LCD_BL, HIGH);
}

// Resolve a palette index to RGB565, with transparency (1) falling through
// to the screen color.
static uint16_t resolveColor(uint8_t idx)
{
  if (idx < 1 || idx > 16) return 0;
  if (idx == 1)
  {
    return tiPalette[screenColorIdx];
  }
  return tiPalette[idx];
}

static void drawCell(int col, int row)
{
  uint8_t ch = (uint8_t)screenBuf[row][col];
  int px = col * CHAR_W + DISPLAY_X_OFFSET;
  int py = row * CHAR_H + DISPLAY_Y_OFFSET;

  uint16_t fg = resolveColor(charFgIdx[ch]);
  uint16_t bg = resolveColor(charBgIdx[ch]);

  uint16_t pixBuf[64];
  for (int y = 0; y < 8; y++)
  {
    uint8_t bits = charPatterns[ch][y];
    for (int x = 0; x < 8; x++)
    {
      pixBuf[y * 8 + x] = (bits & 0x80) ? fg : bg;
      bits <<= 1;
    }
  }
  tft.drawRGBBitmap(px, py, pixBuf, 8, 8);
}

static void refreshScreen()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if (screenBuf[r][c] != prevScreenBuf[r][c])
      {
        drawCell(c, r);
        prevScreenBuf[r][c] = screenBuf[r][c];
      }
    }
  }
}

static void redrawScreen()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      drawCell(c, r);
      prevScreenBuf[r][c] = screenBuf[r][c];
    }
  }
}

static void scrollUp()
{
  memcpy(&screenBuf[0][0], &screenBuf[1][0], COLS * (ROWS - 1));
  memset(&screenBuf[ROWS - 1][0], 0x20, COLS);
  refreshScreen();
  int y = (ROWS - 1) * CHAR_H + DISPLAY_Y_OFFSET;
  tft.fillRect(DISPLAY_X_OFFSET, y, COLS * CHAR_W, CHAR_H, bgColor);
}

static void printChar(char c)
{
  // Mirror output to serial terminal for copy/paste
  Serial.write(c);
  if (c == '\n')
  {
    Serial.write('\r');
  }

  // TI behavior: cursor always on bottom row (ROWS-1 = 23).
  // '\n' scrolls up one row and resets column to 0.
  if (c == '\n')
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
    return;
  }

  // Column wrap — scroll up and start fresh at col 0
  if (cursorCol >= COLS)
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
  }

  screenBuf[cursorRow][cursorCol] = c;
  drawCell(cursorCol, cursorRow);
  prevScreenBuf[cursorRow][cursorCol] = c;
  cursorCol++;
}

static void printString(const char* str)
{
  while (*str)
  {
    printChar(*str++);
  }
}

static void printLine(const char* str)
{
  printString(str);
  printChar('\n');
}

// TI-style error print: blank line, error message, blank line, plus a
// BEL (0x07) to the serial terminal so monitors that honor it beep.
static void printError(const char* str)
{
  printLine("");
  printLine(str);
  printLine("");
  Serial.write(0x07);
}

static void clearScreen()
{
  memset(screenBuf, ' ', COLS * ROWS);
  fillBackground(bgColor);
  // TI behavior: cursor on bottom row after CLEAR
  cursorCol = 0;
  cursorRow = ROWS - 1;
}

// Move cursor to bottom row for INPUT (TI behavior — INPUT always at row 24).
// With the new print model, we just need to ensure we're at col 0.
static void gfxPrepareInput()
{
  if (cursorCol > 0)
  {
    printChar('\n');
  }
}

// Reset graphics to editor defaults (called when program ends)
// Reset all character colors to default: fg=black(2), bg=transparent(1).
// Screen color defaults to cyan(8), so transparent bg shows cyan.
static void gfxResetColors()
{
  for (int i = 0; i < 256; i++)
  {
    charFgIdx[i] = 2;   // black
    charBgIdx[i] = 1;   // transparent (→ screen color)
  }
  screenColorIdx = 8;   // cyan
  fgColor = tiPalette[2];
  bgColor = tiPalette[8];
}

static void gfxReset()
{
  gfxResetColors();
  initCharPatterns();
  // Redraw each cell in place — drawCell paints both bg and fg, so no
  // fillScreen is needed. Avoids the cyan flash.
  redrawScreen();
}

// Graphics callbacks for CALL HCHAR, VCHAR, GCHAR, SCREEN, COLOR

static void gfxSetChar(int row, int col, char ch)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
  screenBuf[row][col] = ch;
  drawCell(col, row);
  prevScreenBuf[row][col] = ch;
}

static char gfxGetChar(int row, int col)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return 32;
  return screenBuf[row][col];
}

static void gfxSetScreenColor(int colorIdx)
{
  if (colorIdx < 1 || colorIdx > 16) return;
  screenColorIdx = colorIdx;
  bgColor = tiPalette[colorIdx];
  // Redraw each cell to pick up the new screen color (transparent bg chars
  // resolve to this). drawCell fully paints each cell — no flash needed.
  redrawScreen();
}

// CALL COLOR(set, fg, bg) — sets colors for a group of 8 characters.
// Extended BASIC character sets:
//   Set 1 = chars 32-39, Set 2 = 40-47, ... Set 16 = 152-159
// Move cursor to a specific position (for DISPLAY AT, ACCEPT AT).
// row, col are 0-based.
static void gfxMoveCursor(int row, int col)
{
  if (row < 0) row = 0;
  if (row >= ROWS) row = ROWS - 1;
  if (col < 0) col = 0;
  if (col >= COLS) col = COLS - 1;
  cursorRow = row;
  cursorCol = col;
}

// CALL KEY: read one key from Serial or BLE keyboard without blocking.
// Returns 0 if no key available, else the character code.
static int gfxReadKey()
{
  if (Serial.available())
  {
    return Serial.read() & 0xFF;
  }
  if (bleKbAvailable())
  {
    return bleKbRead() & 0xFF;
  }
  return 0;
}

// CALL CHAR: redefine a character's 8x8 bitmap pattern
static void gfxSetCharPattern(int charCode, const uint8_t* pattern)
{
  if (charCode < 0 || charCode > 255) return;
  memcpy(charPatterns[charCode], pattern, 8);
  // Redraw any cells using this character
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if ((uint8_t)screenBuf[r][c] == (uint8_t)charCode)
      {
        drawCell(c, r);
      }
    }
  }
}

// CALL CHARPAT: read a character's current 8×8 pattern
static void gfxGetCharPattern(int charCode, uint8_t* out)
{
  if (charCode < 0 || charCode > 255)
  {
    memset(out, 0, 8);
    return;
  }
  memcpy(out, charPatterns[charCode], 8);
}

// CALL CHARSET: reset characters 32-127 to their ROM default patterns.
// Leaves user-defined graphics slots (128+) alone.
static void gfxResetCharset()
{
  for (int i = 32; i < 128; i++)
  {
    memcpy_P(charPatterns[i], tiFont[i], 8);
  }
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= 32 && ch < 128) drawCell(c, r);
    }
  }
}

static void gfxSetCharColor(int charSet, int fg, int bg)
{
  if (charSet < 1 || charSet > 16) return;
  if (fg < 1 || fg > 16) return;
  if (bg < 1 || bg > 16) return;

  int firstChar = 32 + (charSet - 1) * 8;
  for (int i = 0; i < 8; i++)
  {
    int ch = firstChar + i;
    if (ch >= 0 && ch < 256)
    {
      charFgIdx[ch] = (uint8_t)fg;
      charBgIdx[ch] = (uint8_t)bg;
    }
  }

  // Redraw any cells using characters in this set
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= firstChar && ch < firstChar + 8)
      {
        drawCell(c, r);
      }
    }
  }
}

// Fill the display: char area = bgColor, borders (outside 24 rows) = black.
static void fillBackground(uint16_t bg)
{
  tft.fillScreen(0x0000);   // black everywhere first
  tft.fillRect(DISPLAY_X_OFFSET, DISPLAY_Y_OFFSET,
               COLS * CHAR_W, ROWS * CHAR_H, bg);
}

// TI-Texas logo — 3×3 character grid taken straight from the TI title
// screen. Each entry is the CALL CHAR pattern for one cell (row,col).
// Cells are laid out row-major: 129=(1,1) 130=(1,2) 131=(1,3)
//                                132=(2,1) 133=(2,2) 134=(2,3)
//                                135=(3,1) 136=(3,2) 137=(3,3)
static const uint8_t tiLogoChars[9][8] =
{
  {0x00, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},  // (1,1)
  {0x00, 0xFC, 0x04, 0x05, 0x05, 0x04, 0x06, 0x02},  // (1,2)
  {0x00, 0x00, 0x80, 0x40, 0x40, 0x80, 0x00, 0x0C},  // (1,3)
  {0x03, 0xFF, 0x80, 0xC0, 0x40, 0x60, 0x38, 0x1C},  // (2,1)
  {0x0C, 0x19, 0x21, 0x21, 0x3D, 0x05, 0x05, 0x05},  // (2,2)
  {0x12, 0xBA, 0x8A, 0x8A, 0xBA, 0xA1, 0xA1, 0xA1},  // (2,3)
  {0x0E, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},  // (3,1)
  {0xC4, 0xE2, 0x31, 0x10, 0x18, 0x0C, 0x07, 0x03},  // (3,2)
  {0x22, 0x4C, 0x90, 0x20, 0x40, 0x40, 0x20, 0xE0},  // (3,3)
};

// Redefine char codes 129..137 with the logo patterns and place them in
// a 3×3 grid on screen with the top-left at (startRow, startCol).
static void drawTexasLogo(int startRow, int startCol)
{
  for (int i = 0; i < 9; i++)
  {
    memcpy(charPatterns[129 + i], tiLogoChars[i], 8);
  }
  for (int r = 0; r < 3; r++)
  {
    for (int c = 0; c < 3; c++)
    {
      int ch = 129 + r * 3 + c;
      screenBuf[startRow + r][startCol + c] = (char)ch;
      drawCell(startCol + c, startRow + r);
    }
  }
}

// 8x8 copyright glyph (©) used in the splash-screen copyright line.
static const uint8_t copyrightBitmap[8] =
{
  0x3C,  // ..####..
  0x42,  // .#....#.
  0x99,  // #..##..#
  0xA1,  // #.#....#
  0xA1,  // #.#....#
  0x99,  // #..##..#
  0x42,  // .#....#.
  0x3C,  // ..####..
};

// TI-99/4A boot screen: colored stripes top and bottom, centered text.
// Pattern approximates the 1981 TI home computer startup screen.
static void showBootScreen()
{
  // Clear display to cyan in the char area, black outside
  fillBackground(tiPalette[8]);

  // Redefine char 128 as the © copyright symbol
  memcpy(charPatterns[128], copyrightBitmap, 8);

  // Stripe colors (approximating the TI pattern left to right)
  const uint8_t stripes[] = {
    9, 4, 2, 12, 13, 14,        // left group
    5, 3, 14, 9, 15, 6, 10, 12, 9   // right group
  };
  const int numStripes = sizeof(stripes);

  // Top and bottom colored bars: each stripe ~2 chars wide, 3 rows tall
  // 15 stripes + 1 gap = 16 slots. The character area is 28 × 8 = 224
  // pixels wide, so 224 / 16 = 14 pixels per slot fits exactly.
  const int stripeW = 14;
  const int stripeH = 24;        // 3 rows × 8px
  const int gapEnd  = 7 * stripeW;   // gap occupies slot 6 (after 6 stripes)

  // Top band — TI rows 1-3
  int topY = DISPLAY_Y_OFFSET;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft.fillRect(x, topY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Bottom band — TI rows 19-21 (0-indexed 18-20)
  int bottomY = DISPLAY_Y_OFFSET + 18 * CHAR_H;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft.fillRect(x, bottomY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Draw centered text directly via the framebuffer.
  // Our display is 28 cols; "TEXAS INSTRUMENTS" (17) → col 5 start.
  auto drawText = [](const char* text, int row) {
    int len = strlen(text);
    int col = (COLS - len) / 2;
    if (col < 0) col = 0;
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  // Texas logo — 3×3 char grid at TI rows 6-8 (0-indexed 5-7)
  drawTexasLogo(5, (COLS - 3) / 2);

  drawText("TEXAS INSTRUMENTS",             9);   // TI row 10
  drawText("HOME COMPUTER",                11);   // TI row 12
  drawText("READY-PRESS ANY KEY TO BEGIN", 16);   // TI row 17
  drawText("\x80" "1981    TEXAS INSTRUMENTS", 22);   // TI row 23 with ©

  Serial.println("PRESS ANY KEY TO CONTINUE");

  // Wait for any key — from Serial or BLE keyboard. Keep BLE scanning
  // alive so reconnect can complete while we're sitting here.
  while (!Serial.available() && !bleKbAvailable())
  {
    bleKbTask();
    yield();
    delay(10);
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  // Clear and show the menu screen
  fillBackground(tiPalette[8]);
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  auto drawText2 = [](const char* text, int row, int col) {
    int len = strlen(text);
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  drawText2("TEXAS INSTRUMENTS",     0, 5);
  drawText2("HOME COMPUTER",         1, 7);
  drawText2("PRESS",                 3, 2);
  drawText2("1 FOR TI BASIC",        5, 2);
  drawText2("2 FOR TI EXTENDED BASIC", 7, 2);

  Serial.println("PRESS 1 OR 2 TO CONTINUE");

  while (!Serial.available() && !bleKbAvailable())
  {
    bleKbTask();
    yield();
    delay(10);
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  clearScreen();
}

static void showStatus(const char* msg)
{
  tft.fillRect(0, STATUS_Y, SCREEN_W, SCREEN_H - STATUS_Y, 0x18E3);
  tft.setTextColor(0xFFFF, 0x18E3);
  tft.setCursor(2, STATUS_Y + 2);
  tft.print(msg);
  tft.setTextColor(fgColor, bgColor);
}

// ---------------------------------------------------------------------------
// Execution Manager instance (declared early so the line editor can look up
// program lines for line-number + UP-arrow recall)
// ---------------------------------------------------------------------------
static ExecManager em;

// ---------------------------------------------------------------------------
// Line editor (shared by getInputLine and checkInput)
//
// Supports TI-style keys: DEL (7), INS toggle (4), ERASE (2), CLEAR (12 =
// break), REDO (14), arrows (8/9/10/11), line-number + UP for recall.
// Single-row editing only — long lines aren't wrapped visually but are
// kept in the buffer and submitted intact on Enter.
// ---------------------------------------------------------------------------

static char inputBuf[MAX_INPUT_LEN + 1];
static int  inputPos = 0;          // len of input so far (for main loop)
static bool inputReady = false;

static char lastCommandLine[MAX_INPUT_LEN + 1] = {0};
static bool editInsertMode = false;

// NUMBER mode: when active, editorBeginLine pre-fills the prompt with the
// next auto-incrementing line number. Set by cmdNumber(); cleared when the
// user presses Enter on an empty line.
static int  numModeStart  = 0;
static int  numModeIncr   = 0;
static int  numModeNext   = 0;
static bool numModeActive = false;

// Current editor mode (see line_editor.h). Starts in ENTRY on every new
// line, flips to EDIT on successful recall (REDO or <N>+UP/<N>+DOWN).
static EditMode editMode = EM_ENTRY;

// The program line currently under edit — used by EDIT-mode UP/DOWN
// to move to the previous/next program line. -1 when not in EDIT mode.
static int lastRecalledLineNum = -1;

// Forward decls (needed by line-number recall and UP/DOWN commit)
static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize);
static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen);

// Draw a solid block cursor at the current position (inverted colors)
static void drawCursor(bool visible)
{
  int px = cursorCol * CHAR_W + DISPLAY_X_OFFSET;
  int py = cursorRow * CHAR_H + DISPLAY_Y_OFFSET;
  if (visible)
  {
    tft.fillRect(px, py, CHAR_W, CHAR_H, tiPalette[2]);
  }
  else
  {
    drawCell(cursorCol, cursorRow);
  }
}

// Sync global cursorCol/cursorRow with the edit state's logical position
static void editSyncCursor(const LineEdit& s)
{
  int col = s.startCol + s.pos;
  if (col >= COLS) col = COLS - 1;
  cursorCol = col;
  cursorRow = s.startRow;
}

// Redraw buffer content starting at `fromPos`, plus `eraseExtra` trailing
// cells (used after a shrink). Single-row only.
static void redrawLineTail(const LineEdit& s, int fromPos, int eraseExtra)
{
  for (int i = fromPos; i < s.len; i++)
  {
    int col = s.startCol + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = s.buf[i];
    drawCell(col, s.startRow);
  }
  for (int i = 0; i < eraseExtra; i++)
  {
    int col = s.startCol + s.len + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = ' ';
    drawCell(col, s.startRow);
  }
}

// Double-buffered serial input for fast paste.
//
// Serial.available()/Serial.read() is called by the editor at the editor's
// own pace — about one character per display refresh. That's far slower
// than USB CDC can deliver bytes during a paste, so the Arduino-CDC ring
// buffer (~4 KB) overflows and lines get dropped.
//
// pasteDrainSerial() sucks every byte out of Serial into a much larger
// local ring buffer as often as we can call it (top of loop, inside the
// editor's blink/yield points, inside getInputLine, etc.). editorReadChar
// then reads from this buffer, decoupling the producer (USB) from the
// consumer (display-bound editor) entirely.
#define PASTE_BUF_SIZE 16384
static uint8_t pasteBuf[PASTE_BUF_SIZE];
static int pasteHead = 0;
static int pasteTail = 0;

static void pasteDrainSerial()
{
  while (Serial.available())
  {
    int next = (pasteHead + 1) % PASTE_BUF_SIZE;
    if (next == pasteTail) break;   // full — back-pressure onto Serial
    pasteBuf[pasteHead] = (uint8_t)Serial.read();
    pasteHead = next;
  }
}

static bool pasteAvailable()
{
  return pasteHead != pasteTail;
}

static int pasteRead()
{
  if (pasteHead == pasteTail) return -1;
  uint8_t c = pasteBuf[pasteTail];
  pasteTail = (pasteTail + 1) % PASTE_BUF_SIZE;
  return c;
}

// Reads the next editor byte from the paste buffer (Serial side) or BLE,
// normalizing Serial line endings (\r\n → one Enter; lone \n → \r) and
// dropping tabs (since \t = RIGHT-arrow in the TI encoding).
// Returns -1 if nothing is available.
static int editorReadChar()
{
  static bool skipNextLf = false;

  // Top up from Serial before each read so we don't block on the editor's
  // pace while USB has more bytes waiting.
  pasteDrainSerial();

  while (pasteAvailable())
  {
    uint8_t c = (uint8_t)pasteRead();
    if (c == '\r') { skipNextLf = true;  return '\r'; }
    if (c == '\n')
    {
      if (skipNextLf) { skipNextLf = false; continue; }
      return '\r';
    }
    skipNextLf = false;
    if (c == '\t') continue;
    return c;
  }

  if (bleKbAvailable())
  {
    return bleKbRead();
  }
  return -1;
}

// Find the program index of a line with this lineNum, or -1 if absent.
static int findProgramLineIndex(int lineNum)
{
  for (int i = 0; i < em.programSize(); i++)
  {
    if (em.getLine(i)->lineNum == lineNum) return i;
  }
  return -1;
}

// Replace buffer contents with `src`. Used by REDO and line-number recall.
static void editReplaceLine(LineEdit& s, const char* src)
{
  int oldLen = s.len;
  int n = 0;
  while (src[n] && n < s.maxLen)
  {
    s.buf[n] = src[n];
    n++;
  }
  s.buf[n] = '\0';
  s.len = n;
  s.pos = 0;
  redrawLineTail(s, 0, (oldLen > s.len) ? (oldLen - s.len) : 0);
  editSyncCursor(s);
}

// Re-tokenize the current edit buffer and store it back into the program.
// Called before UP/DOWN navigation so edits to the line are preserved as
// if Enter had been pressed. Returns false only on tokenize failure.
static bool commitEditedLine(const LineEdit& s)
{
  int p = 0;
  while (p < s.len && s.buf[p] == ' ') p++;
  if (p >= s.len) return true;
  if (!isdigit((unsigned char)s.buf[p])) return true;

  int lineNum = 0;
  while (p < s.len && isdigit((unsigned char)s.buf[p]))
  {
    lineNum = lineNum * 10 + (s.buf[p] - '0');
    p++;
  }
  while (p < s.len && s.buf[p] == ' ') p++;

  if (p >= s.len)
  {
    em.deleteLine((uint16_t)lineNum);
    return true;
  }

  uint8_t toks[MAX_LINE_TOKENS];
  int len = tokenizeLine(&s.buf[p], toks, MAX_LINE_TOKENS);
  if (len < 0) return false;
  em.storeLine((uint16_t)lineNum, toks, len);
  return true;
}

// Load the program line at `idx` into the edit buffer, flip to EDIT mode,
// and remember the line number so subsequent UP/DOWN browse prev/next.
static void loadProgramLineToEdit(LineEdit& s, int idx)
{
  ProgramLine* pl = em.getLine(idx);
  if (!pl) return;
  char tmp[MAX_INPUT_LEN + 1];
  int n = snprintf(tmp, sizeof(tmp), "%d ", pl->lineNum);
  detokenizeLine(pl->tokens, pl->length, &tmp[n], sizeof(tmp) - n);
  editReplaceLine(s, tmp);
  lastRecalledLineNum = pl->lineNum;
  editMode = EM_EDIT;
}

// Test whether the current buffer contains nothing but decimal digits.
static bool editBufferIsAllDigits(const LineEdit& s)
{
  if (s.len == 0) return false;
  for (int i = 0; i < s.len; i++)
  {
    if (!isdigit((unsigned char)s.buf[i])) return false;
  }
  return true;
}

// Remove the char at `s.pos` (if any) and redraw the tail.
static void editDeleteAtCursor(LineEdit& s)
{
  if (s.pos >= s.len) return;
  for (int i = s.pos; i < s.len - 1; i++) s.buf[i] = s.buf[i + 1];
  s.len--;
  s.buf[s.len] = '\0';
  redrawLineTail(s, s.pos, 1);
  editSyncCursor(s);
}

// Backspace: move cursor left then delete the char now under it.
static void editBackspace(LineEdit& s)
{
  if (s.pos == 0) return;
  s.pos--;
  editDeleteAtCursor(s);
}

// Insert or overwrite `c` at the cursor and advance.
static void editTypeChar(LineEdit& s, uint8_t c)
{
  if (s.len >= s.maxLen) return;

  if (editInsertMode && s.pos < s.len)
  {
    for (int i = s.len; i > s.pos; i--) s.buf[i] = s.buf[i - 1];
    s.buf[s.pos] = c;
    s.len++;
    s.buf[s.len] = '\0';
    redrawLineTail(s, s.pos, 0);
    s.pos++;
    editSyncCursor(s);
  }
  else
  {
    s.buf[s.pos] = c;
    if (s.pos == s.len)
    {
      s.len++;
      s.buf[s.len] = '\0';
    }
    int col = s.startCol + s.pos;
    if (col < COLS)
    {
      screenBuf[s.startRow][col] = c;
      drawCell(col, s.startRow);
    }
    Serial.write(c);       // mirror to serial for paste visibility
    s.pos++;
    editSyncCursor(s);
  }
}

// Wipe the current line and return to ENTRY mode.
static void editEraseLine(LineEdit& s)
{
  int oldLen = s.len;
  s.len = 0;
  s.pos = 0;
  s.buf[0] = '\0';
  redrawLineTail(s, 0, oldLen);
  editSyncCursor(s);
  editMode = EM_ENTRY;
  lastRecalledLineNum = -1;
}

static EditResult processEditChar(uint8_t c, LineEdit& s)
{
  // ----- handled identically in both modes -----

  // Enter: commit line. Only match '\r' — '\n' (10) is DOWN on TI.
  // Serial's '\n' is normalized to '\r' at the read site.
  if (c == '\r')
  {
    // NUMBER mode: if user pressed Enter without adding anything past the
    // auto-fill, exit NUMBER mode and throw away the buffer so we don't
    // accidentally delete the line with that number.
    if (numModeActive && s.historyEnabled && editorBufferIsAutoFillOnly(s))
    {
      numModeActive = false;
      s.len = 0;
      s.pos = 0;
      s.buf[0] = '\0';
    }
    s.buf[s.len] = '\0';
    if (s.historyEnabled)
    {
      strncpy(lastCommandLine, s.buf, sizeof(lastCommandLine) - 1);
      lastCommandLine[sizeof(lastCommandLine) - 1] = '\0';
    }
    cursorCol = s.startCol + s.len;
    if (cursorCol >= COLS) cursorCol = COLS - 1;
    cursorRow = s.startRow;
    printChar('\n');
    editMode = EM_ENTRY;
    lastRecalledLineNum = -1;
    return EDIT_SUBMITTED;
  }

  // CLEAR — break
  if (c == 12) return EDIT_BROKEN;

  // ERASE (FCTN+3) — wipe line in either mode, drop back to ENTRY
  if (c == 2)
  {
    editEraseLine(s);
    return EDIT_CONTINUE;
  }

  // INS (FCTN+2) — toggle insert mode (global flag, both edit modes)
  if (c == 4)
  {
    editInsertMode = !editInsertMode;
    return EDIT_CONTINUE;
  }

  // REDO (FCTN+8) — reload last-entered line, flip to EDIT
  if (c == 14)
  {
    if (s.historyEnabled && lastCommandLine[0] != '\0')
    {
      editReplaceLine(s, lastCommandLine);
      editMode = EM_EDIT;
    }
    return EDIT_CONTINUE;
  }

  // BKSP (127) — delete previous char. Works in both modes since cursor
  // naturally sits at end during ENTRY.
  if (c == 127)
  {
    editBackspace(s);
    return EDIT_CONTINUE;
  }

  // Printable — typing always feeds the buffer
  if (c >= 32 && c < 127)
  {
    editTypeChar(s, c);
    return EDIT_CONTINUE;
  }

  // ----- Cursor movement & DEL work in every edit context -----

  // LEFT (8, FCTN+S) — cursor left
  if (c == 8)
  {
    if (s.pos > 0) { s.pos--; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // RIGHT (9, FCTN+D) — cursor right
  if (c == 9)
  {
    if (s.pos < s.len) { s.pos++; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // DEL (7, FCTN+1) — delete char at cursor
  if (c == 7)
  {
    editDeleteAtCursor(s);
    return EDIT_CONTINUE;
  }

  // ----- UP/DOWN are mode-aware -----
  //
  //   INPUT (historyEnabled=false): no-op
  //   ENTRY (editor prompt, not yet recalled): if the buffer is all digits,
  //     jump to EDIT mode on that program line
  //   EDIT  (a line is currently under edit): commit the current buffer,
  //     then move to the previous/next program line; past the boundary
  //     exits EDIT mode

  if (c == 11)   // UP (FCTN+E)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to previous line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx > 0)
    {
      printChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx - 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  if (c == 10)   // DOWN (FCTN+X)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to next line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx < em.programSize() - 1)
    {
      printChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx + 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  return EDIT_CONTINUE;
}

static bool getInputLine(char* buf, int bufSize)
{
  LineEdit s = { buf, bufSize - 1, 0, 0, cursorCol, cursorRow, false };
  buf[0] = '\0';

  bool cursorVisible = false;
  unsigned long lastBlink = 0;
  const unsigned long BLINK_MS = 400;

  while (true)
  {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_MS)
    {
      cursorVisible = !cursorVisible;
      editSyncCursor(s);
      drawCursor(cursorVisible);
      lastBlink = now;
    }

    bleKbTask();

    int c;
    while ((c = editorReadChar()) >= 0)
    {
      if (cursorVisible)
      {
        editSyncCursor(s);
        drawCursor(false);
        cursorVisible = false;
      }

      EditResult r = processEditChar((uint8_t)c, s);
      if (r == EDIT_SUBMITTED) return true;
      if (r == EDIT_BROKEN)    return false;   // INPUT aborted
    }
    yield();
  }
}

// ---------------------------------------------------------------------------
// Editor prompt (non-blocking, called from loop)
// ---------------------------------------------------------------------------
static bool editorCursorVisible = false;
static unsigned long editorLastBlink = 0;
static const unsigned long EDITOR_BLINK_MS = 400;

static LineEdit editorState = { inputBuf, MAX_INPUT_LEN, 0, 0, 0, 0, true };
static bool editorLineActive = false;   // true between start-of-line and Enter

static void editorBeginLine()
{
  editorState.buf = inputBuf;
  editorState.maxLen = MAX_INPUT_LEN;
  editorState.len = 0;
  editorState.pos = 0;
  editorState.startCol = cursorCol;
  editorState.startRow = cursorRow;
  editorState.historyEnabled = true;
  inputBuf[0] = '\0';
  inputPos = 0;
  editorLineActive = true;

  // NUMBER mode: pre-fill with the next auto line number + space
  if (numModeActive)
  {
    char numStr[8];
    int n = snprintf(numStr, sizeof(numStr), "%d ", numModeNext);
    for (int i = 0; i < n; i++)
    {
      editTypeChar(editorState, (uint8_t)numStr[i]);
    }
    numModeNext += numModeIncr;
  }
}

// True if the buffer is exactly "<digits> " (at least one trailing space)
// with nothing else — the NUMBER-mode auto-fill form. Lets us detect
// "Enter without adding anything" so we can exit NUMBER mode cleanly
// instead of deleting the line.
static bool editorBufferIsAutoFillOnly(const LineEdit& s)
{
  if (s.len == 0) return false;
  int p = 0;
  if (!isdigit((unsigned char)s.buf[p])) return false;
  while (p < s.len && isdigit((unsigned char)s.buf[p])) p++;
  if (p >= s.len || s.buf[p] != ' ') return false;
  while (p < s.len && s.buf[p] == ' ') p++;
  return p >= s.len;
}

static void editorCursorTick()
{
  if (!editorLineActive) return;
  unsigned long now = millis();
  if (now - editorLastBlink >= EDITOR_BLINK_MS)
  {
    editorCursorVisible = !editorCursorVisible;
    editSyncCursor(editorState);
    drawCursor(editorCursorVisible);
    editorLastBlink = now;
  }
}

static void checkInput()
{
  if (!editorLineActive)
  {
    editorBeginLine();
  }

  editorCursorTick();

  int c;
  while ((c = editorReadChar()) >= 0)
  {
    if (editorCursorVisible)
    {
      editSyncCursor(editorState);
      drawCursor(false);
      editorCursorVisible = false;
    }

    EditResult r = processEditChar((uint8_t)c, editorState);
    inputPos = editorState.len;

    if (r == EDIT_SUBMITTED)
    {
      inputReady = true;
      editorLineActive = false;
      return;
    }
    if (r == EDIT_BROKEN)
    {
      // No running program at the prompt — CLEAR just stays where it is
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// Forward declaration for recursive use by cmdOld
// ---------------------------------------------------------------------------
static void processInput(const char* input);

// ---------------------------------------------------------------------------
// Command callbacks (invoked by Token Parser for immediate commands)
// ---------------------------------------------------------------------------

static void cmdNew()
{
  em.clearProgram();
  clearScreen();
  printLine("** READY **");
  showStatus("NEW program");
}

// LIST [n[-m]] / LIST n- / LIST -m / LIST  (full)
// startLine == 0 → from beginning, endLine == -1 → to end.
static void cmdList(int startLine, int endLine)
{
  char buf[256];
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* line = em.getLine(i);
    if (startLine > 0 && line->lineNum < startLine) continue;
    if (endLine   > 0 && line->lineNum > endLine)   break;
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    printLine(buf);
  }
}

static void cmdRun()
{
  em.run();
}

static void cmdBye()
{
  clearScreen();
  printLine("** GOODBYE **");
  delay(500);
  ESP.restart();
}

static void cmdSave(const char* filename)
{
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  File f = LittleFS.open(path, "w");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  char buf[256];
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* line = em.getLine(i);
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    f.println(buf);
  }
  f.close();

  char msg[48];
  snprintf(msg, sizeof(msg), "SAVED: %s", path);
  printLine(msg);
}

static void cmdOld(const char* filename)
{
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  File f = LittleFS.open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  em.clearProgram();
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
    }
  }
  f.close();

  char msg[48];
  snprintf(msg, sizeof(msg), "LOADED: %s", path);
  printLine(msg);
}

// MERGE "filename" — load a text-format program from LittleFS and fold
// its lines into the current program. Line-number collisions overwrite
// (matching TI's MERGE behavior). Unlike OLD, the existing program
// is NOT cleared first.
static void cmdMerge(const char* filename)
{
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  File f = LittleFS.open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  int merged = 0;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
      merged++;
    }
  }
  f.close();

  char msg[48];
  snprintf(msg, sizeof(msg), "MERGED: %s (%d lines)", path, merged);
  printLine(msg);
}

static void cmdDir()
{
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  if (!f)
  {
    printLine("  (no files)");
  }
  while (f)
  {
    char buf[48];
    snprintf(buf, sizeof(buf), "  %-20s %d", f.name(), f.size());
    printLine(buf);
    f = root.openNextFile();
  }
}

// SIZE — print free memory, TI-style. Real TI reported stack + program
// space separately; we approximate with free heap (for stack/vars) and
// bytes remaining in the tokenized-program buffer (for program).
static void cmdSize()
{
  char buf[48];
  int programUsed = 0;
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* pl = em.getLine(i);
    if (pl) programUsed += pl->length + 4;    // rough per-line overhead
  }
  int programFree = (MAX_LINES * 140) - programUsed;  // approximate
  if (programFree < 0) programFree = 0;

  snprintf(buf, sizeof(buf), "%d BYTES OF STACK FREE",
           (int)ESP.getFreeHeap());
  printLine(buf);
  snprintf(buf, sizeof(buf), "%d BYTES OF PROGRAM SPACE FREE", programFree);
  printLine(buf);
}

// TRACE / UNTRACE — forward to the execution manager so it can print
// line numbers as they execute.
static void cmdTrace(bool enable)
{
  em.setTrace(enable);
}

// Breakpoint list — stored in exec_manager. cmdBreak is called with
// add=true for BREAK and add=false for UNBREAK. A zero-length list
// means "all" (BREAK alone does nothing at prompt, UNBREAK alone clears).
static void cmdBreak(const int* lines, int count, bool add)
{
  if (count == 0)
  {
    if (!add) em.clearBreakpoints();
    return;
  }
  for (int i = 0; i < count; i++)
  {
    if (add) em.addBreakpoint(lines[i]);
    else     em.removeBreakpoint(lines[i]);
  }
}

// NUMBER [start, increment] — enters auto-line-number mode. Handled by
// the editor loop; we just set the state flags and first line number.
static void cmdNumber(int startLine, int increment)
{
  numModeStart = startLine;
  numModeIncr  = (increment > 0) ? increment : 10;
  numModeNext  = startLine;
  numModeActive = true;
}

// RESEQUENCE [start, increment] — renumber program lines. Also updates
// GOTO / GOSUB / THEN / ELSE line-number references inside each line.
static void cmdResequence(int startLine, int increment)
{
  if (increment <= 0) increment = 10;
  if (startLine <= 0) startLine = 100;

  int n = em.programSize();
  if (n == 0) return;

  // Build oldLineNum → newLineNum mapping
  static uint16_t mapOld[MAX_LINES];
  static uint16_t mapNew[MAX_LINES];
  for (int i = 0; i < n; i++)
  {
    mapOld[i] = em.getLine(i)->lineNum;
    mapNew[i] = (uint16_t)(startLine + i * increment);
  }

  // Rewrite each line's tokens: replace old line-number references with
  // the new numbers (as ASCII digits in TOK_UNQUOTED_STR form). Tokens
  // following GOTO/GOSUB/THEN/ELSE/RESTORE hold line numbers.
  for (int i = 0; i < n; i++)
  {
    ProgramLine* pl = em.getLine(i);
    uint8_t newToks[MAX_LINE_TOKENS];
    int outPos = 0;
    int p = 0;
    bool expectLineNum = false;
    while (p < pl->length && pl->tokens[p] != TOK_EOL)
    {
      uint8_t t = pl->tokens[p];
      if (t == TOK_GOTO || t == TOK_GOSUB || t == TOK_THEN ||
          t == TOK_ELSE || t == TOK_RESTORE)
      {
        expectLineNum = true;
        newToks[outPos++] = t;
        p++;
        continue;
      }
      if (expectLineNum && t == TOK_UNQUOTED_STR)
      {
        uint8_t slen = pl->tokens[p + 1];
        char num[8];
        int copyLen = (slen < 7) ? slen : 7;
        memcpy(num, &pl->tokens[p + 2], copyLen);
        num[copyLen] = '\0';
        int oldNum = atoi(num);
        int newNum = oldNum;
        for (int j = 0; j < n; j++)
        {
          if (mapOld[j] == oldNum) { newNum = mapNew[j]; break; }
        }
        char buf[8];
        int nLen = snprintf(buf, sizeof(buf), "%d", newNum);
        newToks[outPos++] = TOK_UNQUOTED_STR;
        newToks[outPos++] = (uint8_t)nLen;
        memcpy(&newToks[outPos], buf, nLen);
        outPos += nLen;
        p += 2 + slen;
        expectLineNum = false;
        continue;
      }
      // Comma after GOTO/GOSUB inside ON ... GOTO list: keep expectLineNum
      if (expectLineNum && t == TOK_COMMA)
      {
        newToks[outPos++] = t;
        p++;
        continue;
      }
      expectLineNum = false;
      newToks[outPos++] = t;
      p++;
    }
    newToks[outPos++] = TOK_EOL;
    memcpy(pl->tokens, newToks, outPos);
    pl->length = outPos;
  }

  // Finally, rewrite the line numbers themselves
  for (int i = 0; i < n; i++)
  {
    em.getLine(i)->lineNum = mapNew[i];
  }
}

// --- File I/O shims (wired into TokenParser via setFileCallbacks) ---
//
// The file_io.h layer routes OPEN/CLOSE/PRINT#/INPUT#/EOF() to either
// LittleFS (FLASH./SDCARD.) or a mounted V9T9 disk image (DSKn.).
// Display-agnostic — same shims work on both the RGB-panel and OTG
// builds.
static int shimFileOpen(int unit, const char* spec, int mode,
                        int flags, int recLen)
{
  return fio::openFile(unit, spec, (fio::Mode)mode, flags, recLen);
}
static int shimFileClose(int unit) { return fio::closeFile(unit); }
static int shimFilePrint(int unit, const char* text)
{
  return fio::printLineTo(unit, text);
}
static int shimFileReadLine(int unit, char* buf, int bufSize)
{
  return fio::readLineFrom(unit, buf, bufSize);
}
static bool shimFileEof(int unit) { return fio::isEof(unit); }
static bool shimFileSeekRec(int unit, long rec)
{
  return fio::seekRecord(unit, rec);
}
static bool shimFileRewind(int unit)
{
  return fio::rewindFile(unit);
}

// --- Sprite stub callbacks ---
//
// The OTG dev board's small ST7789 LCD doesn't render sprites yet
// (TODO: implement a per-pixel sprite layer for this display). For
// now these are no-ops so CALL SPRITE / CALL MOTION / CALL POSITION
// etc. parse and update the sprite state without drawing anything.
// CALL POSITION still returns coherent snapshot values.
static void spriteStubDraw(int slot)  { (void)slot; }
static void spriteStubErase(int slot) { (void)slot; }

// --- CALL JOYST callback ---
static void readJoystick(int unit, int* outX, int* outY)
{
  *outX = bleGpJoystickX(unit);
  *outY = bleGpJoystickY(unit);
}

// --- cmdContinue / cmdDelete stubs to round out the command set ---
static void cmdContinue() { em.cont(); }

static void cmdDelete(const char* filename)
{
  if (!filename || !filename[0])
  {
    printError("* BAD FILE NAME");
    return;
  }
  // FLASH-only delete for the OTG variant. The full main-build
  // version handles SDCARD. and DSKn. prefixes too.
  char path[48];
  if (filename[0] == '/')
  {
    snprintf(path, sizeof(path), "%s", filename);
  }
  else
  {
    snprintf(path, sizeof(path), "/%s.bas", filename);
  }
  if (LittleFS.remove(path))
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "DELETED: %s", path);
    printLine(msg);
  }
  else
  {
    printError("* FILE ERROR");
  }
}

// ---------------------------------------------------------------------------
// Command processor (handles typed input)
// ---------------------------------------------------------------------------

static void processInput(const char* input)
{
  int pos = 0;
  while (input[pos] == ' ')
  {
    pos++;
  }

  if (input[pos] == '\0')
  {
    return;
  }

  // Pre-tokenize commands — string-matched immediate commands
  // These don't get tokens; they're handled directly.
  if (strncasecmp(&input[pos], "NEW", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdNew();
    return;
  }
  if (strncasecmp(&input[pos], "RUN", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdRun();
    return;
  }
  if (strncasecmp(&input[pos], "DIR", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdDir();
    return;
  }

  // Numbered line — store in program
  if (isdigit(input[pos]))
  {
    char* endp;
    uint16_t lineNum = (uint16_t)strtol(&input[pos], &endp, 10);
    pos = endp - input;

    while (input[pos] == ' ')
    {
      pos++;
    }

    // Just a line number — delete line
    if (input[pos] == '\0')
    {
      em.deleteLine(lineNum);
      return;
    }

    // Tokenize and store
    uint8_t tokens[MAX_LINE_TOKENS];
    int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
    if (len < 0)
    {
      printError("* SYNTAX ERROR");
      return;
    }

    if (!em.storeLine(lineNum, tokens, len))
    {
      printError("* MEMORY FULL");
      return;
    }

    char status[40];
    snprintf(status, sizeof(status), "Lines: %d  Free: %dK",
             em.programSize(), (int)(ESP.getFreeHeap() / 1024));
    showStatus(status);
    return;
  }

  // Tokenize and execute immediately through the TP
  uint8_t tokens[MAX_LINE_TOKENS];
  int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
  if (len < 0)
  {
    printLine("* SYNTAX ERROR");
    return;
  }

  em.runImmediate(tokens, len);
}

// ---------------------------------------------------------------------------
// Setup and main loop
// ---------------------------------------------------------------------------

void setup()
{
  // Bump the RX buffer up from the 256-byte default so long program
  // pastes aren't dropped before the editor can drain them.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);

  // Initialize LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed!");
  }
  else
  {
    Serial.println("LittleFS mounted.");
  }

  initDisplay();
  initCharPatterns();
  gfxResetColors();

  // Initialize framebuffer
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  clearScreen();

  // Bring up BLE HID keyboard input BEFORE the boot screen so the scan
  // can start reconnecting while the user is still on the splash screens.
  // (F12 or BOOT button = pairing mode.)
  bleKbInit();

  // Show TI boot screen and wait for a key
  showBootScreen();

  // Cursor starts at bottom row (TI behavior)
  cursorRow = ROWS - 1;
  cursorCol = 0;
  printLine("* READY *");
  printString(">");

  // Connect display callbacks to the Token Parser
  em.tp()->setCallbacks(printChar, printString, clearScreen);
  em.tp()->setCommandCallbacks(cmdNew, cmdList, cmdRun, cmdSave,
                               cmdOld, cmdBye, cmdDir);
  em.tp()->setGraphicsCallbacks(gfxSetChar, gfxGetChar,
                                gfxSetScreenColor, gfxSetCharColor,
                                gfxSetCharPattern);
  em.tp()->setReadKey(gfxReadKey);
  em.tp()->setMoveCursor(gfxMoveCursor);
  em.tp()->setGetCharPattern(gfxGetCharPattern);
  em.tp()->setResetCharset(gfxResetCharset);
  em.tp()->setCmdSize(cmdSize);
  em.tp()->setCmdTrace(cmdTrace);
  em.tp()->setCmdBreak(cmdBreak);
  em.tp()->setCmdRes(cmdResequence);
  em.tp()->setCmdNum(cmdNumber);
  em.tp()->setCmdMerge(cmdMerge);
  em.tp()->setCmdDelete(cmdDelete);
  em.tp()->setCmdContinue(cmdContinue);
  em.tp()->setFileCallbacks(shimFileOpen, shimFileClose, shimFilePrint,
                            shimFileReadLine, shimFileEof);
  em.tp()->setFileSeekRec(shimFileSeekRec);
  em.tp()->setFileRewind(shimFileRewind);
  em.tp()->setReadJoystick(readJoystick);
  // Sprite stubs — no rendering on the OTG ST7789 yet, but the
  // language layer needs the callbacks to dispatch CALL SPRITE etc.
  em.tp()->setSpriteCallbacks(spriteStubDraw, spriteStubErase);
  em.tp()->setThrottleCallback([](unsigned long us) {
    em.m_throttleUs = us;
    em.tp()->setThrottleUs(us);
  });
  em.setProgramEnded(gfxReset);
  em.setPrepareInput(gfxPrepareInput);
  em.setPrintLine(printLine);
  em.setPrintError(printError);
  em.setPrintString(printString);
  em.setGetLine(getInputLine);

  char statusBuf[40];
  snprintf(statusBuf, sizeof(statusBuf), "TI BASIC Sim | Free: %dK",
           (int)(ESP.getFreeHeap() / 1024));
  showStatus(statusBuf);

  Serial.println("TI Extended BASIC Simulator v0.2");
  Serial.println("Type BASIC commands. Serial input active.");
}

void loop()
{
  pasteDrainSerial();
  bleKbTask();
  checkInput();

  if (inputReady)
  {
    processInput(inputBuf);
    printString(">");
    inputPos = 0;
    inputReady = false;
  }
}
