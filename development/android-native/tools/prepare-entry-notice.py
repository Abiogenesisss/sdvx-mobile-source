"""Generate the six entry notice lines locally from the 2026101300 module."""
import json
from pathlib import Path

app = Path(__file__).resolve().parents[1]
module = app.parents[1] / '2026101300/LAZY_KFC/contents/modules/soundvoltex.dll'
ranges = ((9451148, 10), (9451170, 36), (9451213, 24),
          (9451254, 42), (9451303, 38), (9451048, 32))
with module.open('rb') as source:
    values = []
    for offset, length in ranges:
        source.seek(offset)
        values.append(json.dumps(source.read(length).decode('cp932'), ensure_ascii=False))
output = app / 'src/main/cpp/entry_notice_2026101300.inc'
output.write_text(',\n'.join(values) + '\n', encoding='utf-8')
print(output)
