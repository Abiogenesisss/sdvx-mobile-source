"""Find an arcade contents directory without relying on a distribution's folder name."""
from pathlib import Path


def locate_contents(root):
    root = Path(root).resolve(strict=True)
    if root.name.casefold() == "data":
        root = root.parent
    candidates = [root, root / "contents", *root.glob("*/contents"), *root.glob("*/*/contents")]
    found = sorted({p.resolve() for p in candidates
                    if (p / "data/others/music_db.xml").is_file() and (p / "modules/soundvoltex.dll").is_file()})
    if len(found) != 1:
        raise ValueError(f"Found {len(found)} HDD layouts under {root}. Pass one contents directory.")
    return found[0]
