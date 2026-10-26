"""skypack.py - build the sky atlas from public NASA/JPL data.

Today it writes docs/assets/solar/atlas.js for the HTML draft; the same fetch becomes the
watch's SD pack (/sky/*.bin) later. Sources (all public, no key):
  moons        https://ssd.jpl.nasa.gov/sats/elem/          mean elements, every known moon
  small bodies https://ssd-api.jpl.nasa.gov/sbdb.api        osculating elements per object
  comets       https://ssd-api.jpl.nasa.gov/sbdb_query.api  comets near perihelion
  flybys       https://ssd-api.jpl.nasa.gov/cad.api         asteroid close approaches to Earth

usage: python tools/skypack.py [--cache build/solar_dl/api] [--out docs/assets/solar/atlas.js]"""
import argparse, datetime as dt, html, json, os, re, time, urllib.parse, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument('--cache', default='build/solar_dl/api')
ap.add_argument('--out', default='docs/assets/solar/atlas.js')
ap.add_argument('--refresh', action='store_true', help='ignore cached downloads')
A = ap.parse_args()
os.makedirs(A.cache, exist_ok=True)
TODAY = dt.date.today()


def get(name, url):
    p = os.path.join(A.cache, name)
    if A.refresh or not os.path.exists(p):
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'nos-skypack'}), timeout=60) as r:
            open(p, 'wb').write(r.read())
        time.sleep(0.3)
    return open(p, encoding='utf-8').read()


def num(s):
    s = re.sub(r'<[^>]+>', '', s).strip().rstrip('.')
    try: return float(s)
    except ValueError: return None


# ---- moons ----
PLANET_OF = {'3': 'earth', '4': 'mars', '5': 'jupiter', '6': 'saturn', '7': 'uranus', '8': 'neptune', '9': 'pluto'}
page = get('sats.html', 'https://ssd.jpl.nasa.gov/sats/elem/sep.html')
parts = re.split(r'(<table.*?</table>)', page, flags=re.S)
moons = []
for k, p in enumerate(parts):
    if not p.startswith('<table'): continue
    m = re.search(r'Epoch:\s*(\d{4})-(\d\d)-(\d\d)\.(\d+)', html.unescape(re.sub(r'<[^>]+>', ' ', parts[k - 1])))
    if not m: continue   # the column-legend table
    y, mo, d, frac = int(m[1]), int(m[2]), int(m[3]), float('0.' + m[4])
    epoch = (dt.datetime(y, mo, d) - dt.datetime(2000, 1, 1, 12)).total_seconds() / 86400 + frac + 2451545.0
    for row in re.findall(r'<tr>(.*?)</tr>', p, re.S):
        c = re.findall(r'<td[^>]*>(.*?)</td>', row, re.S)
        if len(c) < 9: continue
        code = re.sub(r'<[^>]+>', '', c[1]).strip()
        moons.append({'name': html.unescape(re.sub(r'<[^>]+>', '', c[0])).strip().replace('_', ' '),
                      'code': int(code), 'planet': PLANET_OF[code[0]], 'a': num(c[2]), 'e': num(c[3]),
                      'w': num(c[4]), 'M': num(c[5]), 'i': num(c[6]), 'node': num(c[7]), 'P': num(c[8]),
                      'Pw': num(c[9]) if len(c) > 9 else None, 'Pn': num(c[10]) if len(c) > 10 else None,
                      'epoch': round(epoch, 2)})

# ---- named asteroids, dwarf planets, comets ----
NAMED = [('1', 'dwarf'), ('2', 'asteroid'), ('4', 'asteroid'), ('10', 'asteroid'), ('16', 'asteroid'),
         ('433', 'asteroid'), ('101955', 'asteroid'), ('162173', 'asteroid'), ('99942', 'asteroid'),
         ('3200', 'asteroid'), ('2024 YR4', 'asteroid'), ('134340', 'dwarf'), ('136199', 'dwarf'),
         ('136108', 'dwarf'), ('136472', 'dwarf'), ('90377', 'dwarf'), ('486958', 'kbo'),
         ('1P', 'comet'), ('2P', 'comet'), ('67P', 'comet'), ('12P', 'comet'), ('109P', 'comet'),
         ('C/2025 N1', 'interstellar')]
small = []
for sstr, kind in NAMED:
    j = json.loads(get('sbdb_' + re.sub(r'\W', '_', sstr) + '.json',
                       'https://ssd-api.jpl.nasa.gov/sbdb.api?' + urllib.parse.urlencode({'sstr': sstr, 'phys-par': 1})))
    if 'orbit' not in j: print('skip', sstr, j.get('message')); continue
    el = {e['name']: e['value'] for e in j['orbit']['elements']}
    phys = {p['name']: p['value'] for p in j.get('phys_par', [])}
    f = lambda k: float(el[k]) if el.get(k) not in (None, '') else None
    small.append({'name': j['object']['fullname'].strip(), 'short': j['object'].get('shortname') or j['object']['fullname'].strip(),
                  'kind': kind, 'e': f('e'), 'q': f('q'), 'a': f('a'), 'i': f('i'), 'node': f('om'), 'w': f('w'),
                  'tp': f('tp'), 'epoch': float(j['orbit']['epoch']), 'diam': phys.get('diameter'), 'H': phys.get('H')})

lo = (TODAY - dt.date(2000, 1, 1)).days + 2451544.5
q = {'fields': 'full_name,e,q,i,om,w,tp,epoch,M1', 'sb-kind': 'c',
     'sb-cdata': json.dumps({'AND': [f'tp|RG|{lo - 120:.1f}|{lo + 400:.1f}', 'q|LT|2.0']})}
cj = json.loads(get('comets.json', 'https://ssd-api.jpl.nasa.gov/sbdb_query.api?' + urllib.parse.urlencode(q)))
comets = []
for r in cj.get('data', []):
    name, e, qq, i, om, w, tp, ep, M1 = r
    if None in (e, qq, i, om, w, tp): continue
    comets.append({'name': name.strip(), 'kind': 'comet', 'e': float(e), 'q': float(qq), 'i': float(i), 'node': float(om),
                   'w': float(w), 'tp': float(tp), 'epoch': float(ep), 'M1': float(M1) if M1 else None})
comets.sort(key=lambda c: (c['M1'] if c['M1'] is not None else 99))

cad = json.loads(get('cad.json', 'https://ssd-api.jpl.nasa.gov/cad.api?' + urllib.parse.urlencode(
    {'date-min': TODAY.isoformat(), 'date-max': (TODAY + dt.timedelta(days=200)).isoformat(), 'dist-max': '0.02', 'sort': 'date'})))
flybys = [{'des': r[0], 'jd': float(r[2]), 'date': r[3], 'au': float(r[4]), 'kms': float(r[7]), 'H': float(r[10]) if r[10] else None}
          for r in cad.get('data', [])]

# ---- the belt: the brightest numbered asteroids (the watch would carry the whole MPC catalogue on SD) ----
bq = {'fields': 'a,e,i,om,w,ma,epoch,H', 'sb-kind': 'a', 'sb-cdata': json.dumps({'AND': ['H|LT|11.5']}), 'limit': 4000}
bj = json.loads(get('belt.json', 'https://ssd-api.jpl.nasa.gov/sbdb_query.api?' + urllib.parse.urlencode(bq)))
belt = []
for r in bj.get('data', []):
    try: a, e, i, om, w, ma, ep, H = map(float, r)
    except (TypeError, ValueError): continue
    belt.append([round(a, 3), round(e, 3), round(i, 1), round(om, 1), round(w, 1), round(ma, 1), ep, round(H, 1)])

# ---- spacecraft: heliocentric vectors (JPL Horizons), plus orbit elements for planet orbiters ----
# status is hand-kept: what public reports said when this pack was built (Oct 2026)
CRAFT = [(-74, 'MRO', 'mars', 'orbiting'), (-53, 'Mars Odyssey', 'mars', 'orbiting since 2001'),
         (-41, 'Mars Express', 'mars', 'orbiting'), (-143, 'ExoMars TGO', 'mars', 'orbiting'),
         (-62, 'Hope (EMM)', 'mars', 'orbiting'), (-61, 'Juno', 'jupiter', 'status unclear since Sep 2025'),
         (-121, 'BepiColombo', None, 'Mercury orbit insertion planned 21 Nov 2026'),
         (-28, 'JUICE', None, 'cruising to Jupiter (arrives 2031)'), (-159, 'Europa Clipper', None, 'cruising to Jupiter (arrives 2030)'),
         (-255, 'Psyche', None, 'cruising to asteroid Psyche (2029)'), (-49, 'Lucy', None, 'touring Trojan asteroids'),
         (-96, 'Parker Solar Probe', None, 'skimming the Sun'), (-170, 'JWST', None, 'at Sun-Earth L2'),
         (-98, 'New Horizons', None, 'Kuiper belt'), (-31, 'Voyager 1', None, 'interstellar space'),
         (-32, 'Voyager 2', None, 'interstellar space')]
CENTER = {'mars': '500@499', 'jupiter': '500@599'}
HZ = 'https://ssd.jpl.nasa.gov/api/horizons.api?'
day0, day1 = TODAY.isoformat(), (TODAY + dt.timedelta(days=1)).isoformat()


def horizons(name, **kw):
    q = {'format': 'json', 'MAKE_EPHEM': 'YES', 'START_TIME': f"'{day0}'", 'STOP_TIME': f"'{day1}'",
         'STEP_SIZE': "'1d'", 'REF_PLANE': 'ECLIPTIC', **kw}
    r = json.loads(get(name, HZ + urllib.parse.urlencode(q))).get('result', '')
    m = re.search(r'\$\$SOE\s*(.*?)\$\$EOE', r, re.S)
    if not m: return None
    first = re.split(r'\n(?=\d{7}\.\d+ = A\.D\.)', m[1].strip())[0]   # the first record only
    out = {k: float(v) for k, v in re.findall(r'([A-Z]{1,2})\s*=\s*([-+.\dE]+)', first)}
    out['JD'] = float(first.split()[0])
    return out


craft = []
for cid, name, planet, status in CRAFT:
    v = horizons(f'hz_{cid}.json', COMMAND=f"'{cid}'", EPHEM_TYPE='VECTORS', CENTER="'500@10'", VEC_TABLE="'1'", OUT_UNITS="'AU-D'")
    c = {'id': cid, 'name': name, 'planet': planet, 'status': status}
    if v: c['xyz'] = [v['X'], v['Y'], v['Z']]
    if planet:
        e = horizons(f'hz_el_{cid}.json', COMMAND=f"'{cid}'", EPHEM_TYPE='ELEMENTS', CENTER=f"'{CENTER[planet]}'", OUT_UNITS="'KM-S'")
        if e: c['orbit'] = {'a': e['A'], 'e': e['EC'], 'i': e['IN'], 'node': e['OM'], 'w': e['W'], 'M': e['MA'],
                            'P': e['PR'] / 86400, 'epoch': e['JD'] if 'JD' in e else None}
    craft.append(c)

# fast moons drift off their mean elements over decades: refresh the major ones with today's osculating elements
MAJOR = {401: '499', 402: '499', 501: '599', 502: '599', 503: '599', 504: '599', 601: '699', 602: '699', 603: '699',
         604: '699', 605: '699', 606: '699', 608: '699', 701: '799', 702: '799', 703: '799', 704: '799', 705: '799',
         801: '899', 901: '999'}
for m in moons:
    if m['code'] not in MAJOR: continue
    e = horizons(f'hz_moon_{m["code"]}.json', COMMAND=f"'{m['code']}'", EPHEM_TYPE='ELEMENTS',
                 CENTER=f"'500@{MAJOR[m['code']]}'", OUT_UNITS="'KM-S'")
    # keep the mean period: osculating periods include oblateness wobble and drift days later
    if e: m.update({'a': e['A'], 'e': e['EC'], 'i': e['IN'], 'node': e['OM'], 'w': e['W'], 'M': e['MA'],
                    'epoch': e['JD'], 'fresh': 1})

data = {'built': TODAY.isoformat(), 'jd': (TODAY - dt.date(2000, 1, 1)).days + 2451544.5,
        'moons': moons, 'small': small, 'comets': comets, 'flybys': flybys, 'craft': craft, 'belt': belt}
open(A.out, 'w', encoding='utf-8').write(
    '/* Sky atlas snapshot built by tools/skypack.py on ' + TODAY.isoformat() + ' from NASA/JPL SSD public data:\n'
    ' * planetary satellite mean elements, SBDB orbits, SBDB comet query, CAD close approaches. */\n'
    'const ATLAS = ' + json.dumps(data, separators=(',', ':')) + ';\n')
by = {}
for m in moons: by[m['planet']] = by.get(m['planet'], 0) + 1
print('belt', len(belt), 'moons', len(moons), by, 'small', len(small), 'comets', len(comets), 'flybys', len(flybys),
      'bytes', os.path.getsize(A.out))
