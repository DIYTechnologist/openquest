"""Bounded known-orientation translation search without common stereo LEDs.

Every seed uses two distinct LEDs and two nonparallel observed rays. Acceptance
still requires at least four distinct LEDs and a separated optical hypothesis.
No reference poses or previous translation are used in this search.
"""
import numpy as np
from gyro_optical import match_views, refine_optical


def reacquire_translation(model, normals, orientation, views, diagnostics=None):
    observations = []
    for T, rays in views:
        for ray in rays:
            d = T[:3, :3] @ ray
            length = np.linalg.norm(d)
            if not np.isfinite(d).all() or length < 1e-9:
                continue
            observations.append((T[:3, 3], d / length))
    # Explicit work bound: clutter must not cause unbounded pair enumeration.
    if len(observations) < 4 or len(observations) > 32:
        return None
    rotated = model @ orientation.T
    mi, mj = np.where(~np.eye(len(model), dtype=bool))
    candidates = []
    for i, (oi, di) in enumerate(observations):
        Pi = np.eye(3) - np.outer(di, di)
        for oj, dj in observations[i+1:]:
            if abs(di @ dj) > .9995:
                continue
            Pj = np.eye(3) - np.outer(dj, dj)
            A = Pi + Pj
            if np.linalg.cond(A) > 2000:
                continue
            rhs = (oi - rotated[mi]) @ Pi + (oj - rotated[mj]) @ Pj
            translations = np.linalg.solve(A, rhs.T).T
            pi, pj = rotated[mi] + translations - oi, rotated[mj] + translations - oj
            gap = np.linalg.norm(pi @ Pi, axis=1) + np.linalg.norm(pj @ Pj, axis=1)
            valid = ((pi @ di > .08) & (pj @ dj > .08) &
                     (np.linalg.norm(translations, axis=1) < 2.5) & (gap < .008))
            candidates.extend(translations[valid])
    # Quantization only deduplicates seeds, never supplies a measured position.
    if not candidates:
        return None
    candidates = np.asarray(candidates)
    _, indices = np.unique(np.round(candidates / .004).astype(int), axis=0, return_index=True)
    scored = []
    for t in candidates[indices]:
        matches = match_views(model, normals, orientation, t, views, gate=.012)
        unique = {m for _, _, pairs in matches for m, _ in pairs}
        if len(unique) < 4:
            continue
        residuals = []
        for T, rays, pairs in matches:
            p = (rotated + t - T[:3, 3]) @ T[:3, :3]
            residuals.extend(np.linalg.norm(p[m, :2]/p[m, 2] - rays[b, :2]/rays[b, 2]) for m,b in pairs)
        scored.append((-len(unique), float(np.mean(residuals)), t))
    scored.sort(key=lambda x: x[:2])
    refined = []
    shortlist = []
    for _, _, t in scored:
        if any(np.linalg.norm(t-old) < .01 for old in shortlist):
            continue
        shortlist.append(t)
        if len(shortlist) == 48:
            break
    for t in shortlist:
        result = refine_optical(model, normals, orientation, t, views)
        if result is None or result[3] < 4 or result[2] > .003:
            continue
        if any(np.linalg.norm(result[1]-old[1]) < .01 for old in refined):
            continue
        refined.append(result)
    refined.sort(key=lambda r: (-r[3], r[2]))
    if diagnostics is not None:
        diagnostics.update(translation_seeds=len(indices), translation_solutions=len(refined))
    if not refined:
        return None
    best = refined[0]
    # Refuse geometrically distinct, similarly supported alternatives.
    if any(r[3] >= best[3]-1 and r[2] <= best[2] + .0015 for r in refined[1:]):
        return None
    return best


class TranslationConfirmation:
    """Require three consecutive, speed-bounded optical candidates before use."""
    def __init__(self, speed_limit=3., confirmations=3):
        self.speed_limit = speed_limit
        self.confirmations = confirmations
        self.reset()

    def reset(self):
        self.position = None
        self.timestamp = None
        self.count = 0

    def observe(self, position, timestamp, last_position=None, last_timestamp=None):
        position = np.asarray(position)
        if not np.isfinite(position).all():
            self.reset()
            return False
        dt = timestamp-self.timestamp if self.timestamp is not None else 0
        consistent = (self.position is not None and 0 < dt < .12 and
                      np.linalg.norm(position-self.position) <= self.speed_limit*dt+.01)
        self.count = self.count+1 if consistent else 1
        self.position, self.timestamp = position.copy(), timestamp
        if last_position is not None:
            elapsed = timestamp-last_timestamp
            if elapsed <= 0 or np.linalg.norm(position-last_position) > self.speed_limit*elapsed+.01:
                self.reset()
                return False
        return self.count >= self.confirmations
