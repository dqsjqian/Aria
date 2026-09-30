"""Official non-GitHub providers for the shared dependency resolver."""
import csv
import hashlib
import re
from urllib.parse import urljoin


def resolve_sqlite(spec, requested, context):
    page_url = 'https://www.sqlite.org/download.html'
    rows = csv.reader(context.get_text(page_url).splitlines())
    products = [row for row in rows if len(row) >= 5 and row[0] == 'PRODUCT'
                and re.fullmatch(r'\d{4}/sqlite-amalgamation-\d+\.zip', row[2])
                and re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?', row[1])]
    chosen = next((row for row in products if row[1] == requested), None)
    if requested == 'latest':
        if not products:
            raise ValueError('SQLite download page contains no stable amalgamation')
        chosen = max(products, key=lambda row: tuple(map(int, row[1].split('.'))))
    if chosen:
        version, relative, expected_sha3 = chosen[1], chosen[2], chosen[4]
        url = urljoin(page_url, relative)
        data = context.get_bytes(url)
        if hashlib.sha3_256(data).hexdigest() != expected_sha3:
            raise ValueError('SQLite official SHA3-256 mismatch')
        digest = context.download_digest(url, hashlib.sha256(data).hexdigest())
        checksum_source = 'sqlite.org download product CSV; SHA3-256 verified'
    else:
        if not re.fullmatch(r'3\.\d{1,2}\.\d{1,2}(?:\.\d{1,2})?', requested):
            raise ValueError('SQLite version must be a released 3.x.y[.z] version')
        version = requested
        release_url = 'https://www.sqlite.org/releaselog/' + version.replace('.', '_') + '.html'
        release = context.get_text(release_url)
        date = re.search(r'SQLite Release\s+' + re.escape(version) + r'\s+On\s+(\d{4})-\d{2}-\d{2}', release)
        if not date:
            raise ValueError('Cannot determine official SQLite release year: ' + version)
        parts = [int(part) for part in version.split('.')]
        parts += [0] * (4 - len(parts))
        number = str(parts[0]) + ''.join(f'{part:02}' for part in parts[1:])
        url = f'https://www.sqlite.org/{date.group(1)}/sqlite-amalgamation-{number}.zip'
        digest = context.download_digest(url)
        checksum_source = 'SHA256 computed from official sqlite.org release archive'
    return {'version': version, 'tag': version, 'revision': '', 'url': url,
            'sha256': digest, 'checksum_source': checksum_source}


def resolve_quickjs(spec, requested, context):
    base = 'https://bellard.org/quickjs/'
    if requested == 'latest':
        versions = re.findall(r'href=[\"\']quickjs-(\d{4}-\d{2}-\d{2})\.tar\.xz[\"\']', context.get_text(base))
        if not versions:
            raise ValueError('QuickJS page contains no stable source release')
        version = max(versions)
    else:
        if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', requested):
            raise ValueError('QuickJS version must be an official YYYY-MM-DD release')
        version = requested
    url = f'{base}quickjs-{version}.tar.xz'
    return {'version': version, 'tag': version, 'revision': '', 'url': url,
            'sha256': context.download_digest(url),
            'checksum_source': 'SHA256 computed from official bellard.org release archive'}
