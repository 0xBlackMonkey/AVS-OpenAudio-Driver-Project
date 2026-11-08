# AVS OpenAudio Driver Project (avswin)

An open-source Windows 10/11 audio driver for the Intel **Skylake / Kaby Lake audio DSP** ("AVS", cAVS 1.5) on Chromebooks that run Windows.

The DSP side is a port of the Linux `sound/soc/intel/avs` driver. It loads the same DSP base firmware and the same ALSA topology files as Linux. On top of that, a PortCls/WaveRT miniport exposes normal Windows audio endpoints.

It is meant as a **base for other AVS machines**. Board specifics live in a text file (`avswin.cfg`), not in the code, so for many boards porting is configuration only. See [Porting](#porting-to-another-avs-board).

> Not affiliated with or supported by Intel, Google or CoolStar. Experimental: no warranty (GPL-2.0).

## Status

Developed and tested on the **Asus Chromebook Flip C434** (*shyvana*, Kaby Lake, PCI `8086:9D71`), Windows 10 and 11:

| | Hardware | Status |
|---|---|---|
| Speakers | MAX98927 amp on SSP0 | working |
| Headphones | DA7219 codec on SSP1 | working, jack detection with auto-switching |
| Headset microphone | DA7219 on SSP1 | working, appears when a headset is plugged in |
| Built-in microphones | PCH DMIC | working |
| Sleep/resume, device disable/enable | | working |

## How it fits together

```
 Windows audio engine
        |  WaveRT (Wave<n> + Topo<n> filters per endpoint)
 avswin.sys  ── PortCls miniports (adapter.cpp, minwavert.cpp, mintopo.cpp)
        |       C driver: DSP boot (CLDMA), IPC, HDA host DMA streams, jack
        |       port/: Linux avs core (topology parser, paths, messages)
        |  GUID_ADSP_BUS_INTERFACE
 sklhdaudbus.sys (CoolStar)  ── child PDO CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_9D71
        |
 Intel HDA controller + audio DSP ── SSP0: MAX98927 (opengmaxcodec.sys, I2C)
                                     SSP1: DA7219   (da7219.sys, I2C + jack HID)
                                     DMIC
```

avswin talks only to the DSP. The codec chips are configured over I2C by CoolStar's codec drivers:

- avswin tells the codec drivers when an endpoint starts or stops, through the CSAudio callback;
- it reads the headphone jack state from da7219.sys's HID collection.

## Repository layout

```
avswin.sln               Visual Studio solution: the driver + the settings utility
avswin/                  the driver project: avswin.vcxproj, sources, avswin.inf
  port/                  ported Linux avs core (GPL-2.0)
avswin.cfg               example configuration (Asus C434)
tools/avs-settings/      AVS Audio Settings: GUI for avswin.cfg (plain Win32 C)
tools/avs_diag.ps1       shows which audio interfaces/endpoints Windows created
third_party/             the same opengmaxcodec change as a patch file
```

## Building

Requirements:

- Visual Studio 2022 with the *Desktop development with C++* workload;
- the **Windows Driver Kit** matching the installed Windows SDK.

Open `avswin.sln`, choose **Release | x64** (or Debug), then **Build**. Both projects build into `x64\Release\` at the solution root:

- the driver package `avswin\`, with `avswin.sys`, `avswin.inf` and `avswin.cat`. Both configurations log the same way (`DbgPrintEx`);
- `avs-settings.exe`, the settings utility. It is built with the static C runtime, so it runs without the VC++ Redistributable.

The project is test-signed. If no certificate is chosen, the WDK creates a test certificate on the first build. To use your own, pick it under Driver Signing → Test Certificate; that choice is stored in your local `.vcxproj.user`, not in the repository. If you also build CoolStar's drivers, sign all of them with the same certificate: then one `.cer` covers everything. One way to create one:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=My AVS Test" `
    -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
    -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
Export-Certificate -Cert $cert -FilePath my-avs-test.cer
```

## Installing (manually)

You need four things.

1. **Test mode.** Windows x64 loads only Microsoft-signed kernel drivers unless test signing is on. Secure Boot must be off. Restart afterwards.
   ```bat
   bcdedit /set testsigning on
   ```
2. **CoolStar's drivers** for the bus and the codecs:
   - [sklhdaudbus](https://github.com/coolstar/sklhdaudbus): HD Audio bus, creates the ADSP device avswin attaches to;
   - on the C434, [opengmaxcodec](https://github.com/coolstar/opengmaxcodec) (MAX98927) and [da7219](https://github.com/coolstar/da7219).

   Builds from their GitHub sources work, and so do CoolStar's signed builds. Do **not** install CoolStar's `csaudiosof4skl.sys`: it drives the same DSP device as avswin.
3. **Data files** in `C:\Windows\System32\drivers\avs\`:
   - `dsp_basefw.bin`: the AVS base firmware for your platform, from linux-firmware `intel/avs/<platform>/` (SKL/KBL: `intel/avs/skl/`);
   - the topology files your board uses, from linux-firmware `intel/avs/`, decompressed if they are `.zst`. On the C434 these are `max98927-tplg.bin`, `da7219-tplg.bin` and `dmic-tplg.bin`;
   - `avswin.cfg`, adapted to your board (see below).

   Firmware and topologies are not in this repository. Their licenses are listed in linux-firmware's `WHENCE`: Intel's `LICENCE.adsp_sst` for the firmware and Apache-2.0 for the topologies. Ship those license files along if you redistribute them.
4. **The driver:**
   ```bat
   certutil -addstore Root my-avs-test.cer
   certutil -addstore TrustedPublisher my-avs-test.cer
   pnputil /add-driver avswin.inf /install
   ```

### Louder speakers on MAX98927 boards (optional)

Stock opengmaxcodec programs the MAX98927 at +2 dB total gain, while Linux uses +13 dB. Speakers on Windows are therefore noticeably quieter than on Linux, whatever avswin does.

[opengmaxcodec-mod](https://github.com/0xBlackMonkey/opengmaxcodec-mod), a fork of CoolStar's driver with one commit, switches the amp to the Linux defaults and makes the gain adjustable. Build it instead of the original. The same change is in `third_party/opengmaxcodec-gain.patch`, for `git apply` in your own opengmaxcodec clone.

No configuration is needed: without registry values the amp already uses the Linux defaults. To fine-tune, the amp reads these REG_DWORD values from its device key at every power-up:

| Value | Range | Default |
|---|---|---|
| `SpeakerGain` | 1..6 = +3..+18 dB | 5 |
| `DigitalVolume` | 0..0x7F = −16 dB + 0.25 dB/step (0x40 = 0 dB; above clips loud passages) | 0x38 |
| `BoostCurrentLimit` | register R0042, 0x3E = 4.1 A (max) | 0x3E |

To set them, run this as Administrator, then restart the amps (or Windows):

```powershell
Get-PnpDevice | Where-Object { $_.HardwareID -like 'ACPI\MX98927*' } | ForEach-Object {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($_.InstanceId)\Device Parameters"
    Set-ItemProperty $k -Name SpeakerGain   -Value 6    -Type DWord
    Set-ItemProperty $k -Name DigitalVolume -Value 0x40 -Type DWord
}
```

On the C434, `SpeakerGain=6`, `DigitalVolume=0x40` and `DspVolumePercent=200` give loud, clean sound.

## Configuration: `avswin.cfg`

The driver reads `C:\Windows\System32\drivers\avs\avswin.cfg` (falling back to `C:\Windows\avswin.cfg`) once, at device start. After editing it, disable and enable the device in Device Manager, or restart.

Format:

- one `Key=Value` per line; the key starts the line, and the `=` follows it directly;
- `;` starts a comment;
- numbers are decimal or `0x` hex;
- values with spaces go in quotes;
- the file may be at most 8 KB.

| Key | Default | Meaning |
|---|---|---|
| `Tplg<n>` | — | Topology file name, n = 0..3, loaded from the `avs` folder. |
| `Tplg<n>SspMask` | 0 | SSP ports the topology drives (bit per SSP), as in Linux `board_selection.c`. 0 for DMIC. |
| `Endpoint<n>Fe` | — | FE path template feeding Windows endpoint n (0..3), e.g. `ssp0p_fe`, `DMIC FE`. |
| `Endpoint<n>Type` | — | `speaker`, `headphones`, `mic` (built-in), or `headsetmic`. |
| `Endpoint<n>CsAudioType` | per type | CSAudio endpoint type sent to codec drivers on start/stop: speaker 1, headphones 2; mics send nothing. |
| `PlaybackFe` | — | Legacy: one speaker endpoint on this FE when no `Endpoint` keys exist. |
| `JackDetect` | 1 | Read the jack state from the codec driver's HID collection (0 = headphones always shown). |
| `JackHidMatch` | — | `\|`-separated IDs the HID interface name must contain (e.g. `DLGS7219\|VID_2DCF&PID_7219`). |
| `JackHidCollection` | — | Top-level collection in the interface name (e.g. `COL02`). |
| `DspVolumePercent` | 50 | Fixed DSP gain, % of the topology's "DSP Volume" range, 0..200. The Windows slider works in software on top of it. |
| `MicVolumePercent` | 100 | Fixed DMIC gain, % of the "DMIC Volume" range. |
| `HostStreamFrags` | 2 | BDL entries per cyclic DMA buffer. |
| `HdaLinkPowerUp` | 1 | Power up the HDA multi-link per SSP before creating paths. |
| `BootBeepMs` | 0 | Diagnostics: 440 Hz tone through the full stream path right after boot. |
| `BootBeepEndpoint` | 0 | Endpoint used for the boot beep. |
| `TestToneAmplitude` | 3000 | Amplitude of the boot beep (0..32767). |
| `DebugStreams` | 0 | Dump host-stream registers on every start. |
| `IpcDumpPayloads` | 0 | Hex-dump every IPC message (noisy). |
| `CSAudioEndpoint*` | 0/1/2… | CSAudio enum values, from CoolStar's `opengmaxcodec.h`. |

## Settings utility (`avs-settings.exe`)

A single window for the everyday keys of `avswin.cfg`. It needs administrator rights and asks for them itself.

- **Speakers**: DSP gain, plus the MAX98927 gain, volume and current limit. The amplifier settings take effect only with [opengmaxcodec-mod](https://github.com/0xBlackMonkey/opengmaxcodec-mod); the stock driver ignores them.
- **Microphones**: built-in mic gain.
- **Headphones**: jack detection.
- **Diagnostics**: boot beep and dump switches.

The buttons:

- **Apply** rewrites only the lines of these keys in `avswin.cfg`, keeping comments and all other keys. It copies the `Amp*` keys (`AmpSpeakerGain`, `AmpDigitalVolume`, `AmpBoostCurrentLimit`) into each amplifier's `Device Parameters`, then restarts what changed: the amplifiers first, then the AVS device.
- **Defaults** fills the form from `avswin.cfg.default` when that file sits next to `avswin.cfg`, otherwise from built-in values. Apply saves it.
- **Edit file...** opens `avswin.cfg` in Notepad for everything else.

The interface language is English or Russian. It follows the Windows UI language and can be switched live. A choice that differs from the Windows language is remembered as `Lang=EN|RU` in `avswin.cfg`; the driver ignores that key.

## Diagnostics

- **Kernel log**: run [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) as Administrator and enable *Capture Kernel* and *Enable Verbose Kernel Output*. Lines start with `AVS:` (driver; jack lines are `AVS: jack -- ...`) or `AVS-core:` (ported Linux core).
- **`BootBeepMs=1500`** plays a tone through the exact path Windows playback uses (host DMA → FE → BE → SSP). It isolates the DSP side from the Windows audio stack. When DMA runs, the log shows `VERDICT: LPIB MOVING`.
- **Unknown FE name**: when an `Endpoint<n>Fe` is not found, the log lists every path template the loaded topologies offer.
- **`tools\avs_diag.ps1`** (run as Administrator) writes `avs_diag.txt`, listing which KS interfaces and MMDevice endpoints Windows created.

## Porting to another AVS board

1. **Firmware**: the `dsp_basefw.bin` Linux loads for your platform (`dmesg | grep -i avs` shows the paths).
2. **Topologies**: the `*-tplg.bin` files Linux loads for your machine (`dmesg`, or `sound/soc/intel/avs/board_selection.c`), and the SSP each one is bound to (`Tplg<n>SspMask`).
3. **Endpoints**: map Windows endpoints to FE path templates with `Endpoint<n>Fe/Type`.
   - Topologies name them with the SSP number filled in from the mask: `ssp%dp_fe` with mask 0x2 becomes `ssp1p_fe`.
   - Put a wrong name once and the log lists the available ones.
4. **Codec drivers**: whatever drives your codecs on Windows (CoolStar has several).
   - Check whether they react to CSAudio start/stop (`Endpoint<n>CsAudioType`).
   - Check how they report the jack: `JackHidMatch/JackHidCollection`; HID input report ID 2 = `{0x02, 0x01, jack bits}`, as in da7219.sys.
5. **PCI ID**: add your `CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_xxxx` to `avswin.inf`.
6. Bring it up with `BootBeepMs` first, then the Windows endpoints.

Only SKL/KBL (cAVS 1.5, CLDMA firmware load) is implemented. Apollo Lake / Gemini Lake use the APL boot flow (code-loader DMA via a host stream, different IPC registers) and would need work in `avs_dsp_boot.c` / `avs_ipc.c`.

## Known limitations

- Each endpoint has one format: the first variant of its FE template (48 kHz, 16-bit, stereo on the C434).
- No hardware volume or mute nodes: Windows applies endpoint volume in software, and the DSP gain is fixed by `avswin.cfg`.
- Volume keys are handled by the keyboard/input driver, not by this driver.
- Test-signed only. A release-signed build needs Microsoft attestation signing (Partner Center account + EV certificate).

## Licensing

| Part | License |
|---|---|
| This driver (all code unless stated otherwise) | GPL-2.0-only, see `LICENSE` |
| `avswin/port/`: derived from Linux `sound/soc/intel/avs` | GPL-2.0-only |
| `avswin/adsp.h`: interface header from CoolStar's sklhdaudbus | BSD-3-Clause (text in the file) |
| CSAudio / da7219 HID protocol constants | facts taken from CoolStar's Apache-2.0 sources; no code copied |
| `third_party/opengmaxcodec-gain.patch` | a patch to CoolStar's Apache-2.0 opengmaxcodec ([opengmaxcodec-mod](https://github.com/0xBlackMonkey/opengmaxcodec-mod)); Apache-2.0 like the code it changes |
| firmware, topologies | not included; see linux-firmware `WHENCE` |

## Credits

- The Linux AVS driver authors (Intel), whose `sound/soc/intel/avs` this driver ports.
- CoolStar, for sklhdaudbus and the Chromebook codec drivers this driver sits on.
