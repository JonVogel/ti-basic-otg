/*
 * TI BASIC Interpreter — Execution Manager (EM)
 *
 * Manages program flow by feeding lines to the Token Parser
 * and responding to its flow control return values.
 *
 * Responsibilities:
 *   - Program storage (sorted array of tokenized lines)
 *   - RUN: iterate through lines, dispatch to TP
 *   - Handle GOTO, GOSUB, FOR/NEXT flow control
 *   - Handle BREAK (user interrupt)
 */

#ifndef EXEC_MANAGER_H
#define EXEC_MANAGER_H

#include "tp_types.h"
#include "token_parser.h"
#include <Arduino.h>

// Forward declaration for display output
typedef void (*PrintLineFn)(const char* str);
typedef void (*PrintErrorFn)(const char* str);
typedef void (*PrintStringFn2)(const char* str);
typedef bool (*GetLineFn)(char* buf, int bufSize);
typedef void (*ProgramEndedFn)();
typedef void (*PrepareInputFn)();

class ExecManager
{
public:
  ExecManager() : m_programSize(0), m_printLine(NULL), m_printError(NULL),
                  m_printString(NULL), m_getLine(NULL),
                  m_trace(false), m_breakpointCount(0) {}

  void setPrintLine(PrintLineFn fn) { m_printLine = fn; }
  void setPrintError(PrintErrorFn fn) { m_printError = fn; }
  void setPrintString(PrintStringFn2 fn) { m_printString = fn; }
  void setGetLine(GetLineFn fn) { m_getLine = fn; }
  void setProgramEnded(ProgramEndedFn fn) { m_programEnded = fn; }
  void setPrepareInput(PrepareInputFn fn) { m_prepareInput = fn; }
  TokenParser* tp() { return &m_tp; }

  // --- Tracing (TRACE / UNTRACE) ---
  void setTrace(bool on) { m_trace = on; }

  // --- Breakpoints (BREAK / UNBREAK) ---
  static const int MAX_BREAKPOINTS = 16;
  void addBreakpoint(int lineNum)
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum) return;
    }
    if (m_breakpointCount < MAX_BREAKPOINTS)
    {
      m_breakpoints[m_breakpointCount++] = lineNum;
    }
  }
  void removeBreakpoint(int lineNum)
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum)
      {
        for (int j = i; j < m_breakpointCount - 1; j++)
        {
          m_breakpoints[j] = m_breakpoints[j + 1];
        }
        m_breakpointCount--;
        return;
      }
    }
  }
  void clearBreakpoints() { m_breakpointCount = 0; }
  bool hasBreakpoint(int lineNum) const
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum) return true;
    }
    return false;
  }

  // --- Program storage ---

  int programSize() const { return m_programSize; }

  ProgramLine* getLine(int idx)
  {
    return (idx >= 0 && idx < m_programSize) ? m_program[idx] : NULL;
  }

  // Find index where lineNum is or should be inserted
  int findLineIndex(uint16_t lineNum)
  {
    int lo = 0, hi = m_programSize - 1;
    while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      if (m_program[mid]->lineNum == lineNum)
      {
        return mid;
      }
      if (m_program[mid]->lineNum < lineNum)
      {
        lo = mid + 1;
      }
      else
      {
        hi = mid - 1;
      }
    }
    return lo;
  }

  bool storeLine(uint16_t lineNum, const uint8_t* tokens, int length)
  {
    int idx = findLineIndex(lineNum);

    // Replace existing
    if (idx < m_programSize && m_program[idx]->lineNum == lineNum)
    {
      memcpy(m_program[idx]->tokens, tokens, length);
      m_program[idx]->length = length;
      return true;
    }

    // Insert new
    if (m_programSize >= MAX_LINES)
    {
      return false;
    }

    ProgramLine* line = (ProgramLine*)malloc(sizeof(ProgramLine));
    if (!line)
    {
      return false;
    }

    line->lineNum = lineNum;
    memcpy(line->tokens, tokens, length);
    line->length = length;

    for (int i = m_programSize; i > idx; i--)
    {
      m_program[i] = m_program[i - 1];
    }
    m_program[idx] = line;
    m_programSize++;
    return true;
  }

  bool deleteLine(uint16_t lineNum)
  {
    int idx = findLineIndex(lineNum);
    if (idx >= m_programSize || m_program[idx]->lineNum != lineNum)
    {
      return false;
    }

    free(m_program[idx]);
    for (int i = idx; i < m_programSize - 1; i++)
    {
      m_program[i] = m_program[i + 1];
    }
    m_programSize--;
    return true;
  }

  void clearProgram()
  {
    for (int i = 0; i < m_programSize; i++)
    {
      free(m_program[i]);
      m_program[i] = NULL;
    }
    m_programSize = 0;
  }

  // --- Program execution ---

  // Run a single line of tokens in immediate mode (no line number)
  void runImmediate(const uint8_t* tokens, int length)
  {
    TPResponse resp = m_tp.processLine(tokens, length, 0);

    switch (resp.result)
    {
      case TP_NEED_INPUT:
        if (m_prepareInput)
        {
          m_prepareInput();
        }
        if (m_printString)
        {
          m_printString(resp.prompt);
        }
        {
          char inputBuf[MAX_STR_LEN];
          inputBuf[0] = '\0';
          if (m_getLine)
          {
            m_getLine(inputBuf, sizeof(inputBuf));
          }
          m_tp.provideInputValue(inputBuf);
        }
        break;

      case TP_ERROR:
        if (m_printError)
        {
          char buf[60];
          snprintf(buf, sizeof(buf), "* %s", resp.errorMsg);
          m_printError(buf);
        }
        break;

      default:
        break;
    }

    // Reset graphics after any immediate command (TI behavior)
    if (m_programEnded)
    {
      m_programEnded();
    }
  }

  // --- DATA support (called from TP) ---

  // Reset DATA pointer to start or to first DATA at/after lineNum.
  void resetData(uint16_t lineNum)
  {
    if (lineNum == 0)
    {
      m_dataLineIdx = 0;
    }
    else
    {
      m_dataLineIdx = findLineIndex(lineNum);
    }
    m_dataTokenPos = 0;
    m_inDataStmt = false;
  }

  // Get the next DATA value as text. Returns false if no more data.
  bool nextData(char* buf, int bufSize)
  {
    while (m_dataLineIdx < m_programSize)
    {
      ProgramLine* line = m_program[m_dataLineIdx];

      // Scan for DATA token, then collect values until EOL
      while (m_dataTokenPos < line->length &&
             line->tokens[m_dataTokenPos] != TOK_EOL)
      {
        uint8_t tok = line->tokens[m_dataTokenPos];

        if (!m_inDataStmt)
        {
          if (tok == TOK_DATA)
          {
            m_inDataStmt = true;
            m_dataTokenPos++;
            continue;
          }
          m_dataTokenPos++;
          continue;
        }

        // Inside DATA — collect next value
        if (tok == TOK_COMMA || tok == 0x20)
        {
          m_dataTokenPos++;
          continue;
        }

        if (tok == TOK_COLON || tok == TOK_EOL)
        {
          m_inDataStmt = false;
          m_dataTokenPos++;
          continue;
        }

        if (tok == TOK_QUOTED_STR || tok == TOK_UNQUOTED_STR)
        {
          m_dataTokenPos++;
          int slen = line->tokens[m_dataTokenPos++];
          int copyLen = (slen < bufSize - 1) ? slen : bufSize - 1;
          memcpy(buf, &line->tokens[m_dataTokenPos], copyLen);
          buf[copyLen] = '\0';
          m_dataTokenPos += slen;
          return true;
        }

        // Skip anything else
        m_dataTokenPos++;
      }

      // End of line — move to next
      m_dataLineIdx++;
      m_dataTokenPos = 0;
      m_inDataStmt = false;
    }
    return false;
  }

  // Static callback adapters (for callback registration)
  static ExecManager* s_instance;
  static bool cbNextData(char* buf, int bufSize)
  {
    return s_instance ? s_instance->nextData(buf, bufSize) : false;
  }
  static void cbResetData(uint16_t lineNum)
  {
    if (s_instance) s_instance->resetData(lineNum);
  }

  void run()
  {
    if (m_programSize == 0)
    {
      if (m_printError) m_printError("* NO PROGRAM PRESENT");
      return;
    }

    // Install self-reference for data callbacks
    s_instance = this;
    m_tp.setDataCallbacks(cbNextData, cbResetData);
    resetData(0);  // Start data pointer at beginning

    m_tp.reset();
    m_running = true;
    int lineIdx = 0;

    // Drain any leftover characters in the serial buffer
    while (Serial.available())
    {
      Serial.read();
    }

    while (m_running && lineIdx >= 0 && lineIdx < m_programSize)
    {
      ProgramLine* line = m_program[lineIdx];

      // TRACE: print the line number before executing it
      if (m_trace && m_printString)
      {
        char buf[12];
        snprintf(buf, sizeof(buf), "<%d>", line->lineNum);
        m_printString(buf);
      }

      // BREAKPOINT: stop before executing a line that's in the list
      if (hasBreakpoint(line->lineNum))
      {
        if (m_printLine)
        {
          char buf[40];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* BREAKPOINT AT %d", line->lineNum);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);
        }
        m_running = false;
        break;
      }

      TPResponse resp = m_tp.processLine(
        line->tokens, line->length, line->lineNum);

      lineIdx = handleResponse(resp, lineIdx);

      // Check for user break — ESC (0x1B), Ctrl+C (0x03), or TI CLEAR (12),
      // from either Serial or the BLE keyboard. Use peek on Serial so other
      // keys remain available for CALL KEY; the BLE path sets a flag from
      // the notify callback because bleKbPeek would race with reads.
      bool doBreak = false;
      if (Serial.available())
      {
        int c = Serial.peek();
        if (c == 0x1B || c == 0x03 || c == 12)
        {
          Serial.read();  // consume
          doBreak = true;
        }
      }
      if (bleKbBreakRequested)
      {
        bleKbBreakRequested = false;
        // Also drain the matching byte from the BLE ring buffer so it
        // doesn't later look like a keypress for CALL KEY.
        if (bleKbAvailable())
        {
          int c = bleKbPeek();
          if (c == 0x1B || c == 0x03 || c == 12)
          {
            bleKbRead();
          }
        }
        doBreak = true;
      }
      if (doBreak)
      {
        if (m_printLine)
        {
          m_printLine("");
          char buf[40];
          snprintf(buf, sizeof(buf), "* BREAKPOINT AT %d",
                   line->lineNum);
          m_printLine(buf);
        }
        m_running = false;
      }

      yield();  // Prevent watchdog timeout
    }

    if (!m_running || lineIdx >= m_programSize)
    {
      // Reset graphics (colors, char patterns) to editor defaults.
      // Screen content is preserved — only colors/patterns change.
      if (m_programEnded)
      {
        m_programEnded();
      }
      if (m_printLine)
      {
        m_printLine("");
        m_printLine("* READY *");
      }
    }
  }

private:
  ProgramLine* m_program[MAX_LINES];
  int m_programSize;
  bool m_running = false;

  bool m_trace;
  int  m_breakpoints[MAX_BREAKPOINTS];
  int  m_breakpointCount;

  TokenParser m_tp;
  PrintLineFn m_printLine;
  PrintErrorFn m_printError;
  PrintStringFn2 m_printString;
  GetLineFn m_getLine;
  ProgramEndedFn m_programEnded = NULL;
  PrepareInputFn m_prepareInput = NULL;

  // DATA reading state
  int m_dataLineIdx = 0;
  int m_dataTokenPos = 0;
  bool m_inDataStmt = false;    // true when we've entered a DATA statement

  // Process a TPResponse and return the next line index
  int handleResponse(const TPResponse& resp, int currentIdx)
  {
    switch (resp.result)
    {
      case TP_NEXT_TOKEN:
        // Shouldn't reach here — TP should resolve within processLine
        return currentIdx + 1;

      case TP_NEXT_LINE:
        return currentIdx + 1;

      case TP_GOTO_LINE:
      {
        int idx = findLineIndex(resp.lineNum);
        if (idx < m_programSize && m_program[idx]->lineNum == resp.lineNum)
        {
          return idx;
        }
        if (m_printLine)
        {
          char buf[60];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* LINE NOT FOUND IN %d",
                   m_program[currentIdx]->lineNum);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);
        }
        m_running = false;
        return -1;
      }

      case TP_GOTO_AFTER:
      {
        // Go to the line AFTER the specified line (used by RETURN)
        int idx = findLineIndex(resp.lineNum);
        if (idx < m_programSize && m_program[idx]->lineNum == resp.lineNum)
        {
          return idx + 1;
        }
        if (m_printError) m_printError("* RETURN WITHOUT GOSUB");
        m_running = false;
        return -1;
      }

      case TP_NEXT_LOOP:
      {
        // Find the FOR line and re-feed it to the TP
        int forIdx = findLineIndex(resp.lineNum);
        if (forIdx < m_programSize &&
            m_program[forIdx]->lineNum == resp.lineNum)
        {
          ProgramLine* forLine = m_program[forIdx];
          TPResponse loopResp = m_tp.processLine(
            forLine->tokens, forLine->length,
            forLine->lineNum, true);

          if (loopResp.result == TP_END_LOOP)
          {
            // Find the NEXT line and continue after it
            // The NEXT is at currentIdx, so continue from currentIdx + 1
            return currentIdx + 1;
          }
          else
          {
            // Loop continues — go to the line after the FOR
            return forIdx + 1;
          }
        }
        if (m_printError) m_printError("* FOR-NEXT NESTING");
        m_running = false;
        return -1;
      }

      case TP_END_LOOP:
        // Shouldn't reach here directly — handled in NEXT_LOOP
        return currentIdx + 1;

      case TP_NEED_INPUT:
      {
        // Move cursor to bottom of screen (TI behavior)
        if (m_prepareInput)
        {
          m_prepareInput();
        }
        // Display the prompt
        if (m_printString)
        {
          m_printString(resp.prompt);
        }
        // Get input from user (blocking)
        char inputBuf[MAX_STR_LEN];
        inputBuf[0] = '\0';
        if (m_getLine)
        {
          m_getLine(inputBuf, sizeof(inputBuf));
        }
        // Feed input to TP
        m_tp.provideInputValue(inputBuf);
        return currentIdx + 1;
      }

      case TP_FINISHED:
        m_running = false;
        return -1;

      case TP_ERROR:
        if (m_printLine)
        {
          char buf[80];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* %s IN %d",
                   resp.errorMsg, m_program[currentIdx]->lineNum);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);   // BEL
        }
        m_running = false;
        return -1;

      default:
        return currentIdx + 1;
    }
  }
};

// Static member definition
inline ExecManager* ExecManager::s_instance = NULL;

#endif // EXEC_MANAGER_H
