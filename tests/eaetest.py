#!/usr/bin/env python3
"""Assemble and run tests/eaetest.pa under OS/8 on SIMH and on our core; compare checksums."""
import os, sys, shutil, pexpect
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMH = os.environ.get('SIMH_PDP8', 'pdp8')
src = open(os.path.join(ROOT, 'tests', 'eaetest.pa')).read()
def run(kind):
    shutil.copy(os.path.join(ROOT, 'sdcard', 'os8.rk05'), '/tmp/e.rk05')
    if kind == 'simh':
        p = pexpect.spawn(SIMH, encoding='latin1', timeout=60)
        for c in ['set cpu 32k', 'set df disabled', 'set tti ksr', 'att rk0 /tmp/e.rk05']:
            p.expect('sim>'); p.sendline(c)
        p.expect('sim>'); p.sendline('boot rk0')
    else:
        p = pexpect.spawn(os.path.join(ROOT, 'build', 'pdp8host') + ' /tmp/e.rk05', encoding='latin1', timeout=60)
    p.expect(r'\n\.')
    p.send('R PIP\r'); p.expect(r'\*'); p.send('EAE.PA<TTY:\r')
    for l in src.splitlines(): p.send(l + '\r')
    p.send('\x1a'); p.expect(r'\*'); p.send('\x03'); p.expect(r'\n\.')
    p.send('PAL EAE\r'); p.expect(r'\n\.'); out = p.before
    p.send('LOAD EAE\r'); p.expect(r'\n\.')
    p.send('START 200\r'); p.expect(r'\n\.'); out += p.before
    return out
a = run('simh'); b = run('ours')
print(b)
print('MATCH' if a.upper().replace('\r', '') == b.upper().replace('\r', '') else 'DIFF\n' + a)
