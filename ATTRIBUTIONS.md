# Attributions

Wetplate is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks rebate, toner, pitch

<https://github.com/stoatworks-labs/rebate>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the clock-unit voting, the About block, the Diag logger), the harness shape, the --pipe contract with SIGPIPE ignored, the verify script and the negative-control pattern are rebate's and toner's, which had them from pitch; the host clock-unit voting is readout's by way of both.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, with a Swap added so a bucket can change raster without being cleared.

### The resize-mid-run guard — Stoatworks photofinish

<https://github.com/stoatworks-labs/photofinish>  
Licence: MIT  
Copyright: Stoatworks Labs

The trap that a reallocated buffer is a cleared buffer, and the check that guards it, are photofinish's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The wet collodion plate's spectral sensitivity — Hertel, Skladnikiewitz and Schmidt, 1997

The source of the plate's spectral weights in source/Spectral.h, computed by tools/spectral_weights.py. Dirk Hertel, Pia Skladnikiewitz and Irene Schmidt, "The Wet Collodion Process — a Scientific Approach", IS&T's 50th Annual Conference (1997), pp. 600–604, Institute of Applied Photophysics, Technical University of Dresden. Figure 4, "Distributions of relative spectral sensitivity for varying bromide/iodide molar ratios", the 40/60 bromide/iodide curve — the recipe the authors chose as the historical optimum. Digitised from the published figure, not taken from a table: the paper prints none. The fifteen values at 10 nm from 380 to 520 nm in tools/spectral_weights.py are read off the plot to about ±0.03, relative to the sensitivity at 380 nm, and taper to zero past the 500 nm the paper's text gives as the long end. The plate's ultraviolet sensitivity is not represented, because the RGB basis carries no ultraviolet.

### The RGB-to-spectrum basis — Brian Smits, 1999

The other source of the weights. Brian Smits, "An RGB-to-Spectrum Conversion for Reflectances", Journal of Graphics Tools 4(4), 1999, pp. 11–22. The seven basis reflectance spectra (white, cyan, magenta, yellow, red, green, blue) in ten equal bins from 380 to 720 nm, and the rule that composes a reflectance for any RGB from them, which the expose shader runs. The seventy numbers in the script are a transcription of the paper's table. A copy of the paper could not be fetched to check them against in the session that wrote this (every route timed out), so the transcription is checked by the property the paper constructs them with — each secondary plus its complementary primary sums to white in every bin — which they satisfy within 0.08.

### The lights — CIE 15:2004

The third input to the weights. CIE 15:2004, Colorimetry: Illuminant A is defined as a Planckian radiator at 2856 K, and that is Tungsten. Daylight is a Planckian radiator at 6504 K, the correlated colour temperature of D65 — an approximation of D65, which is not Planckian. Flash is a Planckian radiator at 5500 K.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The wet collodion process

Built from what the process is rather than from anyone's implementation: a silver halide plate sensitive to blue and ultraviolet, exposed for seconds while wet; hand-poured collodion that drains toward one corner (the thin-film drainage law is Jeffreys', 1930, thickness growing as the square root of the distance drained past); the tintype and ambrotype as thin negatives viewed by reflection over black; the characteristic (H&D) curve with its toe, straight line and shoulder. The idea of a photochemical process done as a process is rebate's.

## Standards and published specifications

What the implementation is measured against.

- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix used for the pour edge and the defects, written out rather than copied from anyone's source.
- **IEC 61966-2-1** — The sRGB transfer function, both ways.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
