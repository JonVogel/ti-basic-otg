/*
 * TI BASIC Interpreter — Token Parser (TP)
 *
 * State machine that processes tokens one statement at a time.
 * Called by the Execution Manager with a line of tokens.
 * Returns flow control decisions (NEXT_LINE, GOTO, NEXT_LOOP, etc.)
 *
 * The TP owns the variable table and FOR stack.
 * It calls submodules for expression evaluation, PRINT, etc.
 */

#ifndef TOKEN_PARSER_H
#define TOKEN_PARSER_H

#include "tp_types.h"
#include "var_table.h"
#include "expr_parser.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Forward declaration — display output callback
typedef void (*PrintCharFn)(char c);
typedef void (*PrintStringFn)(const char* str);
typedef void (*ClearScreenFn)();

// Graphics callbacks — for CALL HCHAR, VCHAR, GCHAR, SCREEN, COLOR
typedef void (*SetCharFn)(int row, int col, char ch);
typedef char (*GetCharFn)(int row, int col);
typedef void (*SetScreenColorFn)(int colorIdx);
typedef void (*SetCharColorFn)(int charSet, int fg, int bg);
typedef void (*SetCharPatternFn)(int charCode, const uint8_t* pattern8);
typedef void (*GetCharPatternFn)(int charCode, uint8_t* out8);
typedef void (*ResetCharsetFn)();
typedef int  (*ReadKeyFn)();           // returns 0 = no key, else char code
typedef void (*MoveCursorFn)(int row, int col);

// DATA support: EM scans program for next DATA value.
// nextData fills buf with next data value (as text). Returns false if no more.
typedef bool (*NextDataFn)(char* buf, int bufSize);
// resetData resets to the start of program, or to first DATA at/after lineNum.
typedef void (*ResetDataFn)(uint16_t lineNum);

// Command callbacks — provided by the EM for immediate commands
typedef void (*CmdNewFn)();
typedef void (*CmdListFn)();
typedef void (*CmdRunFn)();
typedef void (*CmdSaveFn)(const char* filename);
typedef void (*CmdOldFn)(const char* filename);
typedef void (*CmdMergeFn)(const char* filename);
typedef void (*CmdByeFn)();
typedef void (*CmdDirFn)();
typedef void (*CmdSizeFn)();
typedef void (*CmdTraceFn)(bool enable);
typedef void (*CmdBreakFn)(const int* lines, int count, bool add);
typedef void (*CmdResFn)(int startLine, int increment);
typedef void (*CmdNumFn)(int startLine, int increment);

class TokenParser
{
public:
  TokenParser()
    : m_expr(&m_vars),
      m_printChar(NULL),
      m_printString(NULL),
      m_clearScreen(NULL)
  {
  }

  // Set display output callbacks
  void setCallbacks(PrintCharFn pc, PrintStringFn ps, ClearScreenFn cs)
  {
    m_printChar = pc;
    m_printString = ps;
    m_clearScreen = cs;
  }

  // Set command callbacks
  void setCommandCallbacks(CmdNewFn n, CmdListFn l, CmdRunFn r,
                           CmdSaveFn s, CmdOldFn o, CmdByeFn b,
                           CmdDirFn d)
  {
    m_cmdNew = n;
    m_cmdList = l;
    m_cmdRun = r;
    m_cmdSave = s;
    m_cmdOld = o;
    m_cmdBye = b;
    m_cmdDir = d;
  }

  // Set graphics callbacks
  void setGraphicsCallbacks(SetCharFn sc, GetCharFn gc,
                            SetScreenColorFn ss, SetCharColorFn cc,
                            SetCharPatternFn cp)
  {
    m_setChar = sc;
    m_getChar = gc;
    m_setScreenColor = ss;
    m_setCharColor = cc;
    m_setCharPattern = cp;
  }

  void setReadKey(ReadKeyFn rk) { m_readKey = rk; }
  void setMoveCursor(MoveCursorFn mc) { m_moveCursor = mc; }
  void setGetCharPattern(GetCharPatternFn fn) { m_getCharPattern = fn; }
  void setResetCharset(ResetCharsetFn fn)     { m_resetCharset   = fn; }

  void setCmdSize(CmdSizeFn f)   { m_cmdSize = f; }
  void setCmdTrace(CmdTraceFn f) { m_cmdTrace = f; }
  void setCmdBreak(CmdBreakFn f) { m_cmdBreak = f; }
  void setCmdRes(CmdResFn f)     { m_cmdRes = f; }
  void setCmdNum(CmdNumFn f)     { m_cmdNum = f; }
  void setCmdMerge(CmdMergeFn f) { m_cmdMerge = f; }
  void setDataCallbacks(NextDataFn nd, ResetDataFn rd)
  {
    m_nextData = nd;
    m_resetData = rd;
  }

  // Reset all state for a new RUN
  void reset()
  {
    m_vars.clear();
    m_forDepth = 0;
    m_gosubDepth = 0;
  }

  // Process a full line of tokens. Returns flow control decision.
  TPResponse processLine(const uint8_t* tokens, int length,
                         uint16_t lineNum, bool isLoopIteration = false)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.lineNum = 0;
    resp.errorMsg[0] = '\0';

    int pos = 0;

    while (pos < length && tokens[pos] != TOK_EOL)
    {
      uint8_t tok = tokens[pos];

      // Statement separator
      if (tok == TOK_COLON)
      {
        pos++;
        continue;
      }

      // Raw ASCII identifier = start of implicit assignment (e.g. A=5).
      // If no '=' follows, it's a syntax error (e.g. user typed "RUB").
      if (isIdentStart(tok))
      {
        if (!execAssignment(tokens, &pos))
        {
          resp.result = TP_ERROR;
          snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
          return resp;
        }
        continue;
      }

      // Dispatch based on statement type
      switch (tok)
      {
        case TOK_PRINT:
          pos++;
          execPrint(tokens, &pos);
          break;

        case TOK_LET:
          pos++;
          if (!execAssignment(tokens, &pos))
          {
            resp.result = TP_ERROR;
            snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
            return resp;
          }
          break;

        case TOK_GOTO:
        {
          pos++;
          resp = execGoto(tokens, &pos);
          return resp;
        }

        case TOK_GOSUB:
        {
          pos++;
          resp = execGosub(tokens, &pos, lineNum);
          return resp;
        }

        case TOK_RETURN:
        {
          pos++;
          resp = execReturn();
          return resp;
        }

        case TOK_IF:
        {
          pos++;
          resp = execIf(tokens, &pos, length);
          if (resp.result == TP_GOTO_LINE)
          {
            return resp;
          }
          // If result is NEXT_LINE but we consumed ELSE, keep processing
          if (resp.result == TP_NEXT_LINE)
          {
            return resp;
          }
          // TP_NEXT_TOKEN means keep processing rest of line (THEN clause)
          break;
        }

        case TOK_FOR:
        {
          pos++;
          resp = execFor(tokens, &pos, lineNum, isLoopIteration);
          if (resp.result != TP_NEXT_TOKEN)
          {
            return resp;
          }
          break;
        }

        case TOK_NEXT:
        {
          pos++;
          resp = execNext(tokens, &pos);
          return resp;
        }

        case TOK_CALL:
        {
          pos++;
          resp = execCall(tokens, &pos);
          if (resp.result != TP_NEXT_TOKEN)
          {
            return resp;
          }
          break;
        }

        case TOK_INPUT:
        {
          pos++;
          resp = execInput(tokens, &pos);
          return resp;
        }

        case TOK_LINPUT:
        {
          pos++;
          resp = execInput(tokens, &pos);  // LINPUT works like INPUT for us
          return resp;
        }

        case TOK_DISPLAY:
        {
          pos++;
          execDisplay(tokens, &pos);
          break;
        }

        case TOK_ACCEPT:
        {
          pos++;
          resp = execAccept(tokens, &pos);
          return resp;
        }

        case TOK_DIM:
        {
          pos++;
          execDim(tokens, &pos);
          break;
        }

        case TOK_READ:
        {
          pos++;
          execRead(tokens, &pos);
          break;
        }

        case TOK_DATA:
        {
          // DATA is passively consumed — scan past to EOL
          while (pos < length && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            pos++;
          }
          break;
        }

        case TOK_RESTORE:
        {
          pos++;
          uint16_t lineNum = 0;
          if (tokens[pos] == TOK_UNQUOTED_STR)
          {
            lineNum = (uint16_t)readNumber(tokens, &pos);
          }
          if (m_resetData) m_resetData(lineNum);
          break;
        }

        case TOK_RANDOMIZE:
        {
          pos++;
          // Optional seed value
          if (tokens[pos] != TOK_EOL && tokens[pos] != TOK_COLON)
          {
            float seed = m_expr.evalNumeric(tokens, &pos);
            randomSeed((unsigned long)seed);
          }
          else
          {
            randomSeed(micros());
          }
          break;
        }

        case TOK_ON:
        {
          pos++;
          resp = execOn(tokens, &pos, lineNum);
          if (resp.result == TP_GOTO_LINE)
          {
            return resp;
          }
          break;
        }

        case TOK_OPTION:
        {
          pos++;
          // OPTION BASE 0 or 1
          if (tokens[pos] == TOK_BASE)
          {
            pos++;
            m_optionBase = (int)m_expr.evalNumeric(tokens, &pos);
          }
          else
          {
            while (pos < length && tokens[pos] != TOK_EOL &&
                   tokens[pos] != TOK_COLON)
            {
              pos++;
            }
          }
          break;
        }

        case TOK_LIST:
          if (m_cmdList) m_cmdList();
          return resp;

        case TOK_BYE:
          if (m_cmdBye) m_cmdBye();
          return resp;

        case TOK_SAVE:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdSave) m_cmdSave(filename);
          return resp;
        }

        case TOK_OLD:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdOld) m_cmdOld(filename);
          return resp;
        }

        case TOK_MERGE:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdMerge) m_cmdMerge(filename);
          return resp;
        }

        case TOK_SIZE:
          if (m_cmdSize) m_cmdSize();
          return resp;

        case TOK_TRACE:
          if (m_cmdTrace) m_cmdTrace(true);
          return resp;

        case TOK_UNTRACE:
          if (m_cmdTrace) m_cmdTrace(false);
          return resp;

        case TOK_BREAK:
        case TOK_UNBREAK:
        {
          pos++;
          int lines[16];
          int count = 0;
          while (count < 16 && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            if (tokens[pos] == TOK_UNQUOTED_STR)
            {
              lines[count++] = (int)readNumber(tokens, &pos);
            }
            else
            {
              pos++;
            }
            if (tokens[pos] == TOK_COMMA) pos++;
          }
          if (m_cmdBreak) m_cmdBreak(lines, count, tok == TOK_BREAK);
          return resp;
        }

        case TOK_RES:
        case TOK_NUM:
        {
          pos++;
          int args[2] = {100, 10};
          int n = 0;
          while (n < 2 && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            if (tokens[pos] == TOK_UNQUOTED_STR)
            {
              args[n++] = (int)readNumber(tokens, &pos);
            }
            else
            {
              pos++;
            }
            if (tokens[pos] == TOK_COMMA) pos++;
          }
          if (tok == TOK_RES)
          {
            if (m_cmdRes) m_cmdRes(args[0], args[1]);
          }
          else
          {
            if (m_cmdNum) m_cmdNum(args[0], args[1]);
          }
          return resp;
        }

        case TOK_REM:
          // Skip rest of line
          return resp;  // NEXT_LINE

        case TOK_END:
        case TOK_STOP:
          resp.result = TP_FINISHED;
          return resp;

        default:
          // Unknown token — skip
          pos++;
          break;
      }
    }

    return resp;  // NEXT_LINE
  }

  VarTable* vars() { return &m_vars; }

  // Called by EM after collecting INPUT from user
  void provideInputValue(const char* input)
  {
    provideInput(input);
  }

private:
  VarTable m_vars;
  ExprParser m_expr;

  ForFrame m_forStack[MAX_FOR_DEPTH];
  int m_forDepth = 0;

  // Pending INPUT state
  const uint8_t* m_inputTokens = NULL;
  int m_inputVarStart = 0;

  GosubFrame m_gosubStack[MAX_GOSUB_DEPTH];
  int m_gosubDepth = 0;

  int m_optionBase = 0;  // OPTION BASE 0 or 1 (affects DIM)

  PrintCharFn m_printChar;
  PrintStringFn m_printString;
  ClearScreenFn m_clearScreen;

  CmdNewFn m_cmdNew = NULL;
  CmdListFn m_cmdList = NULL;
  CmdRunFn m_cmdRun = NULL;
  CmdSaveFn m_cmdSave = NULL;
  CmdOldFn m_cmdOld = NULL;
  CmdByeFn m_cmdBye = NULL;
  CmdDirFn m_cmdDir = NULL;
  CmdSizeFn m_cmdSize = NULL;
  CmdTraceFn m_cmdTrace = NULL;
  CmdBreakFn m_cmdBreak = NULL;
  CmdResFn m_cmdRes = NULL;
  CmdNumFn m_cmdNum = NULL;
  CmdMergeFn m_cmdMerge = NULL;

  SetCharFn m_setChar = NULL;
  GetCharFn m_getChar = NULL;
  SetScreenColorFn m_setScreenColor = NULL;
  SetCharColorFn m_setCharColor = NULL;
  SetCharPatternFn m_setCharPattern = NULL;
  GetCharPatternFn m_getCharPattern = NULL;
  ResetCharsetFn   m_resetCharset   = NULL;
  ReadKeyFn m_readKey = NULL;
  MoveCursorFn m_moveCursor = NULL;
  NextDataFn m_nextData = NULL;
  ResetDataFn m_resetData = NULL;

  // Extract filename from tokens (string literal or identifier)
  void extractFilename(const uint8_t* tokens, int* pos, char* buf, int bufSize)
  {
    if (tokens[*pos] == TOK_QUOTED_STR || tokens[*pos] == TOK_UNQUOTED_STR)
    {
      (*pos)++;
      int slen = tokens[*pos];
      (*pos)++;
      int copyLen = (slen < bufSize - 1) ? slen : bufSize - 1;
      memcpy(buf, &tokens[*pos], copyLen);
      buf[copyLen] = '\0';
      *pos += slen;
    }
    else if (isIdentStart(tokens[*pos]))
    {
      bool isStr;
      parseIdent(tokens, pos, buf, bufSize, &isStr);
    }
  }

  // Current cursor column (for PRINT comma tab stops)
  int m_printCol = 0;

  // Check if tokens at pos start a string expression (string var or func)
  bool isStringExprStart(const uint8_t* tokens, int pos)
  {
    if (!isIdentStart(tokens[pos])) return false;
    char name[16];
    peekIdent(tokens, pos, name, sizeof(name));
    int nameLen = strlen(name);
    // Ends with $ → string variable or string function
    if (nameLen > 0 && name[nameLen - 1] == '$') return true;
    return false;
  }

  // --- Statement handlers ---

  void execPrint(const uint8_t* tokens, int* pos, bool addNewline = true)
  {
    bool needNewline = true;

    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      uint8_t tok = tokens[*pos];

      // TAB(n) — move cursor to column n (1-based)
      if (isIdentStart(tok))
      {
        char fname[8];
        int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
        if (strcasecmp(fname, "TAB") == 0 &&
            tokens[*pos + peekLen] == TOK_LPAREN)
        {
          *pos += peekLen + 1;
          int tabCol = (int)m_expr.evalNumeric(tokens, pos) - 1;
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          while (m_printCol < tabCol)
          {
            if (m_printChar) m_printChar(' ');
            m_printCol++;
          }
          needNewline = false;
          continue;
        }
      }

      if (tok == TOK_SEMICOLON)
      {
        (*pos)++;
        needNewline = false;
      }
      else if (tok == TOK_COMMA)
      {
        (*pos)++;
        int nextTab = ((m_printCol / 14) + 1) * 14;
        while (m_printCol < nextTab)
        {
          if (m_printChar)
          {
            m_printChar(' ');
          }
          m_printCol++;
        }
        needNewline = false;
      }
      else if (tok == TOK_QUOTED_STR || isStringExprStart(tokens, *pos))
      {
        // String literal or identifier that's a string var/function
        char strBuf[MAX_STR_LEN];
        m_expr.evalString(tokens, pos, strBuf, sizeof(strBuf));
        if (m_printString)
        {
          m_printString(strBuf);
        }
        m_printCol += strlen(strBuf);
        needNewline = true;
      }
      else if (tok == TOK_UNQUOTED_STR || isIdentStart(tok) ||
               tok == TOK_LPAREN || tok == TOK_MINUS)
      {
        float val = m_expr.evalNumeric(tokens, pos);
        char buf[20];
        // TI BASIC: leading space for positive (sign placeholder) + trailing space
        if (val >= 0)
        {
          snprintf(buf, sizeof(buf), " %g ", val);
        }
        else
        {
          snprintf(buf, sizeof(buf), "%g ", val);
        }
        if (m_printString)
        {
          m_printString(buf);
        }
        m_printCol += strlen(buf);
        needNewline = true;
      }
      else
      {
        (*pos)++;
        needNewline = true;
      }
    }

    if (needNewline && addNewline)
    {
      if (m_printChar)
      {
        m_printChar('\n');
      }
      m_printCol = 0;
    }
  }

  // --- String expression evaluator ---

  // String expression evaluation moved to ExprParser (m_expr.evalString)

  // Returns true on success, false on syntax error (no '=' after identifier).
  bool execAssignment(const uint8_t* tokens, int* pos)
  {
    char vname[MAX_VAR_NAME];
    bool isStr;
    int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

    // Array assignment: A(i)=... or A(i,j)=...
    if (tokens[*pos] == TOK_LPAREN)
    {
      (*pos)++;
      int indices[MAX_ARRAY_DIMS];
      int nIdx = 0;
      while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
             tokens[*pos] != TOK_EOL)
      {
        indices[nIdx++] = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      if (tokens[*pos] != TOK_EQUAL)
      {
        return false;
      }
      (*pos)++;
      if (isStr)
      {
        char buf[MAX_STR_LEN];
        m_expr.evalString(tokens, pos, buf, sizeof(buf));
        m_vars.setArrayStr(vname, vlen, indices, nIdx, buf);
      }
      else
      {
        float val = m_expr.evalNumeric(tokens, pos);
        m_vars.setArrayNum(vname, vlen, indices, nIdx, val);
      }
      return true;
    }

    // Scalar assignment
    if (tokens[*pos] != TOK_EQUAL)
    {
      return false;
    }
    (*pos)++;
    if (isStr)
    {
      char buf[MAX_STR_LEN];
      m_expr.evalString(tokens, pos, buf, sizeof(buf));
      m_vars.setStr(vname, vlen, buf);
    }
    else
    {
      float val = m_expr.evalNumeric(tokens, pos);
      m_vars.setNum(vname, vlen, val);
    }
    return true;
  }

  // READ var1,var2,... — pull values from DATA statements
  void execRead(const uint8_t* tokens, int* pos)
  {
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      if (!isIdentStart(tokens[*pos]))
      {
        (*pos)++;
        continue;
      }

      char vname[MAX_VAR_NAME];
      bool isStr;
      int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

      // Array target READ A(i)
      int indices[MAX_ARRAY_DIMS];
      int nIdx = 0;
      bool isArray = false;
      if (tokens[*pos] == TOK_LPAREN)
      {
        isArray = true;
        (*pos)++;
        while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
               tokens[*pos] != TOK_EOL)
        {
          indices[nIdx++] = (int)m_expr.evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      }

      // Get next data value
      char dataBuf[MAX_STR_LEN];
      if (!m_nextData || !m_nextData(dataBuf, sizeof(dataBuf)))
      {
        return;  // out of data
      }

      if (isStr)
      {
        if (isArray)
          m_vars.setArrayStr(vname, vlen, indices, nIdx, dataBuf);
        else
          m_vars.setStr(vname, vlen, dataBuf);
      }
      else
      {
        float val = strtof(dataBuf, NULL);
        if (isArray)
          m_vars.setArrayNum(vname, vlen, indices, nIdx, val);
        else
          m_vars.setNum(vname, vlen, val);
      }

      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
  }

  // DISPLAY [AT(row,col)[:]] item [;item...]
  // Extended BASIC: like PRINT but with optional position.
  void execDisplay(const uint8_t* tokens, int* pos)
  {
    // Check for AT(row, col)
    if (isIdentStart(tokens[*pos]))
    {
      char fname[8];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      if (strcasecmp(fname, "AT") == 0 &&
          tokens[*pos + peekLen] == TOK_LPAREN)
      {
        *pos += peekLen + 1;   // past AT and (
        int row = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int col = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        // Position cursor (1-based → 0-based)
        if (m_moveCursor) m_moveCursor(row - 1, col - 1);

        // Skip separator (:, ;) before the actual output
        if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
        {
          (*pos)++;
        }
      }
    }

    // Print the items — DISPLAY AT does NOT add a newline (no scroll)
    execPrint(tokens, pos, false);
  }

  // ACCEPT [AT(row,col)[:]] var
  TPResponse execAccept(const uint8_t* tokens, int* pos)
  {
    // Check for AT(row, col)
    if (isIdentStart(tokens[*pos]))
    {
      char fname[8];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      if (strcasecmp(fname, "AT") == 0 &&
          tokens[*pos + peekLen] == TOK_LPAREN)
      {
        *pos += peekLen + 1;
        int row = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int col = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        if (m_moveCursor) m_moveCursor(row - 1, col - 1);

        if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
        {
          (*pos)++;
        }
      }
    }

    // Rest works like INPUT (no prompt)
    TPResponse resp;
    resp.result = TP_NEED_INPUT;
    resp.errorMsg[0] = '\0';
    resp.prompt[0] = '\0';    // no prompt for ACCEPT

    m_inputTokens = tokens;
    m_inputVarStart = *pos;
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      (*pos)++;
    }
    return resp;
  }

  // ON expr GOTO line1,line2,line3  or  ON expr GOSUB line1,line2,line3
  TPResponse execOn(const uint8_t* tokens, int* pos, uint16_t currentLine)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.errorMsg[0] = '\0';

    int index = (int)m_expr.evalNumeric(tokens, pos);

    bool isGosub = false;
    if (tokens[*pos] == TOK_GOSUB)
    {
      isGosub = true;
    }
    (*pos)++;  // past GOTO/GOSUB

    // Walk the line number list. Use the index-th one (1-based).
    int count = 0;
    uint16_t targetLine = 0;
    bool found = false;

    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      count++;
      if (tokens[*pos] == TOK_UNQUOTED_STR)
      {
        float val = readNumber(tokens, pos);
        if (count == index)
        {
          targetLine = (uint16_t)val;
          found = true;
          // Don't break — consume remaining tokens to stay aligned
        }
      }
      else
      {
        (*pos)++;
      }
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }

    if (!found)
    {
      // Index out of range — fall through, continue with next statement
      return resp;
    }

    if (isGosub)
    {
      if (m_gosubDepth >= MAX_GOSUB_DEPTH)
      {
        resp.result = TP_ERROR;
        snprintf(resp.errorMsg, sizeof(resp.errorMsg),
                 "STACK OVERFLOW");
        return resp;
      }
      m_gosubStack[m_gosubDepth++].returnLineNum = currentLine;
    }

    resp.result = TP_GOTO_LINE;
    resp.lineNum = targetLine;
    return resp;
  }

  // DIM A(10) or DIM A(5,10) or DIM A(3,5,2), and string versions
  void execDim(const uint8_t* tokens, int* pos)
  {
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      if (!isIdentStart(tokens[*pos]))
      {
        (*pos)++;
        continue;
      }

      char vname[MAX_VAR_NAME];
      bool isStr;
      int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

      if (tokens[*pos] != TOK_LPAREN) return;
      (*pos)++;

      int dims[MAX_ARRAY_DIMS];
      int nDims = 0;
      while (nDims < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
             tokens[*pos] != TOK_EOL)
      {
        int n = (int)m_expr.evalNumeric(tokens, pos);
        // DIM A(10) creates indices base..10, inclusive.
        // Storage size: n - base + 1 elements.
        // We still allocate n+1 slots for simplicity (index 0 unused if base=1).
        dims[nDims++] = n + 1;
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      m_vars.dimArray(vname, vlen, isStr, nDims, dims);

      // Handle multiple arrays in one DIM: DIM A(10),B(20)
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
  }

  // Read a number from token stream (TOK_UNQUOTED_STR + length + ASCII)
  // Advances pos past the number. Returns 0 if not a number token.
  float readNumber(const uint8_t* tokens, int* pos)
  {
    if (tokens[*pos] != TOK_UNQUOTED_STR)
    {
      return 0;
    }
    (*pos)++;
    int slen = tokens[*pos];
    (*pos)++;
    char buf[32];
    int copyLen = (slen < 31) ? slen : 31;
    memcpy(buf, &tokens[*pos], copyLen);
    buf[copyLen] = '\0';
    *pos += slen;
    return strtof(buf, NULL);
  }

  TPResponse execGoto(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (tokens[*pos] == TOK_UNQUOTED_STR)
    {
      float val = readNumber(tokens, pos);
      resp.result = TP_GOTO_LINE;
      resp.lineNum = (uint16_t)val;
    }
    else
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
    }
    return resp;
  }

  TPResponse execGosub(const uint8_t* tokens, int* pos, uint16_t currentLine)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (m_gosubDepth >= MAX_GOSUB_DEPTH)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "STACK OVERFLOW");
      return resp;
    }

    if (tokens[*pos] == TOK_UNQUOTED_STR)
    {
      float val = readNumber(tokens, pos);

      m_gosubStack[m_gosubDepth++].returnLineNum = currentLine;
      resp.result = TP_GOTO_LINE;
      resp.lineNum = (uint16_t)val;
    }
    else
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
    }
    return resp;
  }

  TPResponse execReturn()
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (m_gosubDepth <= 0)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "RETURN WITHOUT GOSUB");
      return resp;
    }

    // Return to the line AFTER the GOSUB call
    resp.result = TP_GOTO_AFTER;
    resp.lineNum = m_gosubStack[--m_gosubDepth].returnLineNum;
    return resp;
  }

  TPResponse execIf(const uint8_t* tokens, int* pos, int length)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    bool cond = m_expr.evalCondition(tokens, pos);

    if (tokens[*pos] == TOK_THEN)
    {
      (*pos)++;
    }

    if (cond)
    {
      // Check if THEN is followed by a line number (GOTO shorthand)
      if (tokens[*pos] == TOK_NUMBER_LIT)
      {
        return execGoto(tokens, pos);
      }
      // Otherwise, continue processing rest of line (THEN clause)
      resp.result = TP_NEXT_TOKEN;
      return resp;
    }
    else
    {
      // Skip to ELSE or end of line
      int depth = 0;
      while (*pos < length && tokens[*pos] != TOK_EOL)
      {
        if (tokens[*pos] == TOK_ELSE && depth == 0)
        {
          (*pos)++;
          // Check if ELSE is followed by a line number
          if (tokens[*pos] == TOK_NUMBER_LIT)
          {
            return execGoto(tokens, pos);
          }
          resp.result = TP_NEXT_TOKEN;
          return resp;
        }
        (*pos)++;
      }
      resp.result = TP_NEXT_LINE;
      return resp;
    }
  }

  TPResponse execFor(const uint8_t* tokens, int* pos, uint16_t lineNum,
                     bool isIteration)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.errorMsg[0] = '\0';

    if (!isIdentStart(tokens[*pos]))
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
      return resp;
    }

    char vname[MAX_VAR_NAME];
    bool isStr;
    int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

    // Check if this variable is already on the FOR stack
    int stackIdx = findForVar(vname, vlen);

    if (stackIdx >= 0)
    {
      // Iteration — increment and check limit
      ForFrame* f = &m_forStack[stackIdx];
      Variable* v = m_vars.findNum(vname, vlen);
      if (v)
      {
        v->numVal += f->step;

        bool done = (f->step > 0) ? (v->numVal > f->limit)
                                  : (v->numVal < f->limit);
        if (done)
        {
          // Remove this frame and all above it
          m_forDepth = stackIdx;
          resp.result = TP_END_LOOP;
          return resp;
        }
      }
      resp.result = TP_NEXT_LINE;
      return resp;
    }

    // First time — parse FOR var = start TO limit [STEP step]
    float startVal = 0, limitVal = 0, stepVal = 1;

    if (tokens[*pos] == TOK_EQUAL)
    {
      (*pos)++;
      startVal = m_expr.evalNumeric(tokens, pos);
    }
    if (tokens[*pos] == TOK_TO)
    {
      (*pos)++;
      limitVal = m_expr.evalNumeric(tokens, pos);
    }
    if (tokens[*pos] == TOK_STEP)
    {
      (*pos)++;
      stepVal = m_expr.evalNumeric(tokens, pos);
    }

    // Set the loop variable
    m_vars.setNum(vname, vlen, startVal);

    // Push FOR frame
    if (m_forDepth < MAX_FOR_DEPTH)
    {
      ForFrame* f = &m_forStack[m_forDepth++];
      int copyLen = (vlen < MAX_VAR_NAME - 1) ? vlen : MAX_VAR_NAME - 1;
      memcpy(f->varName, vname, copyLen);
      f->varName[copyLen] = '\0';
      for (int i = 0; i < copyLen; i++)
      {
        f->varName[i] = toupper(f->varName[i]);
      }
      f->limit = limitVal;
      f->step = stepVal;
      f->forLineNum = lineNum;
    }

    resp.result = TP_NEXT_LINE;
    return resp;
  }

  TPResponse execNext(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    // Optional variable name after NEXT
    char varName[MAX_VAR_NAME] = "";
    if (isIdentStart(tokens[*pos]))
    {
      bool isStr;
      parseIdent(tokens, pos, varName, sizeof(varName), &isStr);
    }

    if (m_forDepth <= 0)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "NEXT WITHOUT FOR");
      return resp;
    }

    // Find the matching FOR frame (top of stack if no variable specified)
    ForFrame* f = &m_forStack[m_forDepth - 1];

    resp.result = TP_NEXT_LOOP;
    resp.lineNum = f->forLineNum;
    return resp;
  }

  // Helper: read optional argument list "(arg, arg, ...)"
  // Returns number of args read into argv[], each evaluated as numeric.
  int readCallArgs(const uint8_t* tokens, int* pos, float* argv, int maxArgs)
  {
    int count = 0;
    if (tokens[*pos] != TOK_LPAREN) return 0;
    (*pos)++;
    while (count < maxArgs && tokens[*pos] != TOK_RPAREN &&
           tokens[*pos] != TOK_EOL)
    {
      argv[count++] = m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
    if (tokens[*pos] == TOK_RPAREN) (*pos)++;
    return count;
  }

  TPResponse execCall(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    if (!isIdentStart(tokens[*pos]))
    {
      return resp;
    }

    char subName[32];
    bool isStr;
    parseIdent(tokens, pos, subName, sizeof(subName), &isStr);

    if (strcasecmp(subName, "CLEAR") == 0)
    {
      if (m_clearScreen) m_clearScreen();
      return resp;
    }

    if (strcasecmp(subName, "SCREEN") == 0)
    {
      float args[1];
      readCallArgs(tokens, pos, args, 1);
      if (m_setScreenColor) m_setScreenColor((int)args[0]);
      return resp;
    }

    if (strcasecmp(subName, "HCHAR") == 0)
    {
      // CALL HCHAR(row, col, char [, repeat])
      float args[4] = {1, 1, 32, 1};
      int n = readCallArgs(tokens, pos, args, 4);
      if (n < 3) return resp;
      int row = (int)args[0];
      int col = (int)args[1];
      int ch = (int)args[2];
      int rep = (n >= 4) ? (int)args[3] : 1;
      if (m_setChar)
      {
        for (int i = 0; i < rep; i++)
        {
          int r = row - 1;
          int c = col - 1 + i;
          // Wrap to next line if overflow (TI behavior)
          while (c >= 28)
          {
            c -= 28;
            r++;
          }
          if (r >= 0 && r < 24)
          {
            m_setChar(r, c, (char)ch);
          }
        }
      }
      return resp;
    }

    if (strcasecmp(subName, "VCHAR") == 0)
    {
      // CALL VCHAR(row, col, char [, repeat])
      float args[4] = {1, 1, 32, 1};
      int n = readCallArgs(tokens, pos, args, 4);
      if (n < 3) return resp;
      int row = (int)args[0];
      int col = (int)args[1];
      int ch = (int)args[2];
      int rep = (n >= 4) ? (int)args[3] : 1;
      if (m_setChar)
      {
        for (int i = 0; i < rep; i++)
        {
          int r = row - 1 + i;
          int c = col - 1;
          // Wrap to next column if overflow
          while (r >= 24)
          {
            r -= 24;
            c++;
          }
          if (r >= 0 && r < 24 && c >= 0 && c < 28)
          {
            m_setChar(r, c, (char)ch);
          }
        }
      }
      return resp;
    }

    if (strcasecmp(subName, "GCHAR") == 0)
    {
      // CALL GCHAR(row, col, variable)
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int row = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      int col = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      // Third arg is a variable to receive the character code
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        char ch = 32;
        if (m_getChar && row >= 1 && row <= 24 && col >= 1 && col <= 28)
        {
          ch = m_getChar(row - 1, col - 1);
        }
        m_vars.setNum(vname, vlen, (float)(uint8_t)ch);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "CHARSET") == 0)
    {
      // CALL CHARSET — reset chars 32-127 to their ROM default patterns.
      // Leaves user-defined graphics chars (128+) alone.
      if (m_resetCharset) m_resetCharset();
      return resp;
    }

    if (strcasecmp(subName, "CHARPAT") == 0)
    {
      // CALL CHARPAT(char-code, string-var) — read a character's current
      // pattern as a 16-character hex string.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int code = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        if (vIsStr && m_getCharPattern && code >= 0 && code < 256)
        {
          uint8_t pat[8];
          m_getCharPattern(code, pat);
          char hex[17];
          for (int i = 0; i < 8; i++)
          {
            snprintf(&hex[i * 2], 3, "%02X", pat[i]);
          }
          hex[16] = '\0';
          m_vars.setStr(vname, vlen, hex);
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "VERSION") == 0)
    {
      // CALL VERSION(numeric-variable) — returns the Extended BASIC
      // version number (110 = v1.10). Real TI Ext BASIC returns 110
      // since v1.00 was pulled for bugs; we match that.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        m_vars.setNum(vname, vlen, 110.0f);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "COLOR") == 0)
    {
      // CALL COLOR(charset, fg, bg)
      float args[3] = {1, 2, 1};
      int n = readCallArgs(tokens, pos, args, 3);
      if (n < 3) return resp;
      if (m_setCharColor)
      {
        m_setCharColor((int)args[0], (int)args[1], (int)args[2]);
      }
      return resp;
    }

    if (strcasecmp(subName, "KEY") == 0)
    {
      // CALL KEY(mode, key_var, status_var)
      // mode: 0=ignore (always current), 1/2/3=modes (we treat all same)
      // key: char code of pressed key (or -1 if none)
      // status: 1 if new key, 0 if same as last, -1 if no key
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int mode = (int)m_expr.evalNumeric(tokens, pos);
      (void)mode;
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      // Read the key variable name
      char keyVar[MAX_VAR_NAME] = "";
      int keyLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        keyLen = parseIdent(tokens, pos, keyVar, sizeof(keyVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      char stVar[MAX_VAR_NAME] = "";
      int stLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        stLen = parseIdent(tokens, pos, stVar, sizeof(stVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      int key = m_readKey ? m_readKey() : 0;
      int status;
      if (key > 0)
      {
        status = 1;  // new key available
      }
      else
      {
        key = -1;
        status = 0;  // no key
      }

      if (keyLen > 0) m_vars.setNum(keyVar, keyLen, (float)key);
      if (stLen > 0)  m_vars.setNum(stVar, stLen, (float)status);
      return resp;
    }

    if (strcasecmp(subName, "CHAR") == 0)
    {
      // CALL CHAR(char_code, "hex_string")
      // hex_string is 16 hex digits (8 bytes = 8 rows of 8 pixels).
      // Up to 64 digits for 4 consecutive characters.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int charCode = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      char hexBuf[128];
      m_expr.evalString(tokens, pos, hexBuf, sizeof(hexBuf));
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      // Parse hex string into 8-byte pattern, up to 8 chars worth
      int hlen = strlen(hexBuf);
      int numChars = (hlen + 15) / 16;
      for (int c = 0; c < numChars; c++)
      {
        uint8_t pattern[8] = {0};
        for (int b = 0; b < 8; b++)
        {
          int idx = c * 16 + b * 2;
          if (idx + 1 >= hlen) break;
          char hi = hexBuf[idx];
          char lo = hexBuf[idx + 1];
          uint8_t byte = 0;
          if (hi >= '0' && hi <= '9') byte = (hi - '0') << 4;
          else if (hi >= 'A' && hi <= 'F') byte = (hi - 'A' + 10) << 4;
          else if (hi >= 'a' && hi <= 'f') byte = (hi - 'a' + 10) << 4;
          if (lo >= '0' && lo <= '9') byte |= (lo - '0');
          else if (lo >= 'A' && lo <= 'F') byte |= (lo - 'A' + 10);
          else if (lo >= 'a' && lo <= 'f') byte |= (lo - 'a' + 10);
          pattern[b] = byte;
        }
        if (m_setCharPattern)
        {
          m_setCharPattern(charCode + c, pattern);
        }
      }
      return resp;
    }

    resp.result = TP_ERROR;
    snprintf(resp.errorMsg, sizeof(resp.errorMsg),
             "BAD NAME: %s", subName);
    return resp;
  }

  // --- INPUT handling ---
  // INPUT [prompt;] var [, var...]
  TPResponse execInput(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEED_INPUT;
    resp.errorMsg[0] = '\0';
    resp.prompt[0] = '\0';

    // Optional prompt: "text";
    if (tokens[*pos] == TOK_STRING_LIT)
    {
      (*pos)++;
      int slen = tokens[*pos];
      (*pos)++;
      int copyLen = (slen < (int)sizeof(resp.prompt) - 1) ? slen :
                    (int)sizeof(resp.prompt) - 1;
      memcpy(resp.prompt, &tokens[*pos], copyLen);
      resp.prompt[copyLen] = '\0';
      *pos += slen;
      // Expect : or ; separator
      if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
      {
        (*pos)++;
      }
    }
    else
    {
      strcpy(resp.prompt, "? ");
    }

    // Save where the variable list starts (for provideInput)
    m_inputTokens = tokens;
    m_inputVarStart = *pos;

    // Skip to end of statement so EM advances past it
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
    {
      (*pos)++;
    }

    return resp;
  }

  // Called by the EM after user input is received
  void provideInput(const char* input)
  {
    int pos = m_inputVarStart;
    const char* inputPtr = input;

    while (m_inputTokens[pos] != TOK_EOL && m_inputTokens[pos] != TOK_COLON)
    {
      if (m_inputTokens[pos] == TOK_COMMA)
      {
        pos++;
        continue;
      }

      if (isIdentStart(m_inputTokens[pos]))
      {
        char vname[MAX_VAR_NAME];
        bool isStr;
        int vlen = parseIdent(m_inputTokens, &pos, vname,
                              sizeof(vname), &isStr);

        // Extract next value from input (comma-separated)
        char valBuf[MAX_STR_LEN];
        int valLen = 0;
        // Skip leading spaces
        while (*inputPtr == ' ')
        {
          inputPtr++;
        }
        // Read until comma or end
        while (*inputPtr != '\0' && *inputPtr != ',' &&
               valLen < (int)sizeof(valBuf) - 1)
        {
          valBuf[valLen++] = *inputPtr++;
        }
        valBuf[valLen] = '\0';
        // Trim trailing spaces
        while (valLen > 0 && valBuf[valLen - 1] == ' ')
        {
          valBuf[--valLen] = '\0';
        }
        // Skip comma
        if (*inputPtr == ',')
        {
          inputPtr++;
        }

        // Store value
        if (isStr)
        {
          m_vars.setStr(vname, vlen, valBuf);
        }
        else
        {
          m_vars.setNum(vname, vlen, strtof(valBuf, NULL));
        }
      }
      else
      {
        pos++;
      }
    }
  }

  // Find a variable on the FOR stack, returns index or -1
  int findForVar(const char* name, int nameLen)
  {
    for (int i = m_forDepth - 1; i >= 0; i--)
    {
      if ((int)strlen(m_forStack[i].varName) == nameLen &&
          strncasecmp(m_forStack[i].varName, name, nameLen) == 0)
      {
        return i;
      }
    }
    return -1;
  }
};

#endif // TOKEN_PARSER_H
