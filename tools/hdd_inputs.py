"""Find an arcade contents directory without relying on a distribution's folder name."""
from pathlib import Path

PROFILE_SHA256 = "050effda75a63aaab67aff3a49774defe09e4f5ea926c8175bd5cc4288c1a44c"


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


def require_profile(contents):
    import hashlib
    digest = hashlib.sha256((contents / "modules/soundvoltex.dll").read_bytes()).hexdigest()
    if digest != PROFILE_SHA256:
        raise ValueError(f"Recognized the HDD layout but no DLL extraction profile matches SHA-256 {digest}. "
                         "Use a supported module or implement and validate its offsets first.")
