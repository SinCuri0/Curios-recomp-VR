"""Fixture installs for tools/oot/hookshot.py and its two launchers. No game file, no Hookshot and no network: each
install is made in a temporary folder with a few packed bytes, a fake NSO header and a save of three small files.

romfs: what is unpacked equals what was packed, a second run only checks, and a wrong index, an output folder that
overlaps the game's, a folder with other files or too little space stop it before a file is written. save: the save
the runtime would load is the one copied (save-tx/ready-<n>, save/<account>, new, old) into the folder Hookshot reads;
a save Hookshot already has is replaced only after a yes and is kept; each failure of the swap leaves a save in place.
The launchers run in an install whose path has a space and accents (Windows only).

The save tests are skipped while a program named hookshot or ocarina runs, since the script refuses to copy then. One of
them starts a copy of ping (Windows) or sleep under those two names, and ends that process itself.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools' / 'oot' / 'hookshot.py'
WORK = Path(tempfile.mkdtemp(prefix='hookshot-tool-')).resolve()
UID = '5a3c9e1f0b7d24680000000000000001'
ACCT = bytes(range(16))
ACCT_DIR = ACCT[::-1].hex()
NT = os.name == 'nt'
if NT:   # the checks read the English messages; the Windows UI language would pick Portuguese on a Brazilian PC
    os.environ['OOT_LANG'] = 'en'
FILES = {'a.txt': b'alpha', 'dir/b.bin': bytes(range(256)) * 40, 'dir/sub/empty': b'', 'Acao/c d.txt': b'ola'}
failed, checks = [], [0]


def check(name, ok, detail=''):
    checks[0] += 1
    if not ok:
        failed.append(name + ('  <<' + str(detail)[-400:] + '>>' if detail else ''))


def remove(d):
    for p in d.rglob('*'):   # (a read-only file left by a test: Windows refuses to delete it)
        if p.is_file() and not os.access(p, os.W_OK):
            os.chmod(p, 0o600)
    shutil.rmtree(d)


def fresh(name):
    d = WORK / name
    if d.exists():
        remove(d)
    d.mkdir(parents=True)
    return d


def save(d, tag):
    (d / 'slot_00').mkdir(parents=True)
    (d / 'account.sav').write_bytes(b'account ' + tag.encode())
    (d / 'option.sav').write_bytes(b'option ' + tag.encode())
    (d / 'slot_00' / 'progress.sav').write_bytes(b'progress ' + tag.encode() * 100)
    return d


def install(name, files=FILES, build='e0f3ccfa', index=None, **meta):
    """A fixture install: tools/oot/hookshot.py, build/romfs packed, build/exefs, perfil with one committed save."""
    root = fresh(name)
    (root / 'tools' / 'oot').mkdir(parents=True)
    shutil.copy(TOOL, root / 'tools' / 'oot' / 'hookshot.py')
    image, entries = b'', []
    for path, data in files.items():
        entries.append({'path': path, 'offset': len(image), 'size': len(data)})
        image += data
    r = root / 'build' / 'romfs'
    r.mkdir(parents=True)
    (r / 'romfs.bin').write_bytes(image)
    index = {'status': 'pass', 'romfs_size': len(image), 'romfs_sha256': hashlib.sha256(image).hexdigest(),
             'files': entries if index is None else index}
    index.update(meta)
    (r / 'index.json').write_text(json.dumps(index), encoding='utf-8')
    ex = root / 'build' / 'exefs'
    ex.mkdir()
    (ex / 'main').write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(build) + bytes(28) + b'code')
    (ex / 'main.npdm').write_bytes(b'npdm')
    (ex / 'sdk').write_bytes(b'sdk')
    (root / 'perfil').mkdir()
    (root / 'perfil' / 'account.uid').write_bytes(ACCT)
    save(root / 'perfil' / 'save' / ACCT_DIR, 'committed')
    return root


def run(root, *args, env=None, answer=''):
    """The script of a fixture install with these arguments; answer is what it reads when it asks (nothing: no one to ask)."""
    e = {k: v for k, v in {**os.environ, **(env or {})}.items() if v is not None}
    r = subprocess.run([sys.executable, '-I', str(root / 'tools' / 'oot' / 'hookshot.py')] + [str(a) for a in args],
                       capture_output=True, text=True, errors='replace', env=e, input=answer)
    return r.returncode, r.stdout + r.stderr


def tree(d):
    return {p.relative_to(d).as_posix(): p.read_bytes() for p in d.rglob('*') if p.is_file()} if d.is_dir() else None


def module():
    spec = importlib.util.spec_from_file_location('hookshot_tool', TOOL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def inproc(fn, **kw):
    """Runs one command of the module in this process; returns (exit code, what it printed)."""
    buf = io.StringIO()
    code = 0
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            fn(argparse.Namespace(**kw))
        except SystemExit as e:
            code = e.code
    return code, buf.getvalue()


def want(tag):
    return tree(save(fresh('ref'), tag))


def asides(u):
    return sorted(p for p in u.iterdir() if p.name.startswith('save-antes-de-importar-'))


def t_romfs():
    root = install('r1')
    o = root / 'build' / 'hookshot'
    code, out = run(root, 'romfs')
    check('romfs: default run exits 0', code == 0, out)
    check('romfs: the files equal the packed content', tree(o / 'romfs') == FILES)
    check('romfs: exefs is a copy', tree(o / 'exefs') == tree(root / 'build' / 'exefs'))
    check('romfs: the folder holds romfs, exefs and romfs.ok only', sorted(p.name for p in o.iterdir()) == ['exefs', 'romfs', 'romfs.ok'])
    check('romfs: prints the one folder to pick', str(o) in out and 'romfs and exefs folders' in out, out)
    stamps = {p: p.stat().st_mtime_ns for p in o.rglob('*') if p.is_file()}
    time.sleep(0.05)
    code, out = run(root, 'romfs')
    check('romfs: a second run checks and writes nothing again', code == 0 and 'Checking the files unpacked before' in out
          and stamps == {p: p.stat().st_mtime_ns for p in o.rglob('*') if p.is_file()}, out)
    (o / 'romfs' / 'dir' / 'b.bin').write_bytes(b'short')
    code, out = run(root, 'romfs')
    check('romfs: a file of another size is refused', code == 1 and 'is missing or has another size' in out, out)
    (o / 'romfs' / 'dir' / 'b.bin').unlink()
    code, out = run(root, 'romfs')
    check('romfs: a missing file is refused', code == 1 and 'is missing or has another size' in out, out)
    (o / 'romfs.ok').unlink()
    code, out = run(root, 'romfs')
    check('romfs: a folder with files and no mark is refused', code == 1 and 'already holds other files' in out, out)
    shutil.rmtree(o / 'romfs')
    (o / 'romfs.ok').write_text('stale\n')
    code, out = run(root, 'romfs')
    check('romfs: an empty folder with a stale mark is unpacked again', code == 0 and tree(o / 'romfs') == FILES, out)
    for f in (root / 'build' / 'exefs').iterdir():   # (a dump copied from read-only media: the copies are read-only too)
        os.chmod(f, 0o400)
    shutil.rmtree(o)
    code, out = run(root, 'romfs')
    code2, out2 = run(root, 'romfs')
    check('romfs: read-only exefs files: the second run still ends well', code == 0 and code2 == 0
          and 'romfs and exefs folders' in out2 and tree(o / 'exefs') == tree(root / 'build' / 'exefs'), out + out2)


def t_romfs_more():
    cases = [
        ('a path with ..', [{'path': '../x', 'offset': 0, 'size': 1}]),
        ('.. inside a path', [{'path': 'a/../../x', 'offset': 0, 'size': 1}]),
        ('a drive letter', [{'path': 'C:/x', 'offset': 0, 'size': 1}]),
        ('a colon in a name', [{'path': 'a/b:c', 'offset': 0, 'size': 1}]),
        ('a backslash', [{'path': 'a' + chr(92) + 'b', 'offset': 0, 'size': 1}]),
        ('a leading slash', [{'path': '/x', 'offset': 0, 'size': 1}]),
        ('a trailing slash', [{'path': 'x/', 'offset': 0, 'size': 1}]),
        ('a dot part', [{'path': './x', 'offset': 0, 'size': 1}]),
        ('an empty path', [{'path': '', 'offset': 0, 'size': 1}]),
        ('a path that is a number', [{'path': 5, 'offset': 0, 'size': 1}]),
        ('no path', [{'offset': 0, 'size': 1}]),
        ('a size past the image', [{'path': 'x', 'offset': 0, 'size': 10 ** 9}]),
        ('an offset past the image', [{'path': 'x', 'offset': 10 ** 9, 'size': 0}]),
        ('a negative offset', [{'path': 'x', 'offset': -1, 'size': 1}]),
        ('a negative size', [{'path': 'x', 'offset': 0, 'size': -1}]),
        ('a boolean offset', [{'path': 'x', 'offset': True, 'size': 1}]),
        ('a fractional size', [{'path': 'x', 'offset': 0, 'size': 1.0}]),
        ('an entry that is not an object', ['x']),
        ('an empty list', []),
        ('files that is not a list', 'x'),
    ]

    def refused(name, root, text):
        before = tree(root)
        code, out = run(root, 'romfs')
        check('romfs: %s is refused, nothing written' % name, code == 1 and any(t in out for t in text)
              and 'Traceback' not in out and tree(root) == before and not (root / 'build' / 'hookshot').exists(), out)

    for name, index in cases:
        refused('index with ' + name, install('r2', index=index), ('Invalid entry in index.json', 'index.json has no file list'))
    refused('an index that records no verified extraction', install('r2', status='fail'), ('verified extraction',))
    refused('an index made for an image of another size', install('r2', romfs_size=5), ('verified extraction',))
    root = install('r2')
    (root / 'build' / 'romfs' / 'index.json').write_text('{not json', encoding='utf-8')
    refused('an index.json that is not JSON', root, ('Could not finish',))
    root = install('r3')
    (root / 'build' / 'romfs' / 'romfs.bin').unlink()
    refused('no romfs.bin', root, ('game files are not here',))
    root = install('r3')
    (root / 'build' / 'exefs' / 'main').unlink()
    refused('no exefs main', root, ('is missing',))
    root = install('r3', build='deadbeef')
    code, out = run(root, 'romfs')
    check('romfs: another build id warns and unpacks', code == 0 and 'this game is build deadbeef' in out
          and tree(root / 'build' / 'hookshot' / 'romfs') == FILES, out)
    root = install('r3')
    code, out = run(root, 'romfs')
    check('romfs: the supported build id gives no warning', code == 0 and 'Warning' not in out, out)
    root = install('r3')
    (root / 'build' / 'exefs' / 'main').write_bytes(b'XXXX' + bytes(0x100))
    code, out = run(root, 'romfs')
    check('romfs: a main that is not a plain NSO gives no warning', code == 0 and 'Warning' not in out, out)
    root = install('r3')
    other = fresh('r3 out')
    code, out = run(root, 'romfs', '--out', other / 'made here')
    check('romfs: --out elsewhere', code == 0 and tree(other / 'made here' / 'romfs') == FILES
          and tree(other / 'made here' / 'exefs') == tree(root / 'build' / 'exefs')
          and not (root / 'build' / 'hookshot').exists(), out)
    before = tree(root)
    for name, where in (('build (its romfs is the packed one)', root / 'build'),
                        ('a folder inside the packed one', root / 'build' / 'romfs'),
                        ('a folder inside exefs', root / 'build' / 'exefs')):
        code, out = run(root, 'romfs', '--out', where)
        check('romfs: --out %s is refused, nothing written' % name, code == 1 and 'overlaps' in out
              and 'elete' not in out and 'Traceback' not in out and tree(root) == before, out)
    alt = fresh('r3-alt')
    shutil.copytree(root / 'build' / 'exefs', alt / 'exefs')
    before = tree(alt / 'exefs')
    code, out = run(root, 'romfs', '--exefs', alt / 'exefs', '--out', alt)
    check('romfs: --out whose exefs is the source leaves it alone', code == 0 and tree(alt / 'exefs') == before
          and tree(alt / 'romfs') == FILES, out)
    dump = fresh('r3-dump')
    (dump / 'exefs').mkdir()
    (dump / 'exefs' / 'main').write_bytes(b'the main of another dump')
    before = tree(dump)
    code, out = run(root, 'romfs', '--out', dump)
    check('romfs: --out whose exefs holds other files is refused before the unpack', code == 1
          and 'already holds other files' in out and tree(dump) == before, out)
    m = module()
    root = install('r4')
    args = dict(romfs=str(root / 'build' / 'romfs'), exefs=str(root / 'build' / 'exefs'), out=str(root / 'build' / 'hookshot'))
    usage = shutil.disk_usage
    shutil.disk_usage = lambda p: type(usage(p))(10 ** 12, 10 ** 12 - 5, 5)
    try:
        code, out = inproc(m.cmd_romfs, **args)
    finally:
        shutil.disk_usage = usage
    check('romfs: too little free space is refused before any file', code == 1 and 'Not enough free space' in out
          and not tree(root / 'build' / 'hookshot'), out)
    unit, m.cluster = m.cluster, lambda folder: 1 << 20   # (three files with bytes: 3 MiB on such a volume, not 10 KB)
    shutil.disk_usage = lambda p: type(usage(p))(10 ** 12, 10 ** 12, 10 ** 9 + 10 ** 6)
    try:
        code, out = inproc(m.cmd_romfs, **args)
    finally:
        shutil.disk_usage, m.cluster = usage, unit
    check('romfs: the space needed counts whole allocation units', code == 1 and 'Not enough free space' in out, out)
    check('romfs: the allocation unit of a real folder', m.cluster(root) >= 512 and m.cluster(root) % 512 == 0, m.cluster(root))
    code, out = inproc(m.cmd_romfs, **args)
    check('romfs: the same call with room succeeds', code == 0 and tree(root / 'build' / 'hookshot' / 'romfs') == FILES, out)
    image = root / 'build' / 'romfs' / 'romfs.bin'
    cut = fresh('r4-cut')
    code, out = inproc(lambda a: m.unpack(image, [{'path': 'x', 'offset': image.stat().st_size - 2, 'size': 9}], 9, cut))
    check('romfs: an image that ends early stops the unpack', code == 1 and 'romfs.bin ended early' in out, out)
    shutil.rmtree(root / 'build' / 'hookshot')
    m.PT = True
    try:
        code, out = inproc(m.cmd_romfs, **args)
        code2, out2 = inproc(m.cmd_romfs, **args)
    finally:
        m.PT = False
    check('romfs: Portuguese messages, with a decimal comma', code == 0 and code2 == 0 and '(0,0 GB)' in out
          and 'No Hookshot Builder, escolha' in out and 'Conferindo' in out2, out + out2)
    check('romfs: one folder inside another, or the same, overlap; neighbours do not',
          m.overlap(root, root / 'build') and m.overlap(root / 'build', root) and m.overlap(root, root)
          and not m.overlap(root / 'build', root / 'builder') and not m.overlap(root / 'a', root / 'b'))


def t_save():
    root = install('s1')
    src = root / 'perfil' / 'save' / ACCT_DIR
    u = fresh('s1 user')
    dest = u / 'save' / UID
    code, out = run(root, 'save', '--user', u)
    check('save: copy into an empty user folder, with no question', code == 0 and tree(dest) == want('committed')
          and sorted(p.name for p in u.iterdir()) == ['save'] and sorted(p.name for p in (u / 'save').iterdir()) == [UID]
          and 'Save copied to Hookshot' in out and str(dest) in out and 'previous save' not in out, out)
    check('save: the source is not changed', tree(src) == want('committed'))
    shutil.rmtree(src)
    save(src, 'second')
    other = save(u / 'save' / ('ab' * 16), 'someone else')
    past = time.time() - 86400
    for f in dest.rglob('*'):
        os.utime(f, (past, past))

    def as_it_was():
        return tree(dest) == want('committed') and not asides(u) and not (u / 'save-importando').exists()

    code, out = run(root, 'save', '--user', u)
    check('save: Hookshot has a save and no one can be asked: nothing is replaced', code == 1
          and 'run this again with --yes' in out and 'Hookshot already has a save' in out and as_it_was(), out)
    code, out = run(root, 'save', '--user', u, answer='n\n')
    check('save: the answer no leaves it', code == 0 and 'Nothing was changed' in out and as_it_was(), out)
    code, out = run(root, 'save', '--user', u, answer='\n')
    kept = asides(u)
    check('save: Enter replaces an older save and keeps it aside', code == 0 and 'NEWER' not in out
          and tree(dest) == want('second') and len(kept) == 1 and tree(kept[0]) == want('committed')
          and str(kept[0]) in out and not (u / 'save-importando').exists(), out)
    check('save: another account folder beside it is left alone', tree(other) == want('someone else'))
    shutil.rmtree(src)
    save(src, 'third')
    future = time.time() + 86400
    for f in dest.rglob('*'):
        os.utime(f, (future, future))
    code, out = run(root, 'save', '--user', u, answer='\n')
    check("save: Hookshot's save is the newer one: it is said, and Enter leaves it", code == 0 and 'NEWER' in out
          and 'Nothing was changed' in out and tree(dest) == want('second') and len(asides(u)) == 1, out)
    code, out = run(root, 'save', '--user', u, answer='y\n')
    check('save: a yes replaces the newer save and keeps it aside', code == 0 and tree(dest) == want('third')
          and len(asides(u)) == 2, out)
    shutil.rmtree(src)
    save(src, 'fourth')
    code, out = run(root, 'save', '--user', u, '--yes')
    check('save: --yes replaces without asking', code == 0 and tree(dest) == want('fourth') and len(asides(u)) == 3, out)
    m = module()
    real = time.strftime
    time.strftime = lambda fmt, *a: '20260102-030405'
    codes = []
    try:
        for tag in ('fifth', 'sixth', 'seventh'):
            shutil.rmtree(src)
            save(src, tag)
            codes.append(inproc(m.cmd_save, profile=str(root / 'perfil'), user=str(u), hookshot=None, folder=None, yes=True)[0])
    finally:
        time.strftime = real
    base = u / 'save-antes-de-importar-20260102-030405'
    check('save: copies in the same second each keep their own folder', codes == [0, 0, 0] and tree(base) == want('fourth')
          and tree(Path(str(base) + '-2')) == want('fifth') and tree(Path(str(base) + '-3')) == want('sixth')
          and tree(dest) == want('seventh') and len(asides(u)) == 6, codes)
    (u / 'save-importando').mkdir()
    (u / 'save-importando' / 'junk').write_bytes(b'left by a run that failed')
    code, out = run(root, 'save', '--user', u, '--yes')
    check('save: a copy left by a failed run is replaced', code == 0 and tree(dest) == want('seventh')
          and not (u / 'save-importando').exists(), out)
    before = tree(root / 'perfil')
    code, out = run(root, 'save', '--user', src)
    check("save: a user folder inside this install's profile is refused, the profile untouched", code == 1
          and 'one inside the other' in out and tree(root / 'perfil') == before, out)


def t_save_tx():
    def profile(committed=True, **tx):
        root = install('s2')
        p = root / 'perfil'
        if not committed:
            shutil.rmtree(p / 'save' / ACCT_DIR)
        for k, tag in tx.items():
            save(p / 'save-tx' / k.replace('_', '-'), tag)
        return root, p

    def copied(root):
        u = fresh('s2-user')
        code, out = run(root, 'save', '--user', u)
        return code, out, tree(u / 'save' / UID), u

    root, p = profile(work='work', ready_2='r2', ready_10='r10', new='new', old='old')
    (p / 'save-tx' / 'ready-99').write_bytes(b'a file, not a commit')
    for name in ('ready-x', 'ready-0', 'ready-4294967296', 'ready-11a'):   # (names the runtime does not read)
        save(p / 'save-tx' / name, 'not a commit')
    code, out, got, u = copied(root)
    check('save: the highest finished commit wins (ready-10 over ready-2, the committed save, new, old, work)',
          code == 0 and got == want('r10'), out)
    root, p = profile(work='work')
    code, out, got, u = copied(root)
    check('save: with a working copy only, the committed save', code == 0 and got == want('committed'), out)
    root, p = profile(work='work', new='new', old='old')
    code, out, got, u = copied(root)
    check('save: the committed save before new and old', code == 0 and got == want('committed'), out)
    root, p = profile(committed=False, work='work', new='new', old='old')
    code, out, got, u = copied(root)
    check('save: no committed save: new', code == 0 and got == want('new'), out)
    root, p = profile(committed=False, work='work', old='old')
    code, out, got, u = copied(root)
    check('save: no committed save and no new: old', code == 0 and got == want('old'), out)
    root, p = profile(committed=False, work='work')
    before = tree(p)
    code, out, got, u = copied(root)
    check('save: a working copy alone is no save', code == 1 and 'No save in' in out and got is None
          and not any(u.iterdir()) and tree(p) == before, out)
    root, p = profile()
    for f in list((p / 'save' / ACCT_DIR).rglob('*')):
        if f.is_file():
            f.unlink()
    code, out, got, u = copied(root)
    check('save: a committed folder without save files is no save', code == 1 and 'No save in' in out and got is None, out)
    shutil.rmtree(p / 'save' / ACCT_DIR)
    (p / 'save' / ACCT_DIR).mkdir()
    (p / 'save' / ACCT_DIR / 'slot_00').write_bytes(b'a file with the name of a slot')
    code, out, got, u = copied(root)
    check('save: a file named like a slot is no save', code == 1 and 'No save in' in out and got is None, out)
    root, p = profile()
    (p / 'account.uid').unlink()
    code, out, got, u = copied(root)
    check('save: no account.uid is refused', code == 1 and "The profile's account is missing" in out and got is None, out)
    root, p = profile()
    (p / 'account.uid').write_bytes(b'short')
    code, out, got, u = copied(root)
    check('save: an account.uid of another size is refused', code == 1 and "The profile's account is missing" in out, out)
    root, p = profile()
    code, out = run(root, 'save', '--user', fresh('s2-user'), '--profile', WORK / 'no such profile')
    check('save: --profile without a profile is refused', code == 1 and 'Traceback' not in out, out)


def t_save_game():
    """Windows: the game folder given, the one last chosen in Hookshot Builder, and its default one."""
    root = install('s3')
    for name, files, ok in (('image.bin and bin (the launcher layout)', ('image.bin', 'bin/hookshot.exe', 'hookshot.exe'), True),
                            ('image.bin and hookshot.exe (no launcher)', ('image.bin', 'hookshot.exe'), True),
                            ('image.bin and bin', ('image.bin', 'bin/x'), True),
                            ('no image.bin', ('bin/hookshot.exe', 'hookshot.exe'), False),
                            ('image.bin alone', ('image.bin',), False),
                            ('nothing', (), False)):
        g = fresh('s3 game')
        for f in files:
            (g / f).parent.mkdir(parents=True, exist_ok=True)
            (g / f).write_bytes(b'x')
        before = tree(g)
        code, out = run(root, 'save', '--hookshot', g)
        if ok:
            check('save: --hookshot with %s' % name, code == 0 and tree(g / 'user' / 'save' / UID) == want('committed'), out)
        else:
            check('save: --hookshot with %s is refused' % name, code == 1 and "Hookshot's game is not in" in out
                  and tree(g) == before and not (g / 'user').exists(), out)
    home, local = fresh('s3-home'), fresh('s3-local')   # (a LOCALAPPDATA with no Hookshot Builder settings yet)
    g = home / 'Games' / 'Hookshot'
    env = {'USERPROFILE': str(home), 'LOCALAPPDATA': str(local)}
    code, out = run(root, 'save', env=env)
    check('save: no Hookshot in the default folder is refused', code == 1 and str(g) in out and '--hookshot' in out
          and not any(home.iterdir()), out)
    (g / 'bin').mkdir(parents=True)
    (g / 'image.bin').write_bytes(b'x')
    code, out = run(root, 'save', env=env)
    check('save: the default game folder', code == 0 and tree(g / 'user' / 'save' / UID) == want('committed'), out)
    g2 = fresh('s3 chosen')
    (g2 / 'bin').mkdir()
    (g2 / 'image.bin').write_bytes(b'x')
    (local / 'HookshotBuilder').mkdir()
    settings = local / 'HookshotBuilder' / 'builder-settings.json'
    settings.write_text(json.dumps({'Out': str(g2), 'Nup': None, 'Dlss': False}), encoding='utf-8')
    code, out = run(root, 'save', env=env)
    check('save: the folder last chosen in Hookshot Builder goes before the default one', code == 0
          and tree(g2 / 'user' / 'save' / UID) == want('committed') and not asides(g / 'user'), out)
    settings.write_text(json.dumps({'Out': str(WORK / 'no such game')}), encoding='utf-8')
    code, out = run(root, 'save', '--yes', env=env)
    check('save: a chosen folder that holds no game leaves the default one', code == 0 and len(asides(g / 'user')) == 1, out)
    settings.write_text('not json', encoding='utf-8')
    code, out = run(root, 'save', '--yes', env=env)
    check('save: settings that cannot be read are passed over', code == 0 and len(asides(g / 'user')) == 2, out)
    g3 = fresh('s3 dropped')
    (g3 / 'bin').mkdir()
    (g3 / 'image.bin').write_bytes(b'x')
    code, out = run(root, 'save', g3, env=env)
    check('save: a game folder given without an option (dropped on the launcher)', code == 0
          and tree(g3 / 'user' / 'save' / UID) == want('committed'), out)
    u = fresh('s3-user')
    code, out = run(root, 'save', '--user', u, '--hookshot', g3)
    check('save: --user and --hookshot together are refused', code == 2 and 'not allowed with' in out
          and not any(u.iterdir()), out)


def t_save_linux():
    """Linux: the data home, wherever the game is."""
    root = install('s4')
    home, xdg = fresh('s4-home'), fresh('s4-xdg')
    env = {'HOME': str(home), 'XDG_DATA_HOME': str(xdg), 'LC_ALL': None, 'LANG': 'C'}
    data = home / '.local' / 'share' / 'hookshot'
    code, out = run(root, 'save', env=env)
    check('save (Linux): XDG_DATA_HOME/hookshot', code == 0 and tree(xdg / 'hookshot' / 'save' / UID) == want('committed')
          and not any(home.iterdir()), out)
    code, out = run(root, 'save', env={**env, 'XDG_DATA_HOME': None})
    check('save (Linux): ~/.local/share/hookshot without the variable', code == 0
          and tree(data / 'save' / UID) == want('committed'), out)
    code, out = run(root, 'save', '--yes', env={**env, 'XDG_DATA_HOME': ''})
    check('save (Linux): an empty variable counts as none', code == 0 and len(asides(data)) == 1, out)
    code, out = run(root, 'save', '--yes', env={**env, 'XDG_DATA_HOME': 'relative/data'})
    check('save (Linux): a relative variable is not used, as in Hookshot', code == 0 and len(asides(data)) == 2, out)
    code, out = run(root, 'save', '--yes', env={**env, 'LANG': 'pt_BR.UTF-8'})
    check('save (Linux): Portuguese by LANG', code == 0 and 'Save copiado para o Hookshot' in out
          and 'O save anterior do Hookshot ficou em' in out, out)
    code, out = run(root, 'save', '--yes', env={**env, 'LC_ALL': 'pt_BR.UTF-8'})
    check('save (Linux): LC_ALL goes before LANG, as in compilar.sh', code == 0 and 'Save copiado para o Hookshot' in out, out)
    u = fresh('s4-user')
    code, out = run(root, 'save', '--user', u, env=env)
    check('save (Linux): --user', code == 0 and tree(u / 'save' / UID) == want('committed'), out)
    code, out = run(root, 'romfs', env={**env, 'LANG': 'pt_BR.UTF-8'})
    check('romfs (Linux): Portuguese by LANG', code == 0 and 'No Hookshot Builder, escolha' in out, out)


def t_running():
    root = install('s5')
    u = fresh('s5-user')
    d = fresh('s5-proc')
    for game in ('ocarina', 'hookshot'):
        if NT:   # a copy of ping under the game's name: it only waits
            exe = d / (game + '.exe')
            shutil.copy(Path(os.environ['SystemRoot']) / 'System32' / 'ping.exe', exe)
            args = [str(exe), '-n', '40', '127.0.0.1']
        else:
            exe = d / game
            shutil.copy(shutil.which('sleep'), exe)
            args = [str(exe), '40']
        p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(1)
            alive = p.poll() is None
            code, out = run(root, 'save', '--user', u)
        finally:
            p.kill()   # this process only, by its handle
            p.wait()
        check('save: refused while a program named %s runs' % game, alive and code == 1 and 'Close the game (%s' % game in out
              and not any(u.iterdir()), (alive, out))
    time.sleep(0.5)
    code, out = run(root, 'save', '--user', u)
    check('save: accepted once it is closed', code == 0 and tree(u / 'save' / UID) == want('committed'), out)
    m = module()
    real = subprocess.run

    def no_list(*a, **k):
        raise OSError('forced')

    if NT:
        subprocess.run = no_list
        try:
            code, out = inproc(m.cmd_save, profile=str(root / 'perfil'), user=str(u), hookshot=None, folder=None, yes=True)
        finally:
            subprocess.run = real
        check('save: when the running programs cannot be listed, it says so and goes on', code == 0
              and 'could not see whether the game is running' in out, out)


def t_save_failures():
    m = module()
    root = install('s6')
    u = fresh('s6-user')
    old = save(u / 'save' / UID, 'hookshot had this')
    tmp = u / 'save-importando'
    args = dict(profile=str(root / 'perfil'), user=str(u), hookshot=None, folder=None, yes=True)

    def unchanged():
        return tree(old) == want('hookshot had this') and tree(tmp) == want('committed') and not asides(u)

    if NT:   # a file of Hookshot's save held open: Windows refuses to rename its folder
        with open(old / 'account.sav', 'rb'):
            code, out = run(root, 'save', '--user', u, '--yes')
        check("save: Hookshot's save in use: refused, it stays, the copy is kept and named", code == 1
              and 'Traceback' not in out and "Hookshot's save was not changed" in out and str(tmp) in out and unchanged(), out)
    real = Path.rename

    def no_copy_in(self, target):
        if self.name == 'save-importando':
            raise OSError('forced')
        return real(self, target)

    Path.rename = no_copy_in
    try:
        code, out = inproc(m.cmd_save, **args)
    finally:
        Path.rename = real
    check("save: the copy cannot take the place: Hookshot's save goes back", code == 1
          and "Hookshot's save was not changed" in out and str(tmp) in out and unchanged(), out)
    calls = []

    def first_only(self, target):
        calls.append(self.name)
        if len(calls) > 1:
            raise OSError('forced')
        return real(self, target)

    Path.rename = first_only
    try:
        code, out = inproc(m.cmd_save, **args)
    finally:
        Path.rename = real
    kept = asides(u)
    check('save: and when it cannot go back, both folders are named', code == 1 and len(kept) == 1 and not old.exists()
          and tree(kept[0]) == want('hookshot had this') and str(kept[0]) in out and str(tmp) in out
          and tree(tmp) == want('committed') and calls == [UID, 'save-importando', kept[0].name], (calls, out))
    code, out = run(root, 'save', '--user', u)
    check('save: the next run puts the save in place', code == 0 and tree(old) == want('committed') and not tmp.exists()
          and asides(u) == kept and tree(kept[0]) == want('hookshot had this'), out)
    seq = iter([['one'], ['other']])
    real_listing = m.listing
    m.listing = lambda d: next(seq)
    try:
        code, out = inproc(m.cmd_save, **args)
    finally:
        m.listing = real_listing
    check('save: a copy that differs from the source is refused before anything moves', code == 1
          and 'differs from the save' in out and tree(old) == want('committed') and asides(u) == kept, out)
    u2 = fresh('s6-user2')
    (u2 / 'save').write_bytes(b'a file where the save folder goes')
    code, out = run(root, 'save', '--user', u2)
    check('save: a user folder that cannot take the copy is refused', code == 1 and 'Traceback' not in out
          and 'Could not copy the save' in out, out)
    m.PT = True
    try:
        code, out = inproc(m.cmd_save, **args)
    finally:
        m.PT = False
    check('save: Portuguese messages', code == 0 and 'Save copiado para o Hookshot' in out
          and 'O Hookshot já tem um save, de' in out and 'O save anterior do Hookshot ficou em' in out, out)


def python_for(root):
    """A Python where the launchers look for one: .venv when this Python can make it, else a copy of this Python's own
    folder as ferramentas/python (the Python the installer puts there has no venv module)."""
    try:
        import venv
    except ImportError:
        shutil.copytree(Path(sys.executable).resolve().parent, root / 'ferramentas' / 'python')
    else:
        venv.EnvBuilder(with_pip=False).create(root / '.venv')


def t_launchers():
    """The two .bat files in a fixture install whose folder has a space and accents, with a Python of its own."""
    root = install('l1 instala' + chr(231) + chr(227) + 'o')
    (root / 'jogar').mkdir()
    for n in ('hookshot-preparar-jogo.bat', 'hookshot-copiar-save.bat'):
        shutil.copy(ROOT / 'jogar' / n, root / 'jogar' / n)
    python_for(root)
    home, local = fresh('l1 home'), fresh('l1 local')
    g = home / 'Games' / 'Hookshot'
    (g / 'bin').mkdir(parents=True)
    (g / 'image.bin').write_bytes(b'x')
    env = dict(os.environ, USERPROFILE=str(home), LOCALAPPDATA=str(local))

    def bat(name, args='', cwd=WORK, at=root):
        r = subprocess.run('cmd /c ""%s" %s"' % (at / 'jogar' / name, args), stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, errors='replace', env=env, cwd=str(cwd))
        return r.returncode, r.stdout + r.stderr

    code, out = bat('hookshot-preparar-jogo.bat')
    o = root / 'build' / 'hookshot'
    check('launcher: preparar-jogo makes build/hookshot', code == 0 and tree(o / 'romfs') == FILES
          and tree(o / 'exefs') == tree(root / 'build' / 'exefs') and 'romfs and exefs folders' in out, out)
    code, out = bat('hookshot-preparar-jogo.bat', cwd=Path(os.environ['SystemRoot']))
    check('launcher: preparar-jogo again, from another folder, checks', code == 0
          and 'Checking the files unpacked before' in out, out)
    code, out = bat('hookshot-preparar-jogo.bat', '--help')
    check('launcher: --help reaches the script', code == 0 and '--out' in out and '--romfs' in out and '--exefs' in out, out)
    code, out = bat('hookshot-preparar-jogo.bat', '--out "%s"' % (root / 'build'))
    check("launcher: the script's failure is the launcher's exit code", code == 1 and 'overlaps' in out, out)
    code, out = bat('hookshot-copiar-save.bat')
    check('launcher: copiar-save copies into the default game folder', code == 0
          and tree(g / 'user' / 'save' / UID) == want('committed') and 'Save copied to Hookshot' in out, out)
    code, out = bat('hookshot-copiar-save.bat')
    check('launcher: copiar-save again has no one to ask and replaces nothing', code == 1 and '--yes' in out
          and not asides(g / 'user'), out)
    u = fresh('l1 user')
    code, out = bat('hookshot-copiar-save.bat', '--user "%s"' % u)
    check('launcher: copiar-save passes --user on', code == 0 and tree(u / 'save' / UID) == want('committed'), out)
    g2 = fresh('l1 jogo solto')
    (g2 / 'bin').mkdir()
    (g2 / 'image.bin').write_bytes(b'x')
    code, out = bat('hookshot-copiar-save.bat', '"%s"' % g2)
    check('launcher: a game folder dropped on copiar-save gets the save', code == 0
          and tree(g2 / 'user' / 'save' / UID) == want('committed'), out)
    shutil.rmtree(g)
    code, out = bat('hookshot-copiar-save.bat')
    check('launcher: copiar-save without Hookshot says so', code == 1 and "Hookshot's game is not in" in out
          and 'Traceback' not in out, out)
    bare = install('l2 sem python')
    (bare / 'jogar').mkdir()
    shutil.copy(ROOT / 'jogar' / 'hookshot-preparar-jogo.bat', bare / 'jogar')
    code, out = bat('hookshot-preparar-jogo.bat', at=bare)
    check('launcher: an install with no Python says so', code == 1 and 'Python was not found' in out
          and not (bare / 'build' / 'hookshot').exists(), out)


class HookshotTool(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.busy = module().running(('hookshot', 'ocarina'))

    @classmethod
    def tearDownClass(cls):
        remove(WORK)
        print('%d checks' % checks[0])

    def group(self, fn):
        del failed[:]
        fn()
        self.assertEqual(failed, [])

    def saves(self, fn):
        if self.busy:
            self.skipTest('%s runs: the script refuses to copy a save then' % ', '.join(self.busy))
        self.group(fn)

    def test_romfs(self):
        self.group(t_romfs)

    def test_romfs_refusals(self):
        self.group(t_romfs_more)

    def test_save(self):
        self.saves(t_save)

    def test_save_choice(self):
        self.saves(t_save_tx)

    @unittest.skipUnless(NT, 'on Windows the save goes into the game folder')
    def test_save_game_folder(self):
        self.saves(t_save_game)

    @unittest.skipIf(NT, 'on Linux the save goes into the data folder')
    def test_save_data_folder(self):
        self.saves(t_save_linux)

    def test_save_refused_while_the_game_runs(self):
        self.saves(t_running)

    def test_save_failures(self):
        self.saves(t_save_failures)

    @unittest.skipUnless(NT, 'the launchers are .bat files')
    def test_launchers(self):
        self.saves(t_launchers)


if __name__ == '__main__':
    unittest.main()
