"""Package simulator renders as a self-contained, interactive review artifact.

Run build/sim.exe --pip-gallery and --pip-care-gallery in an output directory,
then: python tools/pip_gallery.py build/pip-care-preview docs/research/assets
Requires Pillow. These are actual firmware renderer images, not mockups.
"""
import base64
import io
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

ACTIONS = ['idle', 'curious', 'happy', 'petted', 'boop', 'startled', 'dizzy',
           'rocked', 'sleepy', 'asleep', 'wake', 'play', 'bored', 'stretch',
           'wink', 'peek', 'grumpy', 'lonely', 'excited', 'relieved', 'notify']


def main():
    source, target = map(Path, sys.argv[1:3])
    target.mkdir(parents=True, exist_ok=True)
    animations = {}
    for index, action in enumerate(ACTIONS):
        frames = []
        for frame in range(40):
            path = source / 'pip_frames' / f'frame_{index * 40 + frame:03}.bmp'
            with Image.open(path) as im:
                output = io.BytesIO()
                im.convert('RGB').save(output, format='PNG', optimize=True)
            frames.append('data:image/png;base64,' + base64.b64encode(output.getvalue()).decode())
        animations[action] = frames
    html = '''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pip — expression review</title>
<style>body{background:#101719;color:#d7eeeb;font:16px system-ui;margin:0;padding:32px}
main{max-width:1000px;margin:auto}h1{font-size:34px;margin:0 0 10px}p{line-height:1.6;color:#b0c7c5}
.layout{display:flex;gap:30px;align-items:start;flex-wrap:wrap}img{width:min(466px,85vw);border-radius:50%}
button,select,input{font:inherit}button,select{background:#203436;color:#eafffc;border:1px solid #52726d;padding:9px 14px;border-radius:8px}
nav{display:flex;flex-wrap:wrap;gap:7px;max-width:420px}button[aria-pressed=true]{background:#65d3c3;color:#122823}
.controls{margin:20px 0;display:flex;gap:15px;align-items:center}#label{font-size:24px;text-transform:capitalize}
a{color:#74d9cd}</style><main><h1>Pip's expressive vocabulary</h1>
<p>Actual simulator output from the watch renderer. Each clip covers four seconds, including later gestures and recovery.
These forced performances demonstrate the eye rig; they do not set the live pet's needs or prove physical sensor behavior.</p>
<div class="layout"><div><img id="screen" alt="Pip's animated eyes"><div class="controls">
<button id="pause">Pause</button><label>Speed <select id="speed"><option value="1">1×</option><option value="0.5">½×</option><option value="0.25">¼×</option></select></label>
</div><input id="scrub" aria-label="Animation frame" type="range" min="0" max="39" value="0" style="width:95%"></div>
<div><h2 id="label">Idle</h2><nav id="actions" aria-label="Expressions"></nav><p>Look for attention before movement, asymmetric lids,
complete blinks, inward slanted grumpy lids, and a readable settling phase.</p>
<p><a href="../PET_IMPLEMENTATION_PLAN.md">Research and revised plan</a> · <a href="pip-contact-sheet.png">Contact sheet</a> · <a href="pip-care-screens.png">Care screens</a></p></div></div>
<script>const clips=CLIP_DATA;
let action='idle',frame=0,playing=true,last=0;
const screen=document.getElementById('screen'),scrub=document.getElementById('scrub');
function show(){screen.src=clips[action][frame];scrub.value=frame;}
for(const name of Object.keys(clips)){const b=document.createElement('button');b.textContent=name;
b.setAttribute('aria-pressed',String(name===action));b.onclick=()=>{action=name;frame=0;last=0;document.getElementById('label').textContent=name;
document.querySelectorAll('nav button').forEach(x=>x.setAttribute('aria-pressed',String(x===b)));show();};document.getElementById('actions').append(b);}
document.getElementById('pause').onclick=e=>{playing=!playing;e.target.textContent=playing?'Pause':'Play';};
scrub.oninput=()=>{frame=Number(scrub.value);show();};
function tick(t){if(playing&&t-last>=100/Number(document.getElementById('speed').value)){frame=(frame+1)%40;last=t;show();}requestAnimationFrame(tick);}show();requestAnimationFrame(tick);
</script></main></html>'''
    (target / 'pip-preview.html').write_text(html.replace('CLIP_DATA', json.dumps(animations)), encoding='utf-8')
    sheet = Image.new('RGB', (7 * 190, 3 * 222), '#101719')
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 18)
    for index, action in enumerate(ACTIONS):
        x, y = (index % 7) * 190, (index // 7) * 222
        with Image.open(source / f'pip_{action}.bmp') as im:
            sheet.paste(im.resize((180, 180), Image.Resampling.LANCZOS), (x + 5, y))
        draw.text((x + 95, y + 189), action.capitalize(), font=font, fill='#b8e4dc', anchor='mt')
    sheet.save(target / 'pip-contact-sheet.png')
    care = Image.new('RGB', (3 * 350, 350), '#101719')
    for page in range(3):
        with Image.open(source / f'care_{page}.bmp') as im:
            care.paste(im.resize((350, 350), Image.Resampling.LANCZOS), (page * 350, 0))
    care.save(target / 'pip-care-screens.png')
    print(f'Created {target / "pip-preview.html"}: {len(ACTIONS)} clips, 840 frames')


if __name__ == '__main__':
    main()
