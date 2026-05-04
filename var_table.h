/*
 * TI BASIC Interpreter — Variable Table
 *
 * Manages numeric and string variables (scalar and arrays).
 * String values are stored in a separate string pool.
 * Arrays up to 3 dimensions.
 */

#ifndef VAR_TABLE_H
#define VAR_TABLE_H

#include "tp_types.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

class VarTable
{
public:
  void clear()
  {
    // Free all string values
    for (int i = 0; i < m_strCount; i++)
    {
      if (m_strings[i])
      {
        free(m_strings[i]);
        m_strings[i] = NULL;
      }
    }
    // Free all array data
    for (int i = 0; i < m_varCount; i++)
    {
      if (m_vars[i].arrayData)
      {
        free(m_vars[i].arrayData);
        m_vars[i].arrayData = NULL;
      }
    }
    m_varCount = 0;
    m_strCount = 0;
  }

  // --- Scalar access ---

  Variable* findNum(const char* name, int nameLen)
  {
    return findOrCreate(name, nameLen, false);
  }

  Variable* findStr(const char* name, int nameLen)
  {
    return findOrCreate(name, nameLen, true);
  }

  float getNum(const char* name, int nameLen)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    return (v && v->dimCount == 0) ? v->numVal : 0;
  }

  void setNum(const char* name, int nameLen, float val)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (v && v->dimCount == 0)
    {
      v->numVal = val;
    }
  }

  const char* getStr(const char* name, int nameLen)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (v && v->dimCount == 0 &&
        v->strIndex >= 0 && v->strIndex < m_strCount)
    {
      return m_strings[v->strIndex] ? m_strings[v->strIndex] : "";
    }
    return "";
  }

  void setStr(const char* name, int nameLen, const char* val)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v || v->dimCount != 0)
    {
      return;
    }

    if (v->strIndex >= 0 && v->strIndex < m_strCount)
    {
      free(m_strings[v->strIndex]);
      m_strings[v->strIndex] = NULL;
    }

    v->strIndex = allocString(val);
  }

  // --- Array access ---

  // Dimension an array. dimSizes are INCLUSIVE (DIM A(10) → dimSize=11).
  // Returns true on success.
  bool dimArray(const char* name, int nameLen, bool isString,
                int nDims, const int* dimSizes)
  {
    if (nDims < 1 || nDims > MAX_ARRAY_DIMS) return false;

    Variable* v = findOrCreate(name, nameLen, isString);
    if (!v) return false;

    // Free previous array data if any
    if (v->arrayData)
    {
      free(v->arrayData);
      v->arrayData = NULL;
    }

    v->dimCount = (uint8_t)nDims;
    int total = 1;
    for (int i = 0; i < nDims; i++)
    {
      v->dimSize[i] = (uint16_t)dimSizes[i];
      total *= dimSizes[i];
    }

    if (isString)
    {
      v->arrayData = calloc(total, sizeof(int));
      // Initialize all strIndex to -1 (empty)
      int* p = (int*)v->arrayData;
      for (int i = 0; i < total; i++) p[i] = -1;
    }
    else
    {
      v->arrayData = calloc(total, sizeof(float));
    }
    return v->arrayData != NULL;
  }

  // Compute flat offset from indices
  static int arrayOffset(Variable* v, const int* indices, int nIndices)
  {
    if (nIndices != v->dimCount) return -1;
    int offset = 0;
    int stride = 1;
    for (int i = v->dimCount - 1; i >= 0; i--)
    {
      if (indices[i] < 0 || indices[i] >= v->dimSize[i]) return -1;
      offset += indices[i] * stride;
      stride *= v->dimSize[i];
    }
    return offset;
  }

  float getArrayNum(const char* name, int nameLen,
                    const int* indices, int nIndices)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (!v || v->dimCount == 0) return 0;
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return 0;
    return ((float*)v->arrayData)[off];
  }

  void setArrayNum(const char* name, int nameLen,
                   const int* indices, int nIndices, float val)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (!v) return;
    // Auto-dimension to default size 11 (DIM A(10)) if not already
    if (v->dimCount == 0)
    {
      int defSize = 11;
      int defDims[MAX_ARRAY_DIMS];
      for (int i = 0; i < nIndices; i++) defDims[i] = defSize;
      dimArray(name, nameLen, false, nIndices, defDims);
    }
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return;
    ((float*)v->arrayData)[off] = val;
  }

  const char* getArrayStr(const char* name, int nameLen,
                          const int* indices, int nIndices)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v || v->dimCount == 0) return "";
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return "";
    int idx = ((int*)v->arrayData)[off];
    if (idx < 0 || idx >= m_strCount) return "";
    return m_strings[idx] ? m_strings[idx] : "";
  }

  void setArrayStr(const char* name, int nameLen,
                   const int* indices, int nIndices, const char* val)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v) return;
    if (v->dimCount == 0)
    {
      int defSize = 11;
      int defDims[MAX_ARRAY_DIMS];
      for (int i = 0; i < nIndices; i++) defDims[i] = defSize;
      dimArray(name, nameLen, true, nIndices, defDims);
    }
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return;

    int* arr = (int*)v->arrayData;
    // Free old string at this slot
    if (arr[off] >= 0 && arr[off] < m_strCount)
    {
      free(m_strings[arr[off]]);
      m_strings[arr[off]] = NULL;
    }
    arr[off] = allocString(val);
  }

  int varCount() const { return m_varCount; }

private:
  Variable m_vars[MAX_VARS];
  int m_varCount = 0;

  char* m_strings[MAX_STRINGS];
  int m_strCount = 0;

  Variable* findOrCreate(const char* name, int nameLen, bool isString)
  {
    for (int i = 0; i < m_varCount; i++)
    {
      if (m_vars[i].isString == isString &&
          (int)strlen(m_vars[i].name) == nameLen &&
          strncasecmp(m_vars[i].name, name, nameLen) == 0)
      {
        return &m_vars[i];
      }
    }

    if (m_varCount >= MAX_VARS) return NULL;

    Variable* v = &m_vars[m_varCount++];
    int copyLen = (nameLen < MAX_VAR_NAME - 1) ? nameLen : MAX_VAR_NAME - 1;
    memcpy(v->name, name, copyLen);
    v->name[copyLen] = '\0';
    for (int i = 0; i < copyLen; i++)
    {
      v->name[i] = toupper(v->name[i]);
    }
    v->isString = isString;
    v->dimCount = 0;
    v->arrayData = NULL;
    v->numVal = 0;
    v->strIndex = -1;
    return v;
  }

  int allocString(const char* val)
  {
    if (m_strCount >= MAX_STRINGS) return -1;
    int idx = m_strCount++;
    int len = strlen(val);
    m_strings[idx] = (char*)malloc(len + 1);
    if (m_strings[idx]) strcpy(m_strings[idx], val);
    return idx;
  }
};

#endif // VAR_TABLE_H
