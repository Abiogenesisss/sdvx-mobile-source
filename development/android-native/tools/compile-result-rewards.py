"""Export original NABLA result animations and their original numeral and emblem artwork."""
import argparse
from importlib import import_module
from pathlib import Path

from bemani.format import IFS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenes', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('assets', type=Path)
    parser.add_argument('data', type=Path)
    args = parser.parse_args()
    afp = import_module('prepare-cabinet-afp')
    graphics = args.data / 'graphics/ver07'
    for scene in ('result_force', 'total_result'):
        output = args.assets / scene
        afp.prepare(graphics, output, *afp.SCENES[scene])
        afp.SKILL_TOTAL.copy_static(graphics, output, scene)
    archive = IFS((graphics / 'force.ifs').read_bytes(), decode_textures=True)
    for name in archive.filenames:
        if name.startswith('tex\\em6') and name.endswith('.png'):
            (args.assets / 'result_force' / name.split('\\')[-1]).write_bytes(archive.read_file(name))


if __name__ == '__main__':
    main()
