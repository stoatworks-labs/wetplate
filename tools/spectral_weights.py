#!/usr/bin/env python3
"""The plate's spectral weights, computed from published tables.

Writes source/Spectral.h: for each of the three lights, the plate exposure
produced by each of Smits' seven basis reflectance spectra (white, cyan,
magenta, yellow, red, green, blue) under that light, normalised so that a
white reflector exposes the plate to exactly 1. The shader builds the spectrum
of a pixel's linear RGB the way Smits does -- as a sum of those bases -- so its
exposure is the same sum of these seven numbers, and the whole spectral
integral costs seven multiplies per pixel.

    python3 tools/spectral_weights.py            # rewrite source/Spectral.h
    python3 tools/spectral_weights.py --check    # exit 1 if the header differs

--------------------------------------------------------------------- sources

**The plate's spectral sensitivity** S(lambda). Digitised from Figure 4 of
Dirk Hertel, Pia Skladnikiewitz and Irene Schmidt, "The Wet Collodion
Process -- a Scientific Approach", IS&T's 50th Annual Conference (1997),
pp. 600-604: "Distributions of relative spectral sensitivity for varying
bromide/iodide molar ratios", the 40/60 bromide/iodide curve, which is the
recipe the authors chose as the historical optimum. The figure spans 380 to
500 nm with the sensitivity normalised to 1 at 380 nm. The values below are
READ OFF THE PLOT at 10 nm, not taken from a table: the paper prints no
table. Their accuracy is that of a digitisation, about +/-0.03. The paper's
text puts the long end of sensitivity at 500 nm; the table tapers to zero by
520 nm. Nothing below 380 nm is represented, because the RGB basis (below)
carries no ultraviolet: a clip's pixels say nothing about the UV the plate
also sees, so that part of the plate's sensitivity is not modelled.

**The RGB-to-spectrum basis.** Brian Smits, "An RGB-to-Spectrum Conversion
for Reflectances", Journal of Graphics Tools 4(4), 1999, pp. 11-22: seven
reflectance spectra in ten equal bins from 380 to 720 nm, and the rule that
builds a smooth reflectance for any RGB from them. The seven rows are the
paper's table. They could not be re-fetched from a copy of the paper in the
session that wrote this (every route timed out), so the transcription is
checked by the property the paper constructs them with: each secondary plus
its complementary primary sums to white, bin by bin, within 0.08.

**The lights.** Planck's law, at the colour temperatures the CIE assigns:
Illuminant A is defined as a Planckian radiator at 2856 K (CIE 15:2004), and
that is `Tungsten`. `Daylight` is a Planckian radiator at 6504 K, the
correlated colour temperature of D65 -- an approximation of D65, which is not
Planckian, chosen so that no table has to be typed in from memory and said so
here. `Flash` is 5500 K, the usual figure for an electronic flash tube. Only
the SHAPE of a light over 380-520 nm matters, since the result is normalised
to white.

--------------------------------------------------------------------- method

For a basis spectrum X and a light I, the exposure is

    k_X = integral S(l) I(l) X(l) dl  /  integral S(l) I(l) W(l) dl

over 380..720 nm at 1 nm steps, S linearly interpolated between its 10 nm
points, X piecewise constant over Smits' bins. The division makes k_W = 1 by
construction, so `Light` changes the balance between colours and never the
overall exposure: a photographer sets the exposure for the light there is.
"""
import argparse
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "source" / "Spectral.h"

# Hertel, Skladnikiewitz & Schmidt (1997), Figure 4, the 40/60 curve, read off
# the plot at 10 nm. Relative to the value at 380 nm.
SENSITIVITY_NM = 380
SENSITIVITY = [
    1.00,  # 380
    1.02,  # 390
    1.03,  # 400
    1.05,  # 410
    1.00,  # 420
    0.80,  # 430
    0.35,  # 440
    0.22,  # 450
    0.14,  # 460
    0.10,  # 470
    0.08,  # 480
    0.05,  # 490
    0.03,  # 500
    0.01,  # 510
    0.00,  # 520
]

# Smits (1999), the seven basis reflectances, ten bins of 34 nm from 380 nm.
BASIS_START = 380.0
BASIS_END = 720.0
BASIS = {
    "white":   [1.0000, 1.0000, 0.9999, 0.9993, 0.9992, 0.9998, 1.0000, 1.0000, 1.0000, 1.0000],
    "cyan":    [0.9710, 0.9426, 1.0007, 1.0007, 1.0007, 1.0007, 0.1564, 0.0000, 0.0000, 0.0000],
    "magenta": [1.0000, 1.0000, 0.9685, 0.2229, 0.0000, 0.0458, 0.8369, 1.0000, 1.0000, 0.9959],
    "yellow":  [0.0001, 0.0000, 0.1088, 0.6651, 1.0000, 1.0000, 0.9996, 0.9586, 0.9685, 0.9840],
    "red":     [0.1012, 0.0515, 0.0000, 0.0000, 0.0000, 0.0000, 0.8325, 1.0149, 1.0149, 1.0149],
    "green":   [0.0000, 0.0000, 0.0273, 0.7937, 1.0000, 0.9418, 0.1719, 0.0000, 0.0000, 0.0025],
    "blue":    [1.0000, 1.0000, 0.8916, 0.3323, 0.0000, 0.0000, 0.0003, 0.0369, 0.0483, 0.0496],
}
ORDER = ["white", "cyan", "magenta", "yellow", "red", "green", "blue"]

# Planckian colour temperatures, kelvin.
LIGHTS = [("Daylight", 6504.0), ("Tungsten", 2856.0), ("Flash", 5500.0)]


def sensitivity(nm):
    x = (nm - SENSITIVITY_NM) / 10.0
    if x <= 0.0:
        return SENSITIVITY[0]
    i = int(math.floor(x))
    if i >= len(SENSITIVITY) - 1:
        return SENSITIVITY[-1]
    t = x - i
    return SENSITIVITY[i] * (1.0 - t) + SENSITIVITY[i + 1] * t


def basis(name, nm):
    width = (BASIS_END - BASIS_START) / 10.0
    i = int((nm - BASIS_START) / width)
    if nm >= BASIS_END:
        i = 9
    return BASIS[name][max(0, min(9, i))]


def planck(nm, kelvin):
    """Spectral radiance of a black body, in any consistent unit."""
    h = 6.62607015e-34
    c = 2.99792458e8
    k = 1.380649e-23
    lam = nm * 1e-9
    return (2.0 * h * c * c / lam ** 5) / (math.exp(h * c / (lam * k * kelvin)) - 1.0)


def weights(kelvin):
    """k_X for the seven bases under a Planckian light, white normalised to 1."""
    raw = {}
    for name in ORDER:
        total = 0.0
        for nm in range(int(BASIS_START), int(BASIS_END)):
            l = nm + 0.5
            total += sensitivity(l) * planck(l, kelvin) * basis(name, l)
        raw[name] = total
    white = raw["white"]
    return {name: raw[name] / white for name in ORDER}


def complement_check():
    """Smits builds each secondary so that it and its complementary primary sum
    to white in every bin. That is the check on the transcription above."""
    worst = 0.0
    for a, b in (("red", "cyan"), ("green", "magenta"), ("blue", "yellow")):
        for i in range(10):
            worst = max(worst, abs(BASIS[a][i] + BASIS[b][i] - BASIS["white"][i]))
    return worst


def floatLiteral(v):
    """A C++ float literal that always carries a decimal point: %.9g prints 1.0 as '1'."""
    text = "%.9g" % v
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "f"


def render():
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// GENERATED by tools/spectral_weights.py -- do not edit. Run the script.")
    lines.append("//")
    lines.append("// The plate exposure of each of Smits' seven basis reflectances under each")
    lines.append("// light, white normalised to exactly 1. The shader sums these the way Smits")
    lines.append("// sums the spectra. Sources and method are in the script.")
    lines.append("")
    lines.append("namespace wetplate::spectral")
    lines.append("{")
    lines.append("")
    lines.append("constexpr int kLightCount = %d;" % len(LIGHTS))
    lines.append("")
    lines.append("/// Order: white, cyan, magenta, yellow, red, green, blue.")
    lines.append("struct Weights")
    lines.append("{")
    lines.append("\tconst char* light;")
    lines.append("\tfloat kelvin;")
    lines.append("\tfloat w[ 7 ];")
    lines.append("};")
    lines.append("")
    lines.append("constexpr Weights kWeights[ kLightCount ] = {")
    for name, kelvin in LIGHTS:
        w = weights(kelvin)
        values = ", ".join(floatLiteral(w[n]) for n in ORDER)
        lines.append('\t{ "%s", %.1ff, { %s } },' % (name, kelvin, values))
    lines.append("};")
    lines.append("")
    lines.append("/// The sensitivity table as committed, for the record: relative sensitivity")
    lines.append("/// from %d nm at 10 nm steps." % SENSITIVITY_NM)
    lines.append("constexpr int kSensitivityStartNm = %d;" % SENSITIVITY_NM)
    lines.append("constexpr int kSensitivityCount   = %d;" % len(SENSITIVITY))
    lines.append("constexpr float kSensitivity[ kSensitivityCount ] = { %s };"
                 % ", ".join("%.2ff" % s for s in SENSITIVITY))
    lines.append("")
    lines.append("} // namespace wetplate::spectral")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="compare with the committed header, do not write")
    ap.add_argument("--print", action="store_true", help="print the weights")
    args = ap.parse_args()

    worst = complement_check()
    if worst > 0.08:
        print("the Smits table is mistranscribed: a complementary pair misses white by %.4f" % worst)
        return 1

    text = render()
    if args.print:
        for name, kelvin in LIGHTS:
            w = weights(kelvin)
            print("%-9s %6.0f K  " % (name, kelvin) + "  ".join("%s=%.5f" % (n[0].upper(), w[n]) for n in ORDER))
        print("complement check: worst %.4f" % worst)
    if args.check:
        if not HEADER.exists() or HEADER.read_text() != text:
            print("%s does not match what the script computes -- run tools/spectral_weights.py" % HEADER)
            return 1
        print("%s recomputes from the script's tables (complement check %.4f)" % (HEADER.relative_to(ROOT), worst))
        return 0
    HEADER.write_text(text)
    print("wrote %s" % HEADER.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
