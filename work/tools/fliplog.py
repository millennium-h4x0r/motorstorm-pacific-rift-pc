#!/usr/bin/env python3
"""fliplog.py <file> [start_s] [secs]: frame-interval stats from PS3RECOMP_FLIPLOG.
start_s/secs select a window relative to the first logged present."""
import sys
t = [int(l) for l in open(sys.argv[1]) if l.strip()]
if len(t) < 3: sys.exit('too few presents')
s0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
w = [x for x in t if s0 <= (x - t[0]) / 1e6 < s0 + dur]
iv = sorted((b - a) / 1000.0 for a, b in zip(w, w[1:]))
n = len(iv)
pct = lambda p: iv[min(n - 1, int(p / 100.0 * n))]
mean = sum(iv) / n
sd = (sum((x - mean) ** 2 for x in iv) / n) ** 0.5
span = (w[-1] - w[0]) / 1e6
print('%d frames in %.1f s = %.1f fps | interval ms: mean %.1f sd %.1f p50 %.1f p90 %.1f p99 %.1f max %.1f | >50ms: %d'
      % (n, span, n / span, mean, sd, pct(50), pct(90), pct(99), iv[-1], sum(1 for x in iv if x > 50)))
