// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_settings.c -- "AVS Audio Settings": one dialog for the user-facing
// keys of avswin.cfg, with Apply (save + restart the devices) and Defaults.
//
//  * avswin.cfg lives in %SystemRoot%\System32\drivers\avs\ (the driver's
//    data folder). Only the lines of the keys below are rewritten, in the
//    same "Key=value" form the driver parses (key at line start, case-
//    insensitive, '=' right after it); comments and all other keys stay.
//    The driver reads at most 8 KB, so a larger result is refused.
//  * Amp* keys are not read by avswin.sys: they are copied into each
//    MAX98927's "Device Parameters" registry key, where the patched
//    opengmaxcodec.sys reads SpeakerGain / DigitalVolume / BoostCurrentLimit
//    (third_party/opengmaxcodec-gain.patch).
//  * Apply restarts what changed: the amps (if Amp* changed), then the AVS
//    ADSP device (avswin.sys reads avswin.cfg at start).
//  * Defaults come from avswin.cfg.default (installed next to avswin.cfg),
//    falling back to the built-in table below. They only fill the form;
//    Apply saves them.
//
// Plain Win32 + SetupAPI, x64 only (a 32-bit process cannot restart devices
// on 64-bit Windows).
//
// UI language: Russian for ru/uk/be Windows, else English, switchable live
// with the combo next to the buttons. A choice that differs from that
// automatic one is remembered as "Lang=EN" / "Lang=RU" in avswin.cfg
// (avswin.sys ignores the key); going back to the automatic language
// removes the line again.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "resource.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#endif

#define CFG_MAX_SIZE      8192            // avs_config.c AVS_CONFIG_MAX_FILE_SIZE
#define CFG_ADDED_HDR     "; ===== added by AVS Audio Settings ====="
#define ADSP_ID_PREFIX    L"CSAUDIO\\ADSP"
#define AMP_ID_PREFIX     L"ACPI\\MX98927"

// ---------------------------------------------------------------- strings
enum {
    S_TITLE, S_GRP_SPK, S_LBL_DSPVOL, S_LBL_AMPGAIN, S_LBL_AMPDIG, S_LBL_BOOST,
    S_TOTAL, S_WARN_CLIP, S_WARN_HOT, S_GRP_MIC, S_LBL_MICVOL, S_GRP_HP,
    S_CHK_JACK, S_GRP_DIAG, S_LBL_BEEP, S_LBL_TONE, S_CHK_DBGSTREAM,
    S_CHK_IPCDUMP, S_BTN_EDITFILE, S_BTN_DEFAULTS, S_BTN_APPLY, S_BTN_CLOSE,
    S_DB, S_MUTE, S_ST_DEVICE, S_ST_RUNNING, S_ST_PROBLEM, S_ST_NOTFOUND,
    S_ST_NOCFG, S_ST_APPLYING, S_ST_APPLIED, S_ST_SAVED, S_ST_REBOOT, S_ST_NOCHANGE,
    S_ST_DEFAULTS, S_ERR_WRITE, S_ERR_SIZE, S_ERR_RANGE, S_ERR_REG,
    S_COUNT
};

static const WCHAR* const g_str[S_COUNT][2] = {
    /* S_TITLE        */ { L"AVS Audio Settings", L"Настройки звука AVS" },
    /* S_GRP_SPK      */ { L"Speakers", L"Динамики" },
    /* S_LBL_DSPVOL   */ { L"DSP gain", L"Усиление DSP" },
    /* S_LBL_AMPGAIN  */ { L"Amplifier gain", L"Усиление усилителя" },
    /* S_LBL_AMPDIG   */ { L"Amplifier volume", L"Громкость усилителя" },
    /* S_LBL_BOOST    */ { L"Boost current limit", L"Ограничение тока" },
    /* S_TOTAL        */ { L"Amplifier total: %+.1f dB  (Linux default: +13 dB)",
                           L"Усилитель всего: %+.1f дБ  (по умолчанию в Linux: +13 дБ)" },
    /* S_WARN_CLIP    */ { L"Amplifier volume above 0 dB: loud passages will clip.",
                           L"Громкость усилителя выше 0 дБ: громкие места будут обрезаться." },
    /* S_WARN_HOT     */ { L"Above the Linux defaults: listen for distortion, loud playback heats the speakers.",
                           L"Выше значений Linux: следите за искажениями, громкое воспроизведение приводит к нагреву динамиков." },
    /* S_GRP_MIC      */ { L"Microphones", L"Микрофоны" },
    /* S_LBL_MICVOL   */ { L"Built-in mic gain", L"Встроенный микрофон" },
    /* S_GRP_HP       */ { L"Headphones", L"Наушники" },
    /* S_CHK_JACK     */ { L"Jack detection (switch speakers / headphones automatically)",
                           L"Определение гнезда (автоматчески переключать динамики / наушники)" },
    /* S_GRP_DIAG     */ { L"Diagnostics", L"Диагностика" },
    /* S_LBL_BEEP     */ { L"Boot beep, ms", L"Сигнал при старте, мс" },
    /* S_LBL_TONE     */ { L"Tone amplitude", L"Амплитуда тона" },
    /* S_CHK_DBGSTREAM*/ { L"Stream register dump", L"Дамп регистров потока" },
    /* S_CHK_IPCDUMP  */ { L"IPC payload dump", L"Дамп IPC-сообщений" },
    /* S_BTN_EDITFILE */ { L"Edit file...", L"Файл..." },
    /* S_BTN_DEFAULTS */ { L"Defaults", L"По умолчанию" },
    /* S_BTN_APPLY    */ { L"Apply", L"Применить" },
    /* S_BTN_CLOSE    */ { L"Close", L"Закрыть" },
    /* S_DB           */ { L"dB", L"дБ" },
    /* S_MUTE         */ { L"mute", L"выкл." },
    /* S_ST_DEVICE    */ { L"Audio device: %s.  Speaker amplifiers: %u.",
                           L"Аудиоустройство: %s.  Усилителей динамиков: %u." },
    /* S_ST_RUNNING   */ { L"running", L"работает" },
    /* S_ST_PROBLEM   */ { L"problem code %u", L"ошибка, код %u" },
    /* S_ST_NOTFOUND  */ { L"not found", L"не найдено" },
    /* S_ST_NOCFG     */ { L"avswin.cfg not found - Apply creates it from the defaults.",
                           L"avswin.cfg не найден - «Применить» создаст его из значений по умолчанию." },
    /* S_ST_APPLYING  */ { L"Applying, restarting the devices...", L"Применение, перезапуск устройств..." },
    /* S_ST_APPLIED   */ { L"Applied, devices restarted.", L"Применено, устройства перезапущены." },
    /* S_ST_SAVED     */ { L"Saved (no AVS device to restart).", L"Сохранено (нет устройства AVS для перезапуска)." },
    /* S_ST_REBOOT    */ { L"Saved. Restart Windows to apply.", L"Сохранено. Перезагрузите Windows, чтобы применить." },
    /* S_ST_NOCHANGE  */ { L"No changes.", L"Изменений нет." },
    /* S_ST_DEFAULTS  */ { L"Defaults loaded - press Apply to save them.",
                           L"Загружены значения по умолчанию - нажмите «Применить»." },
    /* S_ERR_WRITE    */ { L"Cannot write %s (error %lu).", L"Не удалось записать %s (ошибка %lu)." },
    /* S_ERR_SIZE     */ { L"avswin.cfg would exceed 8 KB, the driver's limit. Shorten its comments.",
                           L"avswin.cfg превысит 8 КБ - предел драйвера. Сократите комментарии." },
    /* S_ERR_RANGE    */ { L"%s: allowed range is %lu..%lu.", L"%s: допустимый диапазон %lu..%lu." },
    /* S_ERR_REG      */ { L"Cannot write the amplifier settings to the registry (error %lu).",
                           L"Не удалось записать настройки усилителя в реестр (ошибка %lu)." },
};

static int g_lang;                         // 0 = English, 1 = Russian
static int g_autoLang;                     // what Windows' UI language implies
#define T(id) (g_str[(id)][g_lang])

// ------------------------------------------------------------ the keys
enum {
    K_DSPVOL, K_MICVOL, K_AMPGAIN, K_AMPDIG, K_AMPBOOST,
    K_JACK, K_BEEPMS, K_TONEAMP, K_DBGSTREAMS, K_IPCDUMP,
    K_COUNT
};

typedef struct {
    const char*  Key;          // avswin.cfg key
    ULONG        Def;          // built-in default (= shipped avswin.cfg)
    ULONG        Min, Max;
    BOOL         Hex;          // written as 0x..
    const WCHAR* AmpValue;     // non-NULL: also an amp registry value
} KEYDEF;

static const KEYDEF g_keys[K_COUNT] = {
    { "DspVolumePercent",     100,  0,   200,   FALSE, NULL },
    { "MicVolumePercent",     100,  0,   100,   FALSE, NULL },
    { "AmpSpeakerGain",       5,    1,   6,     FALSE, L"SpeakerGain" },
    { "AmpDigitalVolume",     0x38, 0,   0x7F,  TRUE,  L"DigitalVolume" },
    { "AmpBoostCurrentLimit", 0x3E, 0,   0x3E,  TRUE,  L"BoostCurrentLimit" },
    { "JackDetect",           1,    0,   1,     FALSE, NULL },
    { "BootBeepMs",           0,    0,   60000, FALSE, NULL },
    { "TestToneAmplitude",    300,  0,   32767, FALSE, NULL },
    { "DebugStreams",         0,    0,   1,     FALSE, NULL },
    { "IpcDumpPayloads",      0,    0,   1,     FALSE, NULL },
};

static ULONG g_applied[K_COUNT];      // what avswin.cfg / the amps hold now
static ULONG g_defaults[K_COUNT];
static WCHAR g_dataDir[MAX_PATH];     // ...\System32\drivers\avs\  (trailing '\')
static WCHAR g_cfgPath[MAX_PATH];
static WCHAR g_defPath[MAX_PATH];
static HWND  g_dlg;
static BOOL  g_loading;               // suppress change tracking while filling controls

static ULONG ClampKey(int k, ULONG v)
{
    if (v < g_keys[k].Min) return g_keys[k].Min;
    if (v > g_keys[k].Max) return g_keys[k].Max;
    return v;
}

// ------------------------------------------------------------ text file
typedef struct { char* Data; size_t Len; } TEXT;

static BOOL TextLoad(const WCHAR* path, TEXT* t)
{
    t->Data = NULL; t->Len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 1024 * 1024) { CloseHandle(h); return FALSE; }
    t->Data = (char*)malloc((size_t)sz.QuadPart + 1);
    DWORD rd = 0;
    if (!t->Data || !ReadFile(h, t->Data, (DWORD)sz.QuadPart, &rd, NULL)) {
        CloseHandle(h); free(t->Data); t->Data = NULL; return FALSE;
    }
    CloseHandle(h);
    t->Len = rd;
    t->Data[rd] = 0;
    return TRUE;
}

// Same rule as AvsConfigFindValue(): "Key=" at the start of a line.
static BOOL TextFindKey(const TEXT* t, const char* key, size_t* lineStart, size_t* lineEnd)
{
    size_t kl = strlen(key), i = 0;
    while (t->Data && i < t->Len) {
        size_t s = i;
        while (i < t->Len && t->Data[i] != '\n' && t->Data[i] != '\r') i++;
        size_t e = i;
        while (i < t->Len && (t->Data[i] == '\n' || t->Data[i] == '\r')) i++;
        if (e - s > kl && t->Data[s + kl] == '=' && _strnicmp(t->Data + s, key, kl) == 0) {
            *lineStart = s; *lineEnd = e;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL TextGetUlong(const TEXT* t, const char* key, ULONG* out)
{
    size_t s, e;
    if (!TextFindKey(t, key, &s, &e)) return FALSE;
    char tmp[32];
    size_t p = s + strlen(key) + 1, n = 0;
    if (p < e && t->Data[p] == '"') p++;
    while (p < e && n < sizeof(tmp) - 1 && t->Data[p] != '"' && t->Data[p] != ' ' &&
           t->Data[p] != '\t' && t->Data[p] != ';')
        tmp[n++] = t->Data[p++];
    tmp[n] = 0;
    if (n == 0) return FALSE;
    char* end;
    unsigned long v = strtoul(tmp, &end, 0);
    if (*end) return FALSE;
    *out = (ULONG)v;
    return TRUE;
}

// Value of key as text (quotes stripped), FALSE if absent.
static BOOL TextGetString(const TEXT* t, const char* key, char* out, size_t outLen)
{
    size_t s, e, n = 0;
    if (!TextFindKey(t, key, &s, &e) || outLen == 0) return FALSE;
    size_t p = s + strlen(key) + 1;
    if (p < e && t->Data[p] == '"') p++;
    while (p < e && n < outLen - 1 && t->Data[p] != '"' && t->Data[p] != ' ' &&
           t->Data[p] != '\t' && t->Data[p] != ';')
        out[n++] = t->Data[p++];
    out[n] = 0;
    return n > 0;
}

// Deletes key's line (with its line break). FALSE if it was not there.
static BOOL TextRemoveKey(TEXT* t, const char* key)
{
    size_t s, e;
    if (!TextFindKey(t, key, &s, &e)) return FALSE;
    if (e < t->Len && t->Data[e] == '\r') e++;
    if (e < t->Len && t->Data[e] == '\n') e++;
    memmove(t->Data + s, t->Data + e, t->Len - e + 1);   // +1 keeps the terminator
    t->Len -= e - s;
    return TRUE;
}

// Replaces the value of key's line, or appends "key=value" at the end.
static BOOL TextSet(TEXT* t, const char* key, const char* value, BOOL* appendedHeader)
{
    size_t s, e;
    char line[128];
    StringCchPrintfA(line, sizeof(line), "%s=%s", key, value);
    size_t ll = strlen(line);
    const char* nl = (t->Data && strstr(t->Data, "\r\n")) ? "\r\n" : "\n";

    if (TextFindKey(t, key, &s, &e)) {
        size_t newLen = t->Len - (e - s) + ll;
        char* d = (char*)malloc(newLen + 1);
        if (!d) return FALSE;
        memcpy(d, t->Data, s);
        memcpy(d + s, line, ll);
        memcpy(d + s + ll, t->Data + e, t->Len - e);
        d[newLen] = 0;
        free(t->Data); t->Data = d; t->Len = newLen;
        return TRUE;
    }

    const char* hdr = CFG_ADDED_HDR;
    size_t extra = ll + 2 * strlen(nl) + (*appendedHeader ? 0 : strlen(hdr) + 2 * strlen(nl));
    char* d = (char*)malloc(t->Len + extra + 1);
    if (!d) return FALSE;
    size_t n = 0;
    if (t->Len && t->Data) { memcpy(d, t->Data, t->Len); n = t->Len; }
    if (n && d[n - 1] != '\n') { memcpy(d + n, nl, strlen(nl)); n += strlen(nl); }
    if (!*appendedHeader) {
        memcpy(d + n, nl, strlen(nl)); n += strlen(nl);
        memcpy(d + n, hdr, strlen(hdr)); n += strlen(hdr);
        memcpy(d + n, nl, strlen(nl)); n += strlen(nl);
        *appendedHeader = TRUE;
    }
    memcpy(d + n, line, ll); n += ll;
    memcpy(d + n, nl, strlen(nl)); n += strlen(nl);
    d[n] = 0;
    free(t->Data); t->Data = d; t->Len = n;
    return TRUE;
}

// Write via a temporary file + rename, so a failure never leaves half a cfg.
static DWORD TextSave(const WCHAR* path, const TEXT* t)
{
    WCHAR tmp[MAX_PATH];
    StringCchPrintfW(tmp, MAX_PATH, L"%s.tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wr = 0;
    BOOL ok = WriteFile(h, t->Data, (DWORD)t->Len, &wr, NULL) && wr == t->Len;
    DWORD err = ok ? 0 : GetLastError();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); return err ? err : ERROR_WRITE_FAULT; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError(); DeleteFileW(tmp); return err;
    }
    return 0;
}

// ------------------------------------------------------------ devices
typedef BOOL (*DEVICE_FN)(HDEVINFO set, SP_DEVINFO_DATA* dev, void* ctx);

static BOOL HasIdPrefix(HDEVINFO set, SP_DEVINFO_DATA* dev, DWORD prop, const WCHAR* prefix)
{
    WCHAR ids[2048];
    DWORD type;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, prop, &type, (BYTE*)ids, sizeof(ids) - 4, NULL))
        return FALSE;
    ids[_countof(ids) - 1] = ids[_countof(ids) - 2] = 0;
    size_t pl = wcslen(prefix);
    for (WCHAR* p = ids; *p; p += wcslen(p) + 1)
        if (_wcsnicmp(p, prefix, pl) == 0) return TRUE;
    return FALSE;
}

// Calls fn for every present device with a hardware/compatible ID starting
// with prefix; returns how many matched.
static ULONG ForEachDevice(const WCHAR* prefix, DEVICE_FN fn, void* ctx)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    ULONG count = 0;
    SP_DEVINFO_DATA dev;
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (HasIdPrefix(set, &dev, SPDRP_HARDWAREID, prefix) ||
            HasIdPrefix(set, &dev, SPDRP_COMPATIBLEIDS, prefix)) {
            count++;
            if (fn && !fn(set, &dev, ctx)) break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

typedef struct { ULONG Status, Problem; BOOL Found; } DEVSTATE;

static BOOL GetStateFn(HDEVINFO set, SP_DEVINFO_DATA* dev, void* ctx)
{
    (void)set;
    DEVSTATE* st = (DEVSTATE*)ctx;
    st->Found = TRUE;
    if (CM_Get_DevNode_Status(&st->Status, &st->Problem, dev->DevInst, 0) != CR_SUCCESS)
        st->Status = st->Problem = 0;
    return FALSE;                       // first one is enough
}

typedef struct { BOOL NeedReboot; DWORD Error; } RESTART;

// The "disable + enable" Device Manager does: DIF_PROPERTYCHANGE / DICS_PROPCHANGE.
static BOOL RestartFn(HDEVINFO set, SP_DEVINFO_DATA* dev, void* ctx)
{
    RESTART* r = (RESTART*)ctx;
    SP_PROPCHANGE_PARAMS pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = DICS_PROPCHANGE;
    pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
    pc.HwProfile = 0;
    if (!SetupDiSetClassInstallParamsW(set, dev, &pc.ClassInstallHeader, sizeof(pc)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, dev)) {
        r->Error = GetLastError();
        return TRUE;
    }
    SP_DEVINSTALL_PARAMS_W ip;
    ip.cbSize = sizeof(ip);
    if (SetupDiGetDeviceInstallParamsW(set, dev, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
        r->NeedReboot = TRUE;
    return TRUE;
}

typedef struct { const ULONG* Values; DWORD Error; } AMPWRITE;

static BOOL AmpWriteFn(HDEVINFO set, SP_DEVINFO_DATA* dev, void* ctx)
{
    AMPWRITE* w = (AMPWRITE*)ctx;
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE) { w->Error = GetLastError(); return TRUE; }
    for (int k = 0; k < K_COUNT; k++) {
        if (!g_keys[k].AmpValue) continue;
        DWORD v = w->Values[k];
        LONG rc = RegSetValueExW(key, g_keys[k].AmpValue, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        if (rc != ERROR_SUCCESS) w->Error = (DWORD)rc;
    }
    RegCloseKey(key);
    return TRUE;
}

typedef struct { ULONG* Values; BOOL Have[K_COUNT]; } AMPREAD;

static BOOL AmpReadFn(HDEVINFO set, SP_DEVINFO_DATA* dev, void* ctx)
{
    AMPREAD* r = (AMPREAD*)ctx;
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    for (int k = 0; k < K_COUNT; k++) {
        if (!g_keys[k].AmpValue) continue;
        DWORD v, type, cb = sizeof(v);
        if (RegQueryValueExW(key, g_keys[k].AmpValue, NULL, &type, (BYTE*)&v, &cb) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            r->Values[k] = v;
            r->Have[k] = TRUE;
        }
    }
    RegCloseKey(key);
    return FALSE;                       // first amp is representative
}

// ------------------------------------------------------------ settings
static void LoadDefaults(void)
{
    TEXT t;
    for (int k = 0; k < K_COUNT; k++) g_defaults[k] = g_keys[k].Def;
    if (TextLoad(g_defPath, &t)) {
        for (int k = 0; k < K_COUNT; k++) {
            ULONG v;
            if (TextGetUlong(&t, g_keys[k].Key, &v)) g_defaults[k] = ClampKey(k, v);
        }
        free(t.Data);
    }
}

// avswin.cfg -> g_applied; Amp* missing from the cfg come from the amp's
// registry (what the driver really uses), else the defaults.
static BOOL LoadApplied(void)
{
    TEXT t;
    BOOL have = TextLoad(g_cfgPath, &t);
    BOOL inCfg[K_COUNT] = { 0 };
    for (int k = 0; k < K_COUNT; k++) {
        ULONG v;
        g_applied[k] = g_defaults[k];
        if (have && TextGetUlong(&t, g_keys[k].Key, &v)) { g_applied[k] = ClampKey(k, v); inCfg[k] = TRUE; }
    }
    if (have) free(t.Data);

    AMPREAD ar;
    ZeroMemory(&ar, sizeof(ar));
    ULONG regVals[K_COUNT] = { 0 };
    ar.Values = regVals;
    ForEachDevice(AMP_ID_PREFIX, AmpReadFn, &ar);
    for (int k = 0; k < K_COUNT; k++)
        if (g_keys[k].AmpValue && !inCfg[k] && ar.Have[k]) g_applied[k] = ClampKey(k, regVals[k]);
    return have;
}

// ------------------------------------------------------------ dialog
static double AmpDigDb(ULONG v) { return -16.0 + 0.25 * (double)v; }

static void SetText(int id, const WCHAR* s) { SetDlgItemTextW(g_dlg, id, s); }

static void UpdateLabels(void)
{
    WCHAR b[160];
    ULONG dsp = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_DSPVOL, TBM_GETPOS, 0, 0);
    ULONG dig = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_AMPDIG, TBM_GETPOS, 0, 0);
    ULONG mic = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_MICVOL, TBM_GETPOS, 0, 0);
    LRESULT gi = SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_GETCURSEL, 0, 0);
    ULONG gain = gi < 0 ? g_defaults[K_AMPGAIN] : (ULONG)gi + 1;

    if (dsp == 0) StringCchPrintfW(b, 160, L"0 %% (%s)", T(S_MUTE));
    else StringCchPrintfW(b, 160, L"%lu %% (%+.1f %s)", dsp, 20.0 * log10(dsp / 100.0), T(S_DB));
    SetText(IDC_VAL_DSPVOL, b);

    StringCchPrintfW(b, 160, L"0x%02lX (%+.2f %s)", dig, AmpDigDb(dig), T(S_DB));
    SetText(IDC_VAL_AMPDIG, b);

    StringCchPrintfW(b, 160, L"%lu %%", mic);
    SetText(IDC_VAL_MICVOL, b);

    double total = 3.0 * gain + AmpDigDb(dig);
    StringCchPrintfW(b, 160, T(S_TOTAL), total);
    SetText(IDC_TXT_TOTAL, b);

    if (dig > 0x40) SetText(IDC_TXT_WARN, T(S_WARN_CLIP));
    else if (total > 13.0 + 0.01) SetText(IDC_TXT_WARN, T(S_WARN_HOT));
    else SetText(IDC_TXT_WARN, L"");
}

static void ControlsSet(const ULONG* v)
{
    g_loading = TRUE;
    SendDlgItemMessageW(g_dlg, IDC_TRK_DSPVOL, TBM_SETPOS, TRUE, (LPARAM)v[K_DSPVOL]);
    SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_SETCURSEL, (WPARAM)(v[K_AMPGAIN] - 1), 0);
    SendDlgItemMessageW(g_dlg, IDC_TRK_AMPDIG, TBM_SETPOS, TRUE, (LPARAM)v[K_AMPDIG]);
    SendDlgItemMessageW(g_dlg, IDC_CMB_BOOST, CB_SETCURSEL, (WPARAM)(v[K_AMPBOOST] >> 1), 0);
    SendDlgItemMessageW(g_dlg, IDC_TRK_MICVOL, TBM_SETPOS, TRUE, (LPARAM)v[K_MICVOL]);
    CheckDlgButton(g_dlg, IDC_CHK_JACK, v[K_JACK] ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(g_dlg, IDC_EDT_BEEP, v[K_BEEPMS], FALSE);
    SetDlgItemInt(g_dlg, IDC_EDT_TONE, v[K_TONEAMP], FALSE);
    CheckDlgButton(g_dlg, IDC_CHK_DBGSTREAM, v[K_DBGSTREAMS] ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_CHK_IPCDUMP, v[K_IPCDUMP] ? BST_CHECKED : BST_UNCHECKED);
    g_loading = FALSE;
    UpdateLabels();
}

// FALSE (with a message) if an edit field is out of range.
static BOOL ControlsGet(ULONG* v, BOOL quiet)
{
    BOOL ok;
    v[K_DSPVOL]  = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_DSPVOL, TBM_GETPOS, 0, 0);
    v[K_AMPGAIN] = (ULONG)SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_GETCURSEL, 0, 0) + 1;
    v[K_AMPDIG]  = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_AMPDIG, TBM_GETPOS, 0, 0);
    v[K_AMPBOOST] = (ULONG)SendDlgItemMessageW(g_dlg, IDC_CMB_BOOST, CB_GETCURSEL, 0, 0) << 1;
    v[K_MICVOL]  = (ULONG)SendDlgItemMessageW(g_dlg, IDC_TRK_MICVOL, TBM_GETPOS, 0, 0);
    v[K_JACK]    = IsDlgButtonChecked(g_dlg, IDC_CHK_JACK) == BST_CHECKED;
    v[K_DBGSTREAMS] = IsDlgButtonChecked(g_dlg, IDC_CHK_DBGSTREAM) == BST_CHECKED;
    v[K_IPCDUMP] = IsDlgButtonChecked(g_dlg, IDC_CHK_IPCDUMP) == BST_CHECKED;

    static const struct { int Key, Ctl, Label; } edits[] = {
        { K_BEEPMS, IDC_EDT_BEEP, S_LBL_BEEP }, { K_TONEAMP, IDC_EDT_TONE, S_LBL_TONE } };
    for (int i = 0; i < 2; i++) {
        int k = edits[i].Key;
        v[k] = GetDlgItemInt(g_dlg, edits[i].Ctl, &ok, FALSE);
        if (!ok || v[k] < g_keys[k].Min || v[k] > g_keys[k].Max) {
            if (!quiet) {
                WCHAR b[200];
                StringCchPrintfW(b, 200, T(S_ERR_RANGE), T(edits[i].Label), g_keys[k].Min, g_keys[k].Max);
                MessageBoxW(g_dlg, b, T(S_TITLE), MB_ICONWARNING);
                SetFocus(GetDlgItem(g_dlg, edits[i].Ctl));
            }
            return FALSE;
        }
    }
    return TRUE;
}

static void UpdateApplyButton(void)
{
    ULONG v[K_COUNT];
    BOOL dirty = TRUE;
    if (ControlsGet(v, TRUE)) dirty = memcmp(v, g_applied, sizeof(v)) != 0;
    EnableWindow(GetDlgItem(g_dlg, IDC_BTN_APPLY), dirty);
}

static int g_statusMsg = -1;               // extra status line (string id) or -1

static void ShowDeviceStatus(int msgId)
{
    const WCHAR* extra = msgId >= 0 ? T(msgId) : NULL;
    g_statusMsg = msgId;
    DEVSTATE st;
    ZeroMemory(&st, sizeof(st));
    ForEachDevice(ADSP_ID_PREFIX, GetStateFn, &st);
    ULONG amps = ForEachDevice(AMP_ID_PREFIX, NULL, NULL);

    WCHAR state[64], b[400];
    if (!st.Found) StringCchCopyW(state, 64, T(S_ST_NOTFOUND));
    else if (st.Status & DN_HAS_PROBLEM) StringCchPrintfW(state, 64, T(S_ST_PROBLEM), st.Problem);
    else StringCchCopyW(state, 64, T(S_ST_RUNNING));
    StringCchPrintfW(b, 400, T(S_ST_DEVICE), state, amps);
    if (extra && *extra) { StringCchCatW(b, 400, L"\n"); StringCchCatW(b, 400, extra); }
    SetText(IDC_TXT_STATUS, b);
}

static void Apply(void)
{
    ULONG v[K_COUNT];
    WCHAR msg[400];
    if (!ControlsGet(v, FALSE)) return;

    BOOL ampChanged = FALSE, anyChanged = FALSE;
    for (int k = 0; k < K_COUNT; k++) {
        if (v[k] != g_applied[k]) { anyChanged = TRUE; if (g_keys[k].AmpValue) ampChanged = TRUE; }
    }
    if (!anyChanged) { ShowDeviceStatus(S_ST_NOCHANGE); return; }

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ShowDeviceStatus(S_ST_APPLYING);
    UpdateWindow(g_dlg);

    // 1. avswin.cfg (re-read from disk: keeps edits made with "Edit file...")
    TEXT t;
    if (!TextLoad(g_cfgPath, &t)) { t.Data = NULL; t.Len = 0; }
    BOOL hdr = FALSE;
    for (int k = 0; k < K_COUNT; k++) {
        char val[24];
        if (g_keys[k].Hex) StringCchPrintfA(val, sizeof(val), "0x%02lX", v[k]);
        else StringCchPrintfA(val, sizeof(val), "%lu", v[k]);
        TextSet(&t, g_keys[k].Key, val, &hdr);
    }
    if (t.Len > CFG_MAX_SIZE) {
        free(t.Data);
        SetCursor(old);
        MessageBoxW(g_dlg, T(S_ERR_SIZE), T(S_TITLE), MB_ICONERROR);
        ShowDeviceStatus(-1);
        return;
    }
    CreateDirectoryW(g_dataDir, NULL);
    DWORD err = TextSave(g_cfgPath, &t);
    free(t.Data);
    if (err) {
        SetCursor(old);
        StringCchPrintfW(msg, 400, T(S_ERR_WRITE), g_cfgPath, err);
        MessageBoxW(g_dlg, msg, T(S_TITLE), MB_ICONERROR);
        ShowDeviceStatus(-1);
        return;
    }

    // 2. amps: registry + restart, 3. the AVS device
    RESTART r;
    ZeroMemory(&r, sizeof(r));
    if (ampChanged) {
        AMPWRITE w;
        w.Values = v; w.Error = 0;
        ForEachDevice(AMP_ID_PREFIX, AmpWriteFn, &w);
        if (w.Error) {
            StringCchPrintfW(msg, 400, T(S_ERR_REG), w.Error);
            MessageBoxW(g_dlg, msg, T(S_TITLE), MB_ICONWARNING);
        }
        ForEachDevice(AMP_ID_PREFIX, RestartFn, &r);
    }
    ULONG adsp = ForEachDevice(ADSP_ID_PREFIX, RestartFn, &r);

    memcpy(g_applied, v, sizeof(v));
    SetCursor(old);
    ShowDeviceStatus(r.NeedReboot || r.Error ? S_ST_REBOOT : adsp ? S_ST_APPLIED : S_ST_SAVED);
    UpdateApplyButton();
}

// Every language-dependent text of the dialog; safe to call again at any time.
static void ApplyLanguage(void)
{
    static const struct { int Id, Str; } texts[] = {
        { IDC_GRP_SPK, S_GRP_SPK }, { IDC_LBL_DSPVOL, S_LBL_DSPVOL }, { IDC_LBL_AMPGAIN, S_LBL_AMPGAIN },
        { IDC_LBL_AMPDIG, S_LBL_AMPDIG }, { IDC_LBL_BOOST, S_LBL_BOOST }, { IDC_GRP_MIC, S_GRP_MIC },
        { IDC_LBL_MICVOL, S_LBL_MICVOL }, { IDC_GRP_HP, S_GRP_HP }, { IDC_CHK_JACK, S_CHK_JACK },
        { IDC_GRP_DIAG, S_GRP_DIAG }, { IDC_LBL_BEEP, S_LBL_BEEP }, { IDC_LBL_TONE, S_LBL_TONE },
        { IDC_CHK_DBGSTREAM, S_CHK_DBGSTREAM }, { IDC_CHK_IPCDUMP, S_CHK_IPCDUMP },
        { IDC_BTN_EDITFILE, S_BTN_EDITFILE }, { IDC_BTN_DEFAULTS, S_BTN_DEFAULTS },
        { IDC_BTN_APPLY, S_BTN_APPLY }, { IDC_BTN_CLOSE, S_BTN_CLOSE },
    };
    SetWindowTextW(g_dlg, T(S_TITLE));
    for (size_t i = 0; i < _countof(texts); i++) SetText(texts[i].Id, T(texts[i].Str));

    // amplifier gain items carry the "dB" unit: refill, keep the selection
    WCHAR b[64];
    LRESULT sel = SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_GETCURSEL, 0, 0);
    SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_RESETCONTENT, 0, 0);
    for (ULONG g = 1; g <= 6; g++) {
        StringCchPrintfW(b, 64, L"%lu   (+%lu %s)", g, 3 * g, T(S_DB));
        SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_ADDSTRING, 0, (LPARAM)b);
    }
    if (sel >= 0) SendDlgItemMessageW(g_dlg, IDC_CMB_AMPGAIN, CB_SETCURSEL, (WPARAM)sel, 0);

    SendDlgItemMessageW(g_dlg, IDC_CMB_LANG, CB_SETCURSEL, (WPARAM)g_lang, 0);
    UpdateLabels();
    ShowDeviceStatus(g_statusMsg);
}

// Remembers the language in avswin.cfg: "Lang=EN|RU" only when it differs
// from the automatic one. Never creates the file (a cfg without topologies
// would leave the driver with nothing to load).
static void SaveLanguage(void)
{
    TEXT t;
    if (!TextLoad(g_cfgPath, &t)) return;
    BOOL changed, hdr = FALSE;
    if (g_lang == g_autoLang) {
        changed = TextRemoveKey(&t, "Lang");
        // drop our section header again if nothing is left under it
        // (TextSet appended: <newline> header <newline>)
        static const char* const tails[] = { "\r\n" CFG_ADDED_HDR "\r\n", "\n" CFG_ADDED_HDR "\n" };
        for (int i = 0; changed && i < 2; i++) {
            size_t n = strlen(tails[i]);
            if (t.Len >= n && memcmp(t.Data + t.Len - n, tails[i], n) == 0) {
                t.Len -= n;
                t.Data[t.Len] = 0;
                break;
            }
        }
    } else {
        char cur[8] = "";
        const char* want = g_lang ? "RU" : "EN";
        changed = !TextGetString(&t, "Lang", cur, sizeof(cur)) || _stricmp(cur, want) != 0;
        if (changed) TextSet(&t, "Lang", want, &hdr);
    }
    if (changed && t.Len <= CFG_MAX_SIZE) TextSave(g_cfgPath, &t);
    free(t.Data);
}

static void InitControls(HWND dlg)
{
    g_dlg = dlg;

    SendDlgItemMessageW(dlg, IDC_TRK_DSPVOL, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendDlgItemMessageW(dlg, IDC_TRK_DSPVOL, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dlg, IDC_TRK_AMPDIG, TBM_SETRANGE, TRUE, MAKELPARAM(0, 0x7F));
    SendDlgItemMessageW(dlg, IDC_TRK_AMPDIG, TBM_SETPAGESIZE, 0, 4);
    SendDlgItemMessageW(dlg, IDC_TRK_MICVOL, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessageW(dlg, IDC_TRK_MICVOL, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dlg, IDC_EDT_BEEP, EM_SETLIMITTEXT, 5, 0);
    SendDlgItemMessageW(dlg, IDC_EDT_TONE, EM_SETLIMITTEXT, 5, 0);

    WCHAR b[64];
    for (ULONG c = 0; c < 32; c++) {   // MAX98927 R0042 bits 5:1, 1.00 A + 0.1 A per step
        StringCchPrintfW(b, 64, L"%.2f A", 1.0 + 0.1 * c);
        SendDlgItemMessageW(dlg, IDC_CMB_BOOST, CB_ADDSTRING, 0, (LPARAM)b);
    }

    HICON big = NULL, small = NULL;
    WCHAR icoPath[MAX_PATH];
    GetSystemDirectoryW(icoPath, MAX_PATH);
    StringCchCatW(icoPath, MAX_PATH, L"\\SndVol.exe");
    if (ExtractIconExW(icoPath, 0, &big, &small, 1) > 0) {
        if (big) SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (small) SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)small);
    }

    // language names are shown in their own language, never translated
    SendDlgItemMessageW(dlg, IDC_CMB_LANG, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendDlgItemMessageW(dlg, IDC_CMB_LANG, CB_ADDSTRING, 0, (LPARAM)L"Русский");

    LoadDefaults();
    BOOL haveCfg = LoadApplied();
    g_statusMsg = haveCfg ? -1 : S_ST_NOCFG;
    ApplyLanguage();                    // texts, gain items (before ControlsSet), status
    ControlsSet(g_applied);
    UpdateApplyButton();
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        InitControls(dlg);
        return TRUE;

    case WM_HSCROLL:
        UpdateLabels();
        if (!g_loading) UpdateApplyButton();
        return TRUE;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(dlg, IDC_TXT_WARN)) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(196, 32, 32));
            SetBkMode(dc, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CMB_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT l = SendDlgItemMessageW(dlg, IDC_CMB_LANG, CB_GETCURSEL, 0, 0);
                if (l >= 0 && (int)l != g_lang) {
                    g_lang = (int)l;
                    ApplyLanguage();
                    SaveLanguage();
                }
            }
            return TRUE;
        case IDC_CMB_AMPGAIN:
        case IDC_CMB_BOOST:
            if (HIWORD(wp) == CBN_SELCHANGE) { UpdateLabels(); UpdateApplyButton(); }
            return TRUE;
        case IDC_EDT_BEEP:
        case IDC_EDT_TONE:
            if (HIWORD(wp) == EN_CHANGE && !g_loading) UpdateApplyButton();
            return TRUE;
        case IDC_CHK_JACK:
        case IDC_CHK_DBGSTREAM:
        case IDC_CHK_IPCDUMP:
            if (HIWORD(wp) == BN_CLICKED) UpdateApplyButton();
            return TRUE;
        case IDC_BTN_DEFAULTS:
            LoadDefaults();
            ControlsSet(g_defaults);
            UpdateApplyButton();
            ShowDeviceStatus(S_ST_DEFAULTS);
            return TRUE;
        case IDC_BTN_APPLY:
            Apply();
            return TRUE;
        case IDC_BTN_EDITFILE:
            // Notepad inherits our elevation, so it can save the file.
            ShellExecuteW(dlg, L"open", L"notepad.exe", g_cfgPath, NULL, SW_SHOWNORMAL);
            return TRUE;
        case IDC_BTN_CLOSE:
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void InitPaths(void)
{
    WCHAR win[MAX_PATH];
    BOOL wow = FALSE;
    if (!GetWindowsDirectoryW(win, MAX_PATH)) StringCchCopyW(win, MAX_PATH, L"C:\\Windows");
    IsWow64Process(GetCurrentProcess(), &wow);       // a 32-bit build would see SysWOW64
    StringCchPrintfW(g_dataDir, MAX_PATH, L"%s\\%s\\drivers\\avs\\", win, wow ? L"Sysnative" : L"System32");
    StringCchPrintfW(g_cfgPath, MAX_PATH, L"%savswin.cfg", g_dataDir);
    StringCchPrintfW(g_defPath, MAX_PATH, L"%savswin.cfg.default", g_dataDir);
}

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE prev, _In_ PWSTR cmd, _In_ int show)
{
    (void)prev; (void)cmd; (void)show;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    LANGID ui = GetUserDefaultUILanguage();
    WORD pl = PRIMARYLANGID(ui);
    g_autoLang = (pl == LANG_RUSSIAN || pl == LANG_UKRAINIAN || pl == LANG_BELARUSIAN) ? 1 : 0;
    g_lang = g_autoLang;

    InitPaths();
    TEXT t;
    if (TextLoad(g_cfgPath, &t)) {      // remembered choice ("Lang=EN|ENG|RU")
        char l[8];
        if (TextGetString(&t, "Lang", l, sizeof(l))) {
            if (_stricmp(l, "RU") == 0) g_lang = 1;
            else if (_stricmp(l, "EN") == 0 || _stricmp(l, "ENG") == 0) g_lang = 0;
        }
        free(t.Data);
    }
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, DlgProc, 0);
    return 0;
}
