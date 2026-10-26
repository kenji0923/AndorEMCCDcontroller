#!/usr/bin/env python3
"""Install a GitHub GUI release on Windows PCs reached through OpenSSH."""

import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from zipfile import ZipFile

REPOSITORY = "kenji0923/AndorEMCCDcontroller"
PACKAGE = "andor_emccd_gui-windows-x64"
ASSET = PACKAGE + ".zip"
EXECUTABLE = "andor_emccd_gui.exe"

INSPECT = r'''
$target = Join-Path $p.root $p.folder
$running = @(Get-CimInstance Win32_Process -Filter "Name = 'andor_emccd_gui.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase) } |
    Select-Object -ExpandProperty ProcessId)
$manifest = $null
if (Test-Path -LiteralPath (Join-Path $target 'release-install.json')) {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $target 'release-install.json') | ConvertFrom-Json
}
$exeHash = $null
if (Test-Path -LiteralPath (Join-Path $target 'andor_emccd_gui.exe')) {
    $exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $target 'andor_emccd_gui.exe')).Hash.ToLowerInvariant()
}
@{hostname=$env:COMPUTERNAME;target=$target;running=$running;manifest=$manifest;executable_sha256=$exeHash} | ConvertTo-Json -Compress -Depth 8
'''
PREPARE = r'''
$stage = Join-Path ([IO.Path]::GetTempPath()) ('aec-release-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
@{stage=$stage} | ConvertTo-Json -Compress
'''
INSTALL = r'''
$target = Join-Path $p.root $p.folder
$zip = Join-Path $p.stage 'release.zip'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant() -ne $p.sha256) { throw 'Archive checksum mismatch' }
$expanded = Join-Path $p.stage 'expanded'
Expand-Archive -LiteralPath $zip -DestinationPath $expanded
$candidate = Join-Path $expanded $p.package
$exe = Join-Path $candidate 'andor_emccd_gui.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'GUI executable missing from archive' }
$check = Start-Process -FilePath $exe -ArgumentList '--version' -Wait -PassThru -RedirectStandardOutput (Join-Path $p.stage 'version.txt') -RedirectStandardError (Join-Path $p.stage 'error.txt')
if ($check.ExitCode -ne 0) { throw "GUI startup check failed: $($check.ExitCode)" }
New-Item -ItemType Directory -Force -Path $p.root | Out-Null
if (Test-Path -LiteralPath $target) {
    $item = Get-Item -LiteralPath $target
    if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Destination must be an ordinary directory' }
}
$manifest = @{repository=$p.repository;tag=$p.tag;release_url=$p.url;asset=$p.asset;archive_sha256=$p.sha256;installed_utc=[DateTime]::UtcNow.ToString('o');executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()}
$manifest | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $candidate 'release-install.json')
$incoming = Join-Path $p.root ('.aec-stage-' + [Guid]::NewGuid().ToString('N'))
$backup = $null
try {
    # Prepare on the target volume before touching the installed directory.
    Move-Item -LiteralPath $candidate -Destination $incoming
    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'andor_emccd_gui.exe'" |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase) })
    if ($running.Count) { throw 'Destination GUI is running; close it or use --versioned' }
    if (Test-Path -LiteralPath $target) {
        $backup = $target + '.backup-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
        Move-Item -LiteralPath $target -Destination $backup
    }
    try { Move-Item -LiteralPath $incoming -Destination $target }
    catch {
        if ($backup -and !(Test-Path -LiteralPath $target)) { Move-Item -LiteralPath $backup -Destination $target }
        throw
    }
    # Keep the old installation only until the replacement succeeds.
    if ($backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
} finally {
    if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
}
@{hostname=$env:COMPUTERNAME;target=$target;tag=$p.tag;version=(Get-Content -Raw (Join-Path $p.stage 'version.txt')).Trim()} | ConvertTo-Json -Compress
'''
CLEANUP = r'''Remove-Item -LiteralPath $p.stage -Recurse -Force'''


def run(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or
                           f"Command failed: {command[0]}")
    return result.stdout.strip().lstrip("\ufeff")


def powershell_command(script, payload):
    data = base64.b64encode(json.dumps(payload).encode()).decode()
    source = ("$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue';\n"
              "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n"
              "$p = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
              data + "')) | ConvertFrom-Json\n" + script)
    return base64.b64encode(source.encode("utf-16le")).decode()


def validate_archive(path):
    with ZipFile(path) as archive:
        names = []
        for item in archive.infolist():
            name = PurePosixPath(item.filename.replace('\\', '/'))
            if (name.is_absolute() or '..' in name.parts or
                    not name.parts or name.parts[0] != PACKAGE or
                    any(':' in part for part in name.parts) or
                    stat.S_ISLNK(item.external_attr >> 16)):
                raise ValueError(f"Unsafe archive entry: {item.filename}")
            names.append(str(name))
        if f"{PACKAGE}/{EXECUTABLE}" not in names:
            raise ValueError("Release archive does not contain the Windows GUI")
        corrupt = archive.testzip()
        if corrupt:
            raise ValueError(f"Corrupt archive entry: {corrupt}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('hosts', nargs='*', help='SSH aliases (default: micro0 gen0)')
    parser.add_argument('--tag', default='latest', help='Release tag (default: latest stable release)')
    parser.add_argument('--root', default=r'C:\local', help=r'Windows destination root (default: C:\local)')
    parser.add_argument('--versioned', action='store_true', help='Append the release tag to the installed folder name')
    parser.add_argument('--dry-run', action='store_true', help='Resolve release and inspect destinations without writing files')
    default_config = Path.home() / '.ssh/config'
    parser.add_argument('--ssh-config', type=Path, default=default_config if default_config.exists() else None)
    args = parser.parse_args(argv)
    hosts = args.hosts or ['micro0', 'gen0']
    for host in hosts:
        if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.@:-]*', host):
            parser.error(f'Invalid SSH host: {host!r}')
    if not re.fullmatch(r'[A-Za-z]:\\.+', args.root):
        parser.error('--root must be an absolute Windows drive path')
    for program in ['gh', 'ssh', 'scp']:
        if not shutil.which(program):
            parser.error(f'{program} is required')
    gh = ['gh', 'release', 'view']
    if args.tag != 'latest':
        gh.append(args.tag)
    release = json.loads(run(gh + ['--repo', REPOSITORY, '--json', 'tagName,url,assets,isDraft']))
    tag = release['tagName']
    if release['isDraft'] or not re.fullmatch(r'[A-Za-z0-9_.-]+', tag):
        raise ValueError('Release must be published and its tag must be filename-safe')
    if not any(asset['name'] == ASSET for asset in release['assets']):
        raise ValueError(f'{tag} has no {ASSET}')
    folder = PACKAGE + ('-' + tag if args.versioned else '')
    payload = dict(repository=REPOSITORY, tag=tag, url=release['url'], asset=ASSET,
                   package=PACKAGE, root=args.root, folder=folder)
    options = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15']
    if args.ssh_config:
        options += ['-F', str(args.ssh_config)]

    def remote(host, script, data, decode=True):
        output = run(['ssh'] + options + [host, 'powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + powershell_command(script, data)])
        return json.loads(output) if decode else output

    print(f"Release: {tag} ({release['url']})", flush=True)
    failures = []
    with tempfile.TemporaryDirectory(prefix='aec-release-') as temporary:
        archive = Path(temporary) / ASSET
        installer = Path(temporary) / 'install.ps1'
        installer.write_text(
            "param([string]$PayloadBase64)\n"
            "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'\n"
            "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n"
            "$p = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($PayloadBase64)) | ConvertFrom-Json\n"
            + INSTALL, encoding='utf-8')
        if not args.dry_run:
            run(['gh', 'release', 'download', tag, '--repo', REPOSITORY,
                 '--pattern', ASSET, '--dir', temporary])
            validate_archive(archive)
            payload['sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        for host in hosts:
            stage = None
            try:
                before = remote(host, INSPECT, payload)
                print(f"{host} [{before['hostname']}]: {before['target']}", flush=True)
                manifest = before.get('manifest') or {}
                if (not args.dry_run and manifest.get('archive_sha256') == payload['sha256']
                        and manifest.get('tag') == tag
                        and before.get('executable_sha256')
                        and before['executable_sha256'] == manifest.get('executable_sha256')):
                    print('  Already installed; unchanged.', flush=True)
                    continue
                if before['running']:
                    raise RuntimeError('Destination GUI is running; close it or use --versioned')
                if args.dry_run:
                    print('  Would install here. No files changed.', flush=True)
                    continue
                stage = remote(host, PREPARE, payload)['stage']
                data = dict(payload, stage=stage)
                destination = host + ':' + stage.replace('\\', '/') + '/release.zip'
                run(['scp'] + options + [str(archive), destination])
                run(['scp'] + options + [str(installer), host + ':' +
                    stage.replace('\\', '/') + '/install.ps1'])
                encoded = base64.b64encode(json.dumps(data).encode()).decode()
                installed = remote(host,
                    "& (Join-Path $p.stage 'install.ps1') -PayloadBase64 $p.data",
                    dict(stage=stage, data=encoded))
                print(f"  Installed {installed['tag']}: {installed['target']}", flush=True)
            except (RuntimeError, ValueError, KeyError) as error:
                failures.append(host)
                print(f'{host}: ERROR: {error}', file=sys.stderr, flush=True)
            finally:
                if stage:
                    try:
                        remote(host, CLEANUP, dict(payload, stage=stage), decode=False)
                    except RuntimeError as error:
                        print(f'{host}: could not remove staging folder {stage}: {error}', file=sys.stderr)
    return int(bool(failures))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, ValueError) as error:
        sys.exit(str(error))
