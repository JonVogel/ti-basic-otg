# TI Extended BASIC Keyword Status

Status legend:
- **Impl** — implementation in place in the simulator
- **Test** — verified working on device with at least one program
- *(blank)* — not yet implemented / not yet tested

---

## Immediate-mode commands

| Keyword        | Impl | Test | Notes                                          |
|----------------|:----:|:----:|------------------------------------------------|
| NEW            |  ✅  |  ✅  | Clears program and screen                      |
| RUN            |  ✅  |  ✅  |                                                |
| LIST           |  ✅  |  ✅  | Full list only; no range support yet           |
| OLD            |  ✅  |  ✅  | Loads from LittleFS as `/NAME.bas`             |
| SAVE           |  ✅  |  ✅  | Saves text form to LittleFS                    |
| MERGE          |  ✅  |      | Fold file into current program; collisions win |
| BYE            |  ✅  |  ✅  | Restarts the ESP32                             |
| DIR            |  ✅  |  ✅  | Our addition; lists LittleFS files             |
| SIZE           |  ✅  |      | Prints free heap + estimated program space     |
| NUMBER / NUM   |  ✅  |      | Auto line-number input mode                    |
| RESEQUENCE/RES |  ✅  |      | Renumbers lines + GOTO/GOSUB/THEN/ELSE targets |
| BREAK          |  ✅  |      | Set breakpoint line list                       |
| UNBREAK        |  ✅  |      | Clear breakpoints                              |
| TRACE          |  ✅  |      | Prints `<lineN>` before each line              |
| UNTRACE        |  ✅  |      |                                                |
| CON/CONTINUE   |      |      | Needs execution-state preservation             |
| DELETE (file)  |      |      | Delete file from storage                       |

## Program control flow

| Keyword        | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| IF / THEN      |  ✅  |  ✅  |                                            |
| ELSE           |  ✅  |  ✅  |                                            |
| FOR / TO       |  ✅  |  ✅  |                                            |
| STEP           |  ✅  |  ✅  |                                            |
| NEXT           |  ✅  |  ✅  |                                            |
| GOTO / GO TO   |  ✅  |  ✅  |                                            |
| GOSUB          |  ✅  |  ✅  |                                            |
| RETURN         |  ✅  |  ✅  |                                            |
| ON ... GOTO    |  ✅  |  ✅  |                                            |
| ON ... GOSUB   |  ✅  |  ✅  |                                            |
| END            |  ✅  |  ✅  |                                            |
| STOP           |  ✅  |  ✅  |                                            |
| REM / !        |  ✅  |  ✅  | `REM` implemented; `!` tail-comment pending |
| ON BREAK       |      |      |                                            |
| ON ERROR       |      |      |                                            |
| ON WARNING     |      |      |                                            |

## Variables, assignment, I/O

| Keyword        | Impl | Test | Notes                                        |
|----------------|:----:|:----:|----------------------------------------------|
| LET (implicit) |  ✅  |  ✅  | Bare `VAR = expr` supported                  |
| DIM            |  ✅  |  ✅  | 1D/2D/3D arrays, numeric and string          |
| OPTION BASE    |  ✅  |  ✅  | 0 or 1                                       |
| PRINT          |  ✅  |  ✅  | `;` `,` `TAB()` supported                    |
| INPUT          |  ✅  |  ✅  |                                              |
| LINPUT         |  ✅  |  ✅  |                                              |
| DISPLAY AT     |  ✅  |  ✅  |                                              |
| ACCEPT AT      |  ✅  |  ✅  |                                              |
| READ           |  ✅  |  ✅  |                                              |
| DATA           |  ✅  |  ✅  |                                              |
| RESTORE        |  ✅  |  ✅  |                                              |
| RANDOMIZE      |  ✅  |  ✅  |                                              |
| DEF            |      |      | User-defined functions                       |
| SUB            |      |      | User-defined subprograms                     |
| SUBEND         |      |      |                                              |
| SUBEXIT        |      |      |                                              |
| OPEN           |      |      | File / device I/O                            |
| CLOSE          |      |      |                                              |
| PRINT #        |      |      |                                              |
| INPUT #        |      |      |                                              |
| LINPUT #       |      |      |                                              |
| RESTORE #      |      |      |                                              |
| DELETE (file)  |      |      |                                              |
| IMAGE          |      |      | Format string for PRINT USING                |
| PRINT USING    |      |      |                                              |
| DISPLAY USING  |      |      |                                              |

## Operators

| Operator       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| `+ - * / ^`    |  ✅  |  ✅  |                                            |
| `=`, `<>`      |  ✅  |  ✅  |                                            |
| `<`, `<=`      |  ✅  |  ✅  |                                            |
| `>`, `>=`      |  ✅  |  ✅  |                                            |
| `AND`, `OR`    |  ✅  |  ✅  |                                            |
| `NOT`          |  ✅  |  ✅  |                                            |
| `&` concat     |  ✅  |  ✅  |                                            |
| `XOR`          |      |      | Extended BASIC only                        |

## String functions

| Function       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| ASC            |  ✅  |  ✅  |                                            |
| CHR$           |  ✅  |  ✅  |                                            |
| LEN            |  ✅  |  ✅  |                                            |
| POS            |  ✅  |  ✅  |                                            |
| SEG$           |  ✅  |  ✅  |                                            |
| STR$           |  ✅  |  ✅  |                                            |
| VAL            |  ✅  |  ✅  |                                            |
| RPT$           |  ✅  |  ✅  |                                            |

## Numeric functions

| Function       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| ABS            |  ✅  |  ✅  |                                            |
| ATN            |  ✅  |  ✅  |                                            |
| COS            |  ✅  |  ✅  |                                            |
| SIN            |  ✅  |  ✅  |                                            |
| TAN            |  ✅  |  ✅  |                                            |
| EXP            |  ✅  |  ✅  |                                            |
| LOG            |  ✅  |  ✅  |                                            |
| INT            |  ✅  |  ✅  |                                            |
| SGN            |  ✅  |  ✅  |                                            |
| SQR            |  ✅  |  ✅  |                                            |
| RND            |  ✅  |  ✅  | Zero-arg form works without parens         |
| PI             |  ✅  |  ✅  | Zero-arg constant                          |
| MAX            |  ✅  |  ✅  | Extended BASIC                             |
| MIN            |  ✅  |  ✅  | Extended BASIC                             |

## CALL subprograms — graphics

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL CLEAR     |  ✅  |  ✅  |                                            |
| CALL SCREEN    |  ✅  |  ✅  |                                            |
| CALL COLOR     |  ✅  |  ✅  |                                            |
| CALL CHAR      |  ✅  |  ✅  | 8-byte hex pattern                         |
| CALL HCHAR     |  ✅  |  ✅  |                                            |
| CALL VCHAR     |  ✅  |  ✅  |                                            |
| CALL GCHAR     |  ✅  |  ✅  |                                            |
| CALL CHARSET   |  ✅  |      | Resets chars 32-127 to ROM defaults        |
| CALL CHARPAT   |  ✅  |      | `CALL CHARPAT(code, A$)` — 16-char hex     |

## CALL subprograms — sprites (Extended BASIC)

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL SPRITE    |      |      | Create sprite                              |
| CALL MOTION    |      |      | Set sprite motion                          |
| CALL POSITION  |      |      | Read sprite position                       |
| CALL LOCATE    |      |      | Relocate sprite                            |
| CALL COINC     |      |      | Sprite coincidence check                   |
| CALL DISTANCE  |      |      | Sprite-to-sprite distance                  |
| CALL DELSPRITE |      |      |                                            |
| CALL MAGNIFY   |      |      |                                            |
| CALL PATTERN   |      |      | Change sprite pattern                      |

## CALL subprograms — I/O & system

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL KEY       |  ✅  |  ✅  | Mode 0; other modes treated same           |
| CALL VERSION   |  ✅  |      | Returns 110                                |
| CALL JOYST     |      |      |                                            |
| CALL SOUND     |      |      |                                            |
| CALL SAY       |      |      | Speech                                     |
| CALL SPGET     |      |      | Speech                                     |
| CALL ERR       |      |      | Error info from ON ERROR                   |
| CALL INIT      |      |      | Memory Expansion init                      |
| CALL LINK      |      |      | Assembly linkage                           |
| CALL LOAD      |      |      | Memory poke                                |
| CALL PEEK      |      |      | Memory read                                |

---

## Editor / environment features

| Feature                            | Impl | Test | Notes                                   |
|------------------------------------|:----:|:----:|-----------------------------------------|
| Line editor — DEL / INS / ERASE    |  ✅  |  ✅  | FCTN+1/2/3                              |
| Line editor — arrows (L/R/U/D)     |  ✅  |  ✅  | FCTN+S/D/E/X                            |
| REDO recall (FCTN+8)               |  ✅  |  ✅  | Recalls last submitted line             |
| Line-number recall (`<N>` + UP/DN) |  ✅  |  ✅  |                                         |
| UP/DOWN browse in EDIT mode        |  ✅  |  ✅  | Commits current line before navigating  |
| CLEAR breaks running program       |  ✅  |  ✅  | FCTN+4 or Ctrl+C or ESC                 |
| BLE keyboard input                 |  ✅  |  ✅  | F12 or BOOT button = pairing            |
| Serial paste                       |  ✅  |  ✅  | 16 KB decoupled buffer                  |
| Title / menu screen                |  ✅  |  ✅  | Color stripes, 3×3 logo, © char         |
| Error format (blank + msg + BEL)   |  ✅  |  ✅  | TI-style "* MSG IN nn"                  |
