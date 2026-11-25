"""Read VII's achievement initializers from the supplied 2026101300 game DLL.

The 80-byte records end in a CP932 title pointer at +72. The constructor fills
the records with RIP-relative stores; reading the next record would shift IDs
and rewards by one. Source addresses remain in the exported audit metadata.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import pefile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    pe = pefile.PE(data=data, fast_load=True)
    start, end = pe.get_offset_from_rva(0x25720), pe.get_offset_from_rva(0x63adb)
    memory = {}
    for pattern, size in [(rb'\xc7\x05(....)(....)', 4), (rb'\xc6\x05(....)(.)', 1)]:
        for hit in re.finditer(pattern, data[start:end], re.S):
            offset = start + hit.start()
            address = pe.get_rva_from_offset(offset) + 6 + size + struct.unpack('<i', hit[1])[0]
            for byte, value in enumerate(hit[2]): memory[address + byte] = value

    def value(address):
        original = pe.get_data(address, 4).ljust(4, b'\0')
        return int.from_bytes(bytes(memory.get(address + i, original[i]) for i in range(4)), 'little')

    categories = ['','日次プレー','毎日プレー','毎週プレー','プレー回数','通算プレー数',
        'クリアメダル','スコアグレード','SKILL ANALYZER','VOLFORCE CLASS','アピールパーツ',
        'オンラインマッチング','ローカルマッチング','その他マッチング','MEGAMIX BATTLE',
        'ARENA BATTLE','クリアレベル','BLASTER','ΩDimension','HEXA DIVER','VARIANT GATE',
        'ドラマティッククリア','特定期間プレー','楽曲コンテスト','都道府県','地域制覇','イベントミッション','ボルテクイズ']
    rows, excluded = [], []
    for hit in re.finditer(rb'\x48\x8d\x05(....)\x48\x89\x05(....)', data[start:end], re.S):
        offset = start + hit.start()
        rva = pe.get_rva_from_offset(offset)
        text = rva + 7 + struct.unpack('<i', hit[1])[0]
        record = rva + 14 + struct.unpack('<i', hit[2])[0] - 72
        if not 0x800000 < text < 0xa80000 or record < 0xb3bba0: continue
        identifier, category = value(record), value(record + 4)
        if not 0 < identifier <= 1560 or (record - 0xb3bba0) % 80: continue
        at = pe.get_offset_from_rva(text)
        title = data[at:data.index(b'\0', at)].decode('cp932')
        if category == 0:
            excluded.append(identifier)
            continue
        assert category < len(categories)
        numbers = [int(n) for n in re.findall(r'[0-9]+', title)]
        metric, goal, parameter = '', numbers[-1] if numbers else 1, 0
        if category in (1,2,3,4,5): metric = {1:'today',2:'day_chain',3:'week_chain',4:'plays',5:'days'}[category]
        elif category == 6:
            metric = 'lamp'
            parameter = 5 if 'S-PERFECT' in title else 4 if 'PERFECT' in title else 3 if 'ULTIMATE' in title else 2
        elif category == 7:
            metric = 'grade'
            grades = ['S','AAA+','AAA','AA+','AA','A+','A','B','C','D']
            match = re.search(r'GRADE ([A-Z]+\+?)', title)
            if match and match[1] in grades: parameter = grades.index(match[1])
            else: metric = ''
        elif category == 10: metric = 'parts'
        elif category == 16 and numbers:
            metric, parameter = 'level', numbers[0]
            goal = numbers[-1] if len(numbers) > 1 else 1
        if category in (4,5) and 'SOUND VOLTEX' in title and '∇' not in title: metric = ''
        rows.append(dict(id=identifier, category=category, title=title, trophy=value(record+12),
            reward_type=value(record+24), reward=value(record+28), prerequisite=value(record+32),
            metric=metric, goal=goal, parameter=parameter, source_address=hex(0x180000000+rva)))
    assert len(rows) == len({row['id'] for row in rows}) and len(rows) > 1400
    result = dict(source='2026101300/LAZY_KFC/contents/modules/soundvoltex.dll',
                  categories=categories, items=rows, excluded=excluded)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    print(f'Exported {len(rows)} original achievement records; inactive category-zero IDs: {excluded}')


if __name__ == '__main__':
    main()
