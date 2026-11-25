"""Export selection metadata without touching jackets, music, charts or saved records."""
import argparse
import json
import re
import unicodedata
from pathlib import Path
from xml.etree import ElementTree


# Tag IDs 1–45 from NABLA 2026101300: 1802937f0 names, 180290170 filters.
FLOOR_TITLE_RANGES = (
    (0x8FD252, 70), (0x8FCF4A, 29), (0x909E40, 42), (0x8FCFCA, 27),
    (0x909E70, 38), (0x8FD04A, 34), (0x8FD08A, 31), (0x8FD0D2, 47),
    (0x909DA0, 70), (0x909DF0, 74), (0x8FD52A, 34), (0x8FD572, 46),
    (0x8FD5C2, 36), (0x8FD602, 31), (0x900C70, 70), (0x8FD6AA, 34),
    (0x8FD6F2, 39), (0x8FD2BA, 25), (0x8FD2F2, 35), (0x8FD332, 60),
    (0x8FD38A, 35), (0x8FD3CA, 34), (0x8FD412, 57), (0x900A98, 36),
    (0x8FDD82, 55), (0x8FDDE2, 53), (0x8FDE32, 36), (0x8FDE72, 40),
    (0x8FDEBA, 34), (0x8FDEFA, 36), (0x8FDC2A, 36), (0x900C30, 60),
    (0x8FEBE2, 39), (0x8FE88A, 36), (0x900BB8, 34), (0x8FE932, 69),
    (0x8FED32, 37), (0x8FEE62, 63), (0x8FEFE2, 54), (0x8EE010, 45),
    (0x8FF342, 53), (0x8FF6CA, 34), (0x8FF452, 57), (0x8FF4B2, 53),
    (0x9007D0, 44),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('service', type=Path, help='Recording server data/nbl.ts')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    beginner = {int(n) for row in re.findall(r"BEGINNER_MUSIC_FOLDER\\t([\d,]+)", args.service.read_text(encoding='utf-8'))
                for n in row.split(',')}
    database = ElementTree.fromstring((args.game/'others/music_db.xml').read_bytes().decode('cp932'))
    rows = {}
    kana = ('あいうえおぁぃぅぇぉ', 'かきくけこがぎぐげご', 'さしすせそざじずぜぞ',
            'たちつてとだぢづでどっ', 'なにぬねの', 'はひふへほばびぶべぼぱぴぷぺぽ',
            'まみむめも', 'やゆよゃゅょ', 'らりるれろ', 'わをんゎ')
    for music in database:
        info = music.find('info')
        keys = []
        for field in ('title_yomigana', 'artist_yomigana'):
            text = unicodedata.normalize('NFKC', info.findtext(field, '')).casefold()
            keys.append(''.join(chr(ord(c)-0x60) if '\u30a1' <= c <= '\u30f6' else c for c in text))
        first = keys[0][:1]
        group = next((i for i, letters in enumerate(kana) if first and first in letters), 10)
        ident = int(music.attrib['id'])
        rows[str(ident)] = dict(title_key=keys[0], artist_key=keys[1], kana=group,
            date=int(info.findtext('distribution_date')), genre=int(info.findtext('genre')),
            contests=[int(tag.attrib['id']) for tag in music.findall('tag')], beginner=ident in beginner)
    with (args.game.parent / 'modules/soundvoltex.dll').open('rb') as dll:
        titles = []
        for offset, size in FLOOR_TITLE_RANGES:
            dll.seek(offset)
            titles.append(dll.read(size).decode('cp932'))
    rows['floor_contests'] = titles
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rows, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    print(f'{len(rows)-1} songs; {sum(r["beginner"] for key, r in rows.items() if key != "floor_contests")} beginner songs from recording service')


if __name__ == '__main__':
    main()
