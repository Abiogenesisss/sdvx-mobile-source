"""Prepare assets for the two bundled navigators, room catalog, subscreen and startup."""
import argparse
from importlib import import_module
import json
from pathlib import Path
import shutil
import subprocess
import sys
from xml.etree import ElementTree

from bemani.format import IFS, TwoDX
from PIL import Image
from hdd_inputs import locate_contents, require_profile

def prepare(contents, ffmpeg, sdk, parts=None):
    parts = parts or ("catalog","voices","subscreen","startup")
    data = contents/"data"
    graphics = data/"graphics"
    repo = Path(__file__).resolve().parents[1]
    app = repo/"development/android-native"
    tools = app/"tools"
    sys.path.insert(0,str(tools))
    assets = app/"src/main/assets"
    common = app/"assets-reference"
    work = repo/"local-inputs"
    work.mkdir(exist_ok=True)

    def run(name,*paths):
        subprocess.run([sys.executable, str(tools/(name+".py")), *map(str,paths)],check=True)

    if "catalog" in parts or "subscreen" in parts:
        run("extract-selector-data",contents/"modules/soundvoltex.dll",work)

    if "catalog" in parts:
        run("compile-crew", work, data, assets/"crew_selector")
        crew=assets/"crew"
        art=crew/"art"
        art.mkdir(parents=True,exist_ok=True)
        archive = IFS((graphics/"chara/chara_icon.ifs").read_bytes(),decode_binxml=True)
        xml=ElementTree.fromstring(archive.read_file("tex\\texturelist.xml"))
        regions={image.attrib["name"]:image.findtext("imgrect") for atlas in xml.findall("texture") for image in atlas.findall("image")}
        rows=json.loads((work/"crew-selector.json").read_bytes())
        crews=[]
        for row in sorted(rows,key=lambda r:({178:-2,177:-1}.get(r["id"],r["id"]),r["style"])):
            icon,name,title,preview=row.pop("fields")
            row.update(name=name, bundled=row["id"] in (177,178))
            for field,entry in (("icon",icon),("name_image",title)):
                row[field]=""
                if entry not in regions:
                    continue
                out=art/(entry+".png")
                if not out.exists():
                    x1,x2,y1,y2=(int(v)//2 for v in regions[entry].split())
                    Image.frombytes("RGBA",(x2-x1,y2-y1),archive.read_file("tex\\"+entry),"raw","BGRA").save(out)
                row[field]="crew/art/"+out.name
            if preview:
                out=art/(preview+".png")
                shutil.copyfile(graphics/"crew_icon"/out.name,out)
                row["preview"]="crew/art/"+out.name
            if row["bundled"]:
                role,voice,index=("rasis","vo032_rasis_nabra",0) if row["id"]==178 else ("grace","vo033_grace_nabra",1)
                row.update(voice=voice,actors=[index])
                directory=common/"crew"/role
                directory.mkdir(exist_ok=True,parents=True)
                for key,filename in (("icon","icon.png"),("name_image","name.png")):
                    shutil.copyfile(assets/row[key],directory/filename)
            crews.append(row)
        placements=json.loads((work/"crew-placements.json").read_bytes())
        position=import_module("compile-crew-catalog").crew_placements
        actors=[{"model":f"crew/{role}/model/{role}_ver7.model3.json","placements":position(poses)}
                for role,poses in (("rasis",placements[3:]),("grace",placements[:3]))]
        (crew/"catalog.json").write_text(json.dumps(dict(actors=actors,crews=crews),ensure_ascii=False,separators=(",",":")),encoding="utf-8")
        # Kureha is a bundled stage actor. The HDD directory also contains other costumes.
        actor=graphics/"live2d/kureha_9thkac"
        setting=actor/"kureha_9thkac_0101.model3.json"
        refs=json.loads(setting.read_bytes())["FileReferences"]
        names=[setting.name,refs["Moc"],refs["Physics"],*refs["Textures"],
               *(m["File"] for group in refs["Motions"].values() for m in group)]
        for name in names:
            out=crew/"kureha/model"/name
            out.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(actor/name,out)
    if "voices" in parts:
        from tempfile import TemporaryDirectory
        with TemporaryDirectory(prefix="sdvx-voices-") as scratch:
            run("prepare-navigator",data,sdk,ffmpeg,assets/"crew","--work",scratch)
        run("prepare-crew-library",data,ffmpeg,assets/"crew","--bundled")
    if "subscreen" in parts:
        run("compile-subscreen",data,assets/"subscreen","--stickers",work/"sticker-metadata.json")
        run("prepare-achievements",contents/"modules/soundvoltex.dll",assets/"subscreen/achievements.json")
    if "startup" in parts:
        for source,name in (("0000","0000.ttc"),("0001","0001.ttf"),("0003","0003.ttc"),("0005","0005.ttf"),
                            ("atilwb.ttf","atilwb.ttf"),("DF-HeiSeiMaruGothic-W4.ttc","DF-HeiSeiMaruGothic-W4.ttc"),("WKMRGOTH.TTF","WKMRGOTH.TTF")):
            output=common/"font"/name
            output.parent.mkdir(exist_ok=True,parents=True)
            shutil.copyfile(graphics/"font"/source,output)
        startup=assets/"startup"
        startup.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(graphics/"ver07/warning/warning_j.png",startup/"warning_j.png")
        result = common / "result"
        result.mkdir(parents=True,exist_ok=True)
        grid = IFS((graphics / "ver07/result.ifs").read_bytes(),decode_textures=True)
        (result / "bg_grid_result.png").write_bytes(grid.read_file("tex\\bg_grid_result.png"))
        logos=IFS((graphics/"ver07/logo.ifs").read_bytes(),decode_textures=True)
        for name in ("konami_red50.png","e-amu.png"):
            (startup/name).write_bytes(logos.read_file("tex\\"+name))
        bank=TwoDX((data/"sound/sys_sd_sram.2dx").read_bytes())
        subprocess.run([str(ffmpeg),"-hide_banner","-loglevel","error","-y","-i","pipe:0","-c:a","pcm_s16le",
                        str(startup/"sd_sram04_logo_konami.wav")],input=bank.read_file(bank.filenames[0]),check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hdd",type=Path)
    parser.add_argument("--ffmpeg",type=Path,required=True)
    parser.add_argument("--sdk",type=Path,required=True)
    parser.add_argument("--part",choices=("catalog","voices","subscreen","startup"),nargs="+")
    args = parser.parse_args()
    contents = locate_contents(args.hdd)
    require_profile(contents)
    prepare(contents,args.ffmpeg,args.sdk,args.part)
