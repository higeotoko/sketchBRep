"""TIN height lookup and draping of 2D polylines onto it (split at every TIN edge crossing). docs/86."""
import math

class TIN:
    def __init__(self, P, Z, tris):
        self.P, self.Z, self.tris = P, Z, tris
        self.edges = sorted({tuple(sorted(e)) for t in tris for e in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0]))})

    def z(self, x, y, eps=1e-7):
        P, Z = self.P, self.Z
        for t in self.tris:
            (x1, y1), (x2, y2), (x3, y3) = (P[i] for i in t)
            d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
            l1 = ((y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)) / d
            l2 = ((y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)) / d
            l3 = 1 - l1 - l2
            if l1 >= -eps and l2 >= -eps and l3 >= -eps:
                return l1 * Z[t[0]] + l2 * Z[t[1]] + l3 * Z[t[2]]
        return None

    @staticmethod
    def _inter(p1, p2, p3, p4):
        d = (p2[0] - p1[0]) * (p4[1] - p3[1]) - (p2[1] - p1[1]) * (p4[0] - p3[0])
        if abs(d) < 1e-12: return None
        t = ((p3[0] - p1[0]) * (p4[1] - p3[1]) - (p3[1] - p1[1]) * (p4[0] - p3[0])) / d
        u = ((p3[0] - p1[0]) * (p2[1] - p1[1]) - (p3[1] - p1[1]) * (p2[0] - p1[0])) / d
        return t if 0 < t < 1 and -1e-9 <= u <= 1 + 1e-9 else None

    def drape(self, pts2, closed=False):
        """Split the 2D polyline at every TIN edge crossing and lift it onto the TIN.
        Parts outside the TIN are dropped (no extrapolation). Returns (runs, coverage)."""
        pts2 = [tuple(p[:2]) for p in pts2] + ([tuple(pts2[0][:2])] if closed else [])
        runs, cur = [], []
        total = 0.0; covered = 0.0
        for a, b in zip(pts2, pts2[1:]):
            L = math.dist(a, b); total += L
            ts = sorted({0.0, 1.0} | {t for e in self.edges if (t := self._inter(a, b, self.P[e[0]], self.P[e[1]])) is not None})
            for t0, t1 in zip(ts, ts[1:]):
                m = (a[0] + (t0 + t1) / 2 * (b[0] - a[0]), a[1] + (t0 + t1) / 2 * (b[1] - a[1]))
                if self.z(*m) is None:
                    if cur: runs.append(cur); cur = []
                    continue
                s0 = (a[0] + t0 * (b[0] - a[0]), a[1] + t0 * (b[1] - a[1]))
                s1 = (a[0] + t1 * (b[0] - a[0]), a[1] + t1 * (b[1] - a[1]))
                z0, z1 = self.z(*s0, eps=1e-6), self.z(*s1, eps=1e-6)
                if z0 is None or z1 is None:
                    if cur: runs.append(cur); cur = []
                    continue
                q0 = [round(s0[0], 2), round(s0[1], 2), round(z0, 1)]
                q1 = [round(s1[0], 2), round(s1[1], 2), round(z1, 1)]
                if not cur or cur[-1][:2] != q0[:2]:
                    if cur: runs.append(cur)
                    cur = [q0]
                cur.append(q1); covered += (t1 - t0) * L
        if cur: runs.append(cur)
        runs = [r for r in runs if len(r) >= 2]
        return runs, (covered / total if total else 0.0)
