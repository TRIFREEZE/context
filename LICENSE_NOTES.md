# License notes (read before selling Context)

This is not legal advice. Check the current terms yourself before a commercial release.

## JUCE — the one that matters

JUCE 8 is dual-licensed: **AGPLv3** or the **commercial JUCE 8 licence** (EULA at
https://juce.com/legal/juce-8-licence/).

* Selling a closed-source plugin under AGPLv3 isn't possible: AGPLv3 would require
  you to publish Context's full source code under AGPLv3.
* To sell Context closed-source you need a **JUCE commercial licence**. JUCE offers
  several tiers, including a free tier for small businesses under a revenue limit.
  Confirm the current tiers, revenue limits and any attribution requirements on
  juce.com before release, and register under the right one.

## Everything else

* VST3 SDK 3.8.0 is MIT-licensed (bundled with JUCE). The "VST" name/logo has its
  own Steinberg trademark guidelines (`VST3_Usage_Guidelines.pdf` in the SDK).
* Monocypher: BSD-2-Clause or CC0 — no obligations beyond keeping the notice.
* Backend packages (FastAPI etc.) run on your server; permissive licences.
* No GPL/AGPL code other than JUCE's AGPL option is involved; the ASIO SDK (GPLv3)
  is **not** compiled in (`JUCE_ASIO=0`). If you ever enable ASIO for the
  Standalone app, re-check this.

## Generation providers

Context itself contains no provider code. Whatever bridge your customers run must
be used in line with that provider's terms of service. Before selling Context as a
way to use a specific service, confirm that the service permits it (ideally via an
official API or written permission).

## Product name and plugin ID

The name "Context", manufacturer code `Trfz` and plugin code `Cntx` are set in the
top of `CMakeLists.txt`. Changing the name or codes later makes DAWs treat it as a
different plugin (existing projects won't find it), so settle them before release.
Do a trademark search on the final name.
