#!/usr/bin/env python3
"""Measure bitmap growth and verify saved bytes, with all builds outside the repository."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, struct, subprocess
from measure_common import BASELINE_SHA, checked_baseline, fixture, build, guest

class Image:
    def __init__(self, path):
        self.image = path.read_bytes()
        self.size, _, _, _, _, self.inodes, self.bitmap = struct.unpack_from('<7I', self.image, 512)
    def allocated(self):
        bits = self.image[self.bitmap*512:]
        return sum(bool(bits[i//8] & (1 << (i%8))) for i in range(self.size))
    def inode(self, number):
        start = self.inodes*512 + number*64
        fields = struct.unpack_from('<4h14I', self.image, start)
        return fields[0], fields[4], fields[5:]
    def data(self, number):
        _, size, addresses = self.inode(number)
        blocks = list(addresses[:12])
        if addresses[12]: blocks += list(struct.unpack_from('<128I', self.image, addresses[12]*512))
        return b''.join(self.image[b*512:(b+1)*512] for b in blocks[:(size+511)//512])[:size]
    def entries(self, number=1):
        data = self.data(number); entries = {}
        for offset in range(0, len(data), 16):
            ino, name = struct.unpack_from('<H14s', data, offset)
            name = name.split(b'\0')[0].decode()
            if ino and name not in ('.','..'): entries[name] = ino
        return entries
    def lookup(self, path):
        ino = 1
        for name in path.split('/'): ino = self.entries(ino)[name]
        return ino

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, help='Clean pinned MIT xv6-public checkout')
    parser.add_argument('--output', required=True, type=Path, help='New directory for images, logs and results')
    args = parser.parse_args(); baseline = checked_baseline(args.baseline)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parents[1]; results = {}
    for label, original in [('stock_full_copy',baseline),('snapshot',source)]:
        tree = fixture(original, output/label, 'storage')
        code = (source/'benchmarks/storage.c').read_text()
        if label == 'stock_full_copy':
            # Match the extension's filesystem/log capacity; retain stock filesystem algorithms.
            params = (tree/'param.h').read_text()
            params = re.sub(r'(#define FSSIZE\s+)1000', r'\g<1>2000', params)
            params = re.sub(r'(#define MAXOPBLOCKS\s+)10', r'\g<1>20', params)
            (tree/'param.h').write_text(params)
        (tree/'storage.c').write_text(code)
        build(tree, output/(label+'-build.log'))
        if label == 'snapshot':
            # Identical guest programs and README in both images; storage.c reserves room
            # for the extension's extra stat field. init and sh do not use struct stat.
            for name in ['README','_init','_sh','_storage']:
                shutil.copyfile(output/'stock_full_copy'/name, tree/name)
            (tree/'fs.img').unlink()
            with (output/'snapshot-image.log').open('wb') as log:
                subprocess.run(['make','CC=gcc-9','fs.img'], cwd=tree,
                    stdout=log, stderr=subprocess.STDOUT, check=True)
        guest(tree, ['storage prepare'], output/(label+'-prepare.log'))
        before = Image(tree/'fs.img'); live = {name:before.data(ino)
            for name,ino in before.entries().items() if before.inode(ino)[0] == 2}
        counts = {'before_save':before.allocated()}
        guest(tree, ['storage '+('copy' if label == 'stock_full_copy' else 'save')], output/(label+'-save.log'))
        saved = Image(tree/'fs.img'); counts['after_save'] = saved.allocated()
        saved_path = 'saved' if label == 'stock_full_copy' else 'snapshot/01'
        saved_ino = saved.lookup(saved_path)
        entries = saved.entries(saved_ino)
        assert set(entries) == set(live), (entries.keys(), live.keys())
        assert all(saved.data(entries[name]) == data for name,data in live.items())
        guest(tree, ['storage edit'], output/(label+'-edit.log'))
        edited = Image(tree/'fs.img'); counts['after_edit'] = edited.allocated()
        assert all(edited.data(entries[name]) == data for name,data in live.items())
        assert edited.data(edited.lookup('payload')) == live['payload']+b'X'
        counts.update(save_growth_blocks=counts['after_save']-counts['before_save'],
            save_plus_edit_growth_blocks=counts['after_edit']-counts['before_save'],
            saved_files={name:len(data) for name,data in live.items()},
            saved_sha256={name:hashlib.sha256(data).hexdigest() for name,data in live.items()},
            payload_sha256=hashlib.sha256(live['payload']).hexdigest(),
            preserved_bytes_verified=True)
        results[label] = counts
        print(label, json.dumps(counts), flush=True)
    assert results['stock_full_copy']['saved_sha256'] == results['snapshot']['saved_sha256']
    stock = results['stock_full_copy']['save_plus_edit_growth_blocks']
    cow = results['snapshot']['save_plus_edit_growth_blocks']
    results.update(baseline_commit=BASELINE_SHA, block_bytes=512, payload_bytes=65536,
        metric='incremental allocated blocks for one saved namespace plus a one-byte live append',
        reduction_percent=100*(stock-cow)/stock,
        snapshot_initial_metadata_overhead_blocks=results['snapshot']['before_save']-results['stock_full_copy']['before_save'],
        identical_guest_files_verified=True,
        gcc=subprocess.check_output(['gcc-9','-dumpfullversion'], text=True).strip(),
        qemu=subprocess.check_output(['qemu-system-i386','--version'], text=True).splitlines()[0])
    (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
if __name__ == '__main__': main()
