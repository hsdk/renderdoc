/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2017 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#define DBGHELP_TRANSLATE_TCHAR

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <shlobj.h>
#include <tchar.h>
#include <algorithm>
#include <string>
#include <vector>
#include "dbghelp/dbghelp.h"
#include "os/os_specific.h"
#include "serialise/string_utils.h"

class Win32Callstack : public Callstack::Stackwalk
{
public:
  Win32Callstack();
  Win32Callstack(DWORD64 *calls, size_t numLevels);
  ~Win32Callstack();

  void Set(DWORD64 *calls, size_t numLevels);

  size_t NumLevels() const { return m_AddrStack.size(); }
  const uint64_t *GetAddrs() const { return &m_AddrStack[0]; }
private:
  Win32Callstack(const Callstack::Stackwalk &other);

  void Collect();

  vector<DWORD64> m_AddrStack;
};

class Win32CallstackResolver : public Callstack::StackResolver
{
public:
  Win32CallstackResolver(char *moduleDB, size_t DBSize, string pdbSearchPaths,
                         volatile bool *killSignal);
  ~Win32CallstackResolver();

  Callstack::AddressDetails GetAddr(uint64_t addr);

private:
  // must match definition in pdblocate.cpp
  struct AddrInfo
  {
    wchar_t funcName[127];
    wchar_t fileName[127];
    unsigned long lineNum;

    wchar_t *formattedString(const char *commonPath = NULL);
  };

  wstring pdbBrowse(wstring startingPoint);
  wstring LookupModule(wchar_t *modName, GUID guid, DWORD age);

  void *SendRecvPipeMessage(wstring message);

  void OpenPdblocateHandle();
  uint32_t GetModuleID(wstring pdbName, GUID guid, DWORD age);
  void SetModuleBaseAddress(uint32_t moduleId, DWORD64 base);
  AddrInfo GetAddrInfoForModule(uint32_t moduleId, DWORD64 addr);

  struct Module
  {
    wstring name;
    DWORD64 base;
    DWORD size;

    uint32_t moduleId;
  };

  HANDLE pdblocateProcess;
  HANDLE pdblocatePipe;

  vector<wstring> pdbRememberedPaths;
  vector<wstring> pdbIgnores;
  vector<Module> modules;

  char pipeMessageBuf[2048];
};

///////////////////////////////////////////////////

typedef BOOL(CALLBACK *PSYM_ENUMMODULES_CALLBACK64W)(__in PCWSTR ModuleName, __in DWORD64 BaseOfDll,
                                                     __in_opt PVOID UserContext);

typedef BOOL(WINAPI *PSYMINITIALIZEW)(HANDLE, PCTSTR, BOOL);
typedef BOOL(WINAPI *PSYMREFRESHMODULELIST)(HANDLE);
typedef BOOL(WINAPI *PSYMENUMERATEMODULES64W)(HANDLE, PSYM_ENUMMODULES_CALLBACK64W, PVOID);
typedef BOOL(WINAPI *PSYMGETMODULEINFO64W)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);

PSYMINITIALIZEW dynSymInitializeW = NULL;
PSYMREFRESHMODULELIST dynSymRefreshModuleList = NULL;
PSYMENUMERATEMODULES64W dynSymEnumerateModules64W = NULL;
PSYMGETMODULEINFO64W dynSymGetModuleInfo64W = NULL;

void *renderdocBase = NULL;
uint32_t renderdocSize = 0;

// gives us an address to identify this dll with
static int dllLocator = 0;

static bool InitDbgHelp()
{
  static bool doinit = true;
  static bool ret = false;

  if(!doinit)
    return ret;

  doinit = false;

  HMODULE module = NULL;

  // can't reliably co-exist with dbghelp already being used in the process
  if(GetModuleHandleA("dbghelp.dll") != NULL)
  {
    ret = false;
    return false;
  }
  else
  {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_DLL_FILE) ".dll"), path, MAX_PATH - 1);

    wchar_t *slash = wcsrchr(path, '\\');

    if(slash)
    {
      *slash = 0;
    }
    else
    {
      slash = wcsrchr(path, '/');

      if(slash == 0)
      {
        ret = false;
        return false;
      }

      *slash = 0;
    }

#if ENABLED(RDOC_X64)
    wcscat_s(path, L"/pdblocate/x64/dbghelp.dll");
#else
    wcscat_s(path, L"/pdblocate/x86/dbghelp.dll");
#endif

    module = LoadLibraryW(path);
  }

  if(!module)
  {
    RDCWARN("Couldn't open dbghelp.dll");
    ret = false;
    return false;
  }

  dynSymInitializeW = (PSYMINITIALIZEW)GetProcAddress(module, "SymInitializeW");
  dynSymEnumerateModules64W =
      (PSYMENUMERATEMODULES64W)GetProcAddress(module, "SymEnumerateModulesW64");
  dynSymRefreshModuleList = (PSYMREFRESHMODULELIST)GetProcAddress(module, "SymRefreshModuleList");
  dynSymGetModuleInfo64W = (PSYMGETMODULEINFO64W)GetProcAddress(module, "SymGetModuleInfoW64");

  if(!dynSymInitializeW || !dynSymRefreshModuleList || !dynSymEnumerateModules64W ||
     !dynSymGetModuleInfo64W)
  {
    RDCERR("Couldn't get some dbghelp function");
    ret = false;
    return ret;
  }

  dynSymInitializeW(GetCurrentProcess(), L".", TRUE);

  HMODULE hModule = NULL;
  GetModuleHandleEx(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCTSTR)&dllLocator, &hModule);

  if(hModule != NULL)
  {
    MODULEINFO modinfo = {0};

    BOOL result = GetModuleInformation(GetCurrentProcess(), hModule, &modinfo, sizeof(modinfo));

    if(result != FALSE)
    {
      renderdocBase = modinfo.lpBaseOfDll;
      renderdocSize = modinfo.SizeOfImage;
    }
  }

  ret = true;
  return ret;
}

///////////////////////////////////////////////////

struct CV_INFO_PDB70
{
  DWORD CvSignature;
  GUID Signature;
  DWORD Age;
  char PdbFileName[1024];
};

struct EnumBuf
{
  char *bufPtr;
  size_t size;
};

struct EnumModChunk
{
  DWORD64 base;
  DWORD size;
  DWORD age;
  GUID guid;
  size_t imageNameLen;
  // WCHAR* imageName; // follows (null terminated)
};

BOOL CALLBACK EnumModule(PCWSTR ModuleName, DWORD64 BaseOfDll, PVOID UserContext)
{
  EnumBuf *buf = (EnumBuf *)UserContext;

  IMAGEHLP_MODULEW64 ModInfo;
  ZeroMemory(&ModInfo, sizeof(IMAGEHLP_MODULEW64));
  ModInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULEW64);
  BOOL res = dynSymGetModuleInfo64W(GetCurrentProcess(), BaseOfDll, &ModInfo);
  DWORD err = GetLastError();

  if(!res)
  {
    RDCERR("Couldn't get module info for %ls: %d", ModuleName, err);
    return FALSE;
  }

  EnumModChunk chunk;

  chunk.base = BaseOfDll;
  chunk.size = ModInfo.ImageSize;

  // can't get symbol the easy way, let's walk the PE structure.
  // Thanks to http://msdn.microsoft.com/en-us/library/ms809762.aspx
  // and also http://www.debuginfo.com/articles/debuginfomatch.html
  if(ModInfo.PdbSig70.Data1 == 0 && ModInfo.SymType == SymPdb)
  {
    BYTE *addr32 = (BYTE *)BaseOfDll;

#ifndef WIN64
    RDCASSERT((BaseOfDll & 0xffffffff00000000ULL) ==
              0x0ULL);    // we're downcasting technically, make sure.
#endif

    PIMAGE_DOS_HEADER dosheader = (PIMAGE_DOS_HEADER)addr32;

    char *PE00 = (char *)(addr32 + dosheader->e_lfanew);
    PIMAGE_FILE_HEADER fileHeader = (PIMAGE_FILE_HEADER)(PE00 + 4);
    PIMAGE_OPTIONAL_HEADER optHeader =
        (PIMAGE_OPTIONAL_HEADER)((BYTE *)fileHeader + sizeof(IMAGE_FILE_HEADER));

    DWORD dbgOffset = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
    PIMAGE_DEBUG_DIRECTORY debugDir = (PIMAGE_DEBUG_DIRECTORY)(addr32 + dbgOffset);

    CV_INFO_PDB70 *pdb70Data = (CV_INFO_PDB70 *)(addr32 + debugDir->AddressOfRawData);

    chunk.age = pdb70Data->Age;
    chunk.guid = pdb70Data->Signature;
  }
  else
  {
    chunk.age = ModInfo.PdbAge;
    chunk.guid = ModInfo.PdbSig70;
  }

  WCHAR *pdb = ModInfo.CVData;

  if(pdb == NULL || pdb[0] == 0)
    pdb = ModInfo.ImageName;

  chunk.imageNameLen = wcslen(pdb) + 1;    // include null terminator

  if(buf->bufPtr)
  {
    memcpy(buf->bufPtr, &chunk, sizeof(EnumModChunk));
    buf->bufPtr += sizeof(EnumModChunk);
    memcpy(buf->bufPtr, pdb, chunk.imageNameLen * sizeof(WCHAR));
    buf->bufPtr += chunk.imageNameLen * sizeof(WCHAR);
  }

  buf->size += sizeof(EnumModChunk) + chunk.imageNameLen * sizeof(WCHAR);

  return TRUE;
}

void Win32Callstack::Collect()
{
  std::vector<PVOID> stack32;

  stack32.resize(64);

  USHORT num = RtlCaptureStackBackTrace(0, 63, &stack32[0], NULL);

  stack32.resize(num);

  while(!stack32.empty() && (uint64_t)stack32[0] >= (uint64_t)renderdocBase &&
        (uint64_t)stack32[0] <= (uint64_t)renderdocBase + renderdocSize)
  {
    stack32.erase(stack32.begin());
  }

  m_AddrStack.resize(stack32.size());
  for(size_t i = 0; i < stack32.size(); i++)
    m_AddrStack[i] = (DWORD64)stack32[i];
}

Win32Callstack::Win32Callstack()
{
  bool ret = InitDbgHelp();

  if(ret && renderdocBase != NULL)
    Collect();
}

Win32Callstack::Win32Callstack(DWORD64 *calls, size_t numLevels)
{
  Set(calls, numLevels);
}

void Win32Callstack::Set(DWORD64 *calls, size_t numLevels)
{
  m_AddrStack.resize(numLevels);
  for(size_t i = 0; i < numLevels; i++)
    m_AddrStack[i] = calls[i];
}

Win32Callstack::~Win32Callstack()
{
}

wstring Win32CallstackResolver::pdbBrowse(wstring startingPoint)
{
  OPENFILENAMEW ofn;
  RDCEraseMem(&ofn, sizeof(ofn));

  wchar_t outBuf[MAX_PATH * 2];
  wcscpy_s(outBuf, startingPoint.c_str());

  ofn.lStructSize = sizeof(OPENFILENAME);
  ofn.lpstrTitle = L"Locate PDB File";
  ofn.lpstrFilter = L"PDB File\0*.pdb\0";
  ofn.lpstrFile = outBuf;
  ofn.nMaxFile = MAX_PATH * 2 - 1;
  ofn.Flags =
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;    // | OFN_ENABLEINCLUDENOTIFY | OFN_ENABLEHOOK

  BOOL ret = GetOpenFileNameW(&ofn);

  if(ret == FALSE)
    return L"";

  return outBuf;
}

void Win32CallstackResolver::OpenPdblocateHandle()
{
  STARTUPINFOW si;
  RDCEraseEl(si);

  PROCESS_INFORMATION pi;
  RDCEraseEl(pi);

  wchar_t locateCmd[128] = {0};

  wcscpy_s(locateCmd, L"\".\\pdblocate\\pdblocate.exe\"");

  BOOL success = CreateProcessW(NULL, locateCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

  if(!success)
    return;

  Sleep(100);

  CloseHandle(pi.hThread);

  pdblocateProcess = pi.hProcess;

  pdblocatePipe = CreateFileW(L"\\\\.\\pipe\\RenderDoc.pdblocate", GENERIC_READ | GENERIC_WRITE, 0,
                              NULL, OPEN_EXISTING, 0, NULL);

  if(pdblocatePipe == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't open pdblocate pipe");
    CloseHandle(pdblocatePipe);
    pdblocatePipe = NULL;
    return;
  }

  DWORD mode = PIPE_READMODE_MESSAGE;
  success = SetNamedPipeHandleState(pdblocatePipe, &mode, NULL, NULL);

  if(!success)
  {
    RDCERR("Couldn't set pdblocate pipe to message mode");
    CloseHandle(pdblocatePipe);
    pdblocatePipe = NULL;
    return;
  }
}

void *Win32CallstackResolver::SendRecvPipeMessage(wstring message)
{
  if(pdblocatePipe == NULL)
    return NULL;

  DWORD written = 0;
  DWORD msgLen = (DWORD)message.length() * sizeof(wchar_t);
  BOOL success = WriteFile(pdblocatePipe, message.c_str(), msgLen, &written, NULL);

  if(!success || written != msgLen)
    return NULL;

  byte *bufPtr = (byte *)pipeMessageBuf;
  DWORD bufSize = sizeof(pipeMessageBuf);

  do
  {
    DWORD read = 0;
    success = ReadFile(pdblocatePipe, bufPtr, bufSize, &read, NULL);

    RDCDEBUG("'%ls' -> %lu", message.c_str(), read);

    DWORD err = GetLastError();
    if(!success && err != ERROR_MORE_DATA)
    {
      break;
    }

    bufPtr += read;
    bufSize -= read;
  } while(!success);

  RDCDEBUG("buf: %02x %02x %02x %02x", bufPtr[0], bufPtr[1], bufPtr[2], bufPtr[3]);

  return pipeMessageBuf;
}

std::wstring Win32CallstackResolver::LookupModule(wchar_t *modName, GUID guid, DWORD age)
{
  wchar_t msg[1024] = {0};
  swprintf_s(msg, L"lookup %d  %d %d %d  %d %d %d %d %d %d %d %d  %ls", age, guid.Data1, guid.Data2,
             guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
             guid.Data4[5], guid.Data4[6], guid.Data4[7], modName);

  wchar_t *ret = (wchar_t *)SendRecvPipeMessage(msg);

  wstring result = (ret == NULL ? L"" : ret);

  if(result == L"Not Found")
    result = L"";

  return result;
}

void Win32CallstackResolver::SetModuleBaseAddress(uint32_t moduleId, DWORD64 base)
{
  wchar_t msg[1024];
  swprintf_s(msg, L"baseaddr %d %llu", moduleId, base);
  SendRecvPipeMessage(msg);
}

uint32_t Win32CallstackResolver::GetModuleID(wstring pdbName, GUID guid, DWORD age)
{
  wchar_t msg[1024] = {0};
  swprintf_s(msg, L"getmodule %d  %d %d %d  %d %d %d %d %d %d %d %d  %ls", age, guid.Data1,
             guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
             guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], pdbName.c_str());

  uint32_t *modID = (uint32_t *)SendRecvPipeMessage(msg);

  return modID == NULL ? 0 : *modID;
}

Win32CallstackResolver::AddrInfo Win32CallstackResolver::GetAddrInfoForModule(uint32_t moduleId,
                                                                              DWORD64 addr)
{
  wchar_t msg[1024];
  swprintf_s(msg, L"getaddr %d %llu", moduleId, addr);

  AddrInfo *info = (AddrInfo *)SendRecvPipeMessage(msg);
  return info == NULL ? AddrInfo() : *info;
}

Win32CallstackResolver::Win32CallstackResolver(char *moduleDB, size_t DBSize, string pdbSearchPaths,
                                               volatile bool *killSignal)
{
  wstring configPath = StringFormat::UTF82Wide(FileIO::GetAppFolderFilename("config.ini"));
  {
    FILE *f = NULL;
    _wfopen_s(&f, configPath.c_str(), L"a");
    if(f)
      fclose(f);
  }

  DWORD sz = 2048;
  wchar_t *inputBuf = new wchar_t[sz];

  for(;;)
  {
    DWORD read =
        GetPrivateProfileStringW(L"renderdoc", L"ignores", NULL, inputBuf, sz, configPath.c_str());

    if(read == sz - 1)
    {
      sz *= 2;
      delete[] inputBuf;
      inputBuf = new wchar_t[sz];
      continue;
    }

    break;
  }
  wstring ignores = inputBuf;

  delete[] inputBuf;

  split(ignores, pdbIgnores, L';');

  wstring widepdbsearch = StringFormat::UTF82Wide(pdbSearchPaths);

  split(widepdbsearch, pdbRememberedPaths, L';');

  pdblocateProcess = NULL;
  pdblocatePipe = NULL;

  OpenPdblocateHandle();

  if(memcmp(moduleDB, "WN32CALL", 8))
  {
    RDCWARN("Can't load callstack resolve for this log. Possibly from another platform?");
    return;
  }

  char *chunks = moduleDB + 8;
  char *end = chunks + DBSize - 8;

  EnumModChunk *chunk = (EnumModChunk *)(chunks);
  WCHAR *modName = (WCHAR *)(chunks + sizeof(EnumModChunk));

  if(pdblocatePipe == NULL)
    return;

  // loop over all our modules
  for(; chunks < end; chunks += sizeof(EnumModChunk) + (chunk->imageNameLen) * sizeof(WCHAR))
  {
    chunk = (EnumModChunk *)chunks;
    modName = (WCHAR *)(chunks + sizeof(EnumModChunk));

    if(killSignal && *killSignal)
      break;

    Module m;

    m.name = modName;
    m.base = chunk->base;
    m.size = chunk->size;
    m.moduleId = 0;

    if(find(pdbIgnores.begin(), pdbIgnores.end(), m.name) != pdbIgnores.end())
    {
      RDCWARN("Not attempting to get symbols for %ls", m.name.c_str());

      modules.push_back(m);
      continue;
    }

    // get default pdb (this also looks up symbol server etc)
    // relies on pdblocate. Always done in unicode
    std::wstring defaultPdb = LookupModule(modName, chunk->guid, chunk->age);

    // strip newline
    if(defaultPdb != L"" && defaultPdb[defaultPdb.length() - 1] == '\n')
      defaultPdb.pop_back();

    // if we didn't even get a default pdb we'll have to prompt first time through
    bool failed = false;

    if(defaultPdb == L"")
    {
      defaultPdb = strlower(basename(m.name));

      size_t it = defaultPdb.find(L".dll");
      if(it != wstring::npos)
      {
        defaultPdb[it + 1] = L'p';
        defaultPdb[it + 2] = L'd';
        defaultPdb[it + 3] = L'b';
      }

      it = defaultPdb.find(L".exe");
      if(it != wstring::npos)
      {
        defaultPdb[it + 1] = L'p';
        defaultPdb[it + 2] = L'd';
        defaultPdb[it + 3] = L'b';
      }
      failed = true;
    }

    std::wstring pdbName = defaultPdb;

    int fallbackIdx = -1;

    while(m.moduleId == 0)
    {
      if(failed)
      {
        fallbackIdx++;
        // try one of the folders we've been given, just in case the symbols
        // are there
        if(fallbackIdx < (int)pdbRememberedPaths.size())
        {
          pdbName = pdbRememberedPaths[fallbackIdx] + L"\\" + basename(pdbName);
        }
        else
        {
          pdbName = dirname(defaultPdb) + L"\\" + basename(defaultPdb);

          // prompt for new pdbName, unless it's renderdoc or dbghelp
          if(pdbName.find(L"renderdoc.") != wstring::npos ||
             pdbName.find(L"dbghelp.") != wstring::npos || pdbName.find(L"symsrv.") != wstring::npos)
            pdbName = L"";
          else
            pdbName = pdbBrowse(pdbName);

          // user cancelled, just don't load this pdb
          if(pdbName == L"")
            break;
        }

        failed = false;
      }

      m.moduleId = GetModuleID(pdbName, chunk->guid, chunk->age);

      if(m.moduleId == 0)
      {
        failed = true;
      }
      else
      {
        if(fallbackIdx >= (int)pdbRememberedPaths.size())
        {
          wstring dir = dirname(pdbName);
          if(find(pdbRememberedPaths.begin(), pdbRememberedPaths.end(), dir) ==
             pdbRememberedPaths.end())
          {
            pdbRememberedPaths.push_back(dir);
          }
        }
      }
    }

    // didn't load the pdb? go to the next module.
    if(m.moduleId == 0)
    {
      modules.push_back(m);    // still add the module, with 0 module id

      RDCWARN("Couldn't get symbols for %ls", m.name.c_str());

      // silently ignore renderdoc.dll and dbghelp.dll without asking to permanently ignore
      if(m.name.find(L"renderdoc") != wstring::npos || m.name.find(L"dbghelp") != wstring::npos)
        continue;

      wchar_t text[1024];
      wsprintf(text, L"Do you want to permanently ignore this file?\nPath: %ls", m.name.c_str());

      int ret = MessageBoxW(NULL, text, L"Ignore this pdb?", MB_YESNO);

      if(ret == IDYES)
        pdbIgnores.push_back(m.name);

      continue;
    }

    SetModuleBaseAddress(m.moduleId, chunk->base);

    RDCLOG("Loaded Symbols for %ls", m.name.c_str());

    modules.push_back(m);
  }

  sort(pdbIgnores.begin(), pdbIgnores.end());
  pdbIgnores.erase(unique(pdbIgnores.begin(), pdbIgnores.end()), pdbIgnores.end());
  merge(pdbIgnores, ignores, L';');
  WritePrivateProfileStringW(L"renderdoc", L"ignores", ignores.c_str(), configPath.c_str());
}

Win32CallstackResolver::~Win32CallstackResolver()
{
  if(pdblocatePipe != NULL)
    CloseHandle(pdblocatePipe);
  if(pdblocateProcess != NULL)
    TerminateProcess(pdblocateProcess, 0);
}

Callstack::AddressDetails Win32CallstackResolver::GetAddr(DWORD64 addr)
{
  AddrInfo info;

  info.lineNum = 0;
  memset(info.fileName, 0, sizeof(info.fileName));
  memset(info.fileName, 0, sizeof(info.funcName));

  wcsncpy_s(info.fileName, L"Unknown", 126);
  wsprintfW(info.funcName, L"0x%08I64x", addr);

  for(size_t i = 0; i < modules.size(); i++)
  {
    DWORD64 base = modules[i].base;
    DWORD size = modules[i].size;
    if(addr > base && addr < base + size)
    {
      if(modules[i].moduleId != 0)
        info = GetAddrInfoForModule(modules[i].moduleId, addr);

      // if we didn't get a filename, default to the module name
      if(info.fileName[0] == 0)
        wcsncpy_s(info.fileName, modules[i].name.c_str(), 126);

      break;
    }
  }

  Callstack::AddressDetails ret;
  ret.filename = StringFormat::Wide2UTF8(wstring(info.fileName));
  ret.function = StringFormat::Wide2UTF8(wstring(info.funcName));
  ret.line = info.lineNum;

  return ret;
}

////////////////////////////////////////////////////////////////////
// implement public interface

namespace Callstack
{
void Init()
{
  ::InitDbgHelp();
}

Stackwalk *Collect()
{
  return new Win32Callstack();
}

Stackwalk *Create()
{
  return new Win32Callstack(NULL, 0);
}

StackResolver *MakeResolver(char *moduleDB, size_t DBSize, string pdbSearchPaths,
                            volatile bool *killSignal)
{
  return new Win32CallstackResolver(moduleDB, DBSize, pdbSearchPaths, killSignal);
}

bool GetLoadedModules(char *&buf, size_t &size)
{
  EnumBuf e;
  e.bufPtr = buf;
  e.size = 0;

  if(buf)
  {
    buf[0] = 'W';
    buf[1] = 'N';
    buf[2] = '3';
    buf[3] = '2';
    buf[4] = 'C';
    buf[5] = 'A';
    buf[6] = 'L';
    buf[7] = 'L';

    e.bufPtr += 8;
  }

  e.size += 8;

  bool inited = InitDbgHelp();

  if(inited)
  {
    dynSymRefreshModuleList(GetCurrentProcess());
    dynSymEnumerateModules64W(GetCurrentProcess(), &EnumModule, &e);
  }

  size = e.size;

  return true;
}
};    // namespace Callstack
