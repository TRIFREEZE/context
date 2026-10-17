# Third-party notices

## Compiled into Context.vst3

| Name | Version | Why | License |
|---|---|---|---|
| JUCE | 8.0.12 (commit `29396c22c93392d6738e021b83196283d6e4d850`) | plugin framework, UI, audio, networking, FFT | AGPLv3 **or** commercial JUCE 8 licence — see `LICENSE_NOTES.md` |
| Steinberg VST3 SDK (bundled in JUCE) | 3.8.0 | VST3 plugin format | MIT |
| HarfBuzz (bundled in JUCE) | as shipped in JUCE 8.0.12 | text shaping | Old MIT |
| SheenBidi (bundled in JUCE) | 2.9.0 | bidirectional text | Apache-2.0 |
| zlib (bundled in JUCE) | as shipped in JUCE | compression | zlib |
| libpng / jpeglib (bundled in JUCE) | as shipped in JUCE | image formats | zlib / IJG |
| FLAC, Ogg Vorbis (bundled in JUCE) | as shipped in JUCE | audio codecs (JUCE defaults; Context validates WAV/AIFF only) | BSD |
| Monocypher (+ optional Ed25519) | 4.0.2 (`third_party/monocypher`) | verify license-token signatures | BSD-2-Clause **or** CC0-1.0 |

Not compiled in: the ASIO SDK (GPLv3/proprietary; `JUCE_ASIO=0`), the AAX SDK,
AudioUnit SDK, LV2/VST2/AU hosting.

## Licensing backend (runs on your server; not distributed with the plugin)

| Name | Version | License |
|---|---|---|
| FastAPI | 0.143.3 | MIT |
| Starlette (via FastAPI) | 1.8.x | BSD-3-Clause |
| Uvicorn | 0.54.0 | BSD-3-Clause |
| Pydantic | 2.14.1 | MIT |
| cryptography | 50.0.2 | Apache-2.0 or BSD-3-Clause |
| pytest (tests only) | 9.1.1 | MIT |
| httpx (tests only) | 0.28.1 | BSD-3-Clause |

## Tools (development only)

| Name | Why | License |
|---|---|---|
| CMake, Ninja | build | BSD-3-Clause / Apache-2.0 |
| Python standard library (reference bridge, test drivers) | tests | PSF |

No GPL code is compiled into the plugin. Full license texts: JUCE's `LICENSE.md`
and each bundled module's licence file inside the JUCE checkout;
`third_party/monocypher/LICENCE.md`.
