#!/usr/bin/env python3
"""Differential test: run the same OS/8 session on SIMH and on our core, compare transcripts."""
import pexpect, shutil, sys, re, difflib, time

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMH = os.environ.get('SIMH_PDP8', 'pdp8')           # SIMH PDP-8 simulator binary
OURS = os.path.join(ROOT, 'build', 'pdp8host')
IMG  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'sdcard', 'os8.rk05')

FT = """      DIMENSION A(10)
      DO 10 I=1,10
      A(I)=SQRT(FLOAT(I))*ATAN(1.0)
   10 CONTINUE
      WRITE(4,20) (A(I),I=1,10)
   20 FORMAT(1X,5F12.6)
      X=EXP(2.5)
      Y=ALOG(X)
      Z=SIN(0.5)**2+COS(0.5)**2
      WRITE(4,30) X,Y,Z
   30 FORMAT(1X,3E16.8)
      K=0
      DO 40 I=1,1000
   40 K=K+MOD(I*7,13)
      WRITE(4,50) K
   50 FORMAT(1X,'K=',I8)
      STOP
      END
"""
PA = """/ PAL8 TEST: PRINT 'HELLO' THEN PRIMES < 100 USING EAE-FREE CODE
*200
START,  CLA CLL
        TAD (-5
        DCA CNT
        TAD (MSG-1
        DCA 10
LOOP,   TAD I 10
        JMS TYPE
        ISZ CNT
        JMP LOOP
        TAD (215
        JMS TYPE
        TAD (212
        JMS TYPE
        JMP I (7600
TYPE,   0
        TLS
        TSF
        JMP .-1
        CLA
        JMP I TYPE
CNT,    0
MSG,    310;305;314;314;317
$
"""
BA = """10 PRINT "TRIG";
20 FOR X=0 TO 3 STEP .5
30 PRINT SIN(X);COS(X);ATN(X);EXP(X);LOG(X+1)
40 NEXT X
50 LET S=0
60 FOR I=1 TO 500
70 LET S=S+INT(I/7)*3-I/11
80 NEXT I
90 PRINT "S=";S
100 DIM A(20)
110 FOR I=1 TO 20
120 LET A(I)=I*I-3*I+2
130 NEXT I
140 FOR I=1 TO 20 STEP 4
150 PRINT A(I);
160 NEXT I
170 PRINT
180 END
"""

def pipin(p, name, text):
    p.send('R PIP\r'); p.expect(r'\*')
    p.send(name + '<TTY:\r')
    for line in text.splitlines():
        p.send(line + '\r')
    p.send('\x1a'); p.expect(r'\*')
    p.send('\x03'); p.expect(r'\n\.')

def session(cmd):
    shutil.copy(IMG, '/tmp/diff.rk05')
    if cmd == 'simh':
        p = pexpect.spawn(SIMH, encoding='latin1', timeout=60)
        for c in ['set cpu 32k', 'set df disabled', 'set tti ksr', 'att rk0 /tmp/diff.rk05']:
            p.expect('sim>'); p.sendline(c)
        p.expect('sim>'); p.sendline('boot rk0')
    else:
        p = pexpect.spawn(OURS + ' /tmp/diff.rk05', encoding='latin1', timeout=60)
    log = []
    p.expect(r'\n\.')
    def step(send, expect=r'\n\.', slow=False):
        if slow:          # U/W FOCAL drops type-ahead on SIMH, so type like a human
            for ch in send: p.send(ch); time.sleep(0.03)
        else:
            p.send(send)
        p.expect(expect); log.append(p.before + str(p.after))
    step('DIR SYS:\r')
    pipin(p, 'TEST.FT', FT)
    step('EXECUTE TEST.FT\r')
    pipin(p, 'HELLO.PA', PA)
    step('PAL HELLO/L\r')
    step('LOAD HELLO\r')
    step('START 200\r')
    step('R BASIC\r', 'NEW OR OLD--')
    step('NEW\r', 'FILE NAME--')
    step('BTEST\r', 'READY')
    for l in BA.splitlines(): p.send(l + '\r')
    step('RUN\r', 'READY')
    step('SAVE\r', 'READY')
    step('\x03')
    step('R UWF16K\r', r'\*')
    step('T 22/7,!\r', r'\*', slow=True)
    step('F I=1,10;T %8.04,FSQT(I),!\r', r'\*', slow=True)
    step('\x03')
    step('DIR\r')
    p.close(force=True)
    return log

def norm(s):
    s = s.replace('\r', '').upper()
    return [l.rstrip() for l in s.split('\n')]

a = session('simh'); b = session('ours')
ok = True
for i, (x, y) in enumerate(zip(a, b)):
    if norm(x) != norm(y):
        ok = False
        print('=== MISMATCH in step', i)
        print('\n'.join(difflib.unified_diff(norm(x), norm(y), 'simh', 'ours', lineterm='')))
for x in b: print(x.replace('\r',''))
print('RESULT:', 'IDENTICAL' if ok else 'DIFFERENT')
