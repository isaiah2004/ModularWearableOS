/* watch.js - the Orrery face prototype: 466 x 466 round glass, 30 fps, real positions.
 * Needs ephem.js (maths), atlas.js (ATLAS: JPL data snapshot), textures.js (SOLAR_TEX).
 * Everything drawn here is something the watch can draw: flat fills, 1-2 px lines, dot matrix,
 * and one per-pixel renderer (the globe) that works from a lookup table like the C version would. */
(function () {
'use strict';
const E = window.Ephem, A = window.ATLAS, TEX = window.SOLAR_TEX;
const W = 466, C = 233, D2R = Math.PI / 180, R2D = 180 / Math.PI;
const COL = { bg: '#000', pri: '#fff', sec: '#999', dis: '#666', line2: '#333', line: '#222', surf: '#111', surf2: '#1a1a1a', red: '#d71921' };
const TINT = { sun: '#ffd27a', mercury: '#b5aea4', venus: '#e8d6a8', earth: '#7fb2ff', moon: '#cfcfcf', mars: '#e07a4f',
               jupiter: '#e3c49b', saturn: '#e8d49c', uranus: '#a8e3e8', neptune: '#6f8cff', pluto: '#d9c2a5' };
const NAME = { sun: 'Sun', mercury: 'Mercury', venus: 'Venus', earth: 'Earth', moon: 'Moon', mars: 'Mars', jupiter: 'Jupiter',
               saturn: 'Saturn', uranus: 'Uranus', neptune: 'Neptune', pluto: 'Pluto' };
const PERIOD = { mercury: 87.969, venus: 224.701, earth: 365.256, mars: 686.98, jupiter: 4332.59, saturn: 10759.2,
                 uranus: 30688.5, neptune: 60182, pluto: 90560 };
const CARD_ORDER = ['sun', 'mercury', 'venus', 'earth', 'moon', 'mars', 'jupiter', 'saturn', 'uranus', 'neptune', 'pluto'];
const PLANETS = ['mercury', 'venus', 'earth', 'mars', 'jupiter', 'saturn', 'uranus', 'neptune'];
const INNER = ['mercury', 'venus', 'earth', 'mars'], OUTER = ['jupiter', 'saturn', 'uranus', 'neptune'];
const RADIUS_KM = { sun: 696000, mercury: 2440, venus: 6052, earth: 6371, moon: 1737, mars: 3390, jupiter: 69911,
                    saturn: 58232, uranus: 25362, neptune: 24622, pluto: 1188 };
const PLUTO = A.small.find((s) => /Pluto/.test(s.name));

/* ---------------- options (set from the page) ---------------- */
/* the Sun is fixed at the centre and every planet, Earth included, goes round it (vernal point at 12 o'clock) */
const opt = { fps: 30, tint: true, aod: false, zones: false, globeMode: 'photo',
              show: { planets: true, asteroids: true, comets: true, craft: true }, fromEarth: false,
              drag: 'pan',        /* what a drag on the face does: 'pan' (move the map, flat or tilted), 'look' (orbit the 3D camera) or 'time' */
              lines: true };      /* the red alignment geometry */
/* time rings: three dials that read and set the real date (outer years, middle months, inner days).
 * Collapsed they are three short arcs at top right: a pull tab. Open, the solar system shrinks inside them. */
const RINGS = [{ r: 221, lap: 12 * 365.25, unit: 'year' }, { r: 196, lap: 365.25, unit: 'month' }, { r: 171, lap: 30, unit: 'day' }];
const RING_W = 22, TAB_PHI = 43, TAB_R = [226, 216, 206], ORB_SHRINK = 0.33, GRIP_PHI = 43;
/* filter tab: bottom left */
/* control menu: a pull tab of three arcs at bottom left (mirrors the time tab). Its buttons slide out down the left side;
 * hitting one slides its options out to the right */
/* the handle: a long arc on the right rim, dead centre on 3 o'clock. Only an edge swipe that starts on it and heads
 * inward opens the drawer (taps there do nothing special). The drawer is round icon buttons on an arc inside the rim;
 * hitting one fans its options further in. Back-swipes live on the left half, so the two never meet. */
const HANDLE = { phi0: 70, phi1: 110, r: 226 }, MENU_R = 196, OPT_R = 150, BTN = 17;
/* the handle and the icons share the same 40 deg of rim: 13 deg apart at r 196 is ~44 px between 34 px buttons */
const MENU_PHI = [70.5, 83.5, 96.5, 109.5], OPT_STEP = 15;
/* the touch area is wider than the mark: 47 px deep from the glass edge, 8 deg past each end */
const onHandle = (x, y) => { const r = Math.hypot(x - C, y - C), f = phiOf(x, y); return r >= 186 && f >= HANDLE.phi0 - 8 && f <= HANDLE.phi1 + 8; };
/* zoom dial: a partial ring along the bottom rim from 4:30 to 7:30 (135-225 deg), 0.5x to 4x on a log scale */
const ZOOM = { phi0: 135, phi1: 225, r: 220, w: 20, min: 0.5, max: 4 };
const zoomPhi = (z) => ZOOM.phi1 - (Math.log(z / ZOOM.min) / Math.log(ZOOM.max / ZOOM.min)) * (ZOOM.phi1 - ZOOM.phi0);   /* + at 4:30, - at 7:30 */
const phiZoom = (f) => ZOOM.min * Math.pow(ZOOM.max / ZOOM.min, (ZOOM.phi1 - Math.max(ZOOM.phi0, Math.min(ZOOM.phi1, f))) / (ZOOM.phi1 - ZOOM.phi0));
const phi360 = (x, y) => (phiOf(x, y) + 360) % 360;
/* the event chip / NOW pill sit above the zoom dial so the two never share a touch */
const CHIP_Y = 380;
const PAN_MAX = 210;
const polar = (phi, r) => [233 + r * Math.sin(phi * Math.PI / 180), 233 - r * Math.cos(phi * Math.PI / 180)];   /* phi clockwise from 12 o'clock */
const phiOf = (x, y) => Math.atan2(x - 233, -(y - 233)) * 180 / Math.PI;
/* card touch zones: the globe only takes drags that start in its core, away from the glass edge,
 * so swipes (back, next body, pages) still work everywhere else */
const GLOBE_CORE = 0.62, EDGE_KEEP = 56;
/* the outer band of the glass: a swipe that starts here and heads inward from the left is BACK;
 * one that runs along it is the dial (cards) or a scrub (face). Nothing else ever means back. */
const EDGE_BAND = 36, EDGE_R = 233 - EDGE_BAND;
/* 3D tilt: hold the centre (the time / the Sun) for 0.6 s; a ring fills while you hold */
const HOLD_R = 62, HOLD_SHOW = 150, SCRUB_DEAD = 36;
/* it's a watch face: the controls are locked until you hold anywhere for 1.5 s (and the same locks them again).
 * Letting go between 0.75 s and 1.5 s toggles 3D tilt instead. One ring of dots shows both stages under the finger. */
const HOLD_TILT = 750, HOLD_LOCK = 1500;
/* a flick is real finger speed, not px per event: > 0.6 px/ms over the last 100 ms (~20 px a frame) */
const FLICK = 0.6, TAP_MAX_MS = 450;
function fingerVel(d) {
  const tr = d.trail, now = tr[tr.length - 1];
  if (!now) return [0, 0];
  let o = tr[0];
  for (const p of tr) if (now[0] - p[0] <= 100) { o = p; break; }
  const dt = Math.max(16, now[0] - o[0]);
  return [(now[1] - o[1]) / dt, (now[2] - o[2]) / dt];
}

/* ---------------- canvas ---------------- */
let cv, g;
function setup(canvas) {
  cv = canvas; g = cv.getContext('2d', { willReadFrequently: true });
  cv.width = W; cv.height = H0();
  hookInput();
  requestAnimationFrame(loop);
}
const H0 = () => W;

/* ---------------- time ---------------- */
const st = {
  view: 'face', from: null, tx: 1, txN: 10,          /* orrery zone + morph */
  off: 0, vel: 0, timeAnim: null,                    /* days from now, scrub momentum */
  screen: 'orrery',                                  /* orrery | card | events */
  iris: null, card: null, list: { y: 0, v: 0 }, toast: null, pulse: 0, snapped: null,
  frame: 0, dragging: null, hits: [], tilt: 0, tiltK: 0, tiltA: 0, backX: 0, backAnim: null, locked: true, lockK: 1,
  rings: { open: false, k: 0, e: 0, pulling: false }, az: 0, azVel: 0, camTilt: 64,
  menu: { open: false, k: 0, e: 0, pulling: false, sub: null, subK: 0, subE: 0, hits: [] },
  pan: { x: 0, y: 0, vx: 0, vy: 0 }, panAnim: null, zoom: 1, zoomAnim: null, zoomE: 1, prevJD: NaN, beltScr: null,
};
const nowJD = () => E.jdOf(Date.now());
const simJD = () => nowJD() + st.off;

/* ---------------- easing (EASE_STEADY = cubic-bezier(.61,1,.88,1)) ---------------- */
function bez(x1, y1, x2, y2) {
  const cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx, cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
  return (x) => { let t = x; for (let i = 0; i < 8; i++) { const f = ((ax * t + bx) * t + cx) * t - x, d = (3 * ax * t + 2 * bx) * t + cx; if (Math.abs(f) < 1e-5 || d === 0) break; t -= f / d; } return ((ay * t + by) * t + cy) * t; };
}
const STEADY = bez(0.61, 1, 0.88, 1), INOUT = bez(0.45, 0, 0.2, 1);
/* quadratic ease in-out: two multiplies, no solver, no overshoot. Peak speed is 2x the average, so a full
 * 466 px page over 15 frames peaks at ~62 px/frame: inside the 64 px transition budget */
const QUAD = (t) => (t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t));
const PAGE_FRAMES = 15;

/* ---------------- radial maps: au -> px, one per zone ---------------- */
const FACE_PTS = [[0, 60], [0.387, 82], [0.723, 99], [1, 116], [1.524, 133], [2.1, 141], [3.3, 154], [5.2, 167], [9.54, 185],
                  [19.2, 202], [30.1, 217], [45, 226], [1e6, 226]];
function rFace(au) {
  for (let i = 1; i < FACE_PTS.length; i++) if (au <= FACE_PTS[i][0]) {
    const [a0, r0] = FACE_PTS[i - 1], [a1, r1] = FACE_PTS[i]; return r0 + (r1 - r0) * (au - a0) / (a1 - a0);
  }
  return 226;
}
const RMAP = {
  face: rFace,
  inner: (au) => Math.min(224, 116 * Math.sqrt(au)),
  outer: (au) => Math.min(224, 35.3 * Math.sqrt(au)),
  beyond: (au) => Math.min(226, Math.max(4, 30 + 80 * Math.log10(Math.max(au, 0.6)))),
};
function rmap(au) {
  const a = RMAP[st.view](au);
  if (!st.from || st.tx >= 1) return a;
  const b = RMAP[st.from](au); return b + (a - b) * STEADY(st.tx);
}
/* ecliptic longitude 0 (the vernal point) at 12 o'clock, planets run counter-clockwise */
/* 3D: long-press tilts the ecliptic away from you; bodies lift by their height above it (z, same scale as r) */
const SC = () => 1 - ORB_SHRINK * st.rings.e;   /* the system shrinks to fit inside the open time rings */
function toScreen(x, y, z = 0) {
  const au = Math.hypot(x, y), r = rmap(au), lon = Math.atan2(y, x) + st.az * D2R, t = st.tiltA * D2R, rs = r * SC() * st.zoom;
  const px = -rs * Math.sin(lon), py = -rs * Math.cos(lon), s = au > 1e-6 ? rs / au : 0;
  return [C + px, C + py * Math.cos(t) - z * s * Math.sin(t), r, au];   /* r stays unscaled: it's for zone logic */
}
const zoneMix = (v) => (st.view === v ? (st.from ? STEADY(st.tx) : 1) : (st.from === v ? 1 - STEADY(st.tx) : 0));

/* ---------------- dot-matrix numerals (5x7) ---------------- */
const FONT = {
  '0': ['01110', '10001', '10011', '10101', '11001', '10001', '01110'], '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
  '2': ['01110', '10001', '00001', '00010', '00100', '01000', '11111'], '3': ['11111', '00010', '00100', '00010', '00001', '10001', '01110'],
  '4': ['00010', '00110', '01010', '10010', '11111', '00010', '00010'], '5': ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
  '6': ['00110', '01000', '10000', '11110', '10001', '10001', '01110'], '7': ['11111', '00001', '00010', '00100', '01000', '01000', '01000'],
  '8': ['01110', '10001', '10001', '01110', '10001', '10001', '01110'], '9': ['01110', '10001', '10001', '01111', '00001', '00010', '01100'],
  ':': ['0', '0', '1', '0', '1', '0', '0'], '.': ['0', '0', '0', '0', '0', '0', '1'], '-': ['000', '000', '000', '111', '000', '000', '000'],
  '+': ['000', '000', '010', '111', '010', '000', '000'], '°': ['010', '101', '010', '000', '000', '000', '000'], ' ': ['00', '00', '00', '00', '00', '00', '00'],
};
function dotWidth(s, p) { let w = 0; for (const ch of s) w += ((FONT[ch] || FONT[' '])[0].length + 1) * p; return w - p; }
function dotText(s, cx, cy, p, color, align = 'center') {
  let x = align === 'center' ? cx - dotWidth(s, p) / 2 : align === 'right' ? cx - dotWidth(s, p) : cx;
  const y0 = cy - 3.5 * p + p / 2;
  g.fillStyle = color;
  for (const ch of s) {
    const f = FONT[ch] || FONT[' '];
    for (let r = 0; r < 7; r++) for (let c = 0; c < f[r].length; c++) if (f[r][c] === '1') {
      g.beginPath(); g.arc(x + c * p + p / 2, y0 + r * p, p * 0.4, 0, 7); g.fill();
    }
    x += (f[0].length + 1) * p;
  }
}
/* micro labels: ALL-CAPS mono with tracking; words: the grotesk */
function label(s, x, y, color = COL.sec, align = 'center', size = 11) {
  g.font = `600 ${size}px ui-monospace, "Cascadia Mono", Consolas, monospace`; g.letterSpacing = size > 11 ? '1.2px' : '1.5px';
  g.textAlign = align; g.textBaseline = 'middle'; g.fillStyle = color; g.fillText(s.toUpperCase(), x, y); g.letterSpacing = '0px';
}
function words(s, x, y, size, color = COL.pri, align = 'center', weight = 500) {
  g.font = `${weight} ${size}px "Segoe UI Variable Display", "Segoe UI", system-ui, sans-serif`;
  g.textAlign = align; g.textBaseline = 'middle'; g.fillStyle = color; g.fillText(s, x, y);
}
/* labels that would collide with one already placed this frame are dropped (tap still reads them out) */
let PLACED = [];
function freeLabel(s, x, y, color, align, size) {
  const w = lw(s, size), x0 = align === 'left' ? x : align === 'right' ? x - w : x - w / 2, r = [x0 - 2, y - size / 2 - 2, x0 + w + 2, y + size / 2 + 2];
  if (r[0] < 0 || r[2] > W || PLACED.some((q) => r[0] < q[2] && r[2] > q[0] && r[1] < q[3] && r[3] > q[1])) return false;
  if (Math.hypot(Math.max(Math.abs(r[0] - C), Math.abs(r[2] - C)), Math.max(Math.abs(r[1] - C), Math.abs(r[3] - C))) > 228) return false;
  PLACED.push(r); label(s, x, y, color, align, size); return true;
}
function pill(x, y, w, h, fill) { g.fillStyle = fill; g.beginPath(); g.roundRect(x - w / 2, y - h / 2, w, h, h / 2); g.fill(); }
const lw = (s, size = 11) => { g.font = `600 ${size}px ui-monospace, "Cascadia Mono", Consolas, monospace`; g.letterSpacing = '1.5px'; const w = g.measureText(s.toUpperCase()).width; g.letterSpacing = '0px'; return w; };

/* ---------------- sky state per frame (cached by sim time) ---------------- */
let SKY = null, SKYjd = NaN, BELT = null, BELTjd = NaN;
const ORB = {};
function sky() {
  const jd = simJD();
  if (!SKY || Math.abs(jd - SKYjd) > 1e-4) {
    SKY = E.sky(jd); SKYjd = jd;
    SKY.small = A.small.map((o) => ({ o, p: E.conic(o, jd) }));
    SKY.comets = A.comets.filter((c) => c.M1 !== null && c.M1 < 16).map((o) => ({ o, p: E.conic(o, jd) }));
    SKY.craft = A.craft.filter((c) => !c.planet && c.xyz).map((c) => ({ c, p: { x: c.xyz[0], y: c.xyz[1], z: c.xyz[2] } }));
    if (PLUTO) SKY.bodies.pluto = E.observe(E.conic(PLUTO, jd), SKY.earth);
  }
  if (!ORB.mercury) for (const p of PLANETS) ORB[p] = E.orbit(p, jd, 160);
  return SKY;
}
function belt() {
  const jd = simJD();
  if (BELT && Math.abs(jd - BELTjd) < 0.05) return BELT;
  const n = A.belt.length, out = BELT && BELT.length === n * 3 ? BELT : new Float32Array(n * 3);
  for (let k = 0; k < n; k++) {
    const [a, e, i, om, w, ma, ep, H] = A.belt[k];
    const M = ma + 0.9856076686 / Math.pow(a, 1.5) * (jd - ep);
    let Mr = ((M % 360) + 360) % 360 * D2R, Ea = Mr + e * Math.sin(Mr);
    for (let it = 0; it < 4; it++) Ea -= (Ea - e * Math.sin(Ea) - Mr) / (1 - e * Math.cos(Ea));
    const p = E.rotate(a * (Math.cos(Ea) - e), a * Math.sqrt(1 - e * e) * Math.sin(Ea), i, om, w);
    out[k * 3] = p.x; out[k * 3 + 1] = p.y; out[k * 3 + 2] = H;
  }
  BELT = out; BELTjd = jd; return out;
}

/* ---------------- events (computed here, like the watch would, in a sliding window) ---------------- */
let EVENTS = [], EVwin = [0, 0];
const MISSION = [{ jd: E.jdOf(Date.UTC(2026, 10, 21)), kind: 'mission', bodies: ['mercury'], title: 'BepiColombo reaches Mercury',
                   detail: 'Orbit insertion planned: Mercury gets its first orbiter since MESSENGER.', rank: 3 }];
function flybyEvents() {
  /* car-sized rocks pass every week: list the ones ~25 m and up (H < 26) */
  return A.flybys.filter((f) => f.au < 0.0035 && f.H && f.H < 26).map((f) => ({ jd: f.jd, kind: 'flyby', bodies: ['earth'], title: `Asteroid ${f.des} flies by`,
    detail: `${(f.au / 0.00257).toFixed(2)} lunar distances, ${f.kms.toFixed(1)} km/s${f.H ? `, ~${Math.round(1329 / Math.sqrt(0.14) * Math.pow(10, -f.H / 5) * 1000)} m wide` : ''}.`, rank: 2 }));
}
function cometEvents() {
  return A.comets.filter((c) => c.M1 !== null && c.M1 < 12).map((c) => ({ jd: c.tp, kind: 'comet', bodies: [], title: `Comet ${c.name.replace(/\s*\(.*\)/, '')} at perihelion`,
    detail: `${c.q.toFixed(2)} au from the Sun; total magnitude ${c.M1.toFixed(1)} (lower is brighter).`, rank: 2, comet: c }));
}
function events() {
  const jd = simJD();
  if (jd < EVwin[0] + 30 || jd > EVwin[1] - 400) {
    const a = Math.floor(jd) - 60, b = a + 900;
    EVENTS = E.events(a, b - a).concat(MISSION, flybyEvents(), cometEvents()).filter((e) => e.jd >= a && e.jd <= b && e.rank >= 2)
      .sort((u, v) => u.jd - v.jd);
    EVwin = [a, b];
  }
  return EVENTS;
}
function activeEvent() {
  if (st.snapped) return st.snapped;
  const jd = simJD();
  return events().find((e) => e.jd >= jd - 1.5 && e.jd - jd < 60 && e.kind !== 'flyby' && e.kind !== 'comet') || null;
}
const fmtDate = (jd, withYear = true) => {
  const d = new Date(E.msOf(jd)), m = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'][d.getMonth()];
  return `${String(d.getDate()).padStart(2, '0')} ${m}${withYear ? ' ' + d.getFullYear() : ''}`;
};
const fmtIn = (days) => { const d = Math.round(days); return d === 0 ? 'TODAY' : d > 0 ? `IN ${d} D` : `${-d} D AGO`; };

/* ---------------- planets as flat discs; the sun-side half lit ---------------- */
function bodyR(p) {
  const base = { mercury: 3.5, venus: 5.5, earth: 5.5, mars: 4.5, jupiter: 10, saturn: 8.5, uranus: 6.5, neptune: 6.5, pluto: 2.5 }[p];
  const k = INNER.includes(p) ? 1 + 0.45 * zoneMix('inner') - 0.4 * zoneMix('outer') - 0.5 * zoneMix('beyond')
                              : 1 + 0.25 * zoneMix('outer') - 0.35 * zoneMix('inner') - 0.3 * zoneMix('beyond');
  return Math.max(1.5, base * k);
}
function disc(x, y, r, p, sunX, sunY) {
  const ink = opt.tint ? TINT[p] : COL.pri;
  if (r < 4.5) { g.fillStyle = ink; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill(); return; }
  const a = Math.atan2(sunY - y, sunX - x);
  g.fillStyle = opt.tint ? shade(ink, 0.35) : COL.dis; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill();
  g.fillStyle = ink; g.beginPath(); g.arc(x, y, r, a - Math.PI / 2, a + Math.PI / 2); g.fill();
}
function shade(hex, k) {
  const n = parseInt(hex.slice(1), 16);
  return `rgb(${((n >> 16) & 255) * k | 0},${((n >> 8) & 255) * k | 0},${(n & 255) * k | 0})`;
}

/* ---------------- the orrery (face + zones) ---------------- */
function drawOrrery(dim) {
  const S = sky(), mixF = zoneMix('face'), mixI = zoneMix('inner'), mixO = zoneMix('outer'), mixB = zoneMix('beyond');
  st.hits = []; PLACED = []; const LATE = [];
  const dj = isNaN(st.prevJD) ? 0 : S.jd - st.prevJD;   /* days this frame covered: drives the motion blur */
  if (mixF < 0.99) PLACED.push([C - 50, 8, C + 50, 36]);   /* the time at the top */
  if (st.rings.e > 0.5) PLACED.push([C - 100, C - 44, C + 100, C + 58]);   /* the date read-out inside the rings */
  /* orbits */
  g.lineWidth = 1;
  for (const p of PLANETS) {
    const pts = ORB[p];
    const hot = (INNER.includes(p) && mixI > 0.5) || (OUTER.includes(p) && mixO > 0.5);
    const off = (OUTER.includes(p) && mixI > 0.5) || (p === 'mercury' && mixB > 0.5) || (INNER.includes(p) && mixB > 0.5);
    if (off || dim || (!opt.show.planets && p !== 'earth')) continue;
    g.strokeStyle = hot ? COL.line2 : COL.line; g.beginPath();
    pts.forEach(([x, y, z], i) => { const [sx, sy] = toScreen(x, y, z); i ? g.lineTo(sx, sy) : g.moveTo(sx, sy); });
    g.closePath(); g.stroke();
  }
  /* zone rims: the place you tap to go out a level */
  const ct = Math.cos(st.tiltA * D2R), sc = SC() * st.zoom, azr = st.az * D2R;
  if (mixF < 1 && st.rings.e < 0.5) { g.strokeStyle = COL.line; g.setLineDash([2, 4]); g.beginPath(); g.ellipse(C, C, 224 * sc, 224 * sc * ct, 0, 0, 7); g.stroke(); g.setLineDash([]); }
  if (mixB > 0.01) {
    g.globalAlpha = mixB;
    for (const [au, name] of [[10, '10 AU'], [30, '30 AU'], [100, '100 AU']]) {
      const r = RMAP.beyond(au) * sc; g.strokeStyle = COL.line; g.beginPath(); g.ellipse(C, C, r, r * ct, 0, 0, 7); g.stroke();
      label(name, C + r * 0.71 + 4, C + r * 0.71 * ct - 6, COL.dis, 'left', 9);
    }
    const hp = RMAP.beyond(120) * sc; g.strokeStyle = COL.line2; g.setLineDash([1, 5]); g.beginPath(); g.ellipse(C, C, hp, hp * ct, 0, 0, 7); g.stroke(); g.setLineDash([]);
    freeLabel('heliopause', C, C - hp * ct - 8, COL.dis, 'center', 9);
    g.globalAlpha = 1;
  }
  /* the belt (4000 brightest numbered asteroids; the watch keeps all 1.4 million on SD) */
  if (!dim && opt.show.asteroids) {
    const B = belt(), n = B.length / 3;
    if (!st.beltScr || st.beltScr.length !== n * 2) st.beltScr = new Float32Array(n * 2).fill(NaN);
    const scr = st.beltScr, blurOn = Math.abs(dj) > 0.3;
    for (let k = 0; k < n; k++) {
      const x = B[k * 3], y = B[k * 3 + 1], H = B[k * 3 + 2], kbo = x * x + y * y > 400;
      const vis = kbo ? 1 : 1 - mixB;     /* beyond keeps the Kuiper belt, drops the main belt */
      if (vis < 0.02) continue;
      const [sx, sy, r] = toScreen(x, y);
      if (r > 223) { scr[k * 2] = NaN; continue; }
      g.fillStyle = H < 8 ? '#777' : H < 10 ? '#4a4a4a' : '#333';
      g.globalAlpha = vis;
      /* blur: a short streak from where it was last frame (skipped if it jumped across the screen) */
      const ox = scr[k * 2], oy = scr[k * 2 + 1];
      if (blurOn && ox === ox) {
        const L = Math.hypot(sx - ox, sy - oy);
        if (L > 1.5 && L < 60) { g.strokeStyle = g.fillStyle; g.lineWidth = 1; g.globalAlpha = vis * 0.55; g.beginPath(); g.moveTo(ox, oy); g.lineTo(sx, sy); g.stroke(); g.globalAlpha = vis; }
      }
      g.fillRect(sx - 0.5, sy - 0.5, 1, 1);
      scr[k * 2] = sx; scr[k * 2 + 1] = sy;
    }
    g.globalAlpha = 1;
  }
  const sun = [C, C];
  /* motion blur: when time moves fast (NOW, the rings, a flick, a jump to an event) every planet smears along its own
   * orbit over the time this frame covered, fading toward where it was. At 30 fps that's what the eye expects. */
  if (!dim && dj && opt.show.planets) drawPlanetBlur(S, dj);
  if (!dim && dj && opt.show.planets === false) drawPlanetBlur(S, dj, ['earth']);
  /* the Sun: the locked face shows the time over it; unlocked (and in the zones) it's drawn as a disc */
  const sunA = dim ? 1 - mixF : Math.max(1 - mixF, 1 - st.lockK);
  if (sunA > 0.01) drawSun(sunA, (8 + 3 * mixI) * Math.min(1.6, Math.max(0.75, st.zoom)), dim);
  if (sunA > 0.5 && !dim) st.hits.push({ id: 'sun', x: C, y: C, r: 20 });
  /* comets with tails pointing away from the Sun */
  if (!dim && opt.show.comets) for (const { o, p } of S.comets) {
    const [sx, sy, r, au] = toScreen(p.x, p.y, p.z);
    if (r > 223) continue;
    if (Math.abs(dj) > 0.3) {   /* comets streak too */
      const q = E.conic(o, S.jd - dj), [qx, qy] = toScreen(q.x, q.y, q.z), L = Math.hypot(sx - qx, sy - qy);
      if (L > 2 && L < 90) { g.strokeStyle = COL.dis; g.lineWidth = 2; g.globalAlpha = 0.5; g.beginPath(); g.moveTo(qx, qy); g.lineTo(sx, sy); g.stroke(); g.globalAlpha = 1; }
    }
    const L = Math.min(20, 9 / Math.max(au, 0.3)), ux = (sx - C) / (r || 1), uy = (sy - C) / (r || 1);
    g.strokeStyle = COL.sec; g.lineWidth = 1; g.beginPath(); g.moveTo(sx, sy); g.lineTo(sx + ux * L, sy + uy * L); g.stroke();
    g.fillStyle = COL.pri; g.fillRect(sx - 1, sy - 1, 2, 2);
    st.hits.push({ id: 'comet', o, x: sx, y: sy, r: 10 });
  }
  /* named small bodies and dwarf planets */
  if (!dim) for (const { o, p } of S.small) {
    if (/Pluto/.test(o.name)) continue;
    if (!(o.kind === 'comet' || o.kind === 'interstellar' ? opt.show.comets : opt.show.asteroids)) continue;
    const [sx, sy, r, au] = toScreen(p.x, p.y, p.z);
    if (r > 225) continue;
    const big = o.kind === 'dwarf' || /Vesta|Pallas/.test(o.name);
    g.fillStyle = big ? COL.sec : COL.dis; g.beginPath(); g.arc(sx, sy, big ? 2 : 1.4, 0, 7); g.fill();
    if ((mixI > 0.9 && au < 3.5 && big) || (mixB > 0.9 && au > 20)) LATE.push(() => freeLabel(o.short.replace(/^\d+\s/, ''), sx, sy - 9, COL.dis, 'center', 9));
    st.hits.push({ id: 'small', o, x: sx, y: sy, r: 10, au });
  }
  /* spacecraft in cruise (beyond and zones) */
  if (!dim && mixF < 0.5 && opt.show.craft) for (const { c, p } of S.craft) {
    const [sx, sy, r, au] = toScreen(p.x, p.y, p.z);
    if (r > 225 || (mixI < 0.5 && au < 3)) continue;
    if (Math.hypot(p.x - S.earth.x, p.y - S.earth.y) < 0.03) continue;   /* JWST rides with Earth: it's on Earth's card */
    g.strokeStyle = COL.sec; g.lineWidth = 1; g.beginPath(); g.moveTo(sx - 3, sy); g.lineTo(sx + 3, sy); g.moveTo(sx, sy - 3); g.lineTo(sx, sy + 3); g.stroke();
    if (mixB > 0.9 && au > 20) LATE.push(() => freeLabel(c.name, sx + (sx > C ? -7 : 7), sy + 1, COL.dis, sx > C ? 'right' : 'left', 9));
    st.hits.push({ id: 'craft', c, x: sx, y: sy, r: 10, au });
  }
  /* planets */
  const pos = {};
  for (const p of PLANETS.concat(S.bodies.pluto ? ['pluto'] : [])) {
    const h = S.bodies[p].helio, [sx, sy, r] = toScreen(h.x, h.y, h.z);
    pos[p] = [sx, sy];
    if (st.tiltA > 1 && !dim && (opt.show.planets || p === 'earth')) {   /* a drop line to the ecliptic shows height in 3D */
      const [bx, by] = toScreen(h.x, h.y, 0); g.strokeStyle = COL.line2; g.lineWidth = 1; g.beginPath(); g.moveTo(sx, sy); g.lineTo(bx, by); g.stroke();
    }
    const rr = bodyR(p);
    if (p === 'pluto' && mixB + mixO < 0.3) continue;
    if (!opt.show.planets && p !== 'earth') continue;   /* Earth stays: everything is "in relation to Earth" */
    if (p === 'saturn' && rr > 4) {
      g.strokeStyle = opt.tint ? TINT.saturn : COL.sec; g.lineWidth = 1.2;
      g.beginPath(); g.ellipse(sx, sy, rr * 1.9, rr * 0.55, -0.35, 0, 7); g.stroke();
    }
    if (dim) { g.fillStyle = COL.dis; g.beginPath(); g.arc(sx, sy, Math.max(1.5, rr * 0.7), 0, 7); g.fill(); continue; }
    disc(sx, sy, rr, p, sun[0], sun[1]);
    if (p === 'earth') { g.strokeStyle = COL.pri; g.lineWidth = 1.5; g.beginPath(); g.arc(sx, sy, rr + 4, 0, 7); g.stroke(); }
    st.hits.push({ id: 'planet', p, x: sx, y: sy, r: Math.max(24, rr + 12) });
    /* labels in a zone view */
    const showLbl = (INNER.includes(p) && mixI > 0.6) || (OUTER.includes(p) && (mixO > 0.6 || mixB > 0.6)) || (p === 'pluto' && mixO + mixB > 0.6);
    if (showLbl) {
      const dist = p === 'earth' ? 'YOU' : p === 'pluto' ? `${Math.hypot(h.x - S.earth.x, h.y - S.earth.y, h.z - S.earth.z).toFixed(1)} AU`
        : `${S.bodies[p].dist.toFixed(p === 'mercury' || p === 'venus' || p === 'mars' ? 2 : 1)} AU`;
      /* labels sit outward from the Sun, or inward near the rim */
      const ux = (sx - C) / (r || 1), uy = (sy - C) / (r || 1), o = r > 170 ? -1 : 1, d0 = rr + 16;
      const lx = sx + ux * d0 * o, ly = sy + uy * d0 * o - (uy * o < 0 ? 6 : 0);
      if (freeLabel(NAME[p], lx, ly, COL.pri, 'center', 10)) freeLabel(dist, lx, ly + 11, COL.dis, 'center', 9);
    }
  }
  /* planets that fell outside the zone sit on the rim as ticks: tap the rim to go out */
  if (mixI > 0.05 && opt.show.planets) for (const p of OUTER) {
    const h = S.bodies[p].helio, lon = Math.atan2(h.y, h.x) + azr;
    const x = C - 224 * sc * Math.sin(lon), y = C - 224 * sc * Math.cos(lon) * ct;
    g.globalAlpha = mixI; g.fillStyle = COL.sec; g.beginPath(); g.arc(x, y, 2.5, 0, 7); g.fill();
    freeLabel(NAME[p].slice(0, 3), C - 210 * sc * Math.sin(lon), C - 210 * sc * Math.cos(lon) * ct, COL.dis, 'center', 9); g.globalAlpha = 1;
  }
  if (!dim && opt.fromEarth) drawFromEarth(S, pos);
  LATE.forEach((f) => f());
  st.pos = pos;
  if (!dim && opt.show.planets && opt.lines) drawEventGeometry(S, pos);
  if (!dim) st.prevJD = S.jd;
}

function drawPlanetBlur(S, dj, only) {
  g.lineCap = 'round';
  for (const p of only || PLANETS) {
    const rr = bodyR(p), ink = opt.tint ? TINT[p] : COL.pri;
    const swept = Math.min(1, Math.abs(dj) / PERIOD[p]);             /* fraction of an orbit covered this frame */
    const [hx, hy, r] = toScreen(S.bodies[p].helio.x, S.bodies[p].helio.y, S.bodies[p].helio.z);
    const arcPx = swept * 2 * Math.PI * Math.max(20, Math.hypot(hx - C, hy - C));
    if (arcPx < 3 || r > 225) continue;
    const n = Math.max(3, Math.min(56, Math.ceil(arcPx / 4))), span = Math.sign(dj) * Math.min(Math.abs(dj), PERIOD[p] * 0.999);
    let prev = null;
    for (let k = 0; k <= n; k++) {
      const f = k / n, h = E.helio(p, S.jd - span * (1 - f)), q = toScreen(h.x, h.y, h.z);
      if (prev) {
        g.strokeStyle = ink; g.globalAlpha = 0.05 + 0.4 * Math.pow(f, 1.6);   /* the tail fades out */
        g.lineWidth = Math.max(1.5, rr * 2 * (0.45 + 0.55 * f));
        g.beginPath(); g.moveTo(prev[0], prev[1]); g.lineTo(q[0], q[1]); g.stroke();
      }
      prev = q;
    }
  }
  g.globalAlpha = 1; g.lineCap = 'butt';
}
/* FROM EARTH: thin lines from Earth to the five nearest things you've chosen to show, each with its distance */
function drawFromEarth(S, pos) {
  const e = S.earth, cand = [];
  const add = (name, h) => { const [sx, sy, r] = toScreen(h.x, h.y, h.z); if (r <= 223) cand.push({ name, x: sx, y: sy, d: Math.hypot(h.x - e.x, h.y - e.y, h.z - e.z) }); };
  if (opt.show.planets) {
    for (const p of PLANETS) if (p !== 'earth') add(NAME[p], S.bodies[p].helio);
    if (S.bodies.pluto) add('Pluto', S.bodies.pluto.helio);
  }
  for (const { o, p } of S.small) {
    if (/Pluto/.test(o.name)) continue;
    const isComet = o.kind === 'comet' || o.kind === 'interstellar';
    if (isComet ? opt.show.comets : opt.show.asteroids) add((o.short || o.name).replace(/^\d+\s|^\(|\)$/g, ''), p);
  }
  if (opt.show.comets) for (const { o, p } of S.comets) add(o.name.replace(/\s*\(.*\)/, ''), p);
  if (opt.show.craft) for (const { c, p } of S.craft) if (Math.hypot(p.x - e.x, p.y - e.y) > 0.03) add(c.name, p);
  cand.sort((a, b) => a.d - b.d);
  const E0 = pos.earth, near = cand.slice(0, 5);
  g.strokeStyle = '#555'; g.lineWidth = 1; g.setLineDash([3, 3]);
  for (const c of near) { g.beginPath(); g.moveTo(E0[0], E0[1]); g.lineTo(c.x, c.y); g.stroke(); }
  g.setLineDash([]);
  for (const c of near) {
    const dist = c.d < 0.05 ? `${(c.d / 0.00257).toFixed(1)} LD` : `${c.d.toFixed(2)} AU`;
    freeLabel(`${c.name} ${dist}`, c.x, c.y + (c.y > E0[1] ? 11 : -11), COL.sec, 'center', 9);
  }
}

/* ---------------- time rings + their pull tab ---------------- */
function arcPhi(r, p0, p1) { g.beginPath(); g.arc(C, C, r, (p0 - 90) * D2R, (p1 - 90) * D2R); }
function ringTicks(i, cur) {
  const R = RINGS[i], d = new Date(E.msOf(cur)), lab = [];
  const at = (jd) => (jd - cur) / R.lap * 360;
  const MON = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];
  const jdLocal = (Y, M, D, h = 0) => E.jdOf(new Date(Y, M, D, h).getTime());
  if (R.unit === 'year') {
    for (let Y = d.getFullYear() - 7; Y <= d.getFullYear() + 7; Y++) {
      lab.push({ tick: at(jdLocal(Y, 0, 1)), phi: at(jdLocal(Y, 6, 2)), s: String(Y), major: true });
      for (let m = 1; m < 12; m++) lab.push({ tick: at(jdLocal(Y, m, 1)) });
    }
  } else if (R.unit === 'month') {
    for (let k = -7; k <= 7; k++) {
      const Y = d.getFullYear(), M = d.getMonth() + k;
      lab.push({ tick: at(jdLocal(Y, M, 1)), phi: at(jdLocal(Y, M, 15)), s: MON[((M % 12) + 12) % 12], major: true });
      for (const D of [8, 22]) lab.push({ tick: at(jdLocal(Y, M, D)) });
    }
  } else {
    for (let k = -17; k <= 17; k++) {
      const t = new Date(d.getFullYear(), d.getMonth(), d.getDate() + k), Y = t.getFullYear(), M = t.getMonth(), D = t.getDate();
      lab.push({ tick: at(jdLocal(Y, M, D)), phi: at(jdLocal(Y, M, D, 12)), s: D === 1 ? MON[M] : String(D), major: D === 1 });
    }
  }
  return lab;
}
function drawTimeRings() {
  const R = st.rings, e = R.e;
  if (e < 0.01) {
    /* collapsed: the pull tab, three short arcs at top right. It fades away while the menu is out, and while locked */
    const hide = Math.max(st.menu.e, st.lockK);
    if (hide > 0.99) return;
    g.globalAlpha = 1 - hide;
    TAB_R.forEach((r, i) => { g.strokeStyle = i === 0 ? COL.pri : COL.sec; g.lineWidth = 3; g.lineCap = 'round'; arcPhi(r, TAB_PHI - 14 + i * 2, TAB_PHI + 14 - i * 2); g.stroke(); });
    g.lineCap = 'butt'; g.globalAlpha = 1;
    return;
  }
  const cur = simJD(), span = 28 + 332 * e;
  RINGS.forEach((ring, i) => {
    const r = TAB_R[i] + (ring.r - TAB_R[i]) * e, w = 3 + (RING_W - 3) * e;
    g.strokeStyle = e > 0.6 ? '#141414' : '#3a3a3a'; g.lineWidth = w;
    if (span >= 359) { g.beginPath(); g.arc(C, C, r, 0, 7); } else arcPhi(r, TAB_PHI - span / 2, TAB_PHI + span / 2);
    g.stroke();
    if (e < 0.85) return;
    g.globalAlpha = (e - 0.85) / 0.15;
    const ticks = ringTicks(i, cur);
    let hot = null;
    for (const t of ticks) if (t.s && (!hot || Math.abs(t.phi) < Math.abs(hot.phi))) hot = t;
    for (const t of ticks) {
      if (Math.abs(t.tick) <= 180) {
        const [x0, y0] = polar(t.tick, r - w / 2 + 1), [x1, y1] = polar(t.tick, t.s ? r + w / 2 - 1 : r - w / 2 + 5);
        g.strokeStyle = t.s ? '#444' : '#2c2c2c'; g.lineWidth = 1; g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke();
      }
      if (t.s && t !== hot && Math.abs(t.phi) < 172) {
        const [lx, ly] = polar(t.phi, r);
        label(t.s, lx, ly, t.major ? '#8a8a8a' : '#6a6a6a', 'center', 9);
      }
    }
    if (hot) { const [lx, ly] = polar(hot.phi, r); pill(lx, ly, lw(hot.s, 9) + 12, 16, COL.pri); label(hot.s, lx, ly, '#000', 'center', 9); }
    g.globalAlpha = 1;
  });
  if (e > 0.85) {
    /* the index at 12 o'clock: what sits under it is the time shown in the middle */
    g.globalAlpha = (e - 0.85) / 0.15;
    g.fillStyle = COL.pri; g.beginPath(); g.moveTo(C - 6, 0); g.lineTo(C + 6, 0); g.lineTo(C, 8); g.fill();
    /* the grip: tap it (or pull it) to fold the rings back into the tab */
    const [gx, gy] = polar(GRIP_PHI, RINGS[0].r);
    g.fillStyle = COL.pri; g.beginPath(); g.arc(gx, gy, 10, 0, 7); g.fill();
    g.strokeStyle = '#000'; g.lineWidth = 2; g.beginPath(); g.moveTo(gx - 4, gy - 4); g.lineTo(gx + 4, gy + 4); g.moveTo(gx + 4, gy - 4); g.lineTo(gx - 4, gy + 4); g.stroke();
    g.globalAlpha = 1;
  }
}
/* ---------------- zoom dial (bottom rim, 4:30 to 7:30) ---------------- */
function drawZoomDial() {
  /* it shares the bottom rim with the time rings, so it steps aside while they're out */
  const vis = st.zoomE = Math.max(0, 1 - st.rings.e * 3) * (1 - st.lockK);
  if (vis < 0.01) return;
  g.globalAlpha = vis;
  g.strokeStyle = '#141414'; g.lineWidth = ZOOM.w; g.lineCap = 'round';
  arcPhi(ZOOM.r, ZOOM.phi0 + 2, ZOOM.phi1 - 2); g.stroke(); g.lineCap = 'butt';
  for (const [z, s] of [[0.5, '0.5'], [1, '1'], [2, '2'], [4, '4']]) {
    const f = zoomPhi(z), [x0, y0] = polar(f, ZOOM.r - ZOOM.w / 2 + 2), [x1, y1] = polar(f, ZOOM.r + ZOOM.w / 2 - 2);
    g.strokeStyle = '#3a3a3a'; g.lineWidth = 1; g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke();
    const [lx, ly] = polar(f, ZOOM.r - ZOOM.w / 2 - 9); label(s, lx, ly, '#555', 'center', 8);
  }
  /* - at the 7:30 end, + at the 4:30 end */
  const [mx, my] = polar(ZOOM.phi1 - 6, ZOOM.r), [px, py] = polar(ZOOM.phi0 + 6, ZOOM.r);
  g.strokeStyle = COL.sec; g.lineWidth = 2; g.lineCap = 'round';
  g.beginPath(); g.moveTo(mx - 4, my); g.lineTo(mx + 4, my); g.stroke();
  g.beginPath(); g.moveTo(px - 4, py); g.lineTo(px + 4, py); g.moveTo(px, py - 4); g.lineTo(px, py + 4); g.stroke(); g.lineCap = 'butt';
  /* the thumb: white pill, black ink (NOS "on"), riding the arc */
  const [tx, ty] = polar(zoomPhi(st.zoom), ZOOM.r), t = `${st.zoom < 1 ? st.zoom.toFixed(2) : st.zoom.toFixed(1)}\u00d7`;
  pill(tx, ty, lw(t, 9) + 14, 18, COL.pri); label(t, tx, ty, '#000', 'center', 9);
  g.globalAlpha = 1;
}
const onZoom = (x, y) => { const r = Math.hypot(x - C, y - C), f = phi360(x, y); return st.zoomE > 0.5 && r >= ZOOM.r - ZOOM.w / 2 - 10 && f >= ZOOM.phi0 - 4 && f <= ZOOM.phi1 + 4; };
function setZoom(z, fromDrag) {
  z = Math.max(ZOOM.min, Math.min(ZOOM.max, z));
  if (fromDrag && Math.abs(Math.log(z)) < 0.035) z = 1;   /* a gentle magnet at 1x */
  const marks = [0.5, 1, 2, 4], crossed = marks.some((m) => (st.zoom - m) * (z - m) < 0 || (z === m && st.zoom !== m));
  st.zoom = z; if (crossed) tick();
}

/* ---------------- control menu ---------------- */
const MENU = [
  { id: 'drag', name: 'DRAG', icon: () => opt.drag,
    opts: [['pan', 'PAN', () => opt.drag === 'pan'], ['look', 'LOOK', () => opt.drag === 'look'], ['time', 'TIME', () => opt.drag === 'time'],
           ['centre', 'RECENTRE', () => false]] },
  { id: 'view', name: 'VIEW', icon: () => st.tilt ? 'tilt' : 'overhead',
    opts: [['overhead', 'OVERHEAD', () => !st.tilt], ['tilt', 'TILT', () => !!st.tilt]] },
  { id: 'filter', name: 'FILTER', icon: () => 'filter',
    opts: [['planets', 'PLANETS', () => opt.show.planets], ['asteroids', 'ASTEROIDS', () => opt.show.asteroids], ['comets', 'COMETS', () => opt.show.comets],
           ['craft', 'CRAFT', () => opt.show.craft], ['fromEarth', 'FROM EARTH', () => opt.fromEarth]] },
  { id: 'lines', name: 'LINES', icon: () => 'lines', toggle: () => opt.lines },
];
/* line icons on an 18 px grid: 2 px strokes, round caps */
function icon(name, x, y, col, sc = 0.9) {
  g.save(); g.translate(x, y); g.scale(sc, sc); g.strokeStyle = col; g.fillStyle = col; g.lineWidth = 2; g.lineCap = 'round'; g.lineJoin = 'round';
  const L = (pts) => { g.beginPath(); pts.forEach(([a, b], i) => (i ? g.lineTo(a, b) : g.moveTo(a, b))); g.stroke(); };
  const dot = (a, b, r) => { g.beginPath(); g.arc(a, b, r, 0, 7); g.fill(); };
  const ring = (a, b, r) => { g.beginPath(); g.arc(a, b, r, 0, 7); g.stroke(); };
  switch (name) {
    case 'pan':   L([[-8, 0], [8, 0]]); L([[0, -8], [0, 8]]);
                  L([[-5, -3], [-8, 0], [-5, 3]]); L([[5, -3], [8, 0], [5, 3]]); L([[-3, -5], [0, -8], [3, -5]]); L([[-3, 5], [0, 8], [3, 5]]); break;
    case 'time':  ring(0, 0, 8); L([[0, -5], [0, 0], [4, 2]]); break;
    case 'look':  g.beginPath(); g.arc(0, 0, 8, -2.6, 1.9); g.stroke(); L([[-2.6, 8.6], [-2.6, 4.2], [1.6, 6.4]]); dot(0, 0, 2.4); break;   /* orbit arrow round a dot */
    case 'centre': ring(0, 0, 6); dot(0, 0, 2); L([[0, -10], [0, -8]]); L([[0, 8], [0, 10]]); L([[-10, 0], [-8, 0]]); L([[8, 0], [10, 0]]); break;
    case 'overhead': ring(0, 0, 8); dot(0, 0, 2.2); dot(5.7, -5.7, 2.2); break;
    case 'tilt':  g.beginPath(); g.ellipse(0, 1, 9, 3.6, 0, 0, 7); g.stroke(); dot(0, 1, 2.2); dot(6, -1.5, 2.2); L([[0, -1], [0, -7]]); break;
    case 'filter': L([[-8, -6], [8, -6], [2, 1], [2, 7], [-2, 5], [-2, 1], [-8, -6]]); break;
    case 'planets': ring(0, 0, 5); g.beginPath(); g.ellipse(0, 0, 9.5, 3, -0.35, 0, 7); g.stroke(); break;
    case 'asteroids': dot(-5, -3, 2.2); dot(3, -6, 1.6); dot(6, 2, 2.4); dot(-2, 6, 1.5); dot(1, 0, 1.2); break;
    case 'comets': dot(5, -5, 3); L([[2, -2], [-8, 8]]); L([[4, 0], [-5, 9]]); L([[0, -4], [-9, 5]]); break;
    case 'craft': g.strokeRect(-3, -3, 6, 6); L([[-3, 0], [-5, 0]]); L([[3, 0], [5, 0]]); g.strokeRect(-10, -3, 5, 6); g.strokeRect(5, -3, 5, 6); break;
    case 'fromEarth': ring(-5, 5, 3.2); g.setLineDash([2, 2.5]); L([[-3, 3], [7, -7]]); L([[-2, 5], [8, 2]]); g.setLineDash([]); dot(7, -7, 1.8); dot(8, 2, 1.8); break;
    case 'lines': g.setLineDash([2.5, 2.5]); L([[-9, 7], [9, -7]]); g.setLineDash([]); dot(-7, 5.5, 2.4); dot(0, 0, 2.4); dot(7, -5.5, 2.4); break;
  }
  g.restore();
}
function drawMenu() {
  const M = st.menu, e = M.e;
  if (st.lockK > 0.99) { M.hits = []; return; }
  g.globalAlpha = 1 - st.lockK;
  /* the handle: a long arc on the right rim; a 3-dot chevron inside it points the way in, and goes once the drawer's out */
  g.strokeStyle = e > 0.5 ? COL.pri : '#6a6a6a'; g.lineWidth = 6; g.lineCap = 'round';
  arcPhi(HANDLE.r, HANDLE.phi0, HANDLE.phi1); g.stroke(); g.lineCap = 'butt';
  if (e < 0.6) {
    g.globalAlpha = 1 - e / 0.6; g.fillStyle = COL.sec;
    const cx = C + 212;
    for (const [dx, dy] of [[0, -6], [-5, 0], [0, 6]]) { g.beginPath(); g.arc(cx + dx, C + dy, 1.8, 0, 7); g.fill(); }
    g.globalAlpha = 1;
  }
  M.hits = [];
  if (e < 0.01) { g.globalAlpha = 1; return; }
  MENU.forEach((m, i) => {
    /* buttons come in from the rim, one frame apart, onto an arc inside it */
    const k = STEADY(Math.max(0, Math.min(1, M.k * 1.25 - i * 0.08))), phi = MENU_PHI[i], [x, y] = polar(phi, MENU_R + (1 - k) * 70);
    const open = M.sub === m.id, on = m.toggle ? m.toggle() : false;
    g.fillStyle = on ? COL.pri : '#1c1c1c'; g.beginPath(); g.arc(x, y, BTN, 0, 7); g.fill();
    if (open) { g.strokeStyle = COL.pri; g.lineWidth = 2; g.beginPath(); g.arc(x, y, BTN + 3, 0, 7); g.stroke(); }
    icon(m.icon(), x, y, on ? '#000' : m.toggle ? '#777' : COL.pri, 0.8);
    if (k > 0.9) M.hits.push({ kind: 'main', id: m.id, name: m.name, x, y, r: 26 });
    if (!m.opts || M.sub !== m.id || M.subE < 0.01) return;
    /* its options fan inward, centred on it */
    const n = m.opts.length;
    m.opts.forEach(([key, txt, onf], j) => {
      const fphi = phi + (j - (n - 1) / 2) * OPT_STEP, op = phi + (fphi - phi) * M.subE, or = MENU_R + (OPT_R - MENU_R) * M.subE;
      const [ox, oy] = polar(op, or), lit = onf();
      g.fillStyle = lit ? COL.pri : '#262626'; g.beginPath(); g.arc(ox, oy, BTN - 1, 0, 7); g.fill();
      icon(key, ox, oy, lit ? '#000' : COL.sec, 0.72);
      if (M.subE > 0.9) M.hits.push({ kind: 'opt', id: m.id, name: m.name, key, txt, x: ox, y: oy, r: 24 });
    });
  });
  g.globalAlpha = 1;
}
function menuAction(h) {
  const M = st.menu;
  if (h.kind === 'main') {
    if (h.id === 'lines') { opt.lines = !opt.lines; M.sub = null; toast(`LINES · ${opt.lines ? 'ON' : 'OFF'}`); }
    else { M.sub = M.sub === h.id ? null : h.id; M.subK = 0; if (M.sub) toast(h.name); }
    return tick();
  }
  if (h.id === 'drag') { if (h.key === 'centre') st.panAnim = { fx: st.pan.x, fy: st.pan.y, t: 0, n: 10 }; else opt.drag = h.key; }
  else if (h.id === 'view') st.tilt = h.key === 'tilt' ? 1 : 0;
  else if (h.id === 'filter') { if (h.key === 'fromEarth') opt.fromEarth = !opt.fromEarth; else opt.show[h.key] = !opt.show[h.key]; }
  /* icons only on the glass: a short read-out names what you just set */
  const state = h.id === 'filter' ? ((h.key === 'fromEarth' ? opt.fromEarth : opt.show[h.key]) ? ' ON' : ' OFF') : '';
  toast(`${h.name} · ${h.txt}${state}`);
  tick();
}
/* generous circles; where two overlap, the nearest centre wins */
const menuHit = (x, y) => {
  let best = null, bd = 1e9;
  for (const h of st.menu.hits) { const d = Math.hypot(x - h.x, y - h.y); if (d < h.r && d < bd) { bd = d; best = h; } }
  return best;
};

function drawSun(a, R, dim) {
  const ink = dim ? COL.dis : opt.tint ? TINT.sun : COL.pri;
  g.globalAlpha = a; g.fillStyle = ink;
  g.beginPath(); g.arc(C, C, R, 0, 7); g.fill();   /* a plain flat disc: no rays, nothing glows */
  g.globalAlpha = 1;
}
/* PAN: the map slides under your finger; hit targets and body positions move with it */
function drawOrreryPanned(dim) {
  const px = st.pan.x, py = st.pan.y;
  if (!px && !py) return drawOrrery(dim);
  g.save(); g.translate(px, py); drawOrrery(dim); g.restore();
  for (const h of st.hits) { h.x += px; h.y += py; }
  for (const k in st.pos) st.pos[k] = [st.pos[k][0] + px, st.pos[k][1] + py];
}
/* the single red element: what the active event looks like */
function drawEventGeometry(S, pos) {
  const ev = activeEvent();
  if (!ev || !ev.bodies.length) return;
  const near = Math.abs(ev.jd - simJD());
  const a = near < 2 ? 1 : Math.max(0.35, 1 - near / 60);
  g.strokeStyle = COL.red; g.lineWidth = 1.5; g.globalAlpha = a;
  const E0 = pos.earth, line = (p, q) => { g.beginPath(); g.moveTo(p[0], p[1]); g.lineTo(q[0], q[1]); g.stroke(); };
  if (ev.kind === 'opposition' || ev.kind === 'solar') line([C, C], pos[ev.bodies[0]]), line([C, C], E0);
  else if (ev.kind === 'conjunction') ev.bodies.forEach((b) => line(E0, pos[b]));
  else if (ev.kind === 'elongation') { line(E0, pos[ev.bodies[0]]); line(E0, [C, C]); }
  else if (ev.kind === 'lineup') {
    const lons = ev.bodies.map((b) => Math.atan2(pos[b][0] - E0[0], pos[b][1] - E0[1]));
    ev.bodies.forEach((b) => { g.setLineDash([2, 3]); line(E0, pos[b]); g.setLineDash([]); });
  } else if (ev.kind === 'mission' && pos[ev.bodies[0]]) { g.beginPath(); g.arc(pos[ev.bodies[0]][0], pos[ev.bodies[0]][1], 9, 0, 7); g.stroke(); }
  if (near < 1.5) { g.beginPath(); g.arc(pos[ev.bodies[0]][0], pos[ev.bodies[0]][1], 14 + 3 * Math.sin(st.frame / 6), 0, 7); g.stroke(); }
  g.globalAlpha = 1;
}

/* ---------------- HUD: time, scrub read-out, event chip ---------------- */
function drawHud(dim) {
  const d = new Date(E.msOf(simJD())), mixF0 = zoneMix('face');
  const hh = String(d.getHours()).padStart(2, '0'), mm = String(d.getMinutes()).padStart(2, '0');
  const scrubbing = Math.abs(st.off) > 0.02, ringsUp = !dim && st.rings.e > 0.5;
  const dd = st.dragging, fingerMoving = dd && dd.moved && (dd.mode === 'scrub' || dd.mode === 'ring');
  if (ringsUp) {
    /* with the rings open the middle is the read-out of the date they set, in any zone */
    const days = Math.round(st.off), DOW = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][d.getDay()];
    pill(C, C + 2, 196, 92, '#000');
    dotText(`${String(d.getDate()).padStart(2, '0')}.${String(d.getMonth() + 1).padStart(2, '0')}.${String(d.getFullYear()).slice(2)}`, C, C - 14, 3.6, COL.pri);
    label(`${DOW} ${hh}:${mm}${scrubbing ? ` · ${days > 0 ? '+' : ''}${days} D` : ' · NOW'}`, C, C + 14, COL.sec, 'center', 10);
    if (scrubbing && !fingerMoving) {
      pill(C, C + 42, 74, 26, COL.pri); label('NOW', C, C + 42, '#000', 'center', 10);
      st.chip = { kind: 'now', x: C, y: C + 42, w: 96, h: 40 };
    } else st.chip = null;
    drawHoldRing();
    if (st.pulse > 0) st.pulse--;
    return;
  }
  const mixF = mixF0;
  /* the clock belongs to the locked watch face; unlocked, the Sun has the centre and a travelled date sits at the top */
  const tA = mixF * (dim ? 1 : st.lockK);
  if (!dim && mixF > 0.01 && st.lockK < 0.99 && scrubbing) {
    const days = Math.round(st.off);
    g.globalAlpha = mixF * (1 - st.lockK);
    label(`${fmtDate(simJD())} · ${days > 0 ? '+' : ''}${days} D`, C, 32, COL.sec, 'center', 10);
    g.globalAlpha = 1;
  }
  if (tA > 0.01) {
    g.globalAlpha = tA;
    if (!scrubbing) {
      dotText(`${hh}:${mm}`, C, C - 4, 4.6, dim ? COL.sec : COL.pri);
      label(fmtDate(simJD(), false) + ' ' + ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][d.getDay()], C, C + 30, dim ? COL.dis : COL.sec, 'center', 10);
    } else {
      const days = Math.round(st.off);
      dotText((days > 0 ? '+' : '') + days, C, C - 6, 4.2, COL.pri);
      label(fmtDate(simJD()), C, C + 26, COL.sec, 'center', 10);
    }
    g.globalAlpha = 1;
  }
  if (mixF < 0.99) {
    g.globalAlpha = 1 - mixF;
    if (st.rings.e < 0.05 && (scrubbing || st.lockK > 0.5)) label(scrubbing ? fmtDate(simJD()) : `${hh}:${mm}`, C, 22, COL.sec, 'center', 11);
    const z = { inner: 'INNER', outer: 'OUTER', beyond: 'BEYOND' }[st.view] || '';
    if (z && !dim && st.view !== 'beyond') label(z, C, CHIP_Y + 26, COL.dis, 'center', 9);
    g.globalAlpha = 1;
  }
  if (dim) return;
  /* bottom chip: NOW when you've travelled, else the next event. NOW stays put while a finger is down on it:
   * it used to vanish on touch-down, so the tap fell through to the outer ring */
  if (st.screen === 'orrery' && mixF > 0.5) {
    if (scrubbing && !fingerMoving) {
      pill(C, CHIP_Y, 92, 34, COL.pri); label('NOW', C, CHIP_Y, '#000', 'center', 12);
      st.chip = { kind: 'now', x: C, y: CHIP_Y, w: 120, h: 50 };
    } else if (!scrubbing) {
      const ev = activeEvent() || events().find((e) => e.jd >= simJD());
      if (ev) {
        const txt = `${shortTitle(ev)} · ${fmtIn(ev.jd - simJD())}`, w = lw(txt, 10) + 34;
        pill(C, CHIP_Y, w, 30, COL.surf2);
        g.fillStyle = COL.red; g.beginPath(); g.arc(C - w / 2 + 13, CHIP_Y, 3.5, 0, 7); g.fill();
        label(txt, C + 7, CHIP_Y, COL.pri, 'center', 10);
        st.chip = { kind: 'events', x: C, y: CHIP_Y, w: Math.max(w, 140), h: 50 };
      }
    } else st.chip = null;
  } else st.chip = null;
  drawHoldRing();
  if (st.pulse > 0) { g.strokeStyle = COL.pri; g.globalAlpha = st.pulse / 8; g.lineWidth = 2; g.beginPath(); g.arc(C, C, 231, 0, 7); g.stroke(); g.globalAlpha = 1; st.pulse--; }
}
function drawHoldRing() {
  const dh = st.dragging;
  if (!(dh && dh.hold && !dh.moved && st.screen === 'orrery')) return;
  const t = performance.now() - dh.t0;
  if (t < HOLD_SHOW || dh.stage === 2) return;
  /* 32 dots round the finger: the first half fills by 0.75 s (3D), the second by 1.5 s (lock / unlock) */
  const k = Math.min(1, t / HOLD_LOCK), cx = dh.x0, cy = dh.y0, R = 46;
  g.fillStyle = '#000'; g.beginPath(); g.arc(cx, cy, R + 8, 0, 7); g.arc(cx, cy, R - 8, 0, 7, true); g.fill();
  for (let i = 0; i < 32; i++) {
    const a = i / 32 * Math.PI * 2, on = i < Math.ceil(k * 32), mark = i === 16;
    g.fillStyle = on ? COL.pri : mark ? COL.dis : COL.line2;
    g.beginPath(); g.arc(cx + R * Math.sin(a), cy - R * Math.cos(a), mark ? 3 : on ? 2.2 : 1.6, 0, 7); g.fill();
  }
  const what = t < HOLD_TILT ? '' : st.tilt ? 'FLAT' : '3D';
  const next = st.locked ? 'UNLOCK' : 'LOCK';
  label(t < HOLD_TILT ? next : `${what} · ${next}`, cx, cy - R - 16, t < HOLD_TILT ? COL.dis : COL.pri, 'center', 9);
}
function shortTitle(ev) {
  const N = (b) => NAME[b].toUpperCase();
  switch (ev.kind) {
    case 'opposition': return `${N(ev.bodies[0])} OPPOSITION`;
    case 'conjunction': return `${N(ev.bodies[0])} + ${N(ev.bodies[1])}`;
    case 'elongation': return `${N(ev.bodies[0])} MAX ELONG.`;
    case 'solar': return `${N(ev.bodies[0])} ${/inferior/.test(ev.title) ? 'INF.' : 'SUP.'} CONJ.`;
    case 'lineup': return `${ev.bodies.length}-PLANET LINE-UP`;
    case 'mission': return 'BEPICOLOMBO ARRIVES';
    case 'flyby': return 'ASTEROID FLYBY';
    case 'comet': return 'COMET PERIHELION';
  }
  return ev.title.toUpperCase();
}

/* ================= the globe: lookup-table sphere renderer ================= */
const TEXDATA = {};
function texture(name) {
  if (TEXDATA[name] !== undefined) return TEXDATA[name];
  TEXDATA[name] = null;
  if (!TEX[name]) return null;
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    const x = c.getContext('2d', { willReadFrequently: true }); x.drawImage(img, 0, 0);
    TEXDATA[name] = { w: img.width, h: img.height, px: new Uint32Array(x.getImageData(0, 0, img.width, img.height).data.buffer),
                      a: name === 'saturn_ring' ? x.getImageData(0, 0, img.width, img.height).data : null };
  };
  img.src = TEX[name];
  return null;
}
const TEXNAME = { sun: 'sun', mercury: 'mercury', venus: 'venus', earth: 'earth', moon: 'moon', mars: 'mars', jupiter: 'jupiter',
                  saturn: 'saturn', uranus: 'uranus', neptune: 'neptune' };

/* The table the watch would build once per card (and per tilt step): for every pixel of the disc,
 * its latitude and longitude (with the planet's real tilt toward you) and its view-space normal.
 * Per frame only a longitude add, a texture fetch and a 3-term dot product remain. */
class Globe {
  constructor() { this.key = ''; this.img = null; this.tmp = document.createElement('canvas'); }
  build(R, lat0, rings) {
    const key = `${R}|${lat0.toFixed(1)}|${rings}`;
    if (key === this.key) return;
    this.key = key; this.R = R;
    const ext = rings ? Math.ceil(R * 2.35) : R, N = 2 * ext;
    this.ext = ext; this.N = N;
    this.tmp.width = N; this.tmp.height = N; this.img = this.tmp.getContext('2d').createImageData(N, N);
    const cl = Math.cos(lat0 * D2R), sl = Math.sin(lat0 * D2R);
    /* view basis in body frame (sub-observer longitude 0): z toward you, y = projected pole, x = y cross z */
    const zv = [cl, 0, sl], yv = [-sl, 0, cl], xv = [0, 1, 0];
    this.basis = { xv, yv, zv };
    const n = N * N;
    this.lat = new Float32Array(n); this.lon = new Float32Array(n); this.nz = new Float32Array(n);
    this.nx = new Float32Array(n); this.ny = new Float32Array(n); this.ring = new Float32Array(n).fill(-1); this.ringFront = new Uint8Array(n);
    this.rx = new Float32Array(n); this.ry = new Float32Array(n);
    for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
      const k = j * N + i, px = (i + 0.5 - ext) / R, py = -(j + 0.5 - ext) / R, rr = px * px + py * py;
      if (rr <= 1) {
        const pz = Math.sqrt(1 - rr);
        const bx = px * xv[0] + py * yv[0] + pz * zv[0], by = px * xv[1] + py * yv[1] + pz * zv[1], bz = px * xv[2] + py * yv[2] + pz * zv[2];
        this.lat[k] = Math.asin(bz) * R2D; this.lon[k] = Math.atan2(by, bx) * R2D;
        this.nx[k] = px; this.ny[k] = py; this.nz[k] = pz;
      } else this.nz[k] = -1;
      if (rings) {
        /* orthographic ray toward the viewer hits the equator plane at t */
        const dz = zv[2]; if (Math.abs(dz) < 1e-4) continue;
        const t = -(px * xv[2] + py * yv[2]) / dz;
        const hx = px * xv[0] + py * yv[0] + t * zv[0], hy = px * xv[1] + py * yv[1] + t * zv[1];
        const rho = Math.hypot(hx, hy);
        if (rho > 1.11 && rho < 2.33) {
          this.ring[k] = (rho - 1.11) / (2.33 - 1.11); this.rx[k] = hx; this.ry[k] = hy;
          this.ringFront[k] = rr > 1 || t > Math.sqrt(1 - rr) ? 1 : 0;
        }
      }
    }
  }
  /* sun: planetocentric lat/lon of the sub-solar point relative to the sub-observer longitude */
  render(body, spin, sunLat, sunDLon, mode, tempFn, free) {
    const T = texture(TEXNAME[body] || ''), N = this.N, out = new Uint32Array(this.img.data.buffer);
    const night = body === 'earth' ? texture('earth_night') : null, ringT = body === 'saturn' ? texture('saturn_ring') : null;
    const { xv, yv, zv } = this.basis;
    /* sun vector in body frame (sub-observer lon = 0), then in view frame */
    const sl = (sunDLon) * D2R, sb = [Math.cos(sunLat * D2R) * Math.cos(sl), Math.cos(sunLat * D2R) * Math.sin(sl), Math.sin(sunLat * D2R)];
    const sx = sb[0] * xv[0] + sb[1] * xv[1] + sb[2] * xv[2], sy = sb[0] * yv[0] + sb[1] * yv[1] + sb[2] * yv[2], sz = sb[0] * zv[0] + sb[1] * zv[1] + sb[2] * zv[2];
    /* FREE: a light just over your shoulder (up-left, toward the planet) so every visible pixel is lit, with a little relief */
    const hx = -0.25, hy = 0.3, hz = 0.92;
    const ringLitSide = free || sb[2] * zv[2] > 0;   /* you see the rings' sunlit face when you're on the Sun's side of them */
    const emissive = body === 'sun';
    for (let k = 0; k < N * N; k++) {
      let c = 0;
      const nz = this.nz[k];
      if (nz >= 0) {
        const lon = this.lon[k] + spin, lat = this.lat[k];
        let r, gg, b;
        /* lReal: the real Sun (heat always uses it); l: what shades the picture */
        const lReal = this.nx[k] * sx + this.ny[k] * sy + nz * sz;
        const l = free ? Math.max(0.35, this.nx[k] * hx + this.ny[k] * hy + nz * hz) : lReal;
        if (mode === 'heat' && tempFn) {
          const hc = tempFn(lat, lon, lReal);
          r = hc[0]; gg = hc[1]; b = hc[2];
        } else if (T) {
          const u = ((((lon + 180) / 360) % 1) + 1) % 1, v = (90 - lat) / 180;
          const p = T.px[Math.min(T.h - 1, (v * T.h) | 0) * T.w + Math.min(T.w - 1, (u * T.w) | 0)];
          r = p & 255; gg = (p >> 8) & 255; b = (p >> 16) & 255;
          if (emissive) { const ld = 0.55 + 0.45 * Math.pow(nz, 0.6); r *= ld; gg *= ld; b *= ld; }
          else {
            const lit = Math.max(0, l), s = 0.05 + 0.95 * Math.pow(lit, 0.85);
            if (night && !free && l < 0.12) {
              const q = night.px[Math.min(night.h - 1, (v * night.h) | 0) * night.w + Math.min(night.w - 1, (u * night.w) | 0)];
              const m = Math.min(1, (0.12 - l) / 0.15);
              r = r * s + (q & 255) * 0.9 * m; gg = gg * s + ((q >> 8) & 255) * 0.8 * m; b = b * s + ((q >> 16) & 255) * 0.6 * m;
            } else { r *= s; gg *= s; b *= s; }
          }
        } else {
          /* no public map (Pluto): its own tint, lit by the Sun */
          const t = parseInt((TINT[body] || '#969696').slice(1), 16), s = 0.05 + 0.95 * Math.max(0, l);
          r = ((t >> 16) & 255) * s; gg = ((t >> 8) & 255) * s; b = (t & 255) * s;
        }
        c = 0xff000000 | (Math.min(255, b) << 16) | (Math.min(255, gg) << 8) | Math.min(255, r);
      }
      if (ringT && this.ring[k] >= 0 && (this.ringFront[k] || nz < 0)) {
        const x = Math.min(ringT.w - 1, (this.ring[k] * ringT.w) | 0), a = ringT.a[x * 4 + 3] / 255;
        if (a > 0.02) {
          /* the planet's shadow on the rings */
          const qx = this.rx[k], qy = this.ry[k], dot = qx * sb[0] + qy * sb[1];
          const perp = Math.hypot(qx - dot * sb[0], qy - dot * sb[1], -dot * sb[2]);
          const lit = free ? 0.95 : (dot < 0 && perp < 1) ? 0.12 : ringLitSide ? 0.95 : 0.4;
          const rr = ringT.a[x * 4] * lit, rg = ringT.a[x * 4 + 1] * lit, rb = ringT.a[x * 4 + 2] * lit;
          const br = c ? c & 255 : 0, bg = c ? (c >> 8) & 255 : 0, bb = c ? (c >> 16) & 255 : 0;
          c = 0xff000000 | ((bb + (rb - bb) * a) << 16) | ((bg + (rg - bg) * a) << 8) | (br + (rr - br) * a);
        }
      }
      out[k] = c;
    }
    this.tmp.getContext('2d').putImageData(this.img, 0, 0);
    return !!T;
  }
}
const GLOBES = {};
const globeFor = (b) => GLOBES[b] || (GLOBES[b] = new Globe());

/* 8 discrete heat steps (inferno), NOS-style: progress and scales are discrete */
const HEAT = [[52, 20, 120], [92, 24, 128], [140, 36, 112], [188, 55, 84], [226, 88, 50], [246, 140, 20], [250, 200, 45], [252, 255, 164]];
function heatColor(t) { const i = Math.max(0, Math.min(7, Math.floor(t * 8))); return HEAT[i]; }

/* surface temperature models (K): good to tens of kelvin, labelled as models in the UI */
const TMODEL = {
  mercury: { range: [90, 700], f: (lat, h, l, r) => surfaceT(l, h, 700 * Math.sqrt(0.387 / r), 100, 300, 25) },
  moon: { range: [90, 400], f: (lat, h, l) => surfaceT(l, h, 390, 95, 250, 30) },
  mars: { range: [145, 300], f: (lat, h, l, r, sunLat) => Math.abs(lat) > 72 && lat * sunLat < 0 ? 150 : surfaceT(l, h, 290 * Math.sqrt(1.52 / r), 175, 230, 40) },
  venus: { range: [700, 745], f: (lat) => 737 - 4 * Math.pow(Math.abs(Math.sin(lat * D2R)), 2) },
  earth: { range: [220, 310], f: (lat, h, l, r, sunLat) => 300 - 70 * Math.pow(Math.sin((lat - 0.45 * sunLat) * D2R), 2) + (l > 0 ? 8 * l : -4) },
  jupiter: { range: [160, 170], f: (lat) => 165 + 2 * Math.cos(lat * 9 * D2R) },
  saturn: { range: [130, 138], f: (lat) => 134 + 2 * Math.cos(lat * 7 * D2R) },
  uranus: { range: [72, 80], f: (lat) => 76 + 1.5 * Math.cos(lat * 4 * D2R) },
  neptune: { range: [68, 76], f: (lat) => 72 + 1.5 * Math.cos(lat * 4 * D2R) },
  sun: { range: [5000, 6000], f: (lat, h, l) => 5772 },
  pluto: { range: [33, 55], f: (lat, h, l) => 40 + 12 * Math.max(0, l) },
};
/* airless day side: radiative equilibrium; night: cooling since sunset (h = degrees past the evening terminator) */
function surfaceT(l, h, Tss, Tnight, Tdusk, tau) {
  if (l > 0) return Math.max(Tnight, Tss * Math.pow(l, 0.25));
  return Tnight + (Tdusk - Tnight) * Math.exp(-Math.max(0, h) / tau);
}

/* ---------------- planet card ---------------- */
function pagesFor(b) { return b === 'sun' || b === 'moon' ? ['globe', 'heat', 'facts'] : ['globe', 'heat', 'moons', 'facts']; }
function openCard(body, x, y) {
  st.card = { body, page: 0, pagePos: 0, pageVel: 0, spin: 0, spinVel: 0, tilt: 0, tiltVel: 0, holdSpin: 0, slide: null, mode: opt.globeMode, view: 'now' };
  startIris('card', x, y);
}
function bodyGeometry(body) {
  const jd = simJD();
  if (body === 'pluto') return { obs: { lat: 0, lon: 0 }, sun: { lat: 0, lon: 0 } };
  return E.geometry(body, jd);
}
function cardSky(body) {
  const S = sky();
  if (body === 'sun') return { dist: Math.hypot(S.earth.x, S.earth.y, S.earth.z), r: 0 };
  if (body === 'moon') return { dist: S.moon.km / 149597870.7, r: Math.hypot(S.earth.x, S.earth.y, S.earth.z) };
  if (body === 'earth') return { dist: 0, r: Math.hypot(S.earth.x, S.earth.y, S.earth.z) };
  const h = S.bodies[body].helio, e = S.earth;
  return { dist: Math.hypot(h.x - e.x, h.y - e.y, h.z - e.z), r: Math.hypot(h.x, h.y, h.z), b: S.bodies[body] };
}
function lightTime(au) { const s = au * 499.005; return s < 60 ? `${s.toFixed(1)} S` : s < 3600 ? `${Math.floor(s / 60)} MIN ${Math.round(s % 60)} S` : `${Math.floor(s / 3600)} H ${Math.round((s % 3600) / 60)} MIN`; }

function drawCard(ox) {
  const cd = st.card, pages = pagesFor(cd.body);
  for (let i = 0; i < pages.length; i++) {
    const y0 = (i - cd.pagePos) * W;
    if (y0 > W || y0 < -W) continue;
    g.save(); g.translate(ox || 0, y0);
    ({ globe: pageGlobe, heat: pageHeat, moons: pageMoons, facts: pageFacts })[pages[i]](cd.body, cd);
    g.restore();
  }
  /* page dots on the right edge */
  for (let i = 0; i < pages.length; i++) {
    const on = Math.abs(i - cd.pagePos) < 0.5;
    g.fillStyle = on ? COL.pri : COL.line2; g.beginPath(); g.arc(446, C - (pages.length - 1) * 7 + i * 14, on ? 3 : 2.5, 0, 7); g.fill();
  }
}
function pageGlobe(body, cd) {
  const R = body === 'saturn' ? 88 : 118, cy = 214, G = bodyGeometry(body), cs = cardSky(body);
  const lat0 = Math.max(-89, Math.min(89, G.obs.lat + cd.tilt)), globe = globeFor(body);
  globe.build(R, lat0, body === 'saturn');
  const tf = TMODEL[body], [t0, t1] = tf.range;
  /* spinning moves YOU round the planet: the Sun stays put in space, so its longitude relative to
   * your view shifts by the spin and the night side comes round when you turn the globe */
  const sunDLon = G.sun.lon - G.obs.lon - cd.spin, free = cd.view === 'free';
  const tempFn = (lat, lon, l) => {
    const h = E.norm360(lon - G.sun.lon - 90);   /* surface degrees of rotation since sunset (prograde) */
    return heatColor((tf.f(lat, h, l, cs.r || 1, G.sun.lat) - t0) / (t1 - t0));
  };
  const mode = cd.mode;
  const ok = globe.render(body, cd.spin + G.obs.lon, G.sun.lat, sunDLon, mode === 'heat' ? 'heat' : 'photo', tempFn, free);
  /* the texture's longitude 0 is at its centre; the LUT already holds lon relative to the sub-observer point */
  if (mode === 'dot') drawHalftone(globe, C, cy);
  else g.drawImage(globe.tmp, C - globe.ext, cy - globe.ext);
  if (!ok && mode !== 'heat' && body !== 'pluto') label('[ LOADING ]', C, cy, COL.sec);
  st.globeHit = { x: C, y: cy, r: R };
  words(NAME[body], C, 50, 24, COL.pri);
  /* hero line */
  if (body === 'earth') {
    dotText(cs.r.toFixed(3), C, 354, 3.4, COL.pri); label('AU FROM THE SUN', C, 378, COL.sec, 'center', 10);
  } else {
    dotText(cs.dist < 0.01 ? Math.round(cs.dist * 149597870.7).toString() : cs.dist.toFixed(cs.dist < 2 ? 3 : 2), C, 354, 3.4, COL.pri);
    label(cs.dist < 0.01 ? `KM FROM YOU · ${lightTime(cs.dist)}` : `AU FROM YOU · ${lightTime(cs.dist)}`, C, 378, COL.sec, 'center', 10);
  }
  let sub = '';
  if (cs.b) sub = cs.b.elong < 15 ? `LOST IN THE SUN · ${cs.b.elong.toFixed(0)}°` : `${cs.b.side.toUpperCase()} SKY · ${cs.b.elong.toFixed(0)}° FROM SUN`;
  if (body === 'moon') { const S = sky(); sub = `${Math.round(S.moonLit * 100)}% LIT · ${S.moonWaxing ? 'WAXING' : 'WANING'}`; }
  if (body === 'saturn') sub = `RINGS OPEN ${Math.abs(G.obs.lat).toFixed(1)}° · ` + sub.split(' · ')[0];
  if (body === 'venus' && cs.b) sub = `${Math.round(cs.b.lit * 100)}% LIT · ` + sub.split(' · ')[0];
  if (sub) label(sub, C, 396, COL.dis, 'center', 9);
  label(`[ ${mode.toUpperCase()} ]`, C, 82, COL.dis, 'center', 9);
  /* NOW | FREE: a two-segment switch in the thumb zone (on = white fill + black ink) */
  const sw = 168, sh = 34, sy0 = 428;
  pill(C, sy0, sw, sh, COL.surf2);
  const onX = free ? C + sw / 4 : C - sw / 4;
  pill(onX, sy0, sw / 2 - 4, sh - 6, COL.pri);
  label('NOW', C - sw / 4, sy0, free ? COL.sec : '#000', 'center', 10);
  label('FREE', C + sw / 4, sy0, free ? '#000' : COL.sec, 'center', 10);
  st.viewHit = { x: C, y: sy0, w: sw + 20, h: 50 };
}
/* DOT: the texture as a colour halftone. Each 4 px cell becomes one dot: its colour is the cell's
 * average hue at full strength, its area the cell's brightness, so the map reads at arm's length */
function drawHalftone(globe, cx, cy) {
  const N = globe.N, src = globe.tmp.getContext('2d').getImageData(0, 0, N, N).data, p = 4, ext = globe.ext;
  let mx = 0.05;
  for (let i = 0; i < src.length; i += 4 * 7) if (src[i + 3]) mx = Math.max(mx, (src[i] * 0.3 + src[i + 1] * 0.59 + src[i + 2] * 0.11) / 255);
  for (let y0 = 0; y0 < N; y0 += p) for (let x0 = 0; x0 < N; x0 += p) {
    let r = 0, gg = 0, b = 0, n = 0;
    for (let y = y0; y < Math.min(N, y0 + p); y++) for (let x = x0; x < Math.min(N, x0 + p); x++) {
      const i = (y * N + x) * 4; if (!src[i + 3]) continue;
      r += src[i]; gg += src[i + 1]; b += src[i + 2]; n++;
    }
    if (!n) continue;
    r /= n; gg /= n; b /= n;
    const L = Math.min(1, (r * 0.3 + gg * 0.59 + b * 0.11) / 255 / mx), cover = n / (p * p);
    if (L < 0.04) continue;
    const m = Math.max(r, gg, b, 1), k = (0.6 + 0.4 * L) * 255 / m;   /* hue at full strength; brightness goes into size */
    g.fillStyle = `rgb(${Math.min(255, r * k) | 0},${Math.min(255, gg * k) | 0},${Math.min(255, b * k) | 0})`;
    g.beginPath(); g.arc(cx - ext + x0 + p / 2, cy - ext + y0 + p / 2, Math.max(0.45, p * 0.56 * Math.sqrt(L * cover)), 0, 7); g.fill();
  }
}
function pageHeat(body) {
  words('Heat', C, 50, 24);
  const P = PROFILE[body];
  label(P.title, C, 80, COL.sec, 'center', 10);
  /* plot box */
  const x0 = 92, x1 = 380, y0 = 120, y1 = 320;
  g.strokeStyle = COL.line2; g.lineWidth = 1; g.beginPath(); g.moveTo(x0, y1); g.lineTo(x1, y1); g.moveTo(x0, y0); g.lineTo(x0, y1); g.stroke();
  const [xa, xb] = P.x, [ya, yb] = P.y;
  const X = (v) => x0 + (x1 - x0) * (P.xlog ? Math.log10(v / xa) / Math.log10(xb / xa) : (v - xa) / (xb - xa));
  const fy = (v) => (P.ylog ? Math.log10(v / ya) / Math.log10(yb / ya) : (v - ya) / (yb - ya));
  const Y = (v) => (P.yinv ? y0 + (y1 - y0) * fy(v) : y1 - (y1 - y0) * fy(v));
  /* dot-matrix curve: one dot per 6 px along x, colour = its temperature step */
  const pitch = 6, tr = P.trange || [Math.min(...P.pts.map((p) => p[1])), Math.max(...P.pts.map((p) => p[1]))];
  if (P.vertical) {
    for (let py = y0; py <= y1; py += pitch) {
      const f = P.yinv ? (py - y0) / (y1 - y0) : (y1 - py) / (y1 - y0);
      const v = P.ylog ? ya * Math.pow(yb / ya, f) : ya + (yb - ya) * f;
      const t = interp(P.pts, v);
      const c = heatColor((t - tr[0]) / (tr[1] - tr[0])); g.fillStyle = `rgb(${c})`;
      g.beginPath(); g.arc(X(t), py, 2.4, 0, 7); g.fill();
    }
  } else {
    for (let px = x0; px <= x1; px += pitch) {
      const v = xa + (xb - xa) * (px - x0) / (x1 - x0), t = interp(P.pts, v);
      const c = heatColor((t - tr[0]) / (tr[1] - tr[0])); g.fillStyle = `rgb(${c})`;
      g.beginPath(); g.arc(px, Y(t), 2.4, 0, 7); g.fill();
    }
  }
  for (const [v, s] of P.xt) label(s, X(v), y1 + 14, COL.dis, 'center', 9);
  for (const [v, s] of P.yt) label(s, x0 - 8, Y(v), COL.dis, 'right', 9);
  if (P.mark) { const [v, s] = P.mark; g.strokeStyle = COL.line2; g.setLineDash([2, 3]); g.beginPath();
    if (P.vertical) { g.moveTo(x0, Y(v)); g.lineTo(x1, Y(v)); } else { g.moveTo(X(v), y0); g.lineTo(X(v), y1); }
    g.stroke(); g.setLineDash([]); label(s, P.vertical ? x1 : X(v), P.vertical ? Y(v) - 9 : y0 - 8, COL.sec, P.vertical ? 'right' : 'center', 9); }
  const lo = Math.min(...P.pts.map((p) => p[1])), hi = Math.max(...P.pts.map((p) => p[1]));
  const cel = (k) => `${Math.round(k - 273.15)}`;
  dotText(lo === hi ? cel(lo) : `${cel(lo)} ${cel(hi)}`.replace(' ', '  '), C, 370, 3.4, COL.pri);
  label(lo === hi ? '°C' : '°C  LOW  ·  HIGH', C, 396, COL.sec, 'center', 10);
  label(P.note, C, 418, COL.dis, 'center', 9);
}
function interp(pts, v) {
  if (v <= pts[0][0]) return pts[0][1];
  for (let i = 1; i < pts.length; i++) if (v <= pts[i][0]) { const [a, b] = pts[i - 1], [c, d] = pts[i]; return b + (d - b) * (v - a) / (c - a); }
  return pts[pts.length - 1][1];
}
/* temperature profiles: rocky bodies across one solar day at the equator; giants with depth (pressure) */
const hrs = [[0, '0H'], [6, '6H'], [12, 'NOON'], [18, '18H'], [24, '24H']];
const PROFILE = {
  mercury: { title: 'EQUATOR · ONE SOLAR DAY (176 D)', x: [0, 24], y: [50, 750], xt: hrs, yt: [[100, '100K'], [400, '400K'], [700, '700K']],
             pts: [[0, 105], [5.9, 100], [6, 150], [7, 450], [9, 610], [12, 700], [15, 610], [17, 450], [18, 300], [19, 180], [21, 125], [24, 105]], note: 'MODEL · NO AIR TO HOLD THE HEAT', mark: [12, 'SUN OVERHEAD'] },
  venus: { title: 'ALTITUDE · SAME DAY OR NIGHT', x: [150, 760], y: [0, 100], vertical: true, xt: [[200, '200K'], [450, '450K'], [700, '700K']], yt: [[0, '0KM'], [50, '50'], [100, '100']],
           pts: [[0, 737], [10, 658], [20, 577], [30, 496], [40, 417], [50, 350], [55, 302], [60, 263], [70, 230], [80, 197], [90, 170], [100, 175]], note: 'VIRA MODEL · 92 BAR AT THE GROUND', mark: [55, 'EARTH-LIKE 30°C, 0.5 BAR'] },
  earth: { title: 'MEAN SURFACE · BY LATITUDE', x: [-90, 90], y: [200, 320], xt: [[-90, 'S'], [0, 'EQ'], [90, 'N']], yt: [[220, '220K'], [270, '270K'], [310, '310K']],
           pts: [[-90, 224], [-75, 240], [-60, 268], [-45, 281], [-30, 290], [-15, 297], [0, 299], [15, 299], [30, 293], [45, 281], [60, 267], [75, 255], [90, 250]], note: 'CLIMATE MEAN · REAL DAY USES YOUR BME280' },
  moon: { title: 'EQUATOR · ONE LUNAR DAY (29.5 D)', x: [0, 24], y: [50, 420], xt: hrs, yt: [[100, '100K'], [250, '250K'], [400, '400K']],
          pts: [[0, 100], [5.9, 95], [6, 140], [8, 330], [12, 390], [16, 330], [18, 250], [19, 160], [21, 115], [24, 100]], note: 'LRO DIVINER-LIKE MODEL', mark: [12, 'SUN OVERHEAD'] },
  mars: { title: 'EQUATOR · ONE SOL (24 H 40 M)', x: [0, 24], y: [160, 310], xt: hrs, yt: [[180, '180K'], [240, '240K'], [300, '300K']],
          pts: [[0, 195], [3, 188], [6, 184], [8, 220], [10, 262], [13, 293], [15, 285], [17, 262], [19, 225], [21, 208], [24, 195]], note: 'GROUND · CURIOSITY REMS-LIKE', mark: [13, 'WARMEST'] },
  jupiter: { title: 'DEPTH · PRESSURE (BAR)', x: [100, 450], y: [0.001, 20], ylog: true, vertical: true, xt: [[110, '110K'], [250, '250K'], [420, '420K']],
             yt: [[0.001, '1MB'], [0.1, '0.1'], [1, '1 BAR'], [20, '20']], pts: [[0.001, 160], [0.01, 150], [0.1, 110], [0.3, 120], [1, 165], [3, 230], [10, 330], [20, 410]], note: 'GALILEO PROBE · SAME ALL ROUND', mark: [1, '1 BAR 165K'] },
  saturn: { title: 'DEPTH · PRESSURE (BAR)', x: [70, 300], y: [0.001, 10], ylog: true, vertical: true, xt: [[80, '80K'], [180, '180K'], [280, '280K']],
            yt: [[0.001, '1MB'], [0.1, '0.1'], [1, '1 BAR'], [10, '10']], pts: [[0.001, 140], [0.01, 130], [0.1, 84], [0.3, 95], [1, 134], [3, 180], [10, 265]], note: 'VOYAGER & CASSINI RADIO', mark: [1, '1 BAR 134K'] },
  uranus: { title: 'DEPTH · PRESSURE (BAR)', x: [40, 160], y: [0.001, 10], ylog: true, vertical: true, xt: [[50, '50K'], [100, '100K'], [150, '150K']],
            yt: [[0.001, '1MB'], [0.1, '0.1'], [1, '1 BAR'], [10, '10']], pts: [[0.001, 110], [0.01, 80], [0.1, 53], [0.3, 58], [1, 76], [3, 102], [10, 150]], note: 'VOYAGER 2 · COLDEST AIR IN THE SYSTEM', mark: [0.1, '53K'] },
  neptune: { title: 'DEPTH · PRESSURE (BAR)', x: [40, 160], y: [0.001, 10], ylog: true, vertical: true, xt: [[50, '50K'], [100, '100K'], [150, '150K']],
             yt: [[0.001, '1MB'], [0.1, '0.1'], [1, '1 BAR'], [10, '10']], pts: [[0.001, 140], [0.01, 100], [0.1, 55], [0.3, 60], [1, 72], [3, 97], [10, 145]], note: 'VOYAGER 2 · HOTTER INSIDE THAN THE SUN GIVES', mark: [1, '1 BAR 72K'] },
  sun: { title: 'INSIDE OUT · RADIUS', x: [0, 1.3], y: [3000, 2e7], ylog: true, xt: [[0, 'CORE'], [0.7, '0.7'], [1, 'SURFACE'], [1.3, 'CORONA']],
         yt: [[1e4, '10K'], [1e6, '1M'], [1.5e7, '15M']], pts: [[0, 1.57e7], [0.25, 7e6], [0.5, 3.5e6], [0.7, 2e6], [0.9, 5e5], [0.99, 2e4], [1, 5772], [1.01, 4400], [1.05, 1e4], [1.1, 5e5], [1.3, 1.5e6]], note: 'WHY THE CORONA IS HOTTER IS STILL OPEN', trange: [3000, 2e7] },
  pluto: { title: 'SURFACE · ONE DAY (6.4 D)', x: [0, 24], y: [30, 60], xt: hrs, yt: [[35, '35K'], [45, '45K'], [55, '55K']],
           pts: [[0, 37], [6, 36], [9, 42], [12, 48], [15, 45], [18, 40], [24, 37]], note: 'NEW HORIZONS · NITROGEN ICE' },
};
for (const k in PROFILE) if (PROFILE[k].ylog && PROFILE[k].vertical) {
  /* vertical profiles: y is pressure increasing downward */
  const P = PROFILE[k]; P.yinv = true;
}

/* moons and orbiters, seen face-on from above the planet's north pole */
const IAU_POLE = { mercury: [281.01, 61.42], venus: [272.76, 67.16], earth: [0, 90], mars: [317.27, 54.43], jupiter: [268.06, 64.50],
                   saturn: [40.59, 83.54], uranus: [257.31, -15.18], neptune: [299.36, 43.46], pluto: [132.99, -6.16] };
function poleFrame(body, v) {
  const [ra, de] = IAU_POLE[body], q = E.ecl2eq(v);
  const a1 = (90 + ra) * D2R, c1 = Math.cos(a1), s1 = Math.sin(a1);
  const x = c1 * q.x + s1 * q.y, y = -s1 * q.x + c1 * q.y, z = q.z;
  const a2 = (90 - de) * D2R, c2 = Math.cos(a2), s2 = Math.sin(a2);
  return { x, y: c2 * y + s2 * z };
}
function pageMoons(body) {
  const jd = simJD(), S = sky();
  words(body === 'earth' ? 'Moon & craft' : 'Moons & craft', C, 50, 24);
  const moons = A.moons.filter((m) => m.planet === body);
  const craft = A.craft.filter((c) => c.planet === body);
  const Rk = RADIUS_KM[body], cx = C, cy = 236;
  const fresh = moons.filter((m) => m.fresh || moons.length < 6);
  const minor = moons.filter((m) => !fresh.includes(m));
  const P = body === 'pluto' ? S.bodies.pluto.helio : S.bodies[body].helio;
  const toSun = poleFrame(body, { x: -P.x, y: -P.y, z: -P.z }), toEarth = poleFrame(body, { x: S.earth.x - P.x, y: S.earth.y - P.y, z: S.earth.z - P.z });
  const items = [];
  if (body === 'earth') {
    const m = S.moon, v = { x: m.km * Math.cos(m.lat * D2R) * Math.cos(m.lon * D2R), y: m.km * Math.cos(m.lat * D2R) * Math.sin(m.lon * D2R), z: m.km * Math.sin(m.lat * D2R) };
    items.push({ name: 'Moon', v: poleFrame('earth', v), km: m.km, major: true, info: `${Math.round(m.km).toLocaleString()} KM · ${Math.round(S.moonLit * 100)}% LIT` });
    const jw = A.craft.find((c) => c.name === 'JWST');
    if (jw) { const d = { x: (jw.xyz[0] - S.earth.x) * 149597870.7, y: (jw.xyz[1] - S.earth.y) * 149597870.7, z: (jw.xyz[2] - S.earth.z) * 149597870.7 };
      items.push({ name: 'JWST', craft: true, v: poleFrame('earth', d), km: Math.hypot(d.x, d.y, d.z), info: `JWST · ${(Math.hypot(d.x, d.y, d.z) / 1e6).toFixed(2)} M KM · SUN-EARTH L2 · ${fmtDate(A.jd)}` }); }
  }
  for (const m of fresh) {
    const p = E.satPos(m, jd), v = m.fresh ? poleFrame(body, p) : { x: p.x, y: p.y };
    items.push({ name: m.name, v, km: Math.hypot(p.x, p.y, p.z), major: true, info: `${m.name.toUpperCase()} · ${Math.round(m.a).toLocaleString()} KM · ${m.P.toFixed(2)} D` });
  }
  for (const c of craft) if (c.orbit) {
    const p = E.satPos(c.orbit, jd);
    items.push({ name: c.name, craft: true, v: poleFrame(body, p), km: Math.hypot(p.x, p.y, p.z), info: `${c.name.toUpperCase()} · ${c.status.toUpperCase()}` });
  }
  /* scale: sqrt of distance in planet radii; the farthest labelled item sits at 186 px */
  const maxR = Math.max(4, ...items.map((i) => Math.sqrt(i.km / Rk)));
  const k = 178 / maxR, planetR = Math.max(9, Math.min(18, k * 1));
  const rpx = (km) => Math.min(196, k * Math.sqrt(km / Rk));
  /* orbits of the labelled ones */
  g.strokeStyle = COL.line; g.lineWidth = 1;
  for (const it of items) if (!it.craft) { g.beginPath(); g.arc(cx, cy, rpx(it.km), 0, 7); g.stroke(); }
  if (body === 'saturn') { g.strokeStyle = COL.line2; g.lineWidth = 4; g.beginPath(); g.arc(cx, cy, k * Math.sqrt(1.9), 0, 7); g.stroke(); g.lineWidth = 1; }
  /* minor moons: a band of dots at their real angles */
  for (const m of minor) {
    const p = E.satPos(m, jd), a = Math.atan2(p.y, p.x), r = 200 + (m.code % 7);
    g.fillStyle = COL.dis; g.fillRect(cx + r * Math.cos(a) - 0.75, cy - r * Math.sin(a) - 0.75, 1.5, 1.5);
  }
  /* sun and earth directions on the rim (their labels yield to the title) */
  PLACED = [[C - 90, 30, C + 90, 88]];
  const rimDir = (v, s, col) => { const a = Math.atan2(v.y, v.x), x = cx + 214 * Math.cos(a), y = cy - 214 * Math.sin(a);
    g.fillStyle = col; g.beginPath(); g.arc(x, y, 3, 0, 7); g.fill(); freeLabel(s, cx + 199 * Math.cos(a), cy - 199 * Math.sin(a) + (y > cy ? -2 : 2), COL.dis, 'center', 8); };
  rimDir(toSun, 'SUN', COL.sec);
  if (body !== 'earth') rimDir(toEarth, 'YOU', COL.sec);
  disc(cx, cy, planetR, body, cx + 100 * toSun.x / Math.hypot(toSun.x, toSun.y), cy - 100 * toSun.y / Math.hypot(toSun.x, toSun.y));
  st.moonHits = [];
  items.sort((a, b) => (a.craft ? 1 : 0) - (b.craft ? 1 : 0));
  for (const it of items) {
    const a = Math.atan2(it.v.y, it.v.x), r = rpx(it.km), x = cx + r * Math.cos(a), y = cy - r * Math.sin(a);
    if (it.craft) { g.strokeStyle = COL.sec; g.beginPath(); g.moveTo(x - 3, y - 3); g.lineTo(x + 3, y + 3); g.moveTo(x + 3, y - 3); g.lineTo(x - 3, y + 3); g.stroke(); }
    else { g.fillStyle = COL.pri; g.beginPath(); g.arc(x, y, 3, 0, 7); g.fill(); }
    freeLabel(it.name.replace(' (EMM)', ''), x, y + (y > cy ? 12 : -11), it.craft ? COL.dis : COL.sec, 'center', 8);
    st.moonHits.push({ x, y, r: 16, info: it.info });
  }
  if (!items.length) {
    const lines = { mercury: ['NO MOONS', bepiLine()], venus: ['NO MOONS', 'NO ORBITER SINCE AKATSUKI ENDED, SEP 2025'] }[body] || ['NO MOONS'];
    lines.forEach((s, i) => label(s, C, 330 + i * 18, i ? COL.dis : COL.sec, 'center', i ? 9 : 11));
  }
  const n = moons.length + (body === 'earth' ? 1 : 0);
  label(n ? `${n} KNOWN · ${craft.length} ORBITER${craft.length === 1 ? '' : 'S'}` : fmtDate(jd, false), C, 80, COL.dis, 'center', 9);
}
function bepiLine() {
  const b = A.craft.find((c) => c.name === 'BepiColombo'), m = E.helio('mercury', A.jd);
  if (!b) return '';
  const km = Math.hypot(b.xyz[0] - m.x, b.xyz[1] - m.y, b.xyz[2] - m.z) * 149597870.7;
  return `BEPICOLOMBO ${(km / 1e6).toFixed(1)} M KM OUT · ARRIVES ${fmtIn(MISSION[0].jd - simJD())}`;
}
const FACTS = {
  sun: [['RADIUS', '696,000 KM'], ['SURFACE', '5,500 °C'], ['SPIN', '25 D AT EQUATOR'], ['MASS', '333,000 EARTHS'], ['AGE', '4.6 BN YEARS']],
  mercury: [['RADIUS', '2,440 KM'], ['SOLAR DAY', '176 D'], ['YEAR', '88 D'], ['GRAVITY', '0.38 G'], ['MOONS', 'NONE']],
  venus: [['RADIUS', '6,052 KM'], ['SOLAR DAY', '117 D, BACKWARDS'], ['YEAR', '225 D'], ['GROUND', '464 °C · 92 BAR'], ['GRAVITY', '0.90 G']],
  earth: [['RADIUS', '6,371 KM'], ['DAY', '24 H'], ['YEAR', '365.25 D'], ['TILT', '23.4°'], ['MOONS', '1 + 12,000 SATELLITES']],
  moon: [['RADIUS', '1,737 KM'], ['MONTH', '29.5 D'], ['GRAVITY', '0.17 G'], ['DRIFTING', '3.8 CM / YEAR AWAY']],
  mars: [['RADIUS', '3,390 KM'], ['SOL', '24 H 40 M'], ['YEAR', '687 D'], ['GRAVITY', '0.38 G'], ['HIGHEST', 'OLYMPUS MONS 22 KM']],
  jupiter: [['RADIUS', '69,911 KM'], ['DAY', '9 H 56 M'], ['YEAR', '11.9 Y'], ['GRAVITY', '2.53 G'], ['RED SPOT', 'WIDER THAN EARTH']],
  saturn: [['RADIUS', '58,232 KM'], ['DAY', '10 H 33 M'], ['YEAR', '29.5 Y'], ['DENSITY', 'LESS THAN WATER'], ['RINGS', '~10 M THICK']],
  uranus: [['RADIUS', '25,362 KM'], ['DAY', '17 H 14 M'], ['YEAR', '84 Y'], ['TILT', '98° · ROLLS ON ITS SIDE'], ['VISITED', 'VOYAGER 2, 1986']],
  neptune: [['RADIUS', '24,622 KM'], ['DAY', '16 H 6 M'], ['YEAR', '165 Y'], ['WINDS', '2,100 KM/H'], ['VISITED', 'VOYAGER 2, 1989']],
  pluto: [['RADIUS', '1,188 KM'], ['DAY', '6.4 D'], ['YEAR', '248 Y'], ['MOONS', '5 · CHARON HALF ITS SIZE'], ['VISITED', 'NEW HORIZONS, 2015']],
};
function pageFacts(body) {
  words('Facts', C, 50, 24);
  const cs = cardSky(body), rows = FACTS[body].slice();
  if (body !== 'sun' && body !== 'earth' && body !== 'moon') rows.unshift(['FROM SUN NOW', `${cs.r.toFixed(2)} AU`]);
  const ev = events().find((e) => e.bodies.includes(body) && e.jd >= simJD() - 1);
  if (ev) rows.push(['NEXT', `${shortTitle(ev)} · ${fmtDate(ev.jd, false)}`]);
  rows.slice(0, 7).forEach(([k, v], i) => {
    const y = 104 + i * 44, span = Math.sqrt(Math.max(0, 210 * 210 - (y - C) * (y - C)));
    g.fillStyle = COL.line; g.fillRect(C - span + 30, y + 21, 2 * span - 60, 1);
    label(k, C - span + 34, y, COL.dis, 'left', 9);
    label(v, C + span - 34, y, COL.pri, 'right', 10);
  });
}

/* ---------------- events list ---------------- */
function drawEvents() {
  const evs = events().filter((e) => e.jd >= nowJD() - 0.5).slice(0, 40), rowH = 66, top = 96 - st.list.y;
  words('Events', C, 50, 24);
  st.evHits = [];
  g.save(); g.beginPath(); g.rect(0, 76, W, W - 76); g.clip();
  evs.forEach((e, i) => {
    const y = top + i * rowH;
    if (y < 40 || y > 470) return;
    const span = Math.sqrt(Math.max(0, 222 * 222 - (y + 20 - C) * (y + 20 - C)));
    if (span < 150) return;
    const x0 = 92;   /* one left edge for every row; titles clip to the circle */
    if (i === 0) { g.fillStyle = COL.red; g.beginPath(); g.arc(x0 + 4, y + 9, 3.5, 0, 7); g.fill(); }
    label(`${fmtDate(e.jd, false)} · ${fmtIn(e.jd - nowJD())}`, x0 + (i === 0 ? 14 : 0), y + 9, COL.sec, 'left', 9);
    const room = C + span - 24 - x0;
    let t = e.title; g.font = '500 16px "Segoe UI", system-ui, sans-serif';
    while (g.measureText(t).width > room && t.length > 4) t = t.slice(0, -2) + '…';
    words(t, x0, y + 31, 16, COL.pri, 'left', 500);
    g.fillStyle = COL.line; g.fillRect(x0, y + 52, W - 2 * x0, 1);
    st.evHits.push({ y0: y, y1: y + rowH, e });
  });
  g.restore();
  st.list.max = Math.max(0, evs.length * rowH - 300);
}

/* ---------------- toast (component: 7-9 frames in) ---------------- */
function toast(s) { st.toast = { s, t: 0, life: 75 }; }
function drawToast() {
  const t = st.toast; if (!t) return;
  t.t++; if (t.t > t.life) { st.toast = null; return; }
  const k = Math.min(1, t.t / 8, (t.life - t.t) / 8), y = 334 + (1 - STEADY(Math.max(0, k))) * 40;
  const s = t.s.length > 40 ? t.s.slice(0, 39) + '…' : t.s, w = Math.min(380, lw(s, 10) + 36);
  g.globalAlpha = Math.max(0, k); pill(C, y, w, 34, COL.surf2); label(s, C, y, COL.pri, 'center', 10); g.globalAlpha = 1;
}

/* ---------------- iris (screen transition, 12 frames, hard edge + 2 px grey rim) ---------------- */
function startIris(to, x, y) { st.iris = { to, x, y, t: 0, n: 13, back: false }; }
function closeIris() {
  const b = st.card && st.card.body, p = b && st.pos && st.pos[b === 'moon' ? 'earth' : b];
  st.iris = { to: 'orrery', x: p ? p[0] : st.screen === 'events' ? C : C, y: p ? p[1] : st.screen === 'events' ? CHIP_Y : C, t: 0, n: 11, back: true, from: st.screen };
}
function drawScreen(which) {
  if (which === 'orrery') { drawOrreryPanned(false); drawHud(false); drawTimeRings(); drawZoomDial(); drawMenu(); }
  else if (which === 'card') drawCardSlide();
  else drawEvents();
}
const neighbour = (b, dir) => CARD_ORDER[(CARD_ORDER.indexOf(b) + dir + CARD_ORDER.length) % CARD_ORDER.length];
function drawBodyAt(b, ox) {
  const cd = st.card, keep = [cd.body, cd.pagePos, cd.spin, cd.tilt];
  cd.body = b; cd.pagePos = Math.min(cd.pagePos, pagesFor(b).length - 1);
  if (b !== keep[0]) { cd.spin = 0; cd.tilt = 0; }
  drawCard(ox);
  [cd.body, cd.pagePos, cd.spin, cd.tilt] = keep;
}
function drawCardSlide() {
  const cd = st.card;
  if (cd.slide) {
    const sl = cd.slide, k = sl.k0 + (1 - sl.k0) * STEADY(Math.min(1, sl.t / sl.n)), dir = sl.dir;
    drawBodyAt(sl.from, -dir * k * W); drawBodyAt(cd.body, dir * (1 - k) * W);
  } else if (cd.peek) {
    /* the neighbour follows the finger in: drag left shows the next body, right the previous */
    drawBodyAt(cd.body, cd.peek); drawBodyAt(neighbour(cd.body, cd.peek < 0 ? 1 : -1), cd.peek + (cd.peek < 0 ? W : -W));
  } else drawCard(0);
}
/* interactive back: the screen follows the finger off to the right, the face shows underneath */
function drawBackDrag(which) {
  drawScreen('orrery');
  g.save(); g.translate(st.backX, 0);
  g.fillStyle = '#000'; g.beginPath(); g.arc(C, C, C, 0, 7); g.fill();
  g.strokeStyle = '#333'; g.lineWidth = 2; g.beginPath(); g.arc(C, C, C - 1, 0, 7); g.stroke();
  g.beginPath(); g.arc(C, C, C, 0, 7); g.clip(); drawScreen(which); g.restore();
}

/* ---------------- frame ---------------- */
function frame() {
  st.frame++;
  /* zone morph */
  if (st.from) { st.tx += 1 / st.txN; if (st.tx >= 1) { st.tx = 1; st.from = null; } }
  /* time: momentum after a scrub, or the animated return/jump */
  if (st.timeAnim) {
    const a = st.timeAnim; a.t++; const k = INOUT(Math.min(1, a.t / a.n)); st.off = a.from + (a.to - a.from) * k;
    if (a.t >= a.n) { st.off = a.to; st.timeAnim = null; }
  } else if (!st.dragging && Math.abs(st.vel) > 1e-4) { st.off += st.vel; st.vel *= 0.9; snapCheck(false); }
  if (!st.dragging && !st.timeAnim && Math.abs(st.off) < 0.02) st.off = 0;
  /* 3D tilt: 12 frames; the camera angle is yours to drag while tilted */
  if (st.tiltK !== st.tilt) { st.tiltK += Math.sign(st.tilt - st.tiltK) / 12; if (Math.abs(st.tiltK - st.tilt) < 0.04) st.tiltK = st.tilt; }
  st.tiltA = st.camTilt * STEADY(st.tiltK);
  if (!(st.dragging && st.dragging.mode === 'camera')) {
    st.az += st.azVel; st.azVel *= 0.9; if (Math.abs(st.azVel) < 0.02) st.azVel = 0;
    if (!st.tilt) { st.az = E.norm180(st.az) * 0.8; if (Math.abs(st.az) < 0.05) st.az = 0; }   /* flat again: the vernal point returns to 12 */
  }
  /* rings: 10 frames open/close unless a finger is pulling the tab; filter panel: 8 frames */
  const rg = st.rings;
  if (!rg.pulling) { const tgt = rg.open ? 1 : 0; if (rg.k !== tgt) { rg.k += Math.sign(tgt - rg.k) / 10; if (Math.abs(rg.k - tgt) < 0.05) rg.k = tgt; } }
  rg.e = STEADY(Math.max(0, Math.min(1, rg.k)));
  const M = st.menu;
  if (!M.pulling) { const mt = M.open ? 1 : 0; if (M.k !== mt) { M.k += Math.sign(mt - M.k) / 9; if (Math.abs(M.k - mt) < 0.06) M.k = mt; } }
  M.e = STEADY(Math.max(0, Math.min(1, M.k)));
  const sk = M.sub ? 1 : 0; if (M.subK !== sk) { M.subK += Math.sign(sk - M.subK) / 8; if (Math.abs(M.subK - sk) < 0.07) M.subK = sk; }
  M.subE = STEADY(M.subK);
  if (!M.open && M.k === 0) M.sub = null;
  if (st.zoomAnim) {
    const a = st.zoomAnim; a.t++; const k = STEADY(Math.min(1, a.t / a.n));
    st.zoom = Math.exp(Math.log(a.from) + (Math.log(a.to) - Math.log(a.from)) * k);
    if (a.t >= a.n) { st.zoom = a.to; st.zoomAnim = null; }
  }
  /* the controls fade in on unlock, out on lock (8 frames) */
  const lt = st.locked ? 1 : 0; if (st.lockK !== lt) { st.lockK += Math.sign(lt - st.lockK) / 8; if (Math.abs(st.lockK - lt) < 0.07) st.lockK = lt; }
  /* pan: glide after a flick, or ease home on RECENTRE */
  if (st.panAnim) {
    const a = st.panAnim; a.t++; const k = STEADY(Math.min(1, a.t / a.n));
    st.pan.x = a.fx * (1 - k); st.pan.y = a.fy * (1 - k); st.pan.vx = st.pan.vy = 0;
    if (a.t >= a.n) st.panAnim = null;
  } else if (!(st.dragging && st.dragging.mode === 'pan') && (st.pan.vx || st.pan.vy)) {
    st.pan.x = Math.max(-PAN_MAX, Math.min(PAN_MAX, st.pan.x + st.pan.vx)); st.pan.y = Math.max(-PAN_MAX, Math.min(PAN_MAX, st.pan.y + st.pan.vy));
    st.pan.vx *= 0.88; st.pan.vy *= 0.88; if (Math.hypot(st.pan.vx, st.pan.vy) < 0.2) st.pan.vx = st.pan.vy = 0;
  }
  /* card physics */
  const cd = st.card;
  if (cd) {
    /* pages settle with a gentle quadratic in-out (no spring: nothing overshoots or bounces back) */
    if (!(st.dragging && st.dragging.mode === 'pager')) {
      if (!cd.pageAnim && cd.pagePos !== cd.page) {
        const dist = Math.abs(cd.page - cd.pagePos);
        cd.pageAnim = { from: cd.pagePos, to: cd.page, t: 0, n: Math.max(6, Math.ceil(PAGE_FRAMES * dist)) };
      }
      if (cd.pageAnim) {
        const a = cd.pageAnim; a.t++;
        cd.pagePos = a.from + (a.to - a.from) * QUAD(Math.min(1, a.t / a.n));
        if (a.t >= a.n) { cd.pagePos = a.to; cd.pageAnim = null; }
      }
    }
    if (!(st.dragging && st.dragging.mode === 'globe')) {
      cd.spin += cd.spinVel; cd.spinVel *= 0.93;
      if (cd.view !== 'free') {
        if (Math.abs(cd.spinVel) < 0.3) { cd.holdSpin++; if (cd.holdSpin > 45) cd.spin = E.norm180(cd.spin) * 0.86; } else cd.holdSpin = 0;
        cd.tilt *= 0.8; if (Math.abs(cd.spin) < 0.05) cd.spin = 0; if (Math.abs(cd.tilt) < 0.05) cd.tilt = 0;
      }
    }
    if (cd.slide) { cd.slide.t++; if (cd.slide.t >= cd.slide.n) cd.slide = null; }
    if (cd.peek && !(st.dragging && st.dragging.mode === 'swipe')) { cd.peek *= 0.55; if (Math.abs(cd.peek) < 1) cd.peek = 0; }
  }
  if (st.backAnim) {
    const b = st.backAnim; b.t++; st.backX = b.from + (b.to - b.from) * STEADY(Math.min(1, b.t / b.n));
    if (b.t >= b.n) { st.backAnim = null; if (b.to > 0) { st.screen = 'orrery'; st.card = null; } st.backX = 0; }
  }
  if (st.screen === 'events' && !(st.dragging && st.dragging.mode === 'list')) {
    st.list.y += st.list.v; st.list.v *= 0.9;
    if (st.list.y < 0) { st.list.y *= 0.6; st.list.v = 0; }
    if (st.list.y > (st.list.max || 0)) { st.list.y = st.list.max + (st.list.y - st.list.max) * 0.6; st.list.v = 0; }
  }

  g.setTransform(1, 0, 0, 1, 0, 0); g.fillStyle = '#000'; g.fillRect(0, 0, W, W);
  g.save(); g.beginPath(); g.arc(C, C, C, 0, 7); g.clip();
  if (opt.aod) { drawOrreryPanned(true); drawHud(true); }
  else if (st.iris) {
    const ir = st.iris; ir.t++;
    const k = ir.back ? 1 - STEADY(Math.min(1, ir.t / ir.n)) : STEADY(Math.min(1, ir.t / ir.n));
    const far = Math.max(...[[0, 0], [W, 0], [0, W], [W, W]].map(([x, y]) => Math.hypot(x - ir.x, y - ir.y)));
    const R = 6 + (far - 6) * k;
    drawScreen(ir.back ? 'orrery' : st.screen);
    g.save(); g.beginPath(); g.arc(ir.x, ir.y, R, 0, 7); g.clip(); g.fillStyle = '#000'; g.fillRect(0, 0, W, W);
    drawScreen(ir.back ? ir.from : ir.to); g.restore();
    g.strokeStyle = '#555'; g.lineWidth = 2; g.beginPath(); g.arc(ir.x, ir.y, R, 0, 7); g.stroke();
    if (ir.t >= ir.n) { st.screen = ir.back ? 'orrery' : ir.to; if (ir.back) st.card = null; st.iris = null; }
  } else if (st.backX > 0 && st.screen !== 'orrery') drawBackDrag(st.screen);
  else drawScreen(st.screen);
  drawToast();
  if (opt.zones && !st.iris) drawZones();
  g.restore();
  if (window.onOrreryFrame) window.onOrreryFrame(st);
}
function drawZones() {
  g.strokeStyle = 'rgba(80,160,255,.55)'; g.setLineDash([4, 4]); g.lineWidth = 1;
  const zc = 'rgba(80,160,255,.9)';
  /* the edge band, and the left half of it where back starts */
  g.beginPath(); g.arc(C, C, EDGE_R, 0, 7); g.stroke();
  if (st.screen !== 'orrery' || st.view !== 'face') {
    g.save(); g.setLineDash([]); g.strokeStyle = 'rgba(80,160,255,.3)'; g.lineWidth = EDGE_BAND;
    g.beginPath(); g.arc(C, C, C - EDGE_BAND / 2, Math.PI / 2, Math.PI * 1.5); g.stroke(); g.restore();
    label('BACK →', 30, C, zc, 'left', 9);
  }
  if (st.screen === 'card') {
    if (st.card.page === 0 && st.globeHit) { g.beginPath(); g.arc(st.globeHit.x, st.globeHit.y, st.globeHit.r * GLOBE_CORE, 0, 7); g.stroke(); label('SPIN', st.globeHit.x, st.globeHit.y, zc, 'center', 9); }
    label('DIAL ALONG THE RIM', C, 452, zc, 'center', 8);
    label('← NEXT   PREV →  ·  ↕ PAGES', C, 112, zc, 'center', 8);
    g.setLineDash([]); return;
  }
  if (st.screen !== 'orrery') { g.setLineDash([]); return; }
  if (st.view === 'face') {
    for (const r of [HOLD_R, 158, 222]) { g.beginPath(); g.arc(C, C, r, 0, 7); g.stroke(); }
    label(st.locked ? 'LOCKED · HOLD 1.5 S: UNLOCK · 0.75 S: 3D' : 'TAP: SUN · HOLD 0.75 S: 3D · 1.5 S: LOCK', C, C + 46, zc, 'center', 8);
    label(opt.drag === 'time' ? 'DRAG: TIME' : opt.drag === 'look' && st.tilt ? 'DRAG: LOOK AROUND' : 'DRAG: PAN', C, 448, zc, 'center', 8);
    const [tx, ty] = polar(TAB_PHI, TAB_R[1]);
    g.beginPath(); g.arc(tx, ty, 34, 0, 7); g.stroke(); label('TIME', tx - 30, ty + 30, zc, 'center', 8);
    g.save(); g.setLineDash([]); g.strokeStyle = 'rgba(80,160,255,.3)'; g.lineWidth = EDGE_BAND;
    arcPhi(C - 23, HANDLE.phi0 - 8, HANDLE.phi1 + 8); g.stroke(); g.restore();
    label('\u2190 MENU', C + 180, C, zc, 'right', 8);
    g.save(); g.setLineDash([]); g.strokeStyle = 'rgba(80,160,255,.3)'; g.lineWidth = ZOOM.w + 10;
    arcPhi(ZOOM.r - 5, ZOOM.phi0 - 4, ZOOM.phi1 + 4); g.stroke(); g.restore();
    label('ZOOM', C, C + 186, zc, 'center', 8);
  } else for (const r of [st.view === 'outer' ? 50 : 30, 214]) { g.beginPath(); g.arc(C, C, r, 0, 7); g.stroke(); }
  g.setLineDash([]);
}
let last = 0;
let lastErr = '';
function loop(t) {
  requestAnimationFrame(loop);
  if (t - last < 1000 / opt.fps - 2) return;
  last = t;
  try { frame(); }
  catch (e) {
    /* draw-time faults: report once per message, show it on the glass, and keep running so back still works */
    if (g.reset) g.reset(); else g.setTransform(1, 0, 0, 1, 0, 0);   /* an exception can leave save() states unbalanced */
    if (e.message !== lastErr) { lastErr = e.message; console.error('orrery frame:', e); }
    label('[ DRAW ERROR ]', C, C - 8, COL.red, 'center', 11); label(e.message.slice(0, 40), C, C + 12, COL.sec, 'center', 9);
  }
}

/* ---------------- zones ---------------- */
function goView(v) { if (v === st.view) return; st.from = st.view; st.view = v; st.tx = 0; tick(); }
function tick(strong) { if (navigator.vibrate) navigator.vibrate(strong ? 18 : 6); }

/* ---------------- scrub: grab a planet and drag it round the Sun; time follows ---------------- */
function snapCheck(dragging) {
  const jd = simJD(), per = PERIOD[st.grab || 'earth'] || 365.25, win = per * 2.2 / 360;
  const ev = events().find((e) => Math.abs(e.jd - jd) < win && e.rank >= 2 && e.kind !== 'flyby');
  if (ev && st.snapped !== ev) { st.snapped = ev; st.off = ev.jd - nowJD(); st.pulse = 8; tick(true); if (!dragging) st.vel = 0; }
  else if (!ev && st.snapped) st.snapped = null;
}

/* ---------------- input ---------------- */
function hookInput() {
  const P = (e) => { const r = cv.getBoundingClientRect(); return [(e.clientX - r.left) * W / r.width, (e.clientY - r.top) * W / r.height]; };
  cv.addEventListener('pointerdown', (e) => {
    cv.setPointerCapture(e.pointerId);
    const [x, y] = P(e), r = Math.hypot(x - C, y - C);
    const d = st.dragging = { x0: x, y0: y, x, y, t0: performance.now(), moved: false, mode: null, r0: r, a: Math.atan2(-(x - C), -(y - C)), hist: [], trail: [[performance.now(), x, y]] };
    if (st.iris || st.backAnim) { st.dragging = null; return; }
    /* buttons and tabs claim the touch the moment the finger lands, so nothing underneath can steal the tap */
    if (st.screen === 'orrery' && !opt.aod && !st.locked) {
      const [tx, ty] = polar(TAB_PHI, TAB_R[1]), [gx, gy] = polar(GRIP_PHI, RINGS[0].r);
      const ch = st.chip, mh = st.menu.e > 0.5 ? menuHit(x, y) : null;
      if (mh) { d.target = 'menu'; d.hit = mh; }
      else if (ch && Math.abs(x - ch.x) < ch.w / 2 && Math.abs(y - ch.y) < ch.h / 2) { d.target = 'chip'; d.chip = { ...ch }; }
      else if (!st.rings.open && !st.menu.open && st.menu.e < 0.01 && Math.hypot(x - tx, y - ty) < 34) d.target = 'tab';
      else if (onZoom(x, y)) { d.target = 'zoom'; d.phi = phi360(x, y); }
      else if (st.rings.open && Math.hypot(x - gx, y - gy) < 22) d.target = 'grip';
      else if (st.rings.open && st.rings.e > 0.9) {
        const i = RINGS.findIndex((R) => Math.abs(r - R.r) <= RING_W / 2 + 1);
        if (i >= 0) { d.target = 'ring'; d.ring = i; d.phi = phiOf(x, y); }
      }
    }
    d.handle = st.screen === 'orrery' && !opt.aod && !st.locked && onHandle(x, y);
    /* hold anywhere that isn't a control: 0.75 s arms 3D (it happens on release), 1.5 s locks / unlocks right away */
    if (st.screen === 'orrery' && !opt.aod && !d.target && !d.handle) {
      d.hold = true;
      d.lp = setTimeout(() => { if (st.dragging === d && !d.moved) { d.stage = 1; tick(); } }, HOLD_TILT);
      d.lp2 = setTimeout(() => { if (st.dragging === d && !d.moved) { d.stage = 2; d.long = true; toggleLock(); } }, HOLD_LOCK);
    }
    if (st.screen === 'orrery' && !opt.aod) {
      const h = nearest(x, y, (h) => h.id === 'planet');
      st.grab = h ? h.p : st.view === 'outer' ? 'jupiter' : st.view === 'beyond' ? 'neptune' : 'earth';
      st.timeAnim = null; st.vel = 0;
    }
  });
  cv.addEventListener('pointermove', (e) => {
    const d = st.dragging; if (!d) return;
    const [x, y] = P(e), dx = x - d.x, dy = y - d.y;
    d.trail.push([performance.now(), x, y]); if (d.trail.length > 12) d.trail.shift();
    if (!d.moved && Math.hypot(x - d.x0, y - d.y0) > 8) {
      d.moved = true;
      clearTimeout(d.lp); clearTimeout(d.lp2);
      const hx = Math.abs(x - d.x0) > Math.abs(y - d.y0), mx = x - d.x0, my = y - d.y0;
      /* from the edge band: inward heading right from the left side = back; along the rim = dial (cards) */
      const ux = (d.x0 - C) / (d.r0 || 1), uy = (d.y0 - C) / (d.r0 || 1);
      const radial = mx * ux + my * uy, tangential = Math.abs(mx * uy - my * ux);
      const fromEdge = d.r0 >= EDGE_R, inward = radial < 0 && -radial > tangential;
      const edgeBack = fromEdge && inward && mx > 0 && d.x0 < C;
      if (st.screen === 'orrery' && st.locked) d.mode = 'none';   /* locked: a watch face ignores drags */
      else if (st.screen === 'orrery') {
        /* 3D with the time on the rings: a drag on the face orbits the camera so you can look at everything */
        /* the menu's DRAG setting decides what an open drag does: PAN moves the map (or looks around in TILT), TIME scrubs */
        /* pushing the open drawer back to the left closes it, from the handle or from any of its buttons */
        /* the drawer: an inward edge swipe from the right-hand arc pulls it out; an outward swipe (from it or the arc) puts it away */
        const outward = mx > 0 && hx, edgeIn = d.handle && inward && !st.menu.open;
        const pushBack = st.menu.open && outward && (d.target === 'menu' || d.handle);
        d.mode = edgeIn || pushBack ? 'mpull' : d.target === 'ring' ? 'ring' : d.target === 'tab' || d.target === 'grip' ? 'pull'
          : d.target === 'zoom' ? (edgeBack && st.view !== 'face' ? 'zoneback' : 'zoom')
          : d.target ? 'none'
          : edgeBack && st.view !== 'face' ? 'zoneback' : opt.drag === 'time' ? 'scrub' : opt.drag === 'look' && st.tilt ? 'camera' : 'pan';   /* LOOK only means something in 3D */
        if (d.mode === 'pull') { st.rings.pulling = true; d.wasOpen = st.rings.open; }
        if (d.mode === 'mpull') { st.menu.pulling = true; d.wasOpen = st.menu.open; }
        if (d.mode === 'pan') st.panAnim = null;
      }
      else if (st.screen === 'events') d.mode = edgeBack ? 'back' : 'list';
      else if (st.screen === 'card') {
        const cd = st.card, onGlobe = cd.pagePos === 0 && cd.page === 0 && st.globeHit && d.r0 < C - EDGE_KEEP &&
          Math.hypot(d.x0 - st.globeHit.x, d.y0 - st.globeHit.y) < st.globeHit.r * GLOBE_CORE;
        d.mode = edgeBack ? 'back' : fromEdge && !inward ? 'dial' : onGlobe ? 'globe' : hx ? 'swipe' : 'pager';
      }
    }
    if (d.moved) {
      if (d.mode === 'scrub') {
        const a = Math.atan2(-(x - C), -(y - C));
        let da = (a - d.a) * R2D; if (da > 180) da -= 360; if (da < -180) da += 360; d.a = a;
        if (Math.hypot(x - C, y - C) < SCRUB_DEAD) da = 0;   /* near the centre a twitch is a huge angle: ignore it */
        const days = da / 360 * (PERIOD[st.grab] || 365.25);
        st.off += days; d.hist.push([performance.now(), days]); if (d.hist.length > 6) d.hist.shift();
        const before = Math.floor((simJD() - days) / 30.44), after = Math.floor(simJD() / 30.44);
        if (before !== after) tick();
        snapCheck(true);
      } else if (d.mode === 'ring') {
        /* the ring turns under the finger; values to the right of the index are later, so dragging
         * counter-clockwise brings the future up to it, like the planets' own direction */
        const R = RINGS[d.ring], phi = phiOf(x, y), dphi = E.norm180(phi - d.phi); d.phi = phi;
        const days = -dphi / 360 * R.lap, before = simJD();
        st.off += days; d.hist.push([performance.now(), days]); if (d.hist.length > 6) d.hist.shift();
        const unit = (jd) => { const t = new Date(E.msOf(jd)); return R.unit === 'year' ? t.getFullYear() : R.unit === 'month' ? t.getFullYear() * 12 + t.getMonth() : Math.floor(jd + 0.5); };
        if (unit(before) !== unit(simJD())) tick();
        if (R.unit === 'day') { st.grab = 'earth'; snapCheck(true); }
      } else if (d.mode === 'pull') {
        const k = Math.max(0, Math.min(1, Math.hypot(x - d.x0, y - d.y0) / 150));
        st.rings.k = d.wasOpen ? 1 - k : k;
      } else if (d.mode === 'zoom') {
        /* along the dial: toward 4:30 zooms in, toward 7:30 out; the whole 90 deg covers 0.5x to 4x */
        const f = phi360(x, y), df = f - d.phi; d.phi = f;
        st.zoomAnim = null; setZoom(st.zoom * Math.pow(ZOOM.max / ZOOM.min, -df / (ZOOM.phi1 - ZOOM.phi0)), true);
      } else if (d.mode === 'mpull') {
        /* the drawer follows the finger: leftward pulls it in from the right rim, rightward pushes it back out */
        const k = Math.max(0, Math.min(1, (d.x0 - x) / 140));
        st.menu.k = d.wasOpen ? Math.max(0, Math.min(1, 1 - (x - d.x0) / 140)) : k;
      } else if (d.mode === 'pan') {
        st.pan.x = Math.max(-PAN_MAX, Math.min(PAN_MAX, st.pan.x + dx)); st.pan.y = Math.max(-PAN_MAX, Math.min(PAN_MAX, st.pan.y + dy));
      } else if (d.mode === 'camera') {
        st.az += dx * 0.4; st.azVel = dx * 0.4;
        st.camTilt = Math.max(10, Math.min(85, st.camTilt - dy * 0.35));
      } else if (d.mode === 'globe') {
        /* the surface under the finger follows it: drag right turns the near side right (east comes into view from the left) */
        st.card.spin -= dx / st.globeHit.r * R2D; st.card.spinVel = -dx / st.globeHit.r * R2D;
        const tmax = st.card.view === 'free' ? 88 : 70;   /* FREE lets you look over the poles */
        st.card.tilt = Math.max(-tmax, Math.min(tmax, st.card.tilt + dy / st.globeHit.r * R2D)); st.card.holdSpin = 0;
      } else if (d.mode === 'pager') {
        const n = pagesFor(st.card.body).length;
        st.card.pageAnim = null;
        st.card.pagePos = Math.max(-0.25, Math.min(n - 0.75, st.card.pagePos - dy / W)); st.card.pageVel = -dy / W;
      } else if (d.mode === 'list') { st.list.y -= dy; st.list.v = -dy; }
      else if (d.mode === 'back') { st.backX = Math.max(0, x - d.x0); d.vx = dx; }
      else if (d.mode === 'swipe') { st.card.peek = x - d.x0; d.vx = dx; }
      else if (d.mode === 'dial') {
        const a = Math.atan2(-(x - C), -(y - C));
        let da = (a - d.a) * R2D; if (da > 180) da -= 360; if (da < -180) da += 360;
        d.acc = (d.acc || 0) + da; d.a = a;
        if (Math.abs(d.acc) > 34) { switchBody(d.acc < 0 ? 1 : -1); d.acc = 0; }
      }
    }
    d.x = x; d.y = y;
  });
  const up = (e) => {
    const d = st.dragging; if (!d) return;
    const [x, y] = P(e);
    st.dragging = null; clearTimeout(d.lp); clearTimeout(d.lp2);
    if (d.long) return;
    if (!d.moved && d.stage === 1) { st.tilt = st.tilt ? 0 : 1; tick(true); toast(st.tilt ? '3D' : 'FLAT'); return; }
    /* a long press that wasn't the centre hold does nothing: it is never a late tap */
    if (d.mode === 'pull' || (d.target === 'tab' || d.target === 'grip')) st.rings.pulling = false;
    if (d.mode === 'mpull' || d.target === 'menu') st.menu.pulling = false;
    if (!d.moved) return performance.now() - d.t0 < TAP_MAX_MS ? tap(x, y, d) : undefined;
    d.trail.push([performance.now(), x, y]);
    const [vx, vy] = fingerVel(d);
    if (d.mode === 'pull') { st.rings.open = st.rings.k > 0.4; tick(); }
    /* the drawer sits near the rim, so closing needs only a short push (or a flick out); opening needs 40 % of the pull */
    if (d.mode === 'mpull') { st.menu.open = d.wasOpen ? st.menu.k > 0.75 && vx < FLICK : st.menu.k > 0.4 || -vx > FLICK; tick(); }
    if (d.mode === 'pan') { st.pan.vx = vx * 1000 / opt.fps; st.pan.vy = vy * 1000 / opt.fps; }   /* glide on */
    if (d.mode === 'scrub' || d.mode === 'ring') {
      const recent = d.hist.filter(([t]) => performance.now() - t < 120);
      st.vel = recent.length ? recent.reduce((s, [, v]) => s + v, 0) / recent.length * (60 / opt.fps) : 0;
      if (st.snapped) st.vel = 0;
    } else if (d.mode === 'pager') {
      const n = pagesFor(st.card.body).length, cd = st.card;
      /* a swipe commits after ~56 px or a flick */
      const dp = cd.pagePos - cd.page, step = dp > 0.12 || -vy > FLICK ? 1 : dp < -0.12 || vy > FLICK ? -1 : 0;
      cd.page = Math.max(0, Math.min(n - 1, cd.page + step)); cd.pageAnim = null; tick();
    } else if (d.mode === 'back') {
      /* commit past a quarter of the glass or on a flick; otherwise it springs back */
      const go = st.backX > W * 0.25 || vx > FLICK;
      st.backAnim = { from: st.backX, to: go ? W : 0, t: 0, n: go ? 8 : 6 }; if (go) tick();
    } else if (d.mode === 'zoneback') { if (x - d.x0 > 90 || vx > FLICK) goView('face'); }
    else if (d.mode === 'swipe') {
      /* commit past 90 px, or a flick in the same direction as the drag */
      const cd = st.card, p = cd.peek || 0, go = Math.abs(p) > 90 || (Math.abs(vx) > FLICK && Math.sign(vx) === Math.sign(p));
      if (go) { switchBody(p < 0 ? 1 : -1, Math.min(0.95, Math.abs(p) / W)); cd.peek = 0; }
    }
  };
  cv.addEventListener('pointerup', up); cv.addEventListener('pointercancel', up);
  cv.addEventListener('contextmenu', (e) => { e.preventDefault(); back(); });
  /* desktop: the mouse wheel over the glass zooms too */
  cv.addEventListener('wheel', (e) => { if (st.screen !== 'orrery') return; e.preventDefault(); st.zoomAnim = null; setZoom(st.zoom * Math.exp(-e.deltaY * 0.0015)); }, { passive: false });
}
function nearest(x, y, f) {
  let best = null, bd = 1e9;
  for (const h of st.hits) { if (f && !f(h)) continue; const d = Math.hypot(h.x - x, h.y - y); if (d < h.r && d < bd) { bd = d; best = h; } }
  return best;
}
function switchBody(dir, k0 = 0) {
  const cd = st.card;
  cd.slide = { from: cd.body, dir, t: 0, n: 12, k0 }; cd.body = neighbour(cd.body, dir);
  cd.page = Math.min(cd.page, pagesFor(cd.body).length - 1); cd.pagePos = cd.page; cd.spin = 0; cd.tilt = 0; tick();
}
function tap(x, y, d = {}) {
  if (opt.aod) { opt.aod = false; window.onOrreryOpt && window.onOrreryOpt(opt); return; }
  if (st.iris) return;
  const r = st.screen === 'orrery' ? Math.hypot(x - st.pan.x - C, (y - st.pan.y - C) / Math.max(0.3, Math.cos(st.tiltA * D2R))) / (SC() * st.zoom) : Math.hypot(x - C, y - C);
  if (st.screen === 'orrery' && st.locked) { toast('HOLD 1.5 S TO UNLOCK'); return; }
  if (st.screen === 'orrery') {
    const t = d.target;
    if (t === 'menu') return menuAction(d.hit);
    if (st.menu.open) { st.menu.open = false; tick(); return; }   /* a tap anywhere else folds the menu away */
    if (t === 'tab') { st.rings.open = true; tick(); return; }
    if (t === 'grip') { st.rings.open = false; tick(); return; }
    if (t === 'chip') {
      if (d.chip.kind === 'now') { st.timeAnim = { from: st.off, to: 0, t: 0, n: Math.min(24, 10 + Math.abs(st.off) / 60 | 0) }; st.snapped = null; tick(); }
      else { st.list.y = 0; startIris('events', x, y); tick(); }
      return;
    }
    if (t === 'zoom') {
      /* tap a spot on the dial: fly to that zoom (near 1x it lands on 1x) */
      let z = phiZoom(phi360(x, y)); if (Math.abs(Math.log(z)) < 0.12) z = 1;
      st.zoomAnim = { from: st.zoom, to: z, t: 0, n: 10 }; tick(); return;
    }
    if (d.handle && t !== 'ring') { st.menu.open = true; tick(); return; }   /* the arc is clickable too */
    if (t === 'ring') {
      /* tap a value on a ring: fly to it */
      const R = RINGS[d.ring], days = E.norm180(phiOf(x, y)) / 360 * R.lap;
      st.timeAnim = { from: st.off, to: st.off + days, t: 0, n: 14 }; st.snapped = null; tick(); return;
    }
  }
  if (st.screen === 'card') {
    const cd = st.card, pages = pagesFor(cd.body);
    const vh = st.viewHit;
    if (pages[cd.page] === 'globe' && vh && Math.abs(x - vh.x) < vh.w / 2 && Math.abs(y - vh.y) < vh.h / 2) {
      const want = x < vh.x ? 'now' : 'free';
      if (want !== cd.view) { cd.view = want; if (want === 'now') cd.holdSpin = 46; tick(); }   /* NOW springs straight back */
      return;
    }
    if (pages[cd.page] === 'globe' && st.globeHit && Math.hypot(x - st.globeHit.x, y - st.globeHit.y) < st.globeHit.r) {
      const modes = ['photo', 'heat', 'dot']; cd.mode = modes[(modes.indexOf(cd.mode) + 1) % 3]; tick(); return;
    }
    if (pages[cd.page] === 'moons' && st.moonHits) { const h = st.moonHits.find((h) => Math.hypot(h.x - x, h.y - y) < h.r); if (h) { toast(h.info); tick(); return; } }
    return;
  }
  if (st.screen === 'events') {
    const h = (st.evHits || []).find((h) => y >= h.y0 && y < h.y1);
    if (h) { jumpTo(h.e); back(); }
    return;
  }
  /* orrery: bodies, then zones */
  const h = nearest(x, y);
  if (h && h.id === 'planet') { openCard(h.p, h.x, h.y); tick(); return; }
  if (h && h.id === 'sun' && st.view !== 'face') { openCard('sun', C, C); tick(); return; }
  if (h && (h.id === 'small' || h.id === 'comet' || h.id === 'craft')) {
    const S = sky();
    if (h.id === 'craft') toast(`${h.c.name} · ${h.au.toFixed(h.au < 2 ? 2 : 0)} AU · ${h.c.status}`);
    else {
      const o = h.o, p = h.id === 'comet' ? E.conic(o, simJD()) : null, au = h.au || Math.hypot(p.x, p.y, p.z);
      toast(`${(o.short || o.name).replace(/^\(|\)$/g, '')} · ${au.toFixed(2)} AU${o.diam ? ` · ${Math.round(+o.diam)} KM` : ''}`);
    }
    tick(); return;
  }
  if (st.rings.open) return;   /* with the rings open, empty taps don't change zones */
  if (st.view === 'face') {
    if (r < 62) { openCard('sun', C, C); tick(); }
    else if (r < 158) goView('inner'); else if (r < 222) goView('outer'); else goView('beyond');
    return;
  }
  if (y < 44 && Math.abs(x - C) < 70) return goView('face');
  const core = st.view === 'outer' ? 50 : 30;
  if (r < core) goView(st.view === 'inner' ? 'face' : st.view === 'outer' ? 'inner' : 'outer');
  else if (r > 214) goView(st.view === 'inner' ? 'outer' : st.view === 'outer' ? 'beyond' : 'face');
}
function toggleLock() {
  st.locked = !st.locked;
  if (st.locked) { st.menu.open = false; st.rings.open = false; }
  tick(true); toast(st.locked ? 'LOCKED' : 'UNLOCKED');
}
function jumpTo(e) {
  const to = e.jd - nowJD();
  st.timeAnim = { from: st.off, to, t: 0, n: Math.min(30, 14 + Math.abs(to - st.off) / 40 | 0) };
  st.snapped = e.kind === 'flyby' || e.kind === 'comet' ? null : e;
  if (e.bodies.some((b) => OUTER.includes(b)) && !e.bodies.some((b) => INNER.includes(b) && b !== 'earth')) { if (st.view !== 'face') goView('face'); }
  if (e.kind === 'comet') goView('inner');
}
function back() {
  if (st.iris) return;
  if (st.screen === 'card' || st.screen === 'events') { closeIris(); tick(); }
  else if (st.view !== 'face') goView('face');
  else if (st.off) st.timeAnim = { from: st.off, to: 0, t: 0, n: 16 };
}

window.Orrery = {
  setup, opt, st, back, goView, lock: () => toggleLock(), zoom: (z) => { st.zoomAnim = { from: st.zoom, to: z, t: 0, n: 10 }; }, rings: () => { st.rings.open = !st.rings.open; }, menu: () => { st.menu.open = !st.menu.open; }, tilt: () => { st.tilt = st.tilt ? 0 : 1; }, events, now: () => { st.timeAnim = { from: st.off, to: 0, t: 0, n: 18 }; st.snapped = null; },
  jumpTo, openCard: (b) => openCard(b, C, C), fmtDate, fmtIn, shortTitle,
};
})();
