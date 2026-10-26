/* ephem.js - the maths the watch would run, in JS so the draft shows real positions.
 * Planets: JPL "Approximate Positions of the Planets", Table 1 (valid 1800-2050), Standish.
 * Moon: Astronomical Almanac low-precision series (~0.3 deg). Galilean moons: Meeus ch. 44.
 * Rotation: IAU WGCCRE pole + prime meridian. Everything here is a few hundred lines of C. */
(function (root) {
'use strict';
const D2R = Math.PI / 180, R2D = 180 / Math.PI;
const sin = (d) => Math.sin(d * D2R), cos = (d) => Math.cos(d * D2R);
const norm360 = (a) => ((a % 360) + 360) % 360;
const norm180 = (a) => { a = norm360(a); return a > 180 ? a - 360 : a; };

/* a, e, I, L, long.peri, long.node + rates per Julian century (J2000 ecliptic) */
const EL = {
  mercury: [0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,
            0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081],
  venus:   [0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,
            0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418],
  earth:   [1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
            0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0],
  mars:    [1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,
            0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343],
  jupiter: [5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
            -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106],
  saturn:  [9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,
            -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794],
  uranus:  [19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,
            -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589],
  neptune: [30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,
            0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664],
};
const PLANETS = ['mercury', 'venus', 'earth', 'mars', 'jupiter', 'saturn', 'uranus', 'neptune'];

const jdOf = (ms) => ms / 86400000 + 2440587.5;
const msOf = (jd) => (jd - 2440587.5) * 86400000;

function kepler(Mdeg, e) {
  let M = norm180(Mdeg) * D2R, E = M + e * Math.sin(M);
  for (let i = 0; i < 8; i++) {
    const dE = (M - (E - e * Math.sin(E))) / (1 - e * Math.cos(E));
    E += dE; if (Math.abs(dE) < 1e-9) break;
  }
  return E;
}

/* heliocentric ecliptic J2000, au */
function helio(name, jd) {
  const T = (jd - 2451545.0) / 36525, k = EL[name];
  const a = k[0] + k[6] * T, e = k[1] + k[7] * T, I = k[2] + k[8] * T;
  const L = k[3] + k[9] * T, wb = k[4] + k[10] * T, O = k[5] + k[11] * T;
  const w = wb - O, E = kepler(L - wb, e);
  const xp = a * (Math.cos(E) - e), yp = a * Math.sqrt(1 - e * e) * Math.sin(E);
  const cw = cos(w), sw = sin(w), cO = cos(O), sO = sin(O), cI = cos(I), sI = sin(I);
  return {
    x: (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
    y: (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
    z: (sw * sI) * xp + (cw * sI) * yp,
  };
}

/* the orbit as a closed polyline (for drawing true-shape orbits) */
function orbit(name, jd, n) {
  const T = (jd - 2451545.0) / 36525, k = EL[name], out = [];
  const a = k[0] + k[6] * T, e = k[1] + k[7] * T, I = k[2] + k[8] * T;
  const wb = k[4] + k[10] * T, O = k[5] + k[11] * T, w = wb - O;
  const cw = cos(w), sw = sin(w), cO = cos(O), sO = sin(O), cI = cos(I), sI = sin(I);
  for (let i = 0; i < n; i++) {
    const E = (i / n) * 2 * Math.PI;
    const xp = a * (Math.cos(E) - e), yp = a * Math.sqrt(1 - e * e) * Math.sin(E);
    out.push([(cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
              (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
              (sw * sI) * xp + (cw * sI) * yp]);
  }
  return out;
}

const len = (v) => Math.hypot(v.x, v.y, v.z);
const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const angle = (a, b) => Math.acos(Math.max(-1, Math.min(1, (a.x * b.x + a.y * b.y + a.z * b.z) / (len(a) * len(b))))) * R2D;

/* the Moon, geocentric ecliptic (deg, km) */
function moon(jd) {
  const d = jd - 2451545.0;
  const L = 218.316 + 13.176396 * d, M = 134.963 + 13.064993 * d, F = 93.272 + 13.229350 * d;
  const D = 297.850 + 12.190749 * d, Ms = 357.529 + 0.98560028 * d;
  const lon = L + 6.289 * sin(M) + 1.274 * sin(2 * D - M) + 0.658 * sin(2 * D) + 0.214 * sin(2 * M)
            - 0.186 * sin(Ms) - 0.114 * sin(2 * F);
  const lat = 5.128 * sin(F) + 0.281 * sin(M + F) - 0.278 * sin(F - M) - 0.173 * sin(F - 2 * D);
  const km = 385001 - 20905 * cos(M) - 3699 * cos(2 * D - M) - 2956 * cos(2 * D);
  return { lon: norm360(lon), lat, km };
}

/* any body at heliocentric h, as seen from Earth at E: distance, sky position, elongation, phase.
 * Planets, Pluto, asteroids and comets all go through here, so every card gets the same fields. */
function observe(h, E) {
  const sun = { x: -E.x, y: -E.y, z: -E.z }, b = { helio: h, hlon: norm360(Math.atan2(h.y, h.x) * R2D), r: len(h) };
  const g = sub(h, E);
  b.geo = g; b.dist = len(g);
  b.lon = norm360(Math.atan2(g.y, g.x) * R2D); b.lat = Math.asin(g.z / b.dist) * R2D;
  b.elong = angle(g, sun);
  b.side = norm180(b.lon - norm360(Math.atan2(sun.y, sun.x) * R2D)) > 0 ? 'evening' : 'morning';
  b.phase = angle(sub(E, h), { x: -h.x, y: -h.y, z: -h.z }); /* sun-body-earth angle */
  b.lit = (1 + Math.cos(b.phase * D2R)) / 2;
  return b;
}

/* everything seen from Earth at one instant */
function sky(jd) {
  const E = helio('earth', jd), sun = { x: -E.x, y: -E.y, z: -E.z }, out = { jd, earth: E, bodies: {} };
  for (const p of PLANETS) {
    const h = helio(p, jd);
    out.bodies[p] = p === 'earth' ? { helio: h, hlon: norm360(Math.atan2(h.y, h.x) * R2D), r: len(h) } : observe(h, E);
  }
  out.sunLon = norm360(Math.atan2(sun.y, sun.x) * R2D);
  const m = moon(jd), sl = out.sunLon;
  out.moon = m;
  out.moonElong = Math.acos(cos(m.lat) * cos(m.lon - sl)) * R2D;
  out.moonLit = (1 - cos(out.moonElong)) / 2;
  out.moonWaxing = norm180(m.lon - sl) > 0;
  return out;
}

/* Galilean moons, Meeus ch. 44 (low accuracy, ~0.1 deg in u): u = angle from superior conjunction */
function galilean(jd) {
  const d = jd - 2451545.0;
  const V = 172.74 + 0.00111588 * d, M = 357.529 + 0.9856003 * d;
  const N = 20.020 + 0.0830853 * d + 0.329 * sin(V), J = 66.115 + 0.9025179 * d - 0.329 * sin(V);
  const A = 1.915 * sin(M) + 0.020 * sin(2 * M), B = 5.555 * sin(N) + 0.168 * sin(2 * N), K = J + A - B;
  const R = 1.00014 - 0.01671 * cos(M) - 0.00014 * cos(2 * M);
  const r = 5.20872 - 0.25208 * cos(N) - 0.00611 * cos(2 * N);
  const dl = Math.sqrt(r * r + R * R - 2 * r * R * cos(K)), psi = Math.asin(R / dl * sin(K)) * R2D;
  const t = d - dl / 173;
  let u1 = 163.8069 + 203.4058646 * t + psi - B, u2 = 358.4140 + 101.2916335 * t + psi - B;
  let u3 = 5.7176 + 50.2345180 * t + psi - B, u4 = 224.8092 + 21.4879800 * t + psi - B;
  const G = 331.18 + 50.310482 * t, H = 87.45 + 21.569231 * t;
  u1 += 0.473 * sin(2 * (u1 - u2)); u2 += 1.065 * sin(2 * (u2 - u3)); u3 += 0.165 * sin(G); u4 += 0.843 * sin(H);
  return [
    { name: 'Io', u: norm360(u1), r: 5.9057 - 0.0244 * cos(2 * (u1 - u2)), period: 1.769 },
    { name: 'Europa', u: norm360(u2), r: 9.3966 - 0.0882 * cos(2 * (u2 - u3)), period: 3.551 },
    { name: 'Ganymede', u: norm360(u3), r: 14.9883 - 0.0216 * cos(G), period: 7.155 },
    { name: 'Callisto', u: norm360(u4), r: 26.3627 - 0.1939 * cos(H), period: 16.689 },
  ];
}

/* IAU pole (RA, Dec) and prime meridian W0 + rate (deg/day). Pole drift ignored. */
const IAU = {
  sun: [286.13, 63.87, 84.176, 14.1844000], mercury: [281.0103, 61.4155, 329.5988, 6.1385108],
  venus: [272.76, 67.16, 160.20, -1.4813688], earth: [0, 90, 190.147, 360.9856235],
  moon: [269.9949, 66.5392, 38.3213, 13.17635815], mars: [317.269202, 54.432516, 176.049863, 350.891982443297],
  jupiter: [268.056595, 64.495303, 284.95, 870.5360000], saturn: [40.589, 83.537, 38.90, 810.7939024],
  uranus: [257.311, -15.175, 203.81, -501.1600928], neptune: [299.36, 43.46, 249.978, 541.1397757],
};
const EPS = 23.43928;
const ecl2eq = (v) => ({ x: v.x, y: cos(EPS) * v.y - sin(EPS) * v.z, z: sin(EPS) * v.y + cos(EPS) * v.z });

/* a direction (equatorial) expressed in the body-fixed frame: returns planetocentric lat/lon (east) */
function bodyFixed(name, jd, vEq) {
  const [ra, de, W0, Wd] = IAU[name], W = W0 + Wd * (jd - 2451545.0);
  /* rotate: Rz(W) Rx(90-de) Rz(90+ra) */
  const a1 = (90 + ra) * D2R, c1 = Math.cos(a1), s1 = Math.sin(a1);
  let x = c1 * vEq.x + s1 * vEq.y, y = -s1 * vEq.x + c1 * vEq.y, z = vEq.z;
  const a2 = (90 - de) * D2R, c2 = Math.cos(a2), s2 = Math.sin(a2);
  const y2 = c2 * y + s2 * z, z2 = -s2 * y + c2 * z; y = y2; z = z2;
  const a3 = W * D2R, c3 = Math.cos(a3), s3 = Math.sin(a3);
  const x3 = c3 * x + s3 * y, y3 = -s3 * x + c3 * y;
  const n = Math.hypot(x3, y3, z);
  return { lat: Math.asin(z / n) * R2D, lon: norm360(Math.atan2(y3, x3) * R2D) };
}

/* where the observer and the Sun sit over a body: sub-observer and sub-solar points */
function geometry(name, jd, obsLonLat) {
  const E = helio('earth', jd);
  let P;
  if (name === 'earth') P = E;
  else if (name === 'moon') {
    const m = moon(jd), au = m.km / 149597870.7;
    P = { x: E.x + au * cos(m.lat) * cos(m.lon), y: E.y + au * cos(m.lat) * sin(m.lon), z: E.z + au * sin(m.lat) };
  } else if (name === 'sun') P = { x: 0, y: 0, z: 0 };
  else P = helio(name, jd);
  const toSun = ecl2eq({ x: -P.x, y: -P.y, z: -P.z });
  const sunPt = name === 'sun' ? { lat: 0, lon: 0 } : bodyFixed(name, jd, toSun);
  let obs;
  /* Earth: centred on you (GPS on the watch); without a location, just past the evening terminator's day side */
  if (name === 'earth') obs = obsLonLat ? { lat: obsLonLat[1], lon: norm360(obsLonLat[0]) } : { lat: 18, lon: norm360(sunPt.lon + 40) };
  else obs = bodyFixed(name, jd, ecl2eq(sub(E, P)));
  return { obs, sun: sunPt };
}

/* rotate an in-plane point (x' toward periapsis) by w, i, node into the reference frame */
function rotate(xp, yp, i, node, w) {
  const cw = cos(w), sw = sin(w), cO = cos(node), sO = sin(node), cI = cos(i), sI = sin(i);
  return { x: (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
           y: (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
           z: (sw * sI) * xp + (cw * sI) * yp };
}

/* any asteroid or comet from {e, q, i, node, w, tp}: ellipse, parabola or hyperbola. Heliocentric au. */
const GK = 0.01720209895;
function conic(o, jd) {
  const t = jd - o.tp, e = o.e, q = o.q;
  let xp, yp;
  if (Math.abs(e - 1) < 1e-3) {                       /* near-parabolic: Barker's equation */
    const W = 3 * GK * t / (Math.SQRT2 * Math.pow(q, 1.5)), Y = Math.cbrt(W / 2 + Math.sqrt(W * W / 4 + 1));
    const s = Y - 1 / Y, r = q * (1 + s * s), nu = 2 * Math.atan(s);
    xp = r * Math.cos(nu); yp = r * Math.sin(nu);
  } else if (e < 1) {
    const a = q / (1 - e), E = kepler(GK / Math.pow(a, 1.5) * t * R2D, e);
    xp = a * (Math.cos(E) - e); yp = a * Math.sqrt(1 - e * e) * Math.sin(E);
  } else {
    const a = q / (e - 1), M = GK / Math.pow(a, 1.5) * t;
    let H = Math.asinh(M / e);
    for (let k = 0; k < 30; k++) { const d = (e * Math.sinh(H) - H - M) / (e * Math.cosh(H) - 1); H -= d; if (Math.abs(d) < 1e-10) break; }
    xp = a * (e - Math.cosh(H)); yp = a * Math.sqrt(e * e - 1) * Math.sinh(H);
  }
  return rotate(xp, yp, o.i, o.node, o.w);
}

/* a moon or orbiter around its planet from {a(km), e, i, node, w, M, P(days), epoch}: planet-centred km */
function satPos(o, jd) {
  const E = kepler(o.M + 360 * (jd - o.epoch) / o.P, o.e || 0), e = o.e || 0;
  return rotate(o.a * (Math.cos(E) - e), o.a * Math.sqrt(1 - e * e) * Math.sin(E), o.i, o.node, o.w);
}

/* ---------- events ---------- */
const BRIGHT = ['mercury', 'venus', 'mars', 'jupiter', 'saturn'];
const OUTER = ['mars', 'jupiter', 'saturn', 'uranus', 'neptune'];
const NAMES = { mercury: 'Mercury', venus: 'Venus', earth: 'Earth', mars: 'Mars', jupiter: 'Jupiter',
                saturn: 'Saturn', uranus: 'Uranus', neptune: 'Neptune', moon: 'Moon' };

/* parabolic refinement of an extremum from three daily samples */
function refine(t0, f0, f1, f2) {
  const den = f0 - 2 * f1 + f2;
  const off = den === 0 ? 0 : 0.5 * (f0 - f2) / den;
  return { t: t0 + 1 + Math.max(-1, Math.min(1, off)), v: f1 - 0.25 * (f0 - f2) * off };
}

function events(jd0, days) {
  const S = [];
  for (let i = -1; i <= days + 1; i++) S.push(sky(jd0 + i));
  const ev = [];
  const sep = (s, a, b) => angle(s.bodies[a].geo, s.bodies[b].geo);
  const ps = PLANETS.filter((p) => p !== 'earth');
  for (let i = 1; i < S.length - 1; i++) {
    const a = S[i - 1], b = S[i], c = S[i + 1], t0 = a.jd;
    for (const p of ps) {
      const e0 = a.bodies[p].elong, e1 = b.bodies[p].elong, e2 = c.bodies[p].elong;
      if (e1 >= e0 && e1 > e2) {
        if (OUTER.includes(p) && e1 > 170) {
          const r = refine(t0, e0, e1, e2);
          ev.push({ jd: r.t, kind: 'opposition', bodies: [p], title: `${NAMES[p]} at opposition`,
                    detail: `Opposite the Sun: up all night, closest and brightest (${b.bodies[p].dist.toFixed(2)} au).`, rank: p === 'mars' || p === 'jupiter' || p === 'saturn' ? 3 : 2 });
        } else if (!OUTER.includes(p)) {
          const r = refine(t0, e0, e1, e2);
          ev.push({ jd: r.t, kind: 'elongation', bodies: [p], title: `${NAMES[p]} greatest ${b.bodies[p].side === 'evening' ? 'eastern' : 'western'} elongation`,
                    detail: `${r.v.toFixed(1)} deg from the Sun: best ${b.bodies[p].side} view.`, rank: p === 'venus' ? 3 : 2 });
        }
      }
      if (e1 <= e0 && e1 < e2 && e1 < 12) {
        const r = refine(t0, e0, e1, e2), inf = !OUTER.includes(p) && b.bodies[p].dist < 1;
        ev.push({ jd: r.t, kind: 'solar', bodies: [p], title: `${NAMES[p]} ${inf ? 'inferior' : 'superior'} conjunction`,
                  detail: inf ? `Passes between Earth and the Sun, ${b.bodies[p].dist.toFixed(2)} au away.` : `Behind the Sun: hidden, and radio contact with spacecraft there fades.`, rank: 1 });
      }
    }
    for (let x = 0; x < ps.length; x++) for (let y = x + 1; y < ps.length; y++) {
      const p = ps[x], q = ps[y];
      const lim = BRIGHT.includes(p) && BRIGHT.includes(q) ? 3 : 1.5;
      const s0 = sep(a, p, q), s1 = sep(b, p, q), s2 = sep(c, p, q);
      if (s1 <= s0 && s1 < s2 && s1 < lim) {
        const r = refine(t0, s0, s1, s2), vis = Math.min(b.bodies[p].elong, b.bodies[q].elong);
        ev.push({ jd: r.t, kind: 'conjunction', bodies: [p, q], title: `${NAMES[p]} meets ${NAMES[q]}`,
                  detail: `${Math.max(0.05, r.v).toFixed(1)} deg apart${vis < 15 ? ', but lost in the Sun\'s glare' : `, ${b.bodies[p].side} sky`}.`,
                  sepDeg: r.v, rank: vis < 15 ? 1 : (BRIGHT.includes(p) && BRIGHT.includes(q) ? 3 : 2) });
      }
    }
  }
  /* line-ups: 4+ planets clear of the Sun on one side of the sky; runs collapse into one event */
  let run = null;
  const flush = () => { if (run && run.n >= 4) ev.push(run.e); run = null; };
  for (let i = 1; i < S.length - 1; i++) {
    const s = S[i];
    for (const side of ['evening', 'morning']) {
      const vis = ps.filter((p) => s.bodies[p].elong > 18 && s.bodies[p].side === side);
      const bright = vis.filter((p) => BRIGHT.includes(p)).length;
      const key = side + vis.join(',');
      if (vis.length >= 4 && bright >= 3) {
        if (run && run.key === key) { run.e.end = s.jd; continue; }
        if (run && run.side === side) continue;
        flush();
        run = { key, side, n: vis.length, e: { jd: s.jd, end: s.jd, kind: 'lineup', bodies: vis,
          title: `${vis.length}-planet ${side} line-up`, detail: `${vis.map((p) => NAMES[p]).join(', ')} all in the ${side} sky.`, rank: vis.length >= 5 ? 3 : 2 } };
      } else if (run && run.side === side) flush();
    }
  }
  flush();
  ev.sort((u, v) => u.jd - v.jd);
  return ev.filter((e) => e.jd >= jd0 && e.jd <= jd0 + days);
}

root.Ephem = { observe, jdOf, msOf, helio, orbit, sky, moon, galilean, geometry, events, conic, satPos, rotate, bodyFixed, ecl2eq, PLANETS, NAMES, EL, norm180, norm360 };
if (typeof module !== 'undefined') module.exports = root.Ephem;
})(typeof window !== 'undefined' ? window : globalThis);
