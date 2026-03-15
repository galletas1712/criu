#!/usr/bin/env python3
import argparse
import sys
import json
import os

import pycriu
from . import __version__


def inf(opts):
    if opts['in']:
        return open(opts['in'], 'rb')
    else:
        if sys.stdin.isatty():
            # If we are reading from a terminal (not a pipe) we want text input and not binary
            return sys.stdin
        return sys.stdin.buffer


def outf(opts, decode):
    # Decode means from protobuf to JSON.
    # Use text when writing to JSON else use binaray mode
    if opts['out']:
        mode = 'wb+'
        if decode:
            mode = 'w+'
        return open(opts['out'], mode)
    else:
        if decode:
            return sys.stdout
        return sys.stdout.buffer


def dinf(opts, name):
    return open(os.path.join(opts['dir'], name), mode='rb')


def decode(opts):
    indent = None

    try:
        img = pycriu.images.load(inf(opts), opts['pretty'], opts['nopl'])
    except pycriu.images.MagicException as exc:
        print("Unknown magic %#x.\n"
              "Maybe you are feeding me an image with "
              "raw data(i.e. pages.img)?" % exc.magic, file=sys.stderr)
        sys.exit(1)

    if opts['pretty']:
        indent = 4

    f = outf(opts, True)
    json.dump(img, f, indent=indent)
    if f == sys.stdout:
        f.write("\n")


def encode(opts):
    try:
        img = json.load(inf(opts))
    except UnicodeDecodeError:
        print("Cannot read JSON.\n"
              "Maybe you are feeding me an image with protobuf data? "
              "Encode expects JSON input.", file=sys.stderr)
        sys.exit(1)
    pycriu.images.dump(img, outf(opts, False))


def info(opts):
    infs = pycriu.images.info(inf(opts))
    json.dump(infs, sys.stdout, indent=4)
    print()


def get_task_id(p, val):
    return p[val] if val in p else p['ns_' + val][0]


#
# Explorers
#


class ps_item:
    def __init__(self, p, core):
        self.pid = get_task_id(p, 'pid')
        self.ppid = p['ppid']
        self.p = p
        self.core = core
        self.kids = []


def show_ps(p, opts, depth=0):
    print("%7d%7d%7d   %s%s" %
          (p.pid, get_task_id(p.p, 'pgid'), get_task_id(p.p, 'sid'), ' ' *
           (4 * depth), p.core['tc']['comm']))
    for kid in p.kids:
        show_ps(kid, opts, depth + 1)


def explore_ps(opts):
    pss = {}
    ps_img = pycriu.images.load(dinf(opts, 'pstree.img'))
    for p in ps_img['entries']:
        core = pycriu.images.load(
            dinf(opts, 'core-%d.img' % get_task_id(p, 'pid')))
        ps = ps_item(p, core['entries'][0])
        pss[ps.pid] = ps

    # Build tree
    psr = None
    for pid in pss:
        p = pss[pid]
        if p.ppid == 0:
            psr = p
            continue

        pp = pss[p.ppid]
        pp.kids.append(p)

    print("%7s%7s%7s   %s" % ('PID', 'PGID', 'SID', 'COMM'))
    show_ps(psr, opts)


files_img = None


def ftype_find_in_files(opts, ft, fid):
    global files_img

    if files_img is None:
        try:
            files_img = pycriu.images.load(dinf(opts, "files.img"))['entries']
        except Exception:
            files_img = []

    if len(files_img) == 0:
        return None

    for f in files_img:
        if f['id'] == fid:
            return f

    return None


def ftype_find_in_image(opts, ft, fid, img):
    f = ftype_find_in_files(opts, ft, fid)
    if f:
        if ft['field'] in f:
            return f[ft['field']]
        else:
            return None

    if ft['img'] is None:
        ft['img'] = pycriu.images.load(dinf(opts, img))['entries']
    for f in ft['img']:
        if f['id'] == fid:
            return f
    return None


def ftype_reg(opts, ft, fid):
    rf = ftype_find_in_image(opts, ft, fid, 'reg-files.img')
    return rf and rf['name'] or 'unknown path'


def ftype_pipe(opts, ft, fid):
    p = ftype_find_in_image(opts, ft, fid, 'pipes.img')
    return p and 'pipe[%d]' % p['pipe_id'] or 'pipe[?]'


def ftype_unix(opts, ft, fid):
    ux = ftype_find_in_image(opts, ft, fid, 'unixsk.img')
    if not ux:
        return 'unix[?]'

    n = ux['name'] and ' %s' % ux['name'] or ''
    return 'unix[%d (%d)%s]' % (ux['ino'], ux['peer'], n)


file_types = {
    'REG': {
        'get': ftype_reg,
        'img': None,
        'field': 'reg'
    },
    'PIPE': {
        'get': ftype_pipe,
        'img': None,
        'field': 'pipe'
    },
    'UNIXSK': {
        'get': ftype_unix,
        'img': None,
        'field': 'usk'
    },
}


def ftype_gen(opts, ft, fid):
    return '%s.%d' % (ft['typ'], fid)


files_cache = {}


def get_file_str(opts, fd):
    key = (fd['type'], fd['id'])
    f = files_cache.get(key, None)
    if not f:
        ft = file_types.get(fd['type'], {'get': ftype_gen, 'typ': fd['type']})
        f = ft['get'](opts, ft, fd['id'])
        files_cache[key] = f

    return f


def explore_fds(opts):
    ps_img = pycriu.images.load(dinf(opts, 'pstree.img'))
    for p in ps_img['entries']:
        pid = get_task_id(p, 'pid')
        idi = pycriu.images.load(dinf(opts, 'ids-%s.img' % pid))
        fdt = idi['entries'][0]['files_id']
        fdi = pycriu.images.load(dinf(opts, 'fdinfo-%d.img' % fdt))

        print("%d" % pid)
        for fd in fdi['entries']:
            print("\t%7d: %s" % (fd['fd'], get_file_str(opts, fd)))

        fdi = pycriu.images.load(dinf(opts, 'fs-%d.img' % pid))['entries'][0]
        print("\t%7s: %s" %
              ('cwd', get_file_str(opts, {
                  'type': 'REG',
                  'id': fdi['cwd_id']
              })))
        print("\t%7s: %s" %
              ('root', get_file_str(opts, {
                  'type': 'REG',
                  'id': fdi['root_id']
              })))


class vma_id:
    def __init__(self):
        self.__ids = {}
        self.__last = 1

    def get(self, iid):
        ret = self.__ids.get(iid, None)
        if not ret:
            ret = self.__last
            self.__last += 1
            self.__ids[iid] = ret

        return ret


def explore_mems(opts):
    ps_img = pycriu.images.load(dinf(opts, 'pstree.img'))
    vids = vma_id()
    for p in ps_img['entries']:
        pid = get_task_id(p, 'pid')
        mmi = pycriu.images.load(dinf(opts, 'mm-%d.img' % pid))['entries'][0]

        print("%d" % pid)
        print("\t%-36s    %s" % ('exe',
                                 get_file_str(opts, {
                                     'type': 'REG',
                                     'id': mmi['exe_file_id']
                                 })))

        for vma in mmi['vmas']:
            st = vma['status']
            if st & (1 << 10):
                fn = ' ' + 'ips[%lx]' % vids.get(vma['shmid'])
            elif st & (1 << 8):
                fn = ' ' + 'shmem[%lx]' % vids.get(vma['shmid'])
            elif st & (1 << 11):
                fn = ' ' + 'packet[%lx]' % vids.get(vma['shmid'])
            elif st & ((1 << 6) | (1 << 7)):
                fn = ' ' + get_file_str(opts, {
                    'type': 'REG',
                    'id': vma['shmid']
                })
                if vma['pgoff']:
                    fn += ' + %#lx' % vma['pgoff']
                if st & (1 << 7):
                    fn += ' (s)'
            elif st & (1 << 1):
                fn = ' [stack]'
            elif st & (1 << 2):
                fn = ' [vsyscall]'
            elif st & (1 << 3):
                fn = ' [vdso]'
            elif vma['flags'] & 0x0100:  # growsdown
                fn = ' [stack?]'
            else:
                fn = ''

            if not st & (1 << 0):
                fn += ' *'

            prot = vma['prot'] & 0x1 and 'r' or '-'
            prot += vma['prot'] & 0x2 and 'w' or '-'
            prot += vma['prot'] & 0x4 and 'x' or '-'

            astr = '%08lx-%08lx' % (vma['start'], vma['end'])
            print("\t%-36s%s%s" % (astr, prot, fn))


def explore_rss(opts):
    ps_img = pycriu.images.load(dinf(opts, 'pstree.img'))
    for p in ps_img['entries']:
        pid = get_task_id(p, 'pid')
        vmas = pycriu.images.load(dinf(opts, 'mm-%d.img' %
                                       pid))['entries'][0]['vmas']
        pms = pycriu.images.load(dinf(opts, 'pagemap-%d.img' % pid))['entries']

        print("%d" % pid)
        vmi = 0
        pvmi = -1
        for pm in pms[1:]:
            pstr = '\t%lx / %-8d' % (pm['vaddr'], pm['nr_pages'])
            while vmi < len(vmas) and vmas[vmi]['end'] <= pm['vaddr']:
                vmi += 1

            pme = pm['vaddr'] + (pm['nr_pages'] << 12)
            vstr = ''
            while vmi < len(vmas) and vmas[vmi]['start'] < pme:
                vma = vmas[vmi]
                if vmi == pvmi:
                    vstr += ' ~'
                else:
                    vstr += ' %08lx / %-8d' % (
                        vma['start'], (vma['end'] - vma['start']) >> 12)
                    if vma['status'] & ((1 << 6) | (1 << 7)):
                        vstr += ' ' + get_file_str(opts, {
                            'type': 'REG',
                            'id': vma['shmid']
                        })
                    pvmi = vmi
                vstr += '\n\t%23s' % ''
                vmi += 1

            vmi -= 1

            print('%-24s%s' % (pstr, vstr))


explorers = {
    'ps': explore_ps,
    'fds': explore_fds,
    'mems': explore_mems,
    'rss': explore_rss
}


def explore(opts):
    explorers[opts['what']](opts)


PAGE_SIZE = 4096
ZERO_PAGE = b'\0' * PAGE_SIZE


def _find_pagemaps(d):
    """Find all pagemap files and their associated pages files."""
    pairs = []
    seen_pages_ids = set()

    for name in sorted(os.listdir(d)):
        if not (name.startswith('pagemap-') or
                name.startswith('pagemap-shmem-')):
            continue
        if not name.endswith('.img'):
            continue

        path = os.path.join(d, name)
        with open(path, 'rb') as f:
            pm = pycriu.images.load(f)

        if not pm['entries']:
            continue

        pages_id = pm['entries'][0].get('pages_id')
        if pages_id is None or pages_id in seen_pages_ids:
            continue
        seen_pages_ids.add(pages_id)

        pages_name = 'pages-%d.img' % pages_id
        pages_path = os.path.join(d, pages_name)
        if not os.path.exists(pages_path):
            continue

        pairs.append((name, pages_name, pm))

    return pairs


def _get_nr_pages(entry):
    return entry.get('nr_pages', entry.get('compat_nr_pages', 0))


def _backup(path):
    bak = path + '.bak'
    os.rename(path, bak)
    return bak


def compress_cmd(opts):
    try:
        import lz4.block
    except ImportError:
        print("Error: lz4 Python package is required.\n"
              "Install with: pip install lz4", file=sys.stderr)
        sys.exit(1)

    d = opts['dir']
    in_place = opts.get('in_place', False)
    acceleration = opts.get('acceleration', 1)

    inv_path = os.path.join(d, 'inventory.img')
    with open(inv_path, 'rb') as f:
        inv = pycriu.images.load(f)

    if inv['entries'][0].get('compress'):
        print("Checkpoint in %s is already compressed" % d)
        return

    pairs = _find_pagemaps(d)
    if not pairs:
        print("No pagemap files found in %s" % d)
        return

    print("Compressing checkpoint in %s" % d)

    for pm_name, pages_name, pm in pairs:
        pm_path = os.path.join(d, pm_name)
        pages_path = os.path.join(d, pages_name)
        tmp_pages = pages_path + '.tmp'

        total_pages = 0
        orig_size = 0
        comp_size = 0

        with open(pages_path, 'rb') as pages_in, \
             open(tmp_pages, 'wb') as pages_out:

            for entry in pm['entries'][1:]:
                nr = _get_nr_pages(entry)
                flags = entry.get('flags', 0)

                # Skip parent references (no data in pages file)
                if flags & 0x1 and not (flags & 0x4):
                    continue

                compressed_sizes = []
                total_cs = 0

                for _ in range(nr):
                    page = pages_in.read(PAGE_SIZE)
                    if len(page) != PAGE_SIZE:
                        print("Error: short read in %s" % pages_name,
                              file=sys.stderr)
                        os.unlink(tmp_pages)
                        sys.exit(1)

                    orig_size += PAGE_SIZE
                    total_pages += 1

                    if page == ZERO_PAGE:
                        compressed_sizes.append(0)
                    else:
                        comp = lz4.block.compress(
                            page, store_size=False,
                            acceleration=acceleration)
                        if len(comp) >= PAGE_SIZE:
                            pages_out.write(page)
                            compressed_sizes.append(PAGE_SIZE)
                            total_cs += PAGE_SIZE
                            comp_size += PAGE_SIZE
                        else:
                            pages_out.write(comp)
                            compressed_sizes.append(len(comp))
                            total_cs += len(comp)
                            comp_size += len(comp)

                entry['compressed_size'] = compressed_sizes
                entry['total_compressed_size'] = total_cs

        if not in_place:
            _backup(pages_path)
            _backup(pm_path)

        os.rename(tmp_pages, pages_path)
        with open(pm_path, 'wb') as f:
            pycriu.images.dump(pm, f)

        if orig_size > 0:
            saved = (1 - comp_size / orig_size) * 100
            print("  %s: %d pages (%dK -> %dK, %.1f%% saved)" %
                  (pm_name, total_pages,
                   orig_size // 1024, comp_size // 1024, saved))

    inv['entries'][0]['compress'] = 1  # COMPRESS_PER_PAGE
    if acceleration != 1:
        inv['entries'][0]['compress_acceleration'] = acceleration

    if not in_place:
        _backup(inv_path)

    with open(inv_path, 'wb') as f:
        pycriu.images.dump(inv, f)

    print("Done")


def decompress_cmd(opts):
    try:
        import lz4.block
    except ImportError:
        print("Error: lz4 Python package is required.\n"
              "Install with: pip install lz4", file=sys.stderr)
        sys.exit(1)

    d = opts['dir']
    in_place = opts.get('in_place', False)

    inv_path = os.path.join(d, 'inventory.img')
    with open(inv_path, 'rb') as f:
        inv = pycriu.images.load(f)

    if not inv['entries'][0].get('compress'):
        print("Checkpoint in %s is already decompressed" % d)
        return

    pairs = _find_pagemaps(d)
    if not pairs:
        print("No pagemap files found in %s" % d)
        return

    print("Decompressing checkpoint in %s" % d)

    for pm_name, pages_name, pm in pairs:
        pm_path = os.path.join(d, pm_name)
        pages_path = os.path.join(d, pages_name)
        tmp_pages = pages_path + '.tmp'

        total_pages = 0
        comp_size = 0
        orig_size = 0

        with open(pages_path, 'rb') as pages_in, \
             open(tmp_pages, 'wb') as pages_out:

            for entry in pm['entries'][1:]:
                nr = _get_nr_pages(entry)
                flags = entry.get('flags', 0)

                if flags & 0x1 and not (flags & 0x4):
                    continue

                cs_list = entry.get('compressed_size')

                if not cs_list:
                    # Uncompressed entry, copy through
                    data = pages_in.read(nr * PAGE_SIZE)
                    pages_out.write(data)
                    orig_size += len(data)
                    comp_size += len(data)
                    total_pages += nr
                else:
                    # region_pages > 0 means each compressed_size entry
                    # covers up to region_pages pages as one LZ4 block
                    # (the last block of an entry may be shorter). 0 or
                    # absent means per-page compression (one page each).
                    region_pages = entry.get('region_pages', 0)
                    remaining = nr

                    for cs in cs_list:
                        if region_pages:
                            block_pages = min(region_pages, remaining)
                        else:
                            block_pages = 1
                        block_bytes = block_pages * PAGE_SIZE
                        total_pages += block_pages
                        remaining -= block_pages

                        if cs == 0:
                            pages_out.write(ZERO_PAGE * block_pages)
                            orig_size += block_bytes
                        elif cs == block_bytes:
                            # Stored raw, copy through verbatim
                            data = pages_in.read(cs)
                            if len(data) != cs:
                                print("Error: short read in %s" %
                                      pages_name, file=sys.stderr)
                                os.unlink(tmp_pages)
                                sys.exit(1)
                            pages_out.write(data)
                            orig_size += block_bytes
                            comp_size += cs
                        else:
                            data = pages_in.read(cs)
                            if len(data) != cs:
                                print("Error: short read in %s" %
                                      pages_name, file=sys.stderr)
                                os.unlink(tmp_pages)
                                sys.exit(1)
                            try:
                                page = lz4.block.decompress(
                                    data, uncompressed_size=block_bytes)
                            except Exception as e:
                                print("Error: decompression failed "
                                      "in %s: %s" % (pages_name, e),
                                      file=sys.stderr)
                                os.unlink(tmp_pages)
                                sys.exit(1)
                            pages_out.write(page)
                            orig_size += block_bytes
                            comp_size += cs

                    if remaining != 0:
                        print("Error: block page count mismatch in %s "
                              "(%d pages unaccounted)" %
                              (pages_name, remaining), file=sys.stderr)
                        os.unlink(tmp_pages)
                        sys.exit(1)

                    # Remove compression metadata
                    if 'compressed_size' in entry:
                        del entry['compressed_size']
                    if 'total_compressed_size' in entry:
                        del entry['total_compressed_size']
                    if 'region_pages' in entry:
                        del entry['region_pages']

        if not in_place:
            _backup(pages_path)
            _backup(pm_path)

        os.rename(tmp_pages, pages_path)
        with open(pm_path, 'wb') as f:
            pycriu.images.dump(pm, f)

        print("  %s: %d pages (%dK -> %dK)" %
              (pm_name, total_pages,
               comp_size // 1024, orig_size // 1024))

    inv['entries'][0].pop('compress', None)
    inv['entries'][0].pop('compress_acceleration', None)
    inv['entries'][0].pop('compress_region_size', None)

    if not in_place:
        _backup(inv_path)

    with open(inv_path, 'wb') as f:
        pycriu.images.dump(inv, f)

    print("Done")


def main():
    desc = 'CRiu Image Tool'
    parser = argparse.ArgumentParser(
        description=desc, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('--version', action='version', version=__version__)

    subparsers = parser.add_subparsers(
        help='Use crit CMD --help for command-specific help')

    # Decode
    decode_parser = subparsers.add_parser(
        'decode', help='convert criu image from binary type to json')
    decode_parser.add_argument(
        '--pretty',
        help='Multiline with indents and some numerical fields in field-specific format',
        action='store_true')
    decode_parser.add_argument(
        '-i',
        '--in',
        help='criu image in binary format to be decoded (stdin by default)')
    decode_parser.add_argument(
        '-o',
        '--out',
        help='where to put criu image in json format (stdout by default)')
    decode_parser.set_defaults(func=decode, nopl=False)

    # Encode
    encode_parser = subparsers.add_parser(
        'encode', help='convert criu image from json type to binary')
    encode_parser.add_argument(
        '-i',
        '--in',
        help='criu image in json format to be encoded (stdin by default)')
    encode_parser.add_argument(
        '-o',
        '--out',
        help='where to put criu image in binary format (stdout by default)')
    encode_parser.set_defaults(func=encode)

    # Info
    info_parser = subparsers.add_parser('info', help='show info about image')
    info_parser.add_argument("in")
    info_parser.set_defaults(func=info)

    # Explore
    x_parser = subparsers.add_parser('x', help='explore image dir')
    x_parser.add_argument('dir')
    x_parser.add_argument('what', choices=['ps', 'fds', 'mems', 'rss'])
    x_parser.set_defaults(func=explore)

    # Show
    show_parser = subparsers.add_parser(
        'show', help="convert criu image from binary to human-readable json")
    show_parser.add_argument("in")
    show_parser.add_argument('--nopl',
                             help='do not show entry payload (if exists)',
                             action='store_true')
    show_parser.set_defaults(func=decode, pretty=True, out=None)

    # Compress
    compress_parser = subparsers.add_parser(
        'compress', help='Compress memory pages in a checkpoint directory')
    compress_parser.add_argument('dir')
    compress_parser.add_argument('--in-place', action='store_true',
                                 help='Skip creating backup files')
    compress_parser.add_argument('--acceleration', type=int, default=1,
                                 help='LZ4 acceleration (1=default, higher=faster)')
    compress_parser.set_defaults(func=compress_cmd)

    # Decompress
    decompress_parser = subparsers.add_parser(
        'decompress', help='Decompress memory pages in a checkpoint directory')
    decompress_parser.add_argument('dir')
    decompress_parser.add_argument('--in-place', action='store_true',
                                    help='Skip creating backup files')
    decompress_parser.set_defaults(func=decompress_cmd)

    opts = vars(parser.parse_args())

    if not opts:
        sys.stderr.write(parser.format_usage())
        sys.stderr.write("crit: error: too few arguments\n")
        sys.exit(1)

    opts["func"](opts)


if __name__ == '__main__':
    main()
