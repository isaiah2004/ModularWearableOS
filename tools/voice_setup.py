"""Provision watch voice chat from the ignored .env, without logging credentials.

Run with ESP-IDF Python: python tools/voice_setup.py --port COM4
Optional: --test-audio (3s local microphone/speaker loopback), --ask TEXT.
The key is saved in checked device NVS, never in a firmware image or source ZIP.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
if __package__:
    from .host_support import ROOT
    from .host_transport import HostTransport, SerialLines, validate_command, DeviceError
else:
    from host_support import ROOT
    from host_transport import HostTransport, SerialLines, validate_command, DeviceError

MODELS = ('openai/gpt-4o-mini', 'openai/gpt-5.4-mini', 'openai/gpt-5.4')
DEFAULTS = {'OPENROUTER_STT_MODEL': 'openai/whisper-large-v3-turbo',
            'OPENROUTER_TTS_MODEL': 'hexgrad/kokoro-82m', 'OPENROUTER_TTS_VOICE': 'af_heart',
            'OPENROUTER_DEFAULT_TIER': 'low'}


def read_env(path):
    """Parse literals only: no shell execution, expansion, or dependency on dotenv."""
    values = {}
    for number, line in enumerate(Path(path).read_text(encoding='utf-8-sig').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'): continue
        if line.startswith('export '): line = line[7:].lstrip()
        name, separator, value = line.partition('=')
        name, value = name.strip(), value.strip()
        if not separator or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
            raise ValueError(f'Invalid .env syntax on line {number}; values were not displayed')
        if value.startswith(('"', "'")):
            quote = value[0]
            if len(value) < 2 or not value.endswith(quote):
                raise ValueError(f'Unclosed .env quote on line {number}')
            value = value[1:-1]
        else: value = value.split(' #', 1)[0].rstrip()
        values[name] = value
    return values


def commands(values):
    key = values.get('OPENROUTER_API_KEY', '')
    if not re.fullmatch(r'[A-Za-z0-9_-]{16,127}', key):
        raise ValueError('Fill OPENROUTER_API_KEY in .env with a valid OpenRouter key')
    # These model profiles carry firmware price ceilings. Never silently apply
    # unpriced overrides or route Low to a more expensive model.
    for name, expected in zip(('OPENROUTER_CHAT_MODEL', 'OPENROUTER_CHAT_MODEL_MEDIUM', 'OPENROUTER_CHAT_MODEL_HIGH'), MODELS):
        if values.get(name, expected) != expected:
            raise ValueError(f'{name} must match its firmware budget profile: {expected}')
    result = ['chat key ' + key]
    for field, name in (('stt', 'OPENROUTER_STT_MODEL'), ('tts', 'OPENROUTER_TTS_MODEL'), ('voice', 'OPENROUTER_TTS_VOICE')):
        value = values.get(name) or DEFAULTS[name]
        if not re.fullmatch(r'[A-Za-z0-9_/.:+-]{1,63}', value): raise ValueError(f'Invalid {name}')
        if field in ('stt','tts') and value != DEFAULTS[name]:
            raise ValueError(f'{name} must use the priced firmware profile: {DEFAULTS[name]}')
        result.append('chat ' + field + ' ' + value)
    tier = values.get('OPENROUTER_DEFAULT_TIER', 'low').lower()
    if tier not in ('low','medium','high'): raise ValueError('OPENROUTER_DEFAULT_TIER must be low, medium, or high')
    result.append('chat tier ' + str(('low','medium','high').index(tier)))
    if values.get('WIFI_SSID'):
        ssid, password = values['WIFI_SSID'], values.get('WIFI_PASSWORD','')
        if not 1 <= len(ssid.encode()) <= 32 or (password and not 8 <= len(password.encode()) <= 63):
            raise ValueError('Wi-Fi name or password has an invalid length')
        result.append('chat wifi ' + json.dumps({'ssid': ssid, 'password': password}, ensure_ascii=False, separators=(',',':')))
    # Include the correlated request envelope in the 191-byte USB line budget.
    for command in result: validate_command('req 00000000 ' + command)
    return result


def wait_turn(transport, timeout=100):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        lines = transport.execute('chat status', timeout=10)
        line = next((line for line in lines if line.startswith('NOSCHAT ')), '')
        if 'busy=0' in line:
            for value in lines:
                if value.startswith(('NOSCHAT ', 'NOSCHATSTATUS ')): print(value)
            if 'state=7' in line: raise RuntimeError('Voice operation failed; see device status above')
            return
        time.sleep(.35)
    transport.execute('chat cancel', timeout=10)
    raise TimeoutError('Voice turn timed out and was cancelled')


def require_voice_device(transport):
    board=transport.execute('board',timeout=10)
    if not any('ESP32-S3-Touch-AMOLED-1.43C' in line for line in board):
        raise ValueError('Voice provisioning requires the C board; no key was sent')
    status=transport.execute('chat status',timeout=10)
    if not any(line.startswith('NOSCHAT ') and 'available=1' in line.split() for line in status):
        raise ValueError('Install the voice-capable NOS firmware first; no key was sent')


def wifi_connected(lines):
    return any(line.startswith('NETWORK wifi=Connected / ') for line in lines)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--env',type=Path,default=ROOT/'.env')
    parser.add_argument('--test-audio',action='store_true')
    parser.add_argument('--ask',help='Send one short text prompt to test model and speaker')
    parser.add_argument('--validate',action='store_true',help='Validate .env locally without opening serial')
    args=parser.parse_args(argv)
    try:
        values=read_env(args.env);pending=commands(values)
        if args.validate:
            print('Voice environment valid; credentials were not displayed.');return 0
        link=SerialLines.open(args.port)
        try:
            transport=HostTransport(link);transport.negotiate()
            require_voice_device(transport)
            for command in pending: transport.execute(command,timeout=15)
            print('OpenRouter key and speech configuration saved and read back on the watch.')
            print('Tiers: Low 1c/min, Medium 5c/min, High 10c/min (usage targets).')
            if values.get('WIFI_SSID'):
                until=time.monotonic()+35
                while time.monotonic()<until:
                    lines=transport.execute('network status',timeout=8)
                    if wifi_connected(lines):break
                    time.sleep(.5)
                print('Wi-Fi credentials sent; watch Settings shows connection status.')
            elif wifi_connected(transport.execute('network status',timeout=8)):
                print("Using the watch's saved Wi-Fi profile.")
            else: print('Wi-Fi not supplied; fill WIFI_SSID/WIFI_PASSWORD in .env and rerun, or use watch Settings.')
            if args.test_audio:
                transport.execute('chat test',timeout=10);wait_turn(transport,20)
            if args.ask:
                validate_command('req 00000000 chat ask '+args.ask)
                transport.execute('chat ask '+args.ask,timeout=10);wait_turn(transport)
        finally:link.close()
        return 0
    except (OSError,ValueError,RuntimeError,TimeoutError) as error:
        # Never include a command argument, key, password, or raw API body.
        if isinstance(error,DeviceError): print('voice_setup: device rejected configuration; no credentials displayed',file=sys.stderr)
        else: print('voice_setup: '+str(error),file=sys.stderr)
        return 1

if __name__=='__main__':raise SystemExit(main())
