#!/usr/bin/env python3
"""Run the same TSS/8 console session on SIMH and on our core and compare transcripts."""
import os, sys, re, time, shutil, difflib, pexpect
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMH = os.environ.get('SIMH_PDP8', 'pdp8')
SD = os.path.join(ROOT, 'sdcard')
RF, BIN = os.path.join(SD, 'tss8_rf.dsk'), os.path.join(SD, 'tss8_init.bin')
STEPS = ['START\r', '9-25-78\r', '09:35\r', 'LOGIN 2 LXHE\r', 'R CAT\r', 'R BASIC\r', 'NEW\r', 'PRIMES\r',
         '10 FOR N=2 TO 60\r20 FOR D=2 TO SQR(N)\r30 IF N/D=INT(N/D) THEN 60\r40 NEXT D\r50 PRINT N;\r60 NEXT N\r70 END\r',
         'RUN\r', 'BYE\r', 'R FOCAL\r', 'YES\r', 'TYPE 22/7,!\r', 'FOR I=1,5;TYPE %6.04,FSQT(I),!\r',
         '\x02S\r', 'SYSTAT\r', 'LOGOUT\r']

def session(kind):
    shutil.copy(RF, '/tmp/tss_rf.dsk')
    if kind == 'simh':
        p = pexpect.spawn(SIMH, encoding='latin1', timeout=30)
        for c in ['set cpu 32k', 'set df disabled', 'set rf enabled', 'set tti ksr',
                  'load ' + BIN, 'attach rf /tmp/tss_rf.dsk', 'run 24200']:
            p.expect('sim>'); p.sendline(c)
    else:
        p = pexpect.spawn(os.path.join(ROOT, 'build', 'pdp8host') + ' -tss8 /tmp/tss_rf.dsk ' + BIN + ' 4199',
                          encoding='latin1', timeout=30)
    p.expect(r'ETC\? ')
    out = []
    for s in STEPS:
        for ch in s:
            p.send(ch); time.sleep(0.02)
        end = time.time() + 4
        while time.time() < end:
            try:
                out.append(p.read_nonblocking(4096, timeout=0.2))
            except pexpect.TIMEOUT:
                pass
    p.close(force=True)
    return ''.join(out)

def norm(t):
    t = ''.join(c for c in t if c == '\n' or 32 <= ord(c) < 127).upper()
    t = re.sub(r'\d\d:\d\d:\d\d', 'HH:MM:SS', t)                  # clock readings
    t = re.sub(r'RUNTIME.*', '', t); t = re.sub(r'\d\d:\d\d:\d\d', '', t)
    t = re.sub(r'(CONNECT TIME|UPTIME|RUN TIME|CPU TIME).*', r'\1', t)
    t = re.sub(r'\^Q|\^BS', '', t)
    return [l.rstrip() for l in t.split('\n') if l.strip()]

a, b = session('simh'), session('ours')
d = list(difflib.unified_diff(norm(a), norm(b), 'simh', 'ours', lineterm=''))
print(b.replace('\r', ''))
print('\n'.join(d) if d else '', '\nRESULT:', 'IDENTICAL' if not d else 'DIFFERENT')
