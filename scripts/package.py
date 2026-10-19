from pathlib import Path
import hashlib, zipfile
root = Path(__file__).resolve().parents[1]
paths = {
    'AimpCompanionControl/AimpCompanionControl.dll': root/'dist/AimpCompanionControl-i686.dll',
    'AimpCompanionControl/x64/AimpCompanionControl.dll': root/'dist/AimpCompanionControl-x86_64.dll',
}
for arch in ('', 'x64/'):
    paths[f'AimpCompanionControl/{arch}Langs/english.lng'] = root/'Langs/english.lng'
    paths[f'AimpCompanionControl/{arch}THIRD-PARTY-NOTICES.txt'] = root/'THIRD-PARTY-NOTICES.txt'
output = root/'dist/AimpCompanionControl-0.3.1-preview.aimppack'
with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name, path in sorted(paths.items()):
        info = zipfile.ZipInfo(name, date_time=(2026,10,19,0,0,0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        z.writestr(info, path.read_bytes())
notices = root/'dist/THIRD-PARTY-NOTICES.txt'
notices.write_bytes((root/'THIRD-PARTY-NOTICES.txt').read_bytes())
files = sorted((root/'dist').glob('*.dll')) + [output, notices]
(root/'dist/SHA256SUMS.txt').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files), encoding='utf-8', newline='\n')
print((root/'dist/SHA256SUMS.txt').read_text())
