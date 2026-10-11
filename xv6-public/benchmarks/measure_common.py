"""Common isolated build and QEMU utilities for a controlled xv6 comparison."""
from pathlib import Path
import os, re, select, shutil, subprocess, time
BASELINE_SHA = 'eeb7b415dbcb12cc362d0783e41c3d1f44066b17'

def checked_baseline(path):
    path = Path(path).resolve()
    sha = subprocess.check_output(['git','rev-parse','HEAD'], cwd=path, text=True).strip()
    dirty = subprocess.check_output(['git','status','--porcelain'], cwd=path, text=True).strip()
    if sha != BASELINE_SHA or dirty:
        raise ValueError('Use a clean MIT xv6-public checkout at '+BASELINE_SHA)
    return path

def fixture(source, destination, program):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
        '.git','benchmarks','*.o','*.d','*.img','*.asm','*.sym','_*','kernel','mkfs','bootblock'))
    makefile = destination/'Makefile'
    text = makefile.read_text()
    text = re.sub(r'UPROGS=\\.*?(?=\nfs.img:)', 'UPROGS=\\\n\t_init\\\n\t_sh\\\n\t_'+program+'\\\n', text, flags=re.S)
    makefile.write_text(text)
    return destination

def build(tree, output):
    with output.open('wb') as log:
        subprocess.run(['make','-j4','CC=gcc-9','xv6.img','fs.img'], cwd=tree,
            stdout=log, stderr=subprocess.STDOUT, check=True)

def guest(tree, commands, output):
    proc = subprocess.Popen(['qemu-system-i386','-nographic','-monitor','none',
        '-drive','file=fs.img,index=1,media=disk,format=raw',
        '-drive','file=xv6.img,index=0,media=disk,format=raw','-smp','1','-m','512'],
        cwd=tree, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = bytearray()
    def prompt(timeout):
        start = len(log); deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if select.select([proc.stdout], [], [], .2)[0]:
                data = os.read(proc.stdout.fileno(), 65536)
                if not data: raise RuntimeError('QEMU exited before prompt')
                log.extend(data)
                if bytes(log[start:]).endswith(b'$ '): return
        raise TimeoutError('Guest did not finish within '+str(timeout)+' seconds')
    try:
        prompt(30)
        for command in commands:
            proc.stdin.write(command.encode()+b'\n'); proc.stdin.flush(); prompt(60)
    finally:
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        output.write_bytes(log)
    text = log.decode(errors='replace')
    if 'FAIL' in text or 'panic:' in text: raise RuntimeError(text)
    return text
