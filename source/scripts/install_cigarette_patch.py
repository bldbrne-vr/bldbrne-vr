"""Apply a staged Kos Lights patch with preflight hashes and recoverable backups.
Never touches saves. Run with the emulator closed. Default is a dry run.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def contained(root, relative):
    # Restrict the manifest to the three native resource types this patch owns.
    name = relative.replace('\\', '/')
    if not (name == 'param/gameparam/gameparam.parambnd.dcx' or
            name == 'menu/menu_knowledge_09000.tpf.dcx' or
            re.fullmatch(r'msg/[A-Za-z0-9_-]+/item\.msgbnd\.dcx', name)):
        raise ValueError(f'Unexpected patch path: {relative}')
    path = root / name
    if root not in path.resolve().parents or path.is_symlink():
        raise ValueError(f'Path escapes root: {relative}')
    return path


def atomic_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.kos-lights-', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            with source.open('rb') as src:
                shutil.copyfileobj(src, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, target)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def install(stage, game, backup, apply=False):
    stage, game, backup = (p.resolve() for p in (stage, game, backup))
    if stage == game or stage in game.parents or game in stage.parents:
        raise ValueError('Staging and game directories must be separate')
    if backup.exists() or any(backup == p or p in backup.parents or backup in p.parents
                              for p in (stage, game)):
        raise ValueError('Backup must be a new, separate directory')
    manifest = json.loads((stage/'manifest.json').read_text())
    required = {'param/gameparam/gameparam.parambnd.dcx', 'menu/menu_knowledge_09000.tpf.dcx'}
    if not required <= {k.replace('\\', '/') for k in manifest}:
        raise ValueError('Incomplete patch manifest')
    entries = []
    for relative, hashes in manifest.items():
        src, dst = contained(stage, relative), contained(game, relative)
        before, after = hashes['source_sha256'], hashes['patched_sha256']
        if not isinstance(after, str) or not re.fullmatch('[0-9a-f]{64}', after):
            raise ValueError('Invalid patched hash')
        if before is None and relative.replace('\\', '/') != 'menu/menu_knowledge_09000.tpf.dcx':
            raise ValueError('Only the new icon may be created')
        if not src.is_file() or digest(src) != after or digest(dst) != before:
            raise ValueError(f'Hash mismatch: {relative}; rebuild against this exact installation')
        if dst.exists() and not dst.is_file():
            raise ValueError(f'Not a regular file: {relative}')
        entries.append((relative, src, dst, before, after))
    if not apply:
        return len(entries)
    # Complete the recoverable backup before changing any game resource.
    backup.mkdir(parents=True)
    for relative, _, dst, before, _ in entries:
        if before is not None:
            saved = contained(backup, relative)
            atomic_copy(dst, saved)
            if digest(saved) != before:
                raise ValueError(f'Backup verification failed: {relative}')
    (backup/'manifest.json').write_text(json.dumps(manifest, indent=2))
    written = []
    try:
        for relative, src, dst, before, after in entries:
            if digest(dst) != before or digest(src) != after:
                raise ValueError(f'File changed after preflight: {relative}')
            atomic_copy(src, dst)
            written.append((relative, dst, before, after))
            if digest(dst) != after:
                raise ValueError(f'Write verification failed: {relative}')
    except Exception:
        for relative, dst, before, after in reversed(written):
            if digest(dst) != after:
                raise RuntimeError(f'Concurrent change detected; restore manually from {backup}')
            if before is None:
                dst.unlink()  # Only the explicitly allowlisted, newly created icon.
            else:
                atomic_copy(contained(backup, relative), dst)
        raise
    return len(entries)


def restore(game, backup, apply=False):
    """Restore only our unchanged installed resources; safe to resume after interruption."""
    game, backup = game.resolve(), backup.resolve()
    manifest = json.loads((backup/'manifest.json').read_text())
    actions = []
    for relative, hashes in manifest.items():
        dst, src = contained(game, relative), contained(backup, relative)
        before, after = hashes['source_sha256'], hashes['patched_sha256']
        if before is None and relative.replace('\\','/') != 'menu/menu_knowledge_09000.tpf.dcx':
            raise ValueError('Only the new icon may be removed')
        if before is not None and digest(src) != before:
            raise ValueError(f'Invalid backup: {relative}')
        current = digest(dst)
        if current == before:
            continue
        if current != after:
            raise ValueError(f'Resource changed since install: {relative}; refusing overwrite')
        actions.append((src,dst,before,after))
    if apply:
        for src,dst,before,after in actions:
            if digest(dst) != after:
                raise ValueError('Resource changed after preflight')
            if before is None:
                dst.unlink()
            else:
                atomic_copy(src,dst)
    return len(actions)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('game', type=Path)
    parser.add_argument('backup', type=Path)
    parser.add_argument('--apply', action='store_true', help='Apply after all checks; otherwise dry run')
    parser.add_argument('--restore', action='store_true', help='Restore verified backups instead of installing')
    args = parser.parse_args()
    count = restore(args.game, args.backup, args.apply) if args.restore else install(args.stage, args.game, args.backup, args.apply)
    print(f'{("Restored" if args.restore else "Installed") if args.apply else "Validated"} {count} resources. Saves untouched.')
