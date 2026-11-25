"""Export the original VII APPEAL PARTS and VALKYRIE generator."""
import argparse
from importlib import import_module
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('graphics', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    afp = import_module('prepare-cabinet-afp')
    afp.prepare(args.graphics / 'ver07', args.output, *afp.SCENES['generator_vii'])
    shutil.copyfile(args.graphics / 'apipa_banner/bn_gene_dummy.png', args.output / 'appeal-parts.png')


if __name__ == '__main__':
    main()
