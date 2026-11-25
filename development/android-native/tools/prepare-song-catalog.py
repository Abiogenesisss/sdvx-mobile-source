"""Export every NABLA song and its original jackets, without music or charts.

Existing bundled song folders stay stable so saved records keep their keys.
The original medium jacket is 350px, matching MUSIC SELECT's authored slot;
bundled playable songs retain their 676px NEXT AREA artwork.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import unicodedata
from xml.etree import ElementTree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('bundled', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    database = ElementTree.fromstring((args.game / 'others/music_db.xml').read_bytes().decode('cp932'))
    bundled = {entry['id']: entry for entry in json.loads((args.bundled / 'library.json').read_text(encoding='utf-8'))}
    sources = {int(p.name.split('_', 1)[0]): p for p in (args.game / 'music').iterdir() if p.is_dir()}
    labels = {1: 'INF', 2: 'INF', 3: 'GRV', 4: 'HVN', 5: 'VVD', 6: 'XCD', 7: 'ULT', 8: 'NBL'}
    catalog, details, missing, empty = [], {}, [], []
    args.output.mkdir(parents=True, exist_ok=True)
    for entry in database:
        song_id = int(entry.attrib['id'])
        info = entry.find('info')
        source = sources.get(song_id)
        old = bundled.get(song_id)
        folder = old['folder'] if old else str(song_id)
        song = dict(id=song_id, folder=folder, title=info.findtext('title_name', ''),
                    artist=info.findtext('artist_name', ''), version=int(info.findtext('version')),
                    bpm_min=int(info.findtext('bpm_min')), bpm_max=int(info.findtext('bpm_max')),
                    background=int(info.findtext('bg_no')), charts=[])
        if old and 'music' in old:
            song['music'] = old['music']
        chart_details, jackets = {}, {}
        for row in entry.findall('difficulty/*'):
            level = int(row.findtext('difnum', '0'))
            if level == 0:
                continue
            slot = {'novice': 1, 'advanced': 2, 'exhaust': 3, 'maximum': 5, 'ultimate': 6}.get(row.tag, 4)
            label = {1: 'NOV', 2: 'ADV', 3: 'EXH', 5: 'MXM', 6: 'ULT'}.get(slot)
            if slot == 4:
                label = labels[int(info.findtext('inf_ver'))]
            category = {'INF': 'infinite', 'GRV': 'gravity', 'HVN': 'heavenly', 'VVD': 'vivid',
                        'XCD': 'exceed', 'ULT': 'ultimate', 'NBL': 'nabla'}.get(label, row.tag)
            saved = next((c for c in old['charts'] if c['label'] == label), None) if old else None
            if saved:
                chart, jacket = saved['chart'], saved['jacket']
            else:
                chart = category + '.json'
                image = source / f'jk_{song_id:04d}_{slot}.png' if source else None
                if image and not image.is_file():
                    image = source / f'jk_{song_id:04d}_1.png'
                if not image or not image.is_file():
                    missing.append([song_id, category])
                    continue
                digest = hashlib.sha256(image.read_bytes()).hexdigest()
                jacket = jackets.get(digest)
                if jacket is None:
                    jacket = f'jacket-{slot}.png'
                    jackets[digest] = jacket
                    target = args.output / folder / jacket
                    target.parent.mkdir(exist_ok=True)
                    shutil.copyfile(image, target)
            song['charts'].append(dict(name=category.upper(), label=label, level=level, chart=chart,
                jacket=jacket, effect=row.findtext('effected_by', ''), illustrator=row.findtext('illustrator', ''),
                bundled=saved is not None or song_id == 1438))
            if saved and 'music' in saved:
                song['charts'][-1]['music'] = saved['music']
            chart_details[category] = {'radar': {axis.tag: int(axis.text) for axis in row.findall('radar/*')}}
        if song['charts']:
            text = '\n'.join([song['title'], song['artist'], info.findtext('title_yomigana', ''),
                info.findtext('artist_yomigana', ''), *(c['effect'] + '\n' + c['illustrator'] for c in song['charts'])])
            text = unicodedata.normalize('NFKC', text).lower()
            song['search_key'] = ''.join(chr(ord(c) + 0x60) if '\u3041' <= c <= '\u3096' else c for c in text)
            catalog.append(song)
            details[str(song_id)] = {'charts': chart_details}
        else:
            empty.append(song_id)
    if missing:
        raise RuntimeError(f'Original jacket missing: {missing}')
    for name, data in [('library.json', catalog), ('details.json', details)]:
        (args.output / name).write_text(json.dumps(data, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    report = dict(source='2026101300/LAZY_KFC/contents/data/others/music_db.xml',
                  database_entries=len(database), songs=len(catalog), charts=sum(len(s['charts']) for s in catalog),
                  empty_entries=empty, bundled_songs=sorted(bundled), missing_jackets=missing)
    (args.output / 'catalog-audit.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
