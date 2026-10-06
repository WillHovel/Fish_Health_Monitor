#!/usr/bin/env python3
"""
tools/generate_coeffs.py
=========================
Generates CMSIS-DSP IIR biquad filter coefficients for all species profiles
defined in aquatic_biomonitor_fw and outputs copy-pasteable C array literals.

COEFFICIENT FORMAT
------------------
CMSIS-DSP arm_biquad_cascade_df2T_f32() uses Direct-Form II Transposed (DF2T)
topology.  For each biquad stage the coefficient layout is:

    { b0, b1, b2, -a1, -a2 }

Note the sign inversion on a1 and a2 compared to scipy's convention.

USAGE
-----
    pip install scipy numpy
    python tools/generate_coeffs.py                      # print to stdout
    python tools/generate_coeffs.py --output-header      # write to src/filter_coeffs.h
    python tools/generate_coeffs.py --fs 200             # different sample rate
    python tools/generate_coeffs.py --plot               # visualise frequency responses

AUTHOR  : aquatic_biomonitor_fw project
LICENSE : Apache-2.0
"""

import argparse
import sys
from dataclasses import dataclass, field
from typing import List, Tuple

import numpy as np
from scipy.signal import butter, iirnotch, tf2sos, sosfreqz


# ─── Type aliases ────────────────────────────────────────────────────────────
SosMatrix  = np.ndarray   # shape (n_stages, 6)
CmsisCoeff = List[float]  # flat: [b0,b1,b2,-a1,-a2, ...] for all stages


# ─── Conversion helpers ──────────────────────────────────────────────────────

def sos_to_cmsis(sos: SosMatrix) -> CmsisCoeff:
    """
    Convert a scipy SOS matrix to a CMSIS-DSP flat coefficient list.

    Scipy SOS row:  [b0, b1, b2, a0, a1, a2]  where a0 is always 1.0
    CMSIS stage:    [b0, b1, b2, -a1, -a2]      (a0 absorbed, a1/a2 negated)

    :param sos: scipy SOS array of shape (n_stages, 6)
    :return:    flat list of length n_stages * 5
    """
    out: CmsisCoeff = []
    for stage in sos:
        b0, b1, b2, a0, a1, a2 = stage
        if abs(a0 - 1.0) > 1e-9:
            # Normalise if a0 != 1 (should not happen for butter/iirnotch SOS)
            b0, b1, b2, a1, a2 = b0/a0, b1/a0, b2/a0, a1/a0, a2/a0
        out.extend([b0, b1, b2, -a1, -a2])
    return out


def cmsis_to_c_array(name: str,
                     coeffs: CmsisCoeff,
                     n_stages: int,
                     description: str,
                     species_id: int = -1) -> str:
    """
    Format CMSIS coefficients as a C float32_t array literal.

    :param name:        C variable name (e.g. 'glass_catfish_bpf_coeffs')
    :param coeffs:      flat CMSIS coefficient list
    :param n_stages:    number of biquad stages (len(coeffs) == n_stages*5)
    :param description: comment placed above the array
    :param species_id:  if >= 0, emits a matching #define for the profile
    :return:            multi-line C string
    """
    assert len(coeffs) == n_stages * 5, (
        f"Expected {n_stages * 5} coefficients, got {len(coeffs)}"
    )

    lines = [
        f"/* {'═'*66}",
        f" * {description}",
        f" * Stages : {n_stages}",
        f" * Layout : {{b0, b1, b2, -a1, -a2}} per stage (CMSIS DF2T)",
        f" * {'═'*66} */",
    ]

    lines.append(f"static float32_t {name}[{n_stages * 5}] = {{")
    for s in range(n_stages):
        c = coeffs[s*5:(s+1)*5]
        vals = ",  ".join(f"{v:+.8f}f" for v in c)
        lines.append(f"    /* Stage {s}: b0,       b1,       b2,       -a1,      -a2 */")
        lines.append(f"    {vals},")
    lines.append("};")
    return "\n".join(lines)


# ─── Filter design functions ─────────────────────────────────────────────────

def design_notch(f0_hz: float, q: float, fs: float) -> Tuple[SosMatrix, str]:
    """
    Design a 2nd-order IIR notch filter.

    :param f0_hz: centre (notch) frequency in Hz
    :param q:     quality factor (higher Q → narrower stopband)
    :param fs:    sampling frequency in Hz
    :return:      (sos_matrix, description_string)
    """
    b, a = iirnotch(w0=f0_hz, Q=q, fs=fs)
    sos  = tf2sos(b, a)
    desc = (f"Notch filter — f0={f0_hz} Hz, Q={q}, fs={fs} Hz  "
            f"(mains interference / EMG rejection)")
    return sos, desc


def design_bandpass(lo_hz: float, hi_hz: float, order: int,
                    fs: float) -> Tuple[SosMatrix, str]:
    """
    Design a Butterworth bandpass filter.

    :param lo_hz:  lower -3 dB corner frequency in Hz
    :param hi_hz:  upper -3 dB corner frequency in Hz
    :param order:  filter order (SOS stages = order)
    :param fs:     sampling frequency in Hz
    :return:       (sos_matrix, description_string)
    """
    sos  = butter(N=order, Wn=[lo_hz, hi_hz],
                  btype='bandpass', output='sos', fs=fs)
    desc = (f"Bandpass filter — {lo_hz}–{hi_hz} Hz, "
            f"Butterworth order {order*2}, fs={fs} Hz")
    return sos, desc


# ─── Species filter catalogue ────────────────────────────────────────────────

@dataclass
class SpeciesFilter:
    species_id:  int
    c_var_name:  str
    species_name: str
    sos:         SosMatrix
    description: str


def build_filter_catalogue(fs: float) -> List[SpeciesFilter]:
    """
    Define every filter needed by the firmware.

    Returns a list ordered as: [notch, species_1, species_2, species_3, species_4]
    """
    filters = []

    # ── NOTCH (all species) ─────────────────────────────────────────────
    sos, desc = design_notch(f0_hz=25.0, q=12.0, fs=fs)
    filters.append(SpeciesFilter(
        species_id   = 0,
        c_var_name   = "notch_coeffs",
        species_name = "All species (mains notch)",
        sos          = sos,
        description  = desc,
    ))

    # ── SPECIES 1: Glass Catfish  (0.5–1.5 Hz) ─────────────────────────
    sos, desc = design_bandpass(0.5, 1.5, order=2, fs=fs)
    filters.append(SpeciesFilter(
        species_id   = 1,
        c_var_name   = "glass_catfish_bpf_coeffs",
        species_name = "Glass Catfish (Kryptopterus vitreolus)",
        sos          = sos,
        description  = desc + " | HR 30–70 BPM",
    ))

    # ── SPECIES 2: Atlantic Silverside  (1.3–2.5 Hz) ───────────────────
    sos, desc = design_bandpass(1.3, 2.5, order=2, fs=fs)
    filters.append(SpeciesFilter(
        species_id   = 2,
        c_var_name   = "menidia_menidia_bpf_coeffs",
        species_name = "Atlantic Silverside (Menidia menidia)",
        sos          = sos,
        description  = desc + " | HR 80–120 BPM",
    ))

    # ── SPECIES 3: Inland Silverside  (1.3–2.5 Hz) ─────────────────────
    sos, desc = design_bandpass(1.3, 2.5, order=2, fs=fs)
    filters.append(SpeciesFilter(
        species_id   = 3,
        c_var_name   = "menidia_beryllina_bpf_coeffs",
        species_name = "Inland Silverside (Menidia beryllina)",
        sos          = sos,
        description  = desc + " | HR 80–120 BPM",
    ))

    # ── SPECIES 4: Chromis viridis  (1.5–3.0 Hz) ───────────────────────
    sos, desc = design_bandpass(1.5, 3.0, order=2, fs=fs)
    filters.append(SpeciesFilter(
        species_id   = 4,
        c_var_name   = "chromis_viridis_bpf_coeffs",
        species_name = "Chromis viridis (Blue-green Chromis)",
        sos          = sos,
        description  = desc + " | HR 100–160 BPM",
    ))

    return filters


# ─── Optional frequency response plot ────────────────────────────────────────

def plot_responses(filters: List[SpeciesFilter], fs: float) -> None:
    """Plot frequency magnitude responses for all filters on one figure."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("[WARN] matplotlib not installed — skipping plot.", file=sys.stderr)
        return

    fig, axes = plt.subplots(len(filters), 1,
                             figsize=(10, 3 * len(filters)),
                             sharex=True)
    fig.suptitle("Aquatic Biomonitor — Filter Frequency Responses", fontsize=13)

    freqs = np.linspace(0, fs / 2, 4096)

    for ax, flt in zip(axes, filters):
        w, h = sosfreqz(flt.sos, worN=freqs, fs=fs)
        ax.plot(w, 20 * np.log10(np.abs(h) + 1e-12), linewidth=1.5)
        ax.set_ylabel("Gain (dB)")
        ax.set_title(f"[ID={flt.species_id}] {flt.species_name}", fontsize=9)
        ax.set_ylim(-80, 5)
        ax.grid(True, alpha=0.3)
        ax.axhline(-3, color='r', linestyle='--', linewidth=0.8, label='-3 dB')

    axes[-1].set_xlabel("Frequency (Hz)")
    plt.tight_layout()
    plt.show()


# ─── Output formatting ───────────────────────────────────────────────────────

HEADER_TEMPLATE = """\
/*
 * filter_coeffs.h  —  AUTO-GENERATED by tools/generate_coeffs.py
 * ================================================================
 * DO NOT EDIT BY HAND.  Re-run the script after changing filter specs.
 *
 * Sampling frequency : {fs} Hz
 * Coefficient format : CMSIS-DSP DF2T  {{b0, b1, b2, -a1, -a2}} per stage
 *
 * Usage in main.c:
 *   #include "filter_coeffs.h"
 *   // Replace the placeholder arrays with the arrays below.
 *   // Paste each species array into the matching fish_profile_t .bpf_coeffs
 *   // field and the notch_coeffs[] array into the global notch section.
 */
#pragma once
#include <arm_math.h>

"""


def generate_output(filters: List[SpeciesFilter], fs: float) -> str:
    """Assemble the complete C output string."""
    sections = [HEADER_TEMPLATE.format(fs=fs)]
    for flt in filters:
        n_stages = flt.sos.shape[0]
        coeffs   = sos_to_cmsis(flt.sos)
        sections.append(
            cmsis_to_c_array(
                name        = flt.c_var_name,
                coeffs      = coeffs,
                n_stages    = n_stages,
                description = flt.description,
                species_id  = flt.species_id,
            )
        )
        sections.append("")   # blank line between blocks

    # Append a quick-reference paste guide
    sections.append("/*")
    sections.append(" * PASTE GUIDE — copy each block into main.c:")
    sections.append(" *")
    for flt in filters:
        n_stages = flt.sos.shape[0]
        if flt.species_id == 0:
            sections.append(f" *   notch_coeffs[]           → global notch_coeffs[{n_stages*5}]")
        else:
            sections.append(
                f" *   {flt.c_var_name:<36}"
                f"→ PROFILE_xxx.bpf_coeffs[{n_stages*5}]"
            )
    sections.append(" */")
    return "\n".join(sections)


# ─── Verification (print poles/zeros & gain at passband centre) ──────────────

def verify_filters(filters: List[SpeciesFilter], fs: float) -> None:
    """Print passband gain and stopband attenuation for each filter."""
    print("\n── Verification ──────────────────────────────────────────────")
    for flt in filters:
        freqs = np.linspace(0, fs/2, 10000)
        w, h  = sosfreqz(flt.sos, worN=freqs, fs=fs)
        gain_db = 20 * np.log10(np.abs(h) + 1e-12)

        if flt.species_id == 0:
            # Notch: check attenuation at f0=25 Hz
            idx_f0   = np.argmin(np.abs(w - 25.0))
            atten_db = gain_db[idx_f0]
            print(f"  [ID=0] Notch @ 25 Hz: {atten_db:.1f} dB  (target < -20 dB)")
        else:
            # BPF: check gain at passband centre
            # Centre between -3 dB corners can be found from the SOS
            f_centre = np.sqrt(flt.sos[0, 0] / flt.sos[0, 2]) if False else None
            # Simpler: find frequency of maximum gain
            idx_peak = np.argmax(gain_db)
            f_peak   = w[idx_peak]
            g_peak   = gain_db[idx_peak]
            # Check DC (should be strongly attenuated)
            g_dc     = gain_db[0]
            print(f"  [ID={flt.species_id}] {flt.species_name[:30]:<30}"
                  f"  peak={g_peak:.1f} dB @ {f_peak:.2f} Hz  |  DC={g_dc:.1f} dB")
    print()


# ─── Entry point ─────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate CMSIS-DSP filter coefficients for aquatic_biomonitor_fw",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--fs", type=float, default=100.0, metavar="HZ",
        help="Sampling frequency in Hz (default: 100.0)",
    )
    parser.add_argument(
        "--output-header", action="store_true",
        help="Write output to src/filter_coeffs.h instead of stdout",
    )
    parser.add_argument(
        "--plot", action="store_true",
        help="Show matplotlib frequency response plots",
    )
    parser.add_argument(
        "--verify", action="store_true", default=True,
        help="Print passband gain verification table (default: on)",
    )
    args = parser.parse_args()

    filters = build_filter_catalogue(fs=args.fs)
    output  = generate_output(filters, fs=args.fs)

    if args.output_header:
        import os
        out_path = os.path.join(
            os.path.dirname(__file__), "..", "src", "filter_coeffs.h"
        )
        with open(out_path, "w", encoding="utf-8") as fp:
            fp.write(output)
        print(f"[OK] Written to {out_path}")
    else:
        print(output)

    if args.verify:
        verify_filters(filters, fs=args.fs)

    if args.plot:
        plot_responses(filters, fs=args.fs)


if __name__ == "__main__":
    main()
