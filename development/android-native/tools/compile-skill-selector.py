"""Export the original VII course selector and its local server course lists."""
import argparse
from importlib import import_module
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('server_data', type=Path, nargs='?')
    parser.add_argument('output', type=Path)
    parser.add_argument('--source', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--courses-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    if not args.courses_only:
        afp = import_module('prepare-cabinet-afp')
        graphics = args.game / 'graphics/ver07'
        afp.prepare(graphics, args.output, *afp.SCENES['skill_selector'])
        afp.SKILL_TOTAL.copy_static(graphics, args.output, 'skill_selector')
    if args.server_data is None:
        (args.output/'courses.json').write_text('[]',encoding='utf-8')
        print('No optional service table supplied; no server-defined course sets.')
        return
    source = args.server_data.read_text(encoding='utf-8').split('export const COURSES7 = ',1)[1].split('export const ',1)[0].strip().rstrip(';')
    source = re.sub(r'//[^\n]*','',source)
    source = re.sub(r'\b(\w+)\s*:',r'"\1":',source)
    source = re.sub(r',\s*([}\]])',r'\1',source)
    courses = json.loads(source)
    (args.output/'courses.json').write_text(json.dumps(courses,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
    print('Exported course sets:',len(courses),'courses:',sum(len(row['courses']) for row in courses))


if __name__ == '__main__':
    main()
