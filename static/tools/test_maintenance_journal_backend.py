#!/usr/bin/env python3
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='maintenance-journal-backend-') as d:
    exe = pathlib.Path(d) / 'test.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    f'-I{root / "drivers" / "include"}',
                    f'-I{root / "services" / "include"}',
                    str(root / 'drivers/src/maintenance_journal_backend.c'),
                    str(root / 'services/src/maintenance_journal_codec.c'),
                    str(root / 'static/tests/maintenance_journal_backend/maintenance_journal_backend_test.c'),
                    '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('maintenance journal backend post-state classifier: PASS')
