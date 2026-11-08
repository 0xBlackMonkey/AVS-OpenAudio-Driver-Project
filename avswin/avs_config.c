// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_config.c -- avswin.cfg reader + small file helpers.
//
// All driver data files live next to the DSP firmware:
//   %SystemRoot%\System32\drivers\avs\  avswin.cfg, *-tplg.bin, dsp_basefw.bin
// For older setups avswin.cfg and the topologies are still found in
// %SystemRoot%\ (checked second).
//
// File format: one "Key=Value" per line; lines that don't match a key
// exactly (comments starting with ';', blank lines) are ignored. Numbers
// are parsed with RtlCharToInteger base 0 ("0x.." = hex). A missing file
// or key always falls back to the caller's default -- never fatal.

#include "driver.h"

#define AVS_CONFIG_MAX_FILE_SIZE   8192
#define AVS_CONFIG_PATH            AVS_DATA_DIR L"avswin.cfg"
#define AVS_CONFIG_PATH_OLD        L"\\SystemRoot\\avswin.cfg"
#define AVS_FILE_POOL_TAG          'lfvA'

NTSTATUS
AvsReadFileToBuffer(
    _In_ PCWSTR Path,
    _In_ ULONG MaxSize,
    _Out_ PVOID* Buffer,
    _Out_ PULONG Size
    )
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK ioStatus;
    FILE_STANDARD_INFORMATION info;
    HANDLE file = NULL;
    NTSTATUS status;

    *Buffer = NULL;
    *Size = 0;

    RtlInitUnicodeString(&path, Path);
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateFile(&file, GENERIC_READ, &attrs, &ioStatus, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = ZwQueryInformationFile(file, &ioStatus, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status) || info.EndOfFile.QuadPart <= 0 ||
        info.EndOfFile.QuadPart > (LONGLONG)MaxSize)
    {
        ZwClose(file);
        return NT_SUCCESS(status) ? STATUS_FILE_TOO_LARGE : status;
    }

    ULONG size = (ULONG)info.EndOfFile.QuadPart;
    // +1: always NUL-terminated, so text parsers can rely on it
    PUCHAR buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, size + 1, AVS_FILE_POOL_TAG);
    if (buf == NULL)
    {
        ZwClose(file);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    status = ZwReadFile(file, NULL, NULL, NULL, &ioStatus, buf, size, &offset, NULL);
    ZwClose(file);

    if (!NT_SUCCESS(status) || ioStatus.Information != size)
    {
        ExFreePoolWithTag(buf, AVS_FILE_POOL_TAG);
        return NT_SUCCESS(status) ? STATUS_END_OF_FILE : status;
    }

    buf[size] = '\0';
    *Buffer = buf;
    *Size = size;
    return STATUS_SUCCESS;
}

VOID
AvsFreeFileBuffer(
    _In_opt_ PVOID Buffer
    )
{
    if (Buffer != NULL)
    {
        ExFreePoolWithTag(Buffer, AVS_FILE_POOL_TAG);
    }
}

// avswin.cfg from the data directory, else from the old %SystemRoot% place.
static NTSTATUS
AvsReadConfigFile(
    _Out_ PVOID* Buffer,
    _Out_ PULONG Size
    )
{
    NTSTATUS status = AvsReadFileToBuffer(AVS_CONFIG_PATH, AVS_CONFIG_MAX_FILE_SIZE, Buffer, Size);

    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        status = AvsReadFileToBuffer(AVS_CONFIG_PATH_OLD, AVS_CONFIG_MAX_FILE_SIZE, Buffer, Size);
    }
    return status;
}

// Finds "Key=" at the start of a line; returns pointer to the value and its
// length up to end-of-line, or NULL.
static PCHAR
AvsConfigFindValue(
    _In_ PCHAR Buffer,
    _In_ ULONG Length,
    _In_ PCSTR Key,
    _Out_ PULONG ValueLen
    )
{
    ULONG keyLen = (ULONG)strlen(Key);
    ULONG i = 0;

    while (i < Length)
    {
        ULONG lineStart = i;
        while (i < Length && Buffer[i] != '\n' && Buffer[i] != '\r')
        {
            i++;
        }
        ULONG lineLen = i - lineStart;
        while (i < Length && (Buffer[i] == '\n' || Buffer[i] == '\r'))
        {
            i++;
        }

        if (lineLen > keyLen &&
            Buffer[lineStart + keyLen] == '=' &&
            _strnicmp(Buffer + lineStart, Key, keyLen) == 0)
        {
            PCHAR v = Buffer + lineStart + keyLen + 1;
            ULONG n = 0;
            ULONG max = lineLen - keyLen - 1;

            // "quoted value" may contain spaces (e.g. Endpoint2Fe="DMIC FE")
            if (max > 0 && v[0] == '"')
            {
                v++;
                max--;
                while (n < max && v[n] != '"')
                {
                    n++;
                }
                *ValueLen = n;
                return v;
            }

            while (n < max && v[n] != ' ' && v[n] != '\t' && v[n] != ';')
            {
                n++;
            }
            *ValueLen = n;
            return v;
        }
    }

    *ValueLen = 0;
    return NULL;
}

ULONG
AvsConfigReadUlong(
    _In_ PCSTR Key,
    _In_ ULONG DefaultValue
    )
{
    PVOID buf;
    ULONG size;
    ULONG result = DefaultValue;

    if (!NT_SUCCESS(AvsReadConfigFile(&buf, &size)))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: config -- %ws not readable, using default %s=%lu\n",
            AVS_CONFIG_PATH, Key, DefaultValue));
        return DefaultValue;
    }

    ULONG valLen;
    PCHAR v = AvsConfigFindValue((PCHAR)buf, size, Key, &valLen);
    if (v != NULL && valLen > 0 && valLen < 32)
    {
        CHAR tmp[32];
        ULONG parsed;

        RtlCopyMemory(tmp, v, valLen);
        tmp[valLen] = '\0';
        if (NT_SUCCESS(RtlCharToInteger(tmp, 0, &parsed)))
        {
            result = parsed;
        }
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: config -- %s=%lu%s\n", Key, result, (v != NULL) ? "" : " (default)"));

    AvsFreeFileBuffer(buf);
    return result;
}

BOOLEAN
AvsConfigReadString(
    _In_ PCSTR Key,
    _Out_writes_(BufLen) PCHAR Buf,
    _In_ ULONG BufLen
    )
{
    PVOID file;
    ULONG size;
    BOOLEAN found = FALSE;

    if (BufLen == 0)
    {
        return FALSE;
    }
    Buf[0] = '\0';

    if (!NT_SUCCESS(AvsReadConfigFile(&file, &size)))
    {
        return FALSE;
    }

    ULONG valLen;
    PCHAR v = AvsConfigFindValue((PCHAR)file, size, Key, &valLen);
    if (v != NULL && valLen > 0)
    {
        if (valLen >= BufLen)
        {
            valLen = BufLen - 1;
        }
        RtlCopyMemory(Buf, v, valLen);
        Buf[valLen] = '\0';
        found = TRUE;
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: config -- %s=\"%s\"\n", Key, Buf));
    }

    AvsFreeFileBuffer(file);
    return found;
}
