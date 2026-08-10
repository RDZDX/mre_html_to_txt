# MRE HTML to TXT Converter

This directory contains a **MediaTek Runtime Environment (MRE)** application for the **Nokia 225 (Nokia S30 / MediaTek chipset)**. It converts a fixed EPUB input file on the microSD card into a UTF-8 plain text file on-device.

## Prerequisites

- `arm-none-eabi-gcc`
- `cmake`
- `make` or `ninja`
- An MRE SDK with headers and libraries
- A built copy of [`XimikBoda/TinyMRESDK`](https://github.com/XimikBoda/TinyMRESDK) providing `PackApp` and `PackRes`

## Environment variables

```bash
export MRE_SDK=/path/to/mre-sdk
export TinyMRESDK=/path/to/TinyMRESDK
```

## Build

```bash
cd mre_epub_to_txt
cmake --preset arm-release-unix   # or arm-release on Windows
cmake --build build
```

The packaged app will be produced as `build/epub_to_txt.vxp`.

## File

- [html_to_txt.vxp](https://rdzdx.github.io/mre_html_to_txt/html_to_txt.vxp)

## Links

- https://github.com/XimikBoda/CmakeMreTemplate
- https://github.com/XimikBoda/TinyMRESDK
