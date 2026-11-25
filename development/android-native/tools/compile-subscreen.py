"""Export original VII room/play submonitor pages and catalogue artwork.

AFP retains the authored layout, clipping and transitions. Dynamic images use
named original slots. The phone supplies text through the Android keyboard.
"""
import argparse
from array import array
from importlib import import_module
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
from xml.etree import ElementTree
from PIL import Image
from bemani.format import IFS
from bemani.format.dxt import DXTBuffer


def atlas_images(source, output, *, dxt1_alpha=False, regions=None):
    archive = IFS(source.read_bytes(), decode_binxml=True)
    manifest = ElementTree.fromstring(archive.read_file('tex\\texturelist.xml'))
    files = {}
    output.mkdir(parents=True, exist_ok=True)
    for atlas in manifest.findall('texture'):
        for entry in atlas.findall('image'):
            name = entry.attrib['name']
            rect = [int(v) // 2 for v in entry.find('imgrect').text.split()]
            width, height = rect[1] - rect[0], rect[3] - rect[2]
            file = output / (name + '.png')
            data = archive.read_file('tex\\' + name)
            if atlas.attrib['format'] == 'dxt1':
                if dxt1_alpha:
                    words = array('H', data)
                    words.byteswap()
                    bitmap = Image.frombytes('RGBA', (width, height), words.tobytes(), 'bcn', 1)
                else:
                    bitmap = Image.frombytes('RGBA', (width, height), DXTBuffer(width, height).DXT1Decompress(data, swap=True))
            else:
                bitmap = Image.frombytes('RGBA', (width, height), data, 'raw', 'BGRA')
            if regions is not None:
                left, _, top, _ = [int(v) / 2 for v in entry.findtext('imgrect').split()]
                x1, x2, y1, y2 = [int(v) / 2 for v in entry.findtext('uvrect').split()]
                regions[name] = [x1 - left, y1 - top, x2 - x1, y2 - y1]
            bitmap.save(file)
            files[name] = file
    return files


def appeal_card_thumbnails(graphics, output):
    thumbnails = {}
    regions = {}
    for index in range(1, 14):
        thumbnails.update(atlas_images(graphics / f's_psd_card_{index:02}.ifs', output,
                                       dxt1_alpha=True, regions=regions))
    return thumbnails, regions


def appeal_card_filter(id, genre):
    # Original metadata conversion at soundvoltex.dll 0x180311840.
    mask = 1 << (6 + id // 1000 % 10)
    if genre == 0:
        mask |= 1 << 1
    for flags, bit in [(8, 2), (1, 3), (2, 4), (4 | 16, 5)]:
        if genre & flags:
            mask |= 1 << bit
    return mask


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--stickers', type=Path, default=Path(__file__).with_name('sticker-metadata.json'))
    args = parser.parse_args()
    graphics = args.game / 'graphics'
    afp = import_module('prepare-cabinet-afp')
    afp.prepare(graphics / 'ver07', args.output, *afp.SCENES['subscreen'])
    with TemporaryDirectory(prefix='nabla-subscreen-') as temporary:
        assets = atlas_images(graphics / 'ver07/submonitor_myroom.ifs', Path(temporary))
        shutil.copyfile(assets['icon_star'], args.output / 'favorite.png')
    shutil.copyfile(graphics / 'jk_dummy.png', args.output / 'jacket-empty.png')
    cards = []
    (args.output / 'cards').mkdir(exist_ok=True)
    thumbnails, thumbnail_regions = appeal_card_thumbnails(graphics, args.output / 'card-thumbnails')
    database = ElementTree.fromstring((args.game / 'others/appeal_card.xml').read_bytes().decode('cp932'))
    for row in database:
        info = row.find('info')
        name = info.findtext('texture')
        file = graphics / 'ap_card' / (name + '.png')
        if not file.is_file(): continue
        shutil.copyfile(file, args.output / 'cards' / file.name)
        card_id = int(row.attrib['id'])
        cards.append(dict(id=card_id, name=info.findtext('title', '').replace('[br:0]', '\n'), image='subscreen/cards/' + file.name,
            thumbnail='subscreen/card-thumbnails/' + thumbnails[name].name, thumbnail_uv=thumbnail_regions[name],
            filter_mask=appeal_card_filter(card_id, int(info.findtext('genre', '0'))),
            illustrator=info.findtext('illustrator', ''), sort=int(info.findtext('sort_no', '0')),
            default=info.findtext('is_default', '0') == '1',
            messages=[info.findtext('message_' + char, '').replace('[br:0]', '\n') for char in 'abcdefgh']))
    cards.sort(key=lambda item: (item['sort'], item['id']))
    bases = atlas_images(graphics / 'board_thumbnail01.ifs', args.output / 'bases')
    stickers = atlas_images(graphics / 'sticker_thumbnail01.ifs', args.output / 'stickers')
    sticker_metadata = json.loads(args.stickers.read_bytes())
    catalog = {'cards': cards}
    for key, items, folder in [('bases', bases, 'board_base'), ('stickers', stickers, 'board_sticker')]:
        catalog[key] = []
        for name, thumbnail in sorted(items.items()):
            original = graphics / folder / (name.removesuffix('_t') + '.png')
            target = thumbnail
            if original.is_file():
                target = args.output / key / original.name
                shutil.copyfile(original, target)
            item = dict(id=int(name.split('_')[1]), image='subscreen/' + key + '/' + target.name)
            item['thumbnail'] = f'subscreen/{key}/{thumbnail.name}'
            if key == 'stickers':
                item.update(sticker_metadata[str(item['id'])])
            catalog[key].append(item)
    (args.output / 'catalog.json').write_text(json.dumps(catalog, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    print('Exported original submonitor cards:', len(cards),
          'bases:', len(catalog['bases']), 'stickers:', len(catalog['stickers']))


if __name__ == '__main__':
    main()
