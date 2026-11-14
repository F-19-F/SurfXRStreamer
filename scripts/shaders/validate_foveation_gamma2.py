#!/usr/bin/env python3
"""Validate Gamma2 sharpening mathematics without Vulkan or third-party modules.

Reads production coefficients. Analytic sRGB, derivative extrema, and adversarial
colors are independent references; CPU FP32 models do not certify GPU precision.
Run: python scripts/shaders/validate_foveation_gamma2.py
"""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import struct

NUMBER = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
TOE_LINEAR = .0031308
TOE = math.sqrt(TOE_LINEAR)


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def clamp(x, lo=0., hi=1.):
    return min(max(x, lo), hi)


def encoded(q):
    """Independent exact sRGB transfer after the candidate's q*q output."""
    linear = clamp(q) ** 2
    return 12.92 * linear if linear <= TOE_LINEAR else 1.055 * linear ** (1 / 2.4) - .055


def polynomial(coeff, q, fp32=False):
    value = coeff[-1]
    for c in reversed(coeff[:-1]):
        value = f32(f32(value * q) + c) if fp32 else value * q + c
    return value


def source_values(path):
    source = re.sub(r'//[^\n]*', '', path.read_text(encoding='utf-8'))
    def body(name):
        match = re.search(r'float\s+' + name + r'\([^)]*\)\s*\{([^}]+)\}', source)
        require(match is not None, 'Missing production function: ' + name)
        return match[1]
    def number(pattern, text):
        match = re.search(pattern.replace('@', '(' + NUMBER + ')'), text)
        require(match is not None, 'Unrecognized shader expression: ' + pattern)
        return float(match[1])
    def cubic(text):
        match = re.search(r'\(\(\s*(' + NUMBER + r')\s*\*q\s*(' + NUMBER +
                          r')\)\s*\*q\s*(' + NUMBER + r')\)\s*\*q\s*(' + NUMBER + ')', text)
        require(match is not None, 'Unrecognized production cubic')
        return tuple(f32(float(v)) for v in reversed(match.groups()))
    proxy, cap, delta = (body('gamma2_' + name) for name in ('green_proxy', 'sharpen_cap', 'sharpen_delta'))
    return dict(proxy=cubic(proxy), inverse=cubic(cap), toe=number(r'if\(q<=@\)', proxy),
                interval=number(r'base\.b\)\)-@,', cap), maximum=number(r',@\);\s*$', cap),
                slope=number(r'1\.0/@,', cap), weight=number(r'float kt=@\*', delta),
                denominator_floor=number(r'4\.0\*kt,@\)', delta),
                code_limit=number(r'\(\s*@/255\.0\)', cap))


def roots_on_monotone_intervals(function, turns):
    result = []
    points = [TOE] + sorted(q for q in turns if TOE < q < 1) + [1.]
    for lo, hi in zip(points, points[1:]):
        a, b = function(lo), function(hi)
        if a * b >= 0:
            continue
        for _ in range(60):
            mid = (lo + hi) / 2
            if function(lo) * function(mid) <= 0:
                hi = mid
            else:
                lo = mid
        result.append((lo + hi) / 2)
    return result


def validate(values):
    pc, ic = values['proxy'], values['inverse']
    slope, limit = values['slope'], values['code_limit'] / 255
    floor = 1 / slope
    require(abs(values['toe'] - TOE) < 1e-12, 'Toe must match analytic sRGB')
    require(values['code_limit'] == 23 and 0 < values['weight'] < .25, 'Invalid CAS range')
    require(0 < values['maximum'] <= values['interval'], 'Cap exceeds its interval bound')
    require(slope > 25.84 * TOE, 'Global slope misses the left side of the toe')

    def proxy(q):
        if q <= values['toe']:
            return 12.92 * q * q
        return 1. if q >= 1 else clamp(polynomial(pc, q))

    def cap(rgb):
        q = max(min(rgb) - values['interval'], values['toe'])
        return min(limit * max(floor, polynomial(ic, q)), values['maximum'])

    # g=1/f'(q)-P(q). Its third derivative is strictly decreasing, so recursive
    # monotonic intervals find every interior extremum, rather than trusting a grid.
    a = 1 / (1.055 / 1.2)
    g3 = lambda q: a * 55 / 216 * q ** (-17 / 6) - 6 * ic[3]
    g2 = lambda q: -a * 5 / 36 * q ** (-11 / 6) - 2 * ic[2] - 6 * ic[3] * q
    g1 = lambda q: a / 6 * q ** (-5 / 6) - ic[1] - 2 * ic[2] * q - 3 * ic[3] * q * q
    gap = lambda q: a * q ** (1 / 6) - polynomial(ic, q)
    extrema = roots_on_monotone_intervals(g1, roots_on_monotone_intervals(g2, roots_on_monotone_intervals(g3, [])))
    margin = min(gap(q) for q in [TOE, 1.] + extrema)
    # Three multiplies and adds; constants already rounded to float32. FMA has
    # fewer rounding steps. Reserve another 1e-6 for q-coordinate rounding.
    unit = 2 ** -24
    horner_bound = (6 * unit / (1 - 6 * unit)) * sum(abs(c) for c in ic)
    require(margin > horner_bound + 1e-6, 'Inverse polynomial is not a conservative FP32 lower bound')
    require(polynomial(ic, TOE) <= floor, 'Intervals crossing the toe need the global slope cap')
    require(cap((1., 1., 1.)) <= values['interval'], 'Maximum cap breaks interval argument')

    proxy_error = 0.
    previous = -1.
    for i in range(32769):
        q = f32(i / 32768)
        p = 12.92 * q * q if q <= TOE else (1. if q == 1 else clamp(polynomial(pc, q, True)))
        require(p >= previous, 'Proxy is not monotonic on the FP32 regression grid')
        proxy_error = max(proxy_error, abs(p - encoded(q)) * 255)
        previous = p
    require(proxy_error < 1., 'Proxy error exceeds one encoded code on the regression grid')
    require(proxy(0) == 0 and proxy(1) == 1, 'Proxy endpoints changed')

    # Both signs and colored channels: any q channel may be greater than the
    # minRGB that chooses the common cap. Explicit toe crossings supplement grid.
    halo_max = 0.
    for i in range(257):
        minimum = i / 256
        c = cap((minimum,) * 3)
        probes = [minimum + (1 - minimum) * j / 256 for j in range(257)]
        probes += [TOE + d for d in (-c, 0, c, -1e-7, 1e-7)]
        for q in probes:
            if minimum <= q <= 1:
                for sign in (-1, 1):
                    change = 255 * abs(encoded(q + sign * c) - encoded(q))
                    halo_max = max(halo_max, change)
    require(halo_max <= 23 + 1e-6, 'Adaptive cap exceeds 23 encoded codes')

    def delta(rgb, neighbors, strength):
        lo, hi = proxy(min(rgb[1], *neighbors)), proxy(max(rgb[1], *neighbors))
        t, k = min(lo, 1 - hi), values['weight'] * strength
        denominator = math.sqrt(hi * t) - 4 * k * t
        require(t >= 0 and denominator >= -1e-15, 'Invalid proxy extrema or CAS denominator')
        value = k * t * (4 * rgb[1] - sum(neighbors)) / max(denominator, values['denominator_floor'])
        require(math.isfinite(value), 'Nonfinite CAS result')
        return clamp(value, -cap(rgb), cap(rgb))

    def legacy(rgb, neighbors, strength):
        # Independent original E8 working domain and unrearranged CAS formula.
        b = [math.floor(encoded(v) * 255 + .5) / 255 for v in rgb]
        n = [math.floor(encoded(v) * 255 + .5) / 255 for v in neighbors]
        lo, hi = min(b[1], *n), max(b[1], *n)
        amp = math.sqrt(max(0, min(lo, 1 - hi)) / max(hi, 1e-6))
        w = -.175 * strength * amp
        d = clamp((b[1] + w * sum(n)) / (1 + 4 * w) - b[1], -23 / 255, 23 / 255)
        return [clamp(v + d) for v in b]

    comparisons = {s: dict(channels=0, absolute=0., maximum=0., reference_square=0., cross=0.) for s in (.35, .7, 1.)}
    cases = 0
    levels = (0., 1e-8, 1e-5, TOE - 1e-7, TOE, TOE + 1e-7, .2, .5, .9, 1 - 1e-6, 1.)
    for center in levels:
        for strength in (0., .35, .7, 1.):
            require(delta((center,) * 3, (center,) * 4, strength) == 0, 'Flat fields must remain unchanged')
        for neighbors in itertools.product((0., TOE, .25, .5, 1.), repeat=4):
            for rgb in ((center,) * 3, (0., center, 1.), (TOE - 1e-7, center, TOE + 1e-7)):
                for strength, stats in comparisons.items():
                    d = delta(rgb, neighbors, strength)
                    require(delta(rgb, neighbors, 0.) == 0, 'Zero strength changed output')
                    if min(center, *neighbors) == 0 or max(center, *neighbors) == 1:
                        require(d == 0, 'Known zero-amplitude extrema changed output')
                    result = [encoded(v + d) for v in rgb]
                    reference = legacy(rgb, neighbors, strength)
                    for q, new, old in zip(rgb, result, reference):
                        require(abs(new - encoded(q)) * 255 <= 23 + 1e-6, 'Adversarial RGB exceeds cap')
                        error = abs(new - old) * 255
                        before = encoded(q)
                        stats['channels'] += 1
                        stats['absolute'] += error
                        stats['maximum'] = max(stats['maximum'], error)
                        stats['reference_square'] += (old - before) ** 2
                        stats['cross'] += (new - before) * (old - before)
                    cases += 1
    for stats in comparisons.values():
        stats['mae_codes'] = stats.pop('absolute') / stats['channels']
        stats['effect_gain'] = stats.pop('cross') / stats.pop('reference_square')
    return dict(proxy_grid_max_error_codes=proxy_error, inverse_extrema=extrema,
                inverse_minimum_margin=margin, fp32_horner_error_bound=horner_bound,
                continuous_halo_grid_max_codes=halo_max, adversarial_cases=cases,
                legacy_style_diagnostics=comparisons)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shader', type=Path, default=Path(__file__).resolve().parents[2] / "surfxr" / 'frame/xr/shaders/foveation_gamma2.glsl')
    args = parser.parse_args()
    values = source_values(args.shader)
    result = validate(values)
    print(json.dumps(dict(passed=True, shader_sha256=hashlib.sha256(args.shader.read_bytes()).hexdigest(),
                         coefficients=values, checks=result,
                         limits='Analytic/deterministic CPU checks, not GPU precision or perceptual certification; legacy style diagnostics are not pass/fail quality gates.'), indent=2))


if __name__ == '__main__':
    main()
