#!/usr/bin/env python3
"""Align and sign a release APK. Private signing files never enter the repository."""
import argparse
import hashlib
import os
from pathlib import Path
import secrets
import subprocess


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('unsigned_apk', type=Path)
    parser.add_argument('output_apk', type=Path)
    parser.add_argument('--create-keystore', action='store_true')
    parser.add_argument('--signing-dir', type=Path, default=Path.home() / '.publics-trading-signing')
    args = parser.parse_args()
    java_home = Path(os.environ['JAVA_HOME'])
    sdk = Path(os.environ.get('ANDROID_SDK_ROOT', str(Path.home() / 'Library/Android/sdk')))
    tools = sdk / 'build-tools/36.0.0'
    signing = args.signing_dir
    key = signing / 'publics-trading-release.p12'
    password = signing / 'keystore-password.txt'
    alias = 'publics-trading-release'
    os.umask(0o077)
    if args.create_keystore:
        signing.mkdir(mode=0o700, parents=True, exist_ok=True)
        if key.exists() or password.exists():
            raise SystemExit('Refusing to overwrite existing signing material. Omit --create-keystore to reuse it.')
        with password.open('x') as file:
            file.write(secrets.token_urlsafe(48) + '\n')
        password.chmod(0o600)
        run(java_home / 'bin/keytool', '-genkeypair', '-keystore', key,
            '-storetype', 'PKCS12', '-storepass:file', password,
            '-keypass:file', password, '-alias', alias, '-keyalg', 'RSA',
            '-keysize', '3072', '-sigalg', 'SHA256withRSA', '-validity', '10000',
            '-dname', 'CN=Publics Trading Interface', '-noprompt')
        key.chmod(0o600)
    if not key.is_file() or not password.is_file():
        raise SystemExit('Signing material missing; use --create-keystore on the first run.')
    args.output_apk.parent.mkdir(parents=True, exist_ok=True)
    aligned = args.output_apk.with_suffix('.aligned-unsigned.apk')
    run(tools / 'zipalign', '-P', '16', '-f', '4', args.unsigned_apk, aligned)
    run(tools / 'apksigner', 'sign', '--ks', key, '--ks-key-alias', alias,
        '--ks-pass', 'file:' + str(password),
        '--v1-signing-enabled', 'false', '--v2-signing-enabled', 'true',
        '--v3-signing-enabled', 'true', '--out', args.output_apk, aligned)
    run(tools / 'apksigner', 'verify', '--verbose', '--print-certs', args.output_apk)
    run(tools / 'zipalign', '-c', '-P', '16', '4', args.output_apk)
    aligned.unlink()
    checksum = hashlib.sha256(args.output_apk.read_bytes()).hexdigest()
    args.output_apk.with_suffix('.apk.sha256').write_text(checksum + '  ' + args.output_apk.name + '\n')
    print('Signed APK:', args.output_apk)
    print('Keystore:', key)
    print('Password file (private, not printed):', password)
    print('SHA-256:', checksum)


if __name__ == '__main__':
    main()
