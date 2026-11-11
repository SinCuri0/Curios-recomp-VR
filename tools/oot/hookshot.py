"""Hands an Ocarina Recomp install over to Hookshot (github.com/NotRexed/Hookshot), the port the README recommends:
the game files Hookshot Builder reads, then the save. It reads this install and writes on this PC only; the game
files and the save never leave it.

    ferramentas\\python\\python.exe -I tools\\oot\\hookshot.py romfs [--out <folder>]
    ferramentas\\python\\python.exe -I tools\\oot\\hookshot.py save [<Hookshot's game folder>] [--yes]

jogar\\hookshot-preparar-jogo.bat and jogar\\hookshot-copiar-save.bat run the two with a double click, and --help lists
the other options. On Linux, .venv/bin/python (any Python from 3.8 on) runs the same two commands.

romfs: Hookshot Builder takes the .nup, or a folder that holds romfs and exefs ("romfs and exefs folders"). This
install keeps the ExeFS as a folder (build\\exefs) but the RomFS packed (build\\romfs, romfs.bin and index.json:
tools/pack_romfs.py), so a player who no longer has the .nup makes that folder here, by default build\\hookshot:
romfs, each file of index.json copied out of romfs.bin, and exefs, a copy of build\\exefs. Hookshot Builder
hard-links the RomFS into its game folder when both are on the same drive and copies it otherwise, so the folder can
be deleted once Hookshot is built.

save: both keep the same files (account.sav, option.sav, slot_NN). The save the next start of this install would load
is copied into the one save folder Hookshot reads: <game folder>\\user\\save\\<user id> on Windows,
$XDG_DATA_HOME/hookshot/save/<user id> on Linux (~/.local/share without the variable). The game folder is the one
given (a folder dropped on the .bat), else the one last chosen in Hookshot Builder, else its default, Games\\Hookshot
in the user folder. When Hookshot already has a save, both dates are shown and the copy waits for a yes (--yes gives
it), as jogar/importar-save.ps1 does since a player imported an old save over newer progress. The copy is made in the
same user folder and compared with the source; only then is the save Hookshot had renamed to
save-antes-de-importar-<date> there, and the copy put in its place. No save is deleted. Hookshot's own
scripts/oot_saves.py (docs/saves.md there) copies saves too, both ways, from a command line.

Standard library only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHUNK = 8 << 20
# The game build Hookshot takes (BUILD_ID in its nx2/make_game.py, v2.0.11 and v2.0.12): the start of main's module id.
HOOKSHOT_BUILD = 'e0f3ccfa'
# Hookshot's one account (user_uid in its host/nn_applet.c, high word first): the only save folder it reads.
HOOKSHOT_USER_ID = '5a3c9e1f0b7d24680000000000000001'


def portuguese():
    if os.environ.get('OOT_LANG'):   # pt or en: the tests fix the language, whatever the Windows UI is
        return os.environ['OOT_LANG'].lower().startswith('pt')
    if os.name == 'nt':
        try:
            import ctypes
            return ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff == 0x16   # LANG_PORTUGUESE
        except (AttributeError, OSError):
            return False
    return (os.environ.get('LC_ALL') or os.environ.get('LANG') or '').lower().startswith('pt')   # (as compilar.sh)


PT = portuguese()


def say(pt, en):
    print(pt if PT else en, flush=True)


def fail(pt, en):
    print(pt if PT else en, file=sys.stderr, flush=True)
    sys.exit(1)


def gb(n):
    """n bytes in GB with one decimal, written as the language does (26,3 in Portuguese)."""
    s = '%.1f' % (n / 1e9)
    return s.replace('.', ',') if PT else s


def overlap(a, b):
    """The two folders are one, or one is inside the other (they need not exist)."""
    a, b = (os.path.normcase(os.path.abspath(str(p))).rstrip(os.sep) for p in (a, b))
    return a == b or a.startswith(b + os.sep) or b.startswith(a + os.sep)


# ---- romfs

def build_id(main):
    """main's module id in hex (offset 0x40 of an NSO header), or None when main is not a plain NSO. Read as Hookshot's
    nx2/make_game.py reads it (THIRD_PARTY_NOTICES.md)."""
    with open(main, 'rb') as f:
        head = f.read(0x60)
    return head[0x40:0x60].hex() if len(head) == 0x60 and head[:4] == b'NSO0' else None


def checked_files(meta, image_size):
    """index.json's files in image order. The index is this image's and records a verified extraction (as
    tools/romfs_index.py asks); each file is inside the image, each path relative and free of '..' and drive letters."""
    if not isinstance(meta, dict) or meta.get('status') != 'pass' or meta.get('romfs_size') != image_size:
        fail('O index.json não é o deste romfs.bin, ou não registra uma extração conferida: a instalação está '
             'incompleta.',
             'index.json is not the one of this romfs.bin, or does not record a verified extraction: the install is '
             'incomplete.')
    files = meta.get('files')
    if not isinstance(files, list) or not files:
        fail('O index.json não tem a lista de arquivos.', 'index.json has no file list.')
    for f in files:
        if not isinstance(f, dict):
            fail('Entrada inválida no index.json: %r' % (f,), 'Invalid entry in index.json: %r' % (f,))
        p, off, size = f.get('path'), f.get('offset'), f.get('size')
        parts = p.split('/') if isinstance(p, str) else ['']
        if (type(off) is not int or type(size) is not int or off < 0 or size < 0 or off + size > image_size
                or any(part in ('', '.', '..') or ':' in part or '\\' in part for part in parts)):
            fail('Entrada inválida no index.json: %r' % (p,), 'Invalid entry in index.json: %r' % (p,))
    return sorted(files, key=lambda f: f['offset'])


def cluster(folder):
    """The allocation unit of the volume folder is on (4096 when it cannot be read): a file takes whole units."""
    try:
        if os.name == 'nt':
            import ctypes
            spc, bps, free, total = (ctypes.c_ulong() for _ in range(4))
            root = os.path.splitdrive(os.path.abspath(str(folder)))[0] + os.sep
            if ctypes.windll.kernel32.GetDiskFreeSpaceW(root, ctypes.byref(spc), ctypes.byref(bps), ctypes.byref(free),
                                                        ctypes.byref(total)):
                return spc.value * bps.value or 4096
        else:
            return os.statvfs(folder).f_frsize or 4096
    except (AttributeError, OSError):
        pass
    return 4096


def unpack(image, files, total, dest):
    """Copies each file of the index out of the image into dest."""
    made, done = set(), 0
    step = max(total // 10, 1)
    shown = step
    with open(image, 'rb', buffering=0) as b:
        for f in files:
            dst = dest.joinpath(*f['path'].split('/'))
            if dst.parent not in made:
                dst.parent.mkdir(parents=True, exist_ok=True)
                made.add(dst.parent)
            b.seek(f['offset'])
            left = f['size']
            with open(dst, 'wb') as o:
                while left:
                    chunk = b.read(min(left, CHUNK))
                    if not chunk:
                        fail('O romfs.bin terminou antes do esperado.', 'romfs.bin ended early.')
                    o.write(chunk)
                    left -= len(chunk)
            done += f['size']
            if done >= shown:
                print('  %3d%%' % (done * 100 // max(total, 1)), flush=True)
                shown += step


def cmd_romfs(a):
    src = Path(a.romfs).resolve() if a.romfs else ROOT / 'build' / 'romfs'
    exefs = Path(a.exefs).resolve() if a.exefs else ROOT / 'build' / 'exefs'
    out = Path(a.out).resolve() if a.out else ROOT / 'build' / 'hookshot'
    image, index = src / 'romfs.bin', src / 'index.json'
    if not image.is_file() or not index.is_file():
        fail('Não achei os arquivos do jogo desta instalação (%s, com romfs.bin e index.json). Se você ainda tiver o '
             '.nup, use-o direto no Hookshot Builder.' % src,
             'This install\'s game files are not here (%s, with romfs.bin and index.json). With the .nup, give it to '
             'Hookshot Builder directly.' % src)
    if not (exefs / 'main').is_file():
        fail('Não achei %s.' % (exefs / 'main'), '%s is missing.' % (exefs / 'main'))
    romfs_out, exefs_out, mark = out / 'romfs', out / 'exefs', out / 'romfs.ok'
    same_exefs = exefs_out.is_dir() and os.path.samefile(exefs, exefs_out)   # (--out is the folder that holds --exefs)
    if (overlap(romfs_out, src) or overlap(romfs_out, exefs) or overlap(exefs_out, src)
            or overlap(exefs_out, exefs) and not same_exefs):
        fail('A pasta de saída (%s) se mistura com as pastas do jogo desta instalação (%s e %s). Escolha outra pasta '
             'com --out.' % (out, src, exefs),
             'The output folder (%s) overlaps this install\'s game folders (%s and %s). Choose another folder with '
             '--out.' % (out, src, exefs))
    bid = build_id(exefs / 'main')
    if bid and not bid.startswith(HOOKSHOT_BUILD):
        say('Aviso: este jogo é a build %s; o Hookshot só aceita a build %s, e o Hookshot Builder vai recusar qualquer '
            'outra.' % (bid[:8], HOOKSHOT_BUILD),
            'Warning: this game is build %s; Hookshot takes build %s, and Hookshot Builder will refuse another.'
            % (bid[:8], HOOKSHOT_BUILD))
    with open(index, encoding='utf-8') as f:
        meta = json.load(f)
    files = checked_files(meta, image.stat().st_size)
    total = sum(f['size'] for f in files)
    stamp = '%s %d %d' % (meta.get('romfs_sha256', '-'), len(files), total)

    def other_files(d):   # (never "delete it" alone: the folder may be a player's own dump)
        fail('A pasta %s já tem outros arquivos. Escolha outra pasta com --out, ou apague essa se ela sobrou de uma '
             'execução anterior deste script.' % d,
             'The folder %s already holds other files. Choose another folder with --out, or delete that one if an '
             'earlier run of this script left it.' % d)

    if exefs_out.is_dir() and any(exefs_out.iterdir()) and not same_exefs and listing(exefs_out) != listing(exefs):
        other_files(exefs_out)
    if romfs_out.is_dir() and any(romfs_out.iterdir()):
        if not mark.is_file() or mark.read_text(encoding='utf-8').strip() != stamp:
            other_files(romfs_out)
        say('Conferindo os arquivos já extraídos...', 'Checking the files unpacked before...')
        for f in files:
            p = romfs_out.joinpath(*f['path'].split('/'))
            if not p.is_file() or p.stat().st_size != f['size']:
                fail('Falta %s, ou o tamanho dele não confere. Apague a pasta %s e rode de novo.' % (p, romfs_out),
                     '%s is missing or has another size. Delete the folder %s and run this again.' % (p, romfs_out))
    else:
        romfs_out.mkdir(parents=True, exist_ok=True)
        unit = cluster(romfs_out)
        # (whole allocation units, and 1 GB more: the exefs, the folders, room to spare)
        need, free = sum(-(-f['size'] // unit) * unit for f in files) + 10 ** 9, shutil.disk_usage(romfs_out).free
        if free < need:
            fail('Espaço livre insuficiente: são necessários %s GB em %s, e só há %s GB.'
                 % (gb(need), out, gb(free)),
                 'Not enough free space: %s GB needed in %s, %s GB free.' % (gb(need), out, gb(free)))
        if mark.exists():
            mark.unlink()
        say('Extraindo %d arquivos (%s GB) para %s...' % (len(files), gb(total), romfs_out),
            'Unpacking %d files (%s GB) into %s...' % (len(files), gb(total), romfs_out))
        t0 = time.time()
        unpack(image, files, total, romfs_out)
        mark.write_text(stamp + '\n', encoding='utf-8')
        say('Pronto em %.0f s.' % (time.time() - t0), 'Done in %.0f s.' % (time.time() - t0))
    if not (exefs_out.is_dir() and any(exefs_out.iterdir())):   # (else it is the same folder, or holds the same files)
        shutil.copytree(exefs, exefs_out, dirs_exist_ok=True)
    say('No Hookshot Builder, escolha "romfs and exefs folders" e selecione esta pasta (ou arraste-a para a janela):\n'
        '  %s\n'
        'Se ele pedir as duas pastas uma de cada vez, escolha romfs e exefs, que estão dentro dela. Depois de compilar '
        'o Hookshot, esta pasta pode ser apagada: o Hookshot guarda os arquivos na pasta dele.' % out,
        'In Hookshot Builder, choose "romfs and exefs folders" and pick this folder (or drop it on the window):\n'
        '  %s\n'
        'If it asks for the two folders one by one, they are romfs and exefs inside it. Once Hookshot is built, the '
        'folder can be deleted: Hookshot keeps its own files.' % out)


# ---- save

def account_folder(profile):
    """The save folder's name: account.uid's 16 bytes, reversed, in hex (as jogar/importar-save.ps1 reads it)."""
    uid = profile / 'account.uid'
    data = uid.read_bytes() if uid.is_file() else b''
    if len(data) != 16:
        fail('Não achei a conta do perfil (%s).' % uid, 'The profile\'s account is missing (%s).' % uid)
    return data[::-1].hex()


def newest_save(profile):
    """The save the next start would load, by the rule of save_recover (runtime/fs_host.c): the highest finished commit
    waiting in save-tx/ready-<n> (n as ready_number reads it: decimal, 1 to 2^32 - 1); else save/<account>; else what an
    interrupted commit left in save-tx, new (an older build's) before old. save-tx/work, the changes no Commit took, is
    deleted at start and does not count."""
    tx = profile / 'save-tx'
    ready = []
    for d in (tx.iterdir() if tx.is_dir() else ()):
        m = re.fullmatch('ready-([0-9]+)', d.name)
        if m and 0 < int(m.group(1)) <= 0xffffffff and d.is_dir() and not d.is_symlink():
            ready.append((int(m.group(1)), d))
    if ready:
        return max(ready)[1]
    committed = profile / 'save' / account_folder(profile)
    return next((d for d in (committed, tx / 'new', tx / 'old') if d.is_dir()), committed)


def is_save(d):
    """A folder that holds account.sav, or a slot folder with a file in it."""
    return (d / 'account.sav').is_file() or any(p.is_file() for slot in d.glob('slot_*') if slot.is_dir()
                                                for p in slot.rglob('*'))


def listing(d):
    """Every file under d: its relative name, size and SHA-256."""
    return sorted((p.relative_to(d).as_posix(), p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest())
                  for p in d.rglob('*') if p.is_file())


def newest_file(d):
    """When the newest file under d was written (0 with no file)."""
    return max((p.stat().st_mtime for p in d.rglob('*') if p.is_file()), default=0)


def running(names):
    """Those of these programs that run now, or None when that cannot be seen: tasklist on Windows (called as Hookshot's
    scripts/oot_saves.py calls it: THIRD_PARTY_NOTICES.md), /proc/<pid>/comm on Linux."""
    seen = set()
    try:
        if os.name == 'nt':
            r = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True, errors='replace')
            seen = {line.split('","')[0].strip('"').lower() for line in r.stdout.splitlines()}
        else:
            for comm in Path('/proc').glob('[0-9]*/comm'):
                try:
                    seen.add(comm.read_text(errors='replace').strip().lower())
                except OSError:   # (that process ended meanwhile)
                    pass
    except OSError:   # (no tasklist)
        pass
    if not seen:
        return None
    return sorted(n for n in seen if (n[:-4] if n.endswith('.exe') else n) in names)


def builder_folder():
    """The game folder last chosen in Hookshot Builder's window (Out in its builder-settings.json, v2.0.12), or None."""
    try:
        path = Path(os.environ['LOCALAPPDATA']) / 'HookshotBuilder' / 'builder-settings.json'
        out = json.loads(path.read_text(encoding='utf-8-sig')).get('Out')
        return Path(out) if isinstance(out, str) and out.strip() else None
    except (KeyError, OSError, ValueError, AttributeError):
        return None


def is_game(folder):
    """Hookshot's game folder: image.bin, with the launcher's bin folder or, in a build without the launcher, the game
    as hookshot.exe."""
    return (folder / 'image.bin').is_file() and ((folder / 'bin').is_dir() or (folder / 'hookshot.exe').is_file())


def hookshot_user(a):
    """Hookshot's user folder: --user; on Windows the user folder of the game folder given, else of the one last chosen
    in Hookshot Builder, else of its default one; elsewhere $XDG_DATA_HOME/hookshot (~/.local/share/hookshot), wherever
    the game is."""
    if a.user:
        return Path(a.user).resolve()
    if os.name != 'nt':
        xdg = os.environ.get('XDG_DATA_HOME') or ''   # (as Hookshot's host/hos_main.c: a relative one is not used)
        return (Path(xdg) if xdg.startswith('/') else Path.home() / '.local' / 'share') / 'hookshot'
    if a.hookshot or a.folder:
        game = Path(a.hookshot or a.folder).resolve()
    else:
        game, chosen = Path(os.environ.get('USERPROFILE') or Path.home()) / 'Games' / 'Hookshot', builder_folder()
        if chosen and is_game(chosen):
            game = chosen
    if not is_game(game):
        fail('Não achei o jogo do Hookshot em %s. Compile-o primeiro com o Hookshot Builder. Se ele está em outra pasta, '
             'arraste essa pasta para cima do hookshot-copiar-save.bat, ou indique-a com --hookshot.' % game,
             'Hookshot\'s game is not in %s. Build it first with Hookshot Builder. If it is in another folder, drop that '
             'folder on hookshot-copiar-save.bat, or give it with --hookshot.' % game)
    return game / 'user'


def confirmed(a, src, dest):
    """Hookshot already has a save: both dates are shown, and it is replaced only after a yes (--yes gives it). Enter
    alone is a no when Hookshot's save is the newer one, and where nothing can be asked nothing is replaced. As
    jogar/importar-save.ps1 since a player imported an old save over newer progress."""
    mine, theirs = newest_file(src), newest_file(dest)
    when = lambda t: time.strftime('%d/%m/%Y %H:%M' if PT else '%Y-%m-%d %H:%M', time.localtime(t))
    say('O Hookshot já tem um save, de %s. O desta instalação é de %s.' % (when(theirs), when(mine)),
        'Hookshot already has a save, from %s. This install\'s is from %s.' % (when(theirs), when(mine)))
    if theirs > mine:
        say('ATENÇÃO: o save do Hookshot é MAIS NOVO. Copiar troca esse progresso pelo mais antigo.',
            'WARNING: Hookshot\'s save is NEWER. Copying replaces that progress with the older one.')
    if a.yes:
        return True
    try:
        answer = input(('Substituir o save do Hookshot? O atual fica guardado em save-antes-de-importar-<data>. [%s] '
                        if PT else
                        'Replace Hookshot\'s save? The current one is kept in save-antes-de-importar-<date>. [%s] ')
                       % (('s/N' if PT else 'y/N') if theirs > mine else ('S/n' if PT else 'Y/n'))).strip().lower()
    except EOFError:
        fail('Não dá para perguntar aqui, então nada foi alterado. Para copiar assim mesmo, rode de novo com --yes.',
             'Nothing can be asked here, so nothing was changed. To copy all the same, run this again with --yes.')
    if answer in ('s', 'sim', 'y', 'yes') or not answer and theirs <= mine:
        return True
    say('Nada foi alterado.', 'Nothing was changed.')
    return False


def cmd_save(a):
    profile = Path(a.profile).resolve() if a.profile else ROOT / 'perfil'
    src = newest_save(profile)
    if not is_save(src):
        fail('Não achei nenhum save em %s.' % src, 'No save in %s.' % src)
    busy = running(('hookshot', 'ocarina'))
    if busy:
        fail('Feche o jogo (%s) e tente de novo.' % ', '.join(busy), 'Close the game (%s) and try again.' % ', '.join(busy))
    if busy is None:
        say('Aviso: não consegui ver se o jogo está aberto. Feche-o antes de continuar.',
            'Warning: could not see whether the game is running. Close it before going on.')
    user = hookshot_user(a)
    if overlap(user, profile):
        fail('A pasta do Hookshot (%s) e o perfil desta instalação (%s) não podem ficar uma dentro da outra.'
             % (user, profile),
             'Hookshot\'s folder (%s) and this install\'s profile (%s) cannot be one inside the other.' % (user, profile))
    dest, tmp = user / 'save' / HOOKSHOT_USER_ID, user / 'save-importando'
    if dest.is_dir() and is_save(dest) and not confirmed(a, src, dest):
        return
    try:
        if tmp.exists():   # (left by a run that failed)
            shutil.rmtree(tmp)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, tmp)
    except OSError as e:
        fail('Não consegui copiar o save para %s (%s). O save do Hookshot não foi alterado.' % (tmp, e),
             'Could not copy the save into %s (%s). Hookshot\'s save was not changed.' % (tmp, e))
    if listing(tmp) != listing(src):
        fail('A cópia em %s não confere com o save original. O save do Hookshot não foi alterado.' % tmp,
             'The copy in %s differs from the save. Hookshot\'s save was not changed.' % tmp)
    kept = None
    try:
        if dest.exists():
            stamp = 'save-antes-de-importar-' + time.strftime('%Y%m%d-%H%M%S')
            aside, n = user / stamp, 1
            while aside.exists():   # (two copies in the same second: numbered as Hookshot's scripts/oot_saves.py does)
                n += 1
                aside = user / ('%s-%d' % (stamp, n))
            dest.rename(aside)
            kept = aside
        tmp.rename(dest)
    except OSError as e:
        if kept and not dest.exists():
            try:
                kept.rename(dest)   # Hookshot's save goes back
                kept = None
            except OSError:
                pass
        fail('Não consegui colocar o save no lugar (%s). A cópia ficou em %s%s' % (
                 e, tmp, ' e o save que o Hookshot tinha em %s.' % kept if kept else '; o save do Hookshot não foi alterado.'),
             'Could not put the save in place (%s). The copy is in %s%s' % (
                 e, tmp, ', and Hookshot\'s previous save in %s.' % kept if kept else '; Hookshot\'s save was not changed.'))
    say('Save copiado para o Hookshot:\n  %s' % dest, 'Save copied to Hookshot:\n  %s' % dest)
    if kept:
        say('O save anterior do Hookshot ficou em:\n  %s\nPara voltar a ele, feche o Hookshot e ponha essa pasta no '
            'lugar da de cima.' % kept,
            'Hookshot\'s previous save is in:\n  %s\nTo go back to it, close Hookshot and put that folder in the place '
            'of the one above.' % kept)


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
        sys.stderr.reconfigure(errors='replace')
    if sys.version_info < (3, 8):
        fail('Este script precisa do Python 3.8 ou mais novo.', 'This script needs Python 3.8 or newer.')
    ap = argparse.ArgumentParser(description='Hands this install over to Hookshot: the game files Hookshot Builder '
                                             'reads (romfs), then the save (save).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('romfs', help='make the folder with romfs and exefs that Hookshot Builder takes')
    r.add_argument('--out', help='the folder to make (default build/hookshot): it gets romfs, exefs and romfs.ok')
    r.add_argument('--romfs', help='the packed RomFS folder, with romfs.bin and index.json (default build/romfs)')
    r.add_argument('--exefs', help='the ExeFS folder (default build/exefs)')
    s = sub.add_parser('save', help="copy this install's save into Hookshot's save folder")
    where = s.add_mutually_exclusive_group()
    where.add_argument('folder', nargs='?', help="Hookshot's game folder, on Windows (default: the one last chosen in "
                                                 "Hookshot Builder, else Games\\Hookshot in the user folder)")
    where.add_argument('--hookshot', help='the same folder, by name')
    where.add_argument('--user', help="Hookshot's user folder itself (the game's --user)")
    s.add_argument('--profile', help="this install's profile (default perfil)")
    s.add_argument('--yes', action='store_true', help='replace the save Hookshot already has without asking')
    a = ap.parse_args()
    try:
        (cmd_romfs if a.cmd == 'romfs' else cmd_save)(a)
    except KeyboardInterrupt:
        fail('Interrompido.', 'Interrupted.')
    except (OSError, ValueError) as e:
        fail('Não consegui terminar: %s' % e, 'Could not finish: %s' % e)


if __name__ == '__main__':
    main()
