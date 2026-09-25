#!/usr/bin/env python3
"""Functional test of UWM TSS/8.25 on the emulator core (host build).

SIMH cannot run this monitor as-is (its RF08 comes out of reset with the
completion flag set, which UWM's start-up treats as a disc interrupt), so
instead of a transcript comparison this checks the expected behaviour:
start-up, ^B LOGIN on the console and four multiplexer lines, the message of
the day, SYSTAT with all five jobs, CAT, BASIC on four lines at once, FOCAL,
and LOGOUT."""
import os, re, sys, time, shutil, socket, pexpect

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(ROOT, 'sdcard')
PORT = 4611
shutil.copy(os.path.join(SD, 'uwm_rf.dsk'), '/tmp/uwmtest_rf.dsk')
p = pexpect.spawn('%s -tss8 /tmp/uwmtest_rf.dsk %s %d' % (os.path.join(ROOT, 'build', 'pdp8host'),
                  os.path.join(SD, 'uwm_init.bin'), PORT), encoding='latin1', timeout=20)
failures = []

def check(cond, what):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    if not cond: failures.append(what)

def typ(send, s):
    for ch in s:
        send(ch); time.sleep(0.02)

def drain(read, secs):
    out, end = '', time.time() + secs
    while time.time() < end:
        try: out += read()
        except (pexpect.TIMEOUT, socket.timeout, BlockingIOError): pass
    return out

con = lambda: p.read_nonblocking(8192, timeout=0.2)
p.expect(r'LOAD, DUMP, START\?\? ')
for s in ('START\r', '9-25-84\r', '09:35\r'):
    typ(p.send, s); time.sleep(0.6)
typ(p.send, '\x02LOGIN 2 LXHE\r')
out = drain(con, 2)
check("UWM'S TSS-8/E.25" in out and 'ON K00' in out, 'console login banner')
check('WELCOME TO UWM TSS/8.25' in out, 'LOGIN message of the day')

lines = [socket.create_connection(('127.0.0.1', PORT)) for _ in range(4)]
for s in lines: s.settimeout(0.1)
prog = '10 FOR I=1 TO 2000\r20 LET X=SIN(I)\r30 NEXT I\r40 PRINT "DONE";X\r50 END\r'
outs = [''] * 4
for i, s in enumerate(lines):
    for cmd in ('\r', '\x02LOGIN 2 LXHE\r', 'R BASIC\r', 'NEW\r', 'P%d\r' % i, prog):
        typ(lambda c: s.send(c.encode()), cmd)
        outs[i] += drain(lambda: s.recv(4096).decode('latin1'), 0.4)
for i in range(4):
    check('ON K%02d' % (i + 1) in outs[i], 'login on multiplexer line K%02d' % (i + 1))
for s in lines: typ(lambda c: s.send(c.encode()), 'RUN\r')
end = time.time() + 60
while time.time() < end and not all(re.search(r'DONE\s*-?\.[0-9]', o) for o in outs):
    for i, s in enumerate(lines):
        outs[i] += drain(lambda: s.recv(4096).decode('latin1'), 0.05)
for i in range(4):
    check(re.search(r'DONE\s*\.930\d', outs[i]) is not None, 'BASIC program ran on K%02d' % (i + 1))

typ(p.send, 'SYSTAT\r')
out = drain(con, 2)
check(all(re.search(r'\n\s*%d\s+0, 2\s+K%02d' % (j + 1, j), out.replace('\r', '')) for j in range(5)),
      'SYSTAT lists five jobs on K00-K04')
typ(p.send, 'R CAT\r')
out = drain(con, 3)
check('DISK FILES FOR USER  0, 2' in out and 'BASIC .SAV' in out, 'CAT lists the library')
typ(p.send, 'R FOCAL\r'); time.sleep(1)
typ(p.send, 'YES\r'); time.sleep(0.5)
typ(p.send, 'TYPE 22/7,!\r')
out = drain(con, 2)
check('3.1429' in out, 'FOCAL arithmetic')
typ(p.send, '\x02S\r'); time.sleep(0.5)
typ(p.send, 'LOGOUT\r')
out = drain(con, 3)
check('LOGGED OFF K00' in out, 'LOGOUT')
p.close(force=True)
print('RESULT:', 'PASS' if not failures else 'FAIL (%d)' % len(failures))
sys.exit(1 if failures else 0)
